using System.Buffers.Binary;
using System.Net;
using System.Net.NetworkInformation;
using System.Net.Sockets;
using System.Reflection;
using System.Text;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;

namespace Multiroom.Server.Web.Api;

// Announces the API over mDNS / DNS-SD (RFC 6762, 6763) as
// "Galdrun on <host>._galdrun._tcp.local", so the smartphone app finds this
// server without being told its address. mDNS rather than a UDP beacon like
// the client's and the core's: iOS lets an app browse Bonjour services
// without the multicast entitlement that listening for a broadcast needs.
//
// A responder for this one service and nothing else, IPv4 only: it answers
// the PTR, SRV, TXT and A questions a browser asks for it (and the DNS-SD
// service enumeration), announces itself twice at startup and says goodbye
// (TTL 0) when it stops. Every answer carries the address of the interface
// the question came in on, so a phone on the LAN is never handed a Hyper-V
// switch's address. No probing and no known-answer suppression: the instance
// name contains the host name, which the LAN already keeps unique.
//
// Nothing is announced while the API is bound to loopback (the default): an
// app on another device could not reach it anyway.
public sealed partial class ServiceAnnouncer(ApiEndpoint api, ApiEndpointOptions options,
    ILogger<ServiceAnnouncer> log) : BackgroundService
{
    public const string ServiceType = Branding.ServiceType;

    private const string ServiceEnumeration = "_services._dns-sd._udp.local";
    private const int MdnsPort = 5353;
    private const ushort TypeA = 1;
    private const ushort TypePtr = 12;
    private const ushort TypeTxt = 16;
    private const ushort TypeSrv = 33;
    private const ushort TypeAny = 255;
    private const uint Ttl = 120;
    // RFC 6762 section 6.7: a legacy unicast answer is cached for at most 10 s.
    private const uint LegacyTtl = 10;

    private static readonly IPAddress Group = IPAddress.Parse("224.0.0.251");
    private static readonly IPEndPoint GroupEndPoint = new(Group, MdnsPort);
    private static readonly TimeSpan InterfaceRefresh = TimeSpan.FromSeconds(30);

    private static readonly string Version =
        typeof(ServiceAnnouncer).Assembly.GetCustomAttribute<AssemblyInformationalVersionAttribute>()
            ?.InformationalVersion ?? "unknown";

    private readonly HashSet<IPAddress> _joined = [];
    private List<(int Index, IPAddress Address)> _interfaces = [];
    private DateTime _interfacesListed;

    protected override async Task ExecuteAsync(CancellationToken stoppingToken)
    {
        if (IPAddress.IsLoopback(options.Bind))
        {
            LogLoopback(log, options.Bind);
            return;
        }

        var machine = Dns.GetHostName().Split('.')[0];
        var service = new Service(
            Instance: $"{Branding.InstancePrefix}{Truncate(machine, 50)}.{ServiceType}",
            Host: $"{HostLabel(machine)}.local",
            Port: api.BaseUri?.Port ?? options.Port,
            Txt: ["txtvers=1", $"version={Version}", "path=/api/v1/", $"auth={(options.Token is null ? "none" : "token")}"]);

        Socket socket;
        try
        {
            socket = Open();
        }
        catch (SocketException e)
        {
            LogCannotOpen(log, e.Message);
            return;
        }

        using (socket)
        {
            RefreshInterfaces(socket);
            var addresses = string.Join(", ", _interfaces.Select(i => i.Address));
            LogAnnouncing(log, service.Instance, service.Host, service.Port, addresses);
            try
            {
                // Twice, a second apart (RFC 6762 section 8.3): one datagram
                // may be lost, and the phone may be asleep for the first.
                await AnnounceAsync(socket, service, Ttl, stoppingToken).ConfigureAwait(false);
                await Task.Delay(TimeSpan.FromSeconds(1), stoppingToken).ConfigureAwait(false);
                await AnnounceAsync(socket, service, Ttl, stoppingToken).ConfigureAwait(false);
                await AnswerAsync(socket, service, stoppingToken).ConfigureAwait(false);
            }
            catch (OperationCanceledException) when (stoppingToken.IsCancellationRequested)
            {
            }
            finally
            {
                // Not cancelled: the goodbye is what this path is for.
                await AnnounceAsync(socket, service, 0, CancellationToken.None).ConfigureAwait(false);
            }
        }
    }

    private static Socket Open()
    {
        var socket = new Socket(AddressFamily.InterNetwork, SocketType.Dgram, ProtocolType.Udp);
        try
        {
            // Shared: Windows' own DNS client and Bonjour already hold 5353.
            socket.SetSocketOption(SocketOptionLevel.Socket, SocketOptionName.ReuseAddress, true);
            socket.SetSocketOption(SocketOptionLevel.IP, SocketOptionName.PacketInformation, true);
            socket.SetSocketOption(SocketOptionLevel.IP, SocketOptionName.MulticastTimeToLive, 255);
            socket.Bind(new IPEndPoint(IPAddress.Any, MdnsPort));
            return socket;
        }
        catch
        {
            socket.Dispose();
            throw;
        }
    }

    private async Task AnswerAsync(Socket socket, Service service, CancellationToken cancellationToken)
    {
        var buffer = new byte[9000];
        while (!cancellationToken.IsCancellationRequested)
        {
            SocketReceiveMessageFromResult received;
            try
            {
                received = await socket.ReceiveMessageFromAsync(buffer, SocketFlags.None,
                    new IPEndPoint(IPAddress.Any, 0), cancellationToken).ConfigureAwait(false);
            }
            catch (SocketException)
            {
                // Windows reports an earlier send's ICMP error on the next
                // receive; the socket itself is fine.
                continue;
            }

            if (!TryParseQuery(buffer.AsSpan(0, received.ReceivedBytes), out var id, out var questions))
                continue;
            RefreshInterfaces(socket);
            var address = _interfaces.FirstOrDefault(i => i.Index == received.PacketInformation.Interface).Address;
            if (address is null)
                continue;

            // A querier that is not on 5353 is a plain DNS resolver asking
            // once (RFC 6762 section 6.7): it gets a unicast answer to its id,
            // with its questions echoed. Everyone else is answered on the group.
            var remote = (IPEndPoint)received.RemoteEndPoint;
            var legacy = remote.Port != MdnsPort;
            var (answers, additionals) = Answers(service, questions, address, legacy ? LegacyTtl : Ttl, !legacy);
            if (answers.Count == 0)
                continue;
            var packet = Encode(legacy ? id : (ushort)0, legacy ? questions : [], answers, additionals);
            await SendAsync(socket, packet, address, legacy ? remote : GroupEndPoint, cancellationToken)
                .ConfigureAwait(false);
        }
    }

    // Every interface gets its own copy, carrying its own address.
    private async Task AnnounceAsync(Socket socket, Service service, uint ttl, CancellationToken cancellationToken)
    {
        foreach (var (_, address) in _interfaces)
        {
            var records = Records(service, address, ttl, cacheFlush: true);
            var packet = Encode(0, [], [records.Ptr, records.Srv, records.Txt, records.A], []);
            await SendAsync(socket, packet, address, GroupEndPoint, cancellationToken).ConfigureAwait(false);
        }
    }

    private async Task SendAsync(Socket socket, byte[] packet, IPAddress from, IPEndPoint to,
        CancellationToken cancellationToken)
    {
        try
        {
            socket.SetSocketOption(SocketOptionLevel.IP, SocketOptionName.MulticastInterface, from.GetAddressBytes());
            await socket.SendToAsync(packet, SocketFlags.None, to, cancellationToken).ConfigureAwait(false);
        }
        catch (SocketException e)
        {
            // An interface that went down since it was listed: the next
            // refresh drops it.
            LogSendFailed(log, from, e.Message);
        }
    }

    // The IPv4 interfaces that are up and can multicast, the group joined on
    // each. Listed again every InterfaceRefresh, because a DHCP lease or a
    // cable can change them while the server runs.
    private void RefreshInterfaces(Socket socket)
    {
        if (DateTime.UtcNow - _interfacesListed < InterfaceRefresh)
            return;
        _interfacesListed = DateTime.UtcNow;

        var interfaces = new List<(int, IPAddress)>();
        foreach (var nic in NetworkInterface.GetAllNetworkInterfaces())
        {
            if (nic.OperationalStatus != OperationalStatus.Up || !nic.SupportsMulticast ||
                nic.NetworkInterfaceType == NetworkInterfaceType.Loopback ||
                !nic.Supports(NetworkInterfaceComponent.IPv4))
            {
                continue;
            }
            var properties = nic.GetIPProperties();
            var address = properties.UnicastAddresses
                .Select(u => u.Address)
                .FirstOrDefault(a => a.AddressFamily == AddressFamily.InterNetwork);
            // An API bound to one address is announced on that interface only.
            if (address is null || (!options.Bind.Equals(IPAddress.Any) && !options.Bind.Equals(address)))
                continue;
            interfaces.Add((properties.GetIPv4Properties().Index, address));
            if (_joined.Contains(address))
                continue;
            try
            {
                socket.SetSocketOption(SocketOptionLevel.IP, SocketOptionName.AddMembership,
                    new MulticastOption(Group, address));
                _joined.Add(address);
            }
            catch (SocketException e)
            {
                LogCannotJoin(log, address, e.Message);
            }
        }
        _interfaces = interfaces;
    }

    // --- the records ----------------------------------------------------------

    private sealed record Service(string Instance, string Host, int Port, string[] Txt);

    private readonly record struct Question(string Name, ushort Type, ushort Class);

    private sealed record Record(string Name, ushort Type, bool CacheFlush, uint Ttl, byte[] Data);

    private static (Record Ptr, Record Srv, Record Txt, Record A, Record Enumeration) Records(Service service,
        IPAddress address, uint ttl, bool cacheFlush)
    {
        var srv = new List<byte>();
        WriteUInt16(srv, 0); // priority
        WriteUInt16(srv, 0); // weight
        WriteUInt16(srv, service.Port);
        WriteName(srv, service.Host);

        var txt = new List<byte>();
        foreach (var entry in service.Txt)
        {
            var bytes = Encoding.UTF8.GetBytes(entry);
            txt.Add((byte)bytes.Length);
            txt.AddRange(bytes);
        }

        // PTRs are shared records (many servers answer for one type), so they
        // never set cache-flush; the other three belong to this server alone.
        return (new Record(ServiceType, TypePtr, false, ttl, Name(service.Instance)),
            new Record(service.Instance, TypeSrv, cacheFlush, ttl, [.. srv]),
            new Record(service.Instance, TypeTxt, cacheFlush, ttl, [.. txt]),
            new Record(service.Host, TypeA, cacheFlush, ttl, address.GetAddressBytes()),
            new Record(ServiceEnumeration, TypePtr, false, ttl, Name(ServiceType)));
    }

    private static (List<Record> Answers, List<Record> Additionals) Answers(Service service,
        IReadOnlyList<Question> questions, IPAddress address, uint ttl, bool cacheFlush)
    {
        var records = Records(service, address, ttl, cacheFlush);
        var answers = new List<Record>();
        var additionals = new List<Record>();
        foreach (var question in questions)
        {
            bool Asks(string name, ushort type) =>
                question.Name.Equals(name, StringComparison.OrdinalIgnoreCase) &&
                (question.Type == type || question.Type == TypeAny);

            if (Asks(ServiceType, TypePtr))
            {
                answers.Add(records.Ptr);
                additionals.AddRange([records.Srv, records.Txt, records.A]);
            }
            if (Asks(ServiceEnumeration, TypePtr))
                answers.Add(records.Enumeration);
            if (Asks(service.Instance, TypeSrv))
            {
                answers.Add(records.Srv);
                additionals.Add(records.A);
            }
            if (Asks(service.Instance, TypeTxt))
                answers.Add(records.Txt);
            if (Asks(service.Host, TypeA))
                answers.Add(records.A);
        }
        var distinct = answers.Distinct().ToList();
        return (distinct, [.. additionals.Distinct().Except(distinct)]);
    }

    // --- the wire -------------------------------------------------------------

    private static byte[] Encode(ushort id, IReadOnlyList<Question> questions, IReadOnlyList<Record> answers,
        IReadOnlyList<Record> additionals)
    {
        var packet = new List<byte>(512);
        WriteUInt16(packet, id);
        WriteUInt16(packet, 0x8400); // a response, authoritative
        WriteUInt16(packet, questions.Count);
        WriteUInt16(packet, answers.Count);
        WriteUInt16(packet, 0);
        WriteUInt16(packet, additionals.Count);
        foreach (var question in questions)
        {
            WriteName(packet, question.Name);
            WriteUInt16(packet, question.Type);
            WriteUInt16(packet, question.Class & 0x7FFF);
        }
        foreach (var record in answers.Concat(additionals))
        {
            WriteName(packet, record.Name);
            WriteUInt16(packet, record.Type);
            WriteUInt16(packet, record.CacheFlush ? 0x8001 : 0x0001);
            var ttl = new byte[4];
            BinaryPrimitives.WriteUInt32BigEndian(ttl, record.Ttl);
            packet.AddRange(ttl);
            WriteUInt16(packet, record.Data.Length);
            packet.AddRange(record.Data);
        }
        return [.. packet];
    }

    // A query's id and questions; false for a response, or for anything that
    // does not parse -- the port is open to the whole network.
    private static bool TryParseQuery(ReadOnlySpan<byte> packet, out ushort id, out List<Question> questions)
    {
        id = 0;
        questions = [];
        if (packet.Length < 12)
            return false;
        id = BinaryPrimitives.ReadUInt16BigEndian(packet);
        var flags = BinaryPrimitives.ReadUInt16BigEndian(packet[2..]);
        if ((flags & 0x8000) != 0)
            return false;
        var count = BinaryPrimitives.ReadUInt16BigEndian(packet[4..]);
        var offset = 12;
        for (var i = 0; i < count; i++)
        {
            if (!TryReadName(packet, ref offset, out var name) || offset + 4 > packet.Length)
                return false;
            questions.Add(new Question(name, BinaryPrimitives.ReadUInt16BigEndian(packet[offset..]),
                BinaryPrimitives.ReadUInt16BigEndian(packet[(offset + 2)..])));
            offset += 4;
        }
        return questions.Count > 0;
    }

    // Labels up to the root, following compression pointers (RFC 1035 4.1.4),
    // with a bound on the jumps so a pointer loop is a refusal, not a hang.
    private static bool TryReadName(ReadOnlySpan<byte> packet, ref int offset, out string name)
    {
        name = "";
        var labels = new List<string>();
        var position = offset;
        var jumped = false;
        for (var jumps = 0; ;)
        {
            if (position >= packet.Length)
                return false;
            int length = packet[position];
            if (length == 0)
            {
                position++;
                break;
            }
            if ((length & 0xC0) == 0xC0)
            {
                if (position + 1 >= packet.Length || ++jumps > 16)
                    return false;
                if (!jumped)
                    offset = position + 2;
                jumped = true;
                position = ((length & 0x3F) << 8) | packet[position + 1];
                continue;
            }
            if ((length & 0xC0) != 0 || position + 1 + length > packet.Length)
                return false;
            labels.Add(Encoding.UTF8.GetString(packet.Slice(position + 1, length)));
            position += 1 + length;
        }
        if (!jumped)
            offset = position;
        name = string.Join('.', labels);
        return true;
    }

    private static byte[] Name(string name)
    {
        var bytes = new List<byte>();
        WriteName(bytes, name);
        return [.. bytes];
    }

    // Only the service type and ".local" are split on dots: the instance
    // label is everything before the service type, dots and all.
    private static void WriteName(List<byte> packet, string name)
    {
        var labels = name.EndsWith("." + ServiceType, StringComparison.OrdinalIgnoreCase)
            ? [name[..^(ServiceType.Length + 1)], .. ServiceType.Split('.')]
            : name.Split('.');
        foreach (var label in labels)
        {
            var bytes = Encoding.UTF8.GetBytes(label);
            packet.Add((byte)bytes.Length);
            packet.AddRange(bytes);
        }
        packet.Add(0);
    }

    private static void WriteUInt16(List<byte> packet, int value)
    {
        packet.Add((byte)(value >> 8));
        packet.Add((byte)value);
    }

    // A DNS host label: lower-case letters, digits and '-', at most 63 bytes.
    private static string HostLabel(string machine)
    {
        var label = new string([.. machine.ToLowerInvariant().Select(c => char.IsAsciiLetterOrDigit(c) ? c : '-')])
            .Trim('-');
        return label.Length == 0 ? Branding.Slug : Truncate(label, 63);
    }

    private static string Truncate(string text, int length) => text.Length <= length ? text : text[..length];

    [LoggerMessage(Level = LogLevel.Information,
        Message = "the API is bound to {Bind}; not announced over mDNS, since no other device could reach it")]
    private static partial void LogLoopback(ILogger log, IPAddress bind);

    [LoggerMessage(Level = LogLevel.Information,
        Message = "announcing \"{Instance}\" over mDNS as {Host}:{Port} on {Addresses}")]
    private static partial void LogAnnouncing(ILogger log, string instance, string host, int port, string addresses);

    [LoggerMessage(Level = LogLevel.Warning, Message = "cannot open the mDNS socket ({Reason}); the app will not find this server by itself")]
    private static partial void LogCannotOpen(ILogger log, string reason);

    [LoggerMessage(Level = LogLevel.Debug, Message = "cannot join the mDNS group on {Address}: {Reason}")]
    private static partial void LogCannotJoin(ILogger log, IPAddress address, string reason);

    [LoggerMessage(Level = LogLevel.Debug, Message = "mDNS send from {Address} failed: {Reason}")]
    private static partial void LogSendFailed(ILogger log, IPAddress address, string reason);
}
