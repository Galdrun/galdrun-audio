using System.Net;
using System.Net.Sockets;
using Microsoft.Extensions.Logging;
using SMBLibrary;
using SMBLibrary.Client;
using Multiroom.Server.Abstractions;

namespace Multiroom.Server.Library.Smb;

// One server's sessions as `status` shows them: how many are open, and how
// many are held by a reader, a pass or a resolved stream now.
public sealed record SmbServerStatus(string Server, int Sessions, int InUse);

// The SMB sessions of every NAS the library reads (rule 6): at most
// SessionsPerServer per server, shared by whoever needs one, and closed once
// nobody has held them for IdleTimeout.
//
// A session is one SMBLibrary client, which is synchronous and not
// thread-safe: everything on it runs under its own lock, one request at a
// time, and never on a thread that waits on 4713 (the indexer's thread, a
// media GET, a bounded task). Only SMB 2.0.2 and later are negotiated,
// signing and encryption as the server asks; SMB1 never.
public sealed partial class SmbSessions : IDisposable
{
    public const int SessionsPerServer = 4;

    public static readonly TimeSpan IdleTimeout = TimeSpan.FromSeconds(60);

    // Connect, negotiate and log in. Past it the server is unreachable.
    public static readonly TimeSpan ConnectTimeout = TimeSpan.FromSeconds(10);

    // One request's answer. Long enough for a NAS whose disks spin up.
    private static readonly TimeSpan ResponseTimeout = TimeSpan.FromSeconds(20);

    private readonly Lock _lock = new();
    private readonly List<Session> _sessions = [];
    private readonly Dictionary<string, int> _opening = new(StringComparer.Ordinal);
    private readonly ILogger _log;
    private readonly Timer _idle;
    private bool _disposed;

    public SmbSessions(ILogger<SmbSessions> log)
    {
        _log = log;
        _idle = new Timer(_ => CloseIdle(), null, TimeSpan.FromSeconds(10), TimeSpan.FromSeconds(10));
    }

    public IReadOnlyList<SmbServerStatus> Status
    {
        get
        {
            lock (_lock)
            {
                return
                [
                    .. _sessions.GroupBy(s => s.Server, StringComparer.Ordinal)
                        .Select(g => new SmbServerStatus(g.Key, g.Count(), g.Count(s => s.Users > 0)))
                        .OrderBy(s => s.Server, StringComparer.Ordinal),
                ];
            }
        }
    }

    // A session logged in as `login`, held until the lease is disposed. An
    // idle session of the same login is taken first, then a new one while
    // the server has fewer than SessionsPerServer, then the least used one of
    // the same login is shared.
    public Result<SmbLease, LibraryError> Lease(SmbLogin login)
    {
        Session? chosen;
        Session? evicted = null;
        lock (_lock)
        {
            ObjectDisposedException.ThrowIf(_disposed, this);
            var server = login.Server;
            var mine = _sessions.Where(s => s.Key == login.Key && !s.Broken).MinBy(s => s.Users);
            var open = _sessions.Count(s => s.Server == server) + _opening.GetValueOrDefault(server);
            if (mine is { Users: 0 })
            {
                chosen = mine;
            }
            else if (open < SessionsPerServer)
            {
                chosen = null;
            }
            else if (mine is not null)
            {
                chosen = mine;
            }
            else
            {
                // Another login's idle session makes room for this one.
                evicted = _sessions.FirstOrDefault(s => s.Server == server && s.Users == 0);
                if (evicted is null)
                {
                    return Result.Fail(LibraryError.Unreachable,
                        $"all {SessionsPerServer} sessions to {server} are in use");
                }
                _sessions.Remove(evicted);
                chosen = null;
            }

            if (chosen is not null)
            {
                chosen.Users++;
            }
            else
            {
                _opening[server] = _opening.GetValueOrDefault(server) + 1;
            }
        }

        evicted?.Close();
        if (chosen is not null)
            return new SmbLease(this, chosen);

        var opened = Open(login);
        lock (_lock)
        {
            if (--_opening[login.Server] == 0)
                _opening.Remove(login.Server);
            if (!opened.IsSuccess)
                return opened.Failure;
            if (_disposed)
            {
                opened.Value.Close();
                throw new ObjectDisposedException(nameof(SmbSessions));
            }
            opened.Value.Users = 1;
            _sessions.Add(opened.Value);
        }
        LogOpened(_log, login.Server);
        return new SmbLease(this, opened.Value);
    }

    public void Dispose()
    {
        List<Session> sessions;
        lock (_lock)
        {
            if (_disposed)
                return;
            _disposed = true;
            sessions = [.. _sessions];
            _sessions.Clear();
        }
        _idle.Dispose();
        foreach (var session in sessions)
            session.Close();
    }

    internal void Release(Session session)
    {
        var close = false;
        lock (_lock)
        {
            session.Users--;
            session.IdleSince = Environment.TickCount64;
            if (session.Users == 0 && session.Broken && _sessions.Remove(session))
                close = true;
        }
        if (close)
            session.Close();
    }

    private void CloseIdle()
    {
        List<Session> idle;
        lock (_lock)
        {
            var now = Environment.TickCount64;
            idle = [.. _sessions.Where(s => s.Users == 0 &&
                (s.Broken || now - s.IdleSince >= (long)IdleTimeout.TotalMilliseconds))];
            foreach (var session in idle)
                _sessions.Remove(session);
        }
        foreach (var session in idle)
        {
            session.Close();
            LogClosed(_log, session.Server);
        }
    }

    // Connect and log in, within ConnectTimeout. A client that answers only
    // after that is closed when it does.
    private static Result<Session, LibraryError> Open(SmbLogin login)
    {
        var attempt = Task.Run(() => Connect(login));
        if (attempt.Wait(ConnectTimeout))
            return attempt.Result;
        attempt.ContinueWith(t =>
        {
            if (t.Result.IsSuccess)
                t.Result.Value.Close();
        }, TaskScheduler.Default);
        return Result.Fail(LibraryError.Unreachable,
            $"{login.Server} did not answer within {ConnectTimeout.TotalSeconds:0} s");
    }

    private static Result<Session, LibraryError> Connect(SmbLogin login)
    {
        IPAddress address;
        try
        {
            if (!IPAddress.TryParse(login.Host, out address!))
            {
                var addresses = Dns.GetHostAddresses(login.Host);
                address = addresses.FirstOrDefault(a => a.AddressFamily == AddressFamily.InterNetwork) ??
                    addresses.FirstOrDefault() ??
                    throw new SocketException((int)SocketError.HostNotFound);
            }
        }
        catch (SocketException e)
        {
            return Result.Fail(LibraryError.Unreachable, $"{login.Server}: {e.SocketErrorCode}");
        }

        var client = new PortedClient((int)ResponseTimeout.TotalMilliseconds);
        try
        {
            if (!client.Connect(address, login.Port))
                return Result.Fail(LibraryError.Unreachable, $"{login.Server} refused SMB 2 or later, or did not answer");
            var status = login.User is null
                ? client.Login(login.Domain ?? "", "Guest", "")
                : client.Login(login.Domain ?? "", login.User, login.Password ?? "");
            if (status != NTStatus.STATUS_SUCCESS)
            {
                client.Disconnect();
                return Result.Fail(IsRefusedLogin(status) ? LibraryError.AuthFailed : LibraryError.Unreachable,
                    $"{login.Server} refused the login ({status})");
            }
            return new Session(login.Key, login.Server, client);
        }
#pragma warning disable CA1031 // SMBLibrary throws plain Exception on a broken connection; it is this server's failure to reach.
        catch (Exception e)
#pragma warning restore CA1031
        {
            try
            {
                client.Disconnect();
            }
#pragma warning disable CA1031 // Already failing: a disconnect that fails too changes nothing.
            catch (Exception)
#pragma warning restore CA1031
            {
            }
            return Result.Fail(LibraryError.Unreachable, $"{login.Server}: {e.Message}");
        }
    }

    internal static bool IsRefusedLogin(NTStatus status) => status is NTStatus.STATUS_LOGON_FAILURE
        or NTStatus.STATUS_WRONG_PASSWORD or NTStatus.STATUS_ACCOUNT_RESTRICTION or NTStatus.STATUS_PASSWORD_EXPIRED
        or NTStatus.STATUS_ACCOUNT_DISABLED or NTStatus.STATUS_ACCOUNT_LOCKED_OUT or NTStatus.STATUS_ACCESS_DENIED;

    [LoggerMessage(Level = LogLevel.Debug, Message = "smb: session to {Server} opened")]
    private static partial void LogOpened(ILogger log, string server);

    [LoggerMessage(Level = LogLevel.Debug, Message = "smb: idle session to {Server} closed")]
    private static partial void LogClosed(ILogger log, string server);

    // SMBLibrary connects to 445 only; its port overload is for subclasses,
    // and the tests' server listens on a free port.
    private sealed class PortedClient(int responseTimeoutMilliseconds)
        : SMB2Client(responseTimeoutMilliseconds, enableSMB311Support: true, resolveDfsNamespace: false)
    {
        public bool Connect(IPAddress address, int port) =>
            Connect(address, SMBTransportType.DirectTCPTransport, port);
    }

    // One logged-in client and the shares it has connected to. Users,
    // IdleSince and Broken are the pool's, under its lock; Gate is the
    // client's own.
    internal sealed class Session(string key, string server, SMB2Client client)
    {
        public string Key { get; } = key;
        public string Server { get; } = server;
        public SMB2Client Client { get; } = client;
        public Lock Gate { get; } = new();
        public Dictionary<string, ISMBFileStore> Trees { get; } = new(StringComparer.OrdinalIgnoreCase);
        public int Users { get; set; }
        public long IdleSince { get; set; } = Environment.TickCount64;
        public volatile bool Broken;

        public void Close()
        {
            lock (Gate)
            {
                try
                {
                    foreach (var tree in Trees.Values)
                        tree.Disconnect();
                    Client.Logoff();
                    Client.Disconnect();
                }
#pragma warning disable CA1031 // Closing a session to a server that is gone: nothing is left to fail.
                catch (Exception)
#pragma warning restore CA1031
                {
                }
            }
        }
    }
}
