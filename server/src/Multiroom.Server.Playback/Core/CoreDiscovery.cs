using System.Collections.Concurrent;
using System.Net;
using System.Net.Sockets;
using System.Text.Json;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;
using Multiroom.Server.Abstractions;

namespace Multiroom.Server.Playback.Core;

// A core that announced itself: its id, and where its control API is. Host is
// the beacon's source address, the only one this server can reach it on.
public sealed record DiscoveredCore(string CoreId, string Host, int ControlPort, string? Version,
    DateTimeOffset LastSeen);

// Port 0 takes any free port; the tests use it. SelectionPath is where the
// chosen core's id is kept, in the data directory.
public sealed record CoreDiscoveryOptions(int Port, string SelectionPath);

// The server cores on the network, from their beacons
// (`shared/matp/include/matp/CoreBeacon.h`: one JSON datagram to UDP 4722
// every 2 s), and which of them this server uses.
//
// The choice is stored by id, not address: a core that comes back on a new
// DHCP lease is followed there. With none chosen, CoreLink keeps to
// core.host/core.port from the config, as before discovery existed. A datagram
// that is not a core beacon is dropped silently: the port is open to the
// whole network.
public sealed partial class CoreDiscovery(CoreDiscoveryOptions options, ILogger<CoreDiscovery> log)
    : BackgroundService
{
    public const int DefaultPort = 4722;

    // Three beacon intervals, as the core's own rule for its clients.
    public static readonly TimeSpan PresentWithin = TimeSpan.FromSeconds(6);

    private const int MaxCores = 64;

    private static readonly JsonSerializerOptions Json = new() { PropertyNamingPolicy = JsonNamingPolicy.CamelCase };

    private readonly ConcurrentDictionary<string, DiscoveredCore> _cores = new(StringComparer.Ordinal);
    private readonly Lock _selectionLock = new();
    private readonly TaskCompletionSource<int> _bound = new(TaskCreationOptions.RunContinuationsAsynchronously);
    private string? _selected;
    private bool _loaded;

    // Raised after a selection changed; CoreLink drops its connection on it.
    public event Action? SelectionChanged;

    // The port actually bound, once listening; the tests read it.
    public Task<int> Port => _bound.Task;

    // The chosen core's id, or null to use the configured address.
    public string? SelectedCoreId
    {
        get
        {
            lock (_selectionLock)
            {
                LoadSelection();
                return _selected;
            }
        }
    }

    // Every core heard from within PresentWithin, by id.
    public IReadOnlyList<DiscoveredCore> Cores
    {
        get
        {
            var now = DateTimeOffset.UtcNow;
            return [.. _cores.Values.Where(c => now - c.LastSeen <= PresentWithin).OrderBy(c => c.CoreId, StringComparer.Ordinal)];
        }
    }

    // The chosen core if it is on the network now; null otherwise.
    public DiscoveredCore? Find(string coreId) =>
        _cores.TryGetValue(coreId, out var core) && DateTimeOffset.UtcNow - core.LastSeen <= PresentWithin ? core : null;

    // Only a core that is on the network now can be chosen, so a typo cannot
    // leave the server waiting for a core that does not exist. Null goes back
    // to the configured address.
    public Result<string?, CoreError> Select(string? coreId)
    {
        if (coreId is not null && Find(coreId) is null)
            return Result.Fail(CoreError.NotFound, $"no core {coreId} on the network");
        lock (_selectionLock)
        {
            LoadSelection();
            if (_selected == coreId)
                return coreId;
            var temporary = options.SelectionPath + ".tmp";
            try
            {
                File.WriteAllText(temporary, JsonSerializer.Serialize(new Saved(coreId), Json));
                File.Move(temporary, options.SelectionPath, overwrite: true);
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException)
            {
                return Result.Fail(CoreError.Failed, $"the core selection cannot be written: {e.Message}");
            }
            _selected = coreId;
        }
        LogSelected(log, coreId ?? "the configured address");
        SelectionChanged?.Invoke();
        return coreId;
    }

    protected override async Task ExecuteAsync(CancellationToken stoppingToken)
    {
        UdpClient udp;
        try
        {
            udp = new UdpClient(new IPEndPoint(IPAddress.Any, options.Port));
        }
        catch (SocketException e)
        {
            // Not fatal: the configured core address still works.
            LogCannotListen(log, options.Port, e.Message);
            _bound.TrySetException(e);
            return;
        }
        using (udp)
        {
            var port = ((IPEndPoint)udp.Client.LocalEndPoint!).Port;
            _bound.TrySetResult(port);
            LogListening(log, port, SelectedCoreId ?? "none, using the configured address");
            try
            {
                while (!stoppingToken.IsCancellationRequested)
                {
                    UdpReceiveResult datagram;
                    try
                    {
                        datagram = await udp.ReceiveAsync(stoppingToken).ConfigureAwait(false);
                    }
                    catch (SocketException)
                    {
                        // Windows reports an earlier send's ICMP unreachable on
                        // the next receive; the socket itself is fine.
                        continue;
                    }
                    if (Parse(datagram.Buffer, datagram.RemoteEndPoint.Address) is { } core)
                        Remember(core);
                }
            }
            catch (OperationCanceledException) when (stoppingToken.IsCancellationRequested)
            {
            }
        }
    }

    // Bounded: the port is open to the network, and a stranger inventing ids
    // must not grow this without end. Cores gone quiet make room first.
    private void Remember(DiscoveredCore core)
    {
        var before = _cores.GetValueOrDefault(core.CoreId);
        if (before is null && _cores.Count >= MaxCores)
        {
            foreach (var stale in _cores.Values.Where(c => core.LastSeen - c.LastSeen > PresentWithin))
                _cores.TryRemove(stale.CoreId, out _);
            if (_cores.Count >= MaxCores)
                return;
        }
        _cores[core.CoreId] = core;
        if (before is null || before.Host != core.Host || before.ControlPort != core.ControlPort)
            LogHeard(log, core.CoreId, core.Host, core.ControlPort, core.Version ?? "unknown");
    }

    // The four mandatory fields of CoreBeacon.h; anything else is ignored, so
    // a newer core's beacon is still a core.
    public static DiscoveredCore? Parse(byte[] datagram, IPAddress source)
    {
        try
        {
            using var document = JsonDocument.Parse(datagram);
            var root = document.RootElement;
            if (root.ValueKind != JsonValueKind.Object ||
                !root.TryGetProperty("type", out var type) || type.ValueKind != JsonValueKind.String ||
                type.GetString() != "coreBeacon" ||
                !root.TryGetProperty("protocolVersion", out var version) || !version.TryGetInt32(out _) ||
                !root.TryGetProperty("coreId", out var id) || id.ValueKind != JsonValueKind.String ||
                string.IsNullOrEmpty(id.GetString()) ||
                !root.TryGetProperty("controlPort", out var portElement) || !portElement.TryGetInt32(out var port) ||
                port is < 1 or > 65535)
            {
                return null;
            }
            string? coreVersion = root.TryGetProperty("coreVersion", out var v) && v.ValueKind == JsonValueKind.String
                ? v.GetString()
                : null;
            var host = source.IsIPv4MappedToIPv6 ? source.MapToIPv4() : source;
            return new DiscoveredCore(id.GetString()!, host.ToString(), port, coreVersion, DateTimeOffset.UtcNow);
        }
        catch (JsonException)
        {
            return null;
        }
    }

    // Read once, on first use. A file that cannot be read is logged and means
    // no selection: the configured address is a working server.
    private void LoadSelection()
    {
        if (_loaded)
            return;
        _loaded = true;
        if (!File.Exists(options.SelectionPath))
            return;
        try
        {
            _selected = JsonSerializer.Deserialize<Saved>(File.ReadAllText(options.SelectionPath), Json)?.CoreId;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or JsonException)
        {
            LogUnreadable(log, options.SelectionPath, e.Message);
        }
    }

    private sealed record Saved(string? CoreId);

    [LoggerMessage(Level = LogLevel.Information, Message = "listening for core beacons on UDP {Port}; selected core: {Selected}")]
    private static partial void LogListening(ILogger log, int port, string selected);

    [LoggerMessage(Level = LogLevel.Warning, Message = "cannot listen for core beacons on UDP {Port} ({Reason}); only the configured core address is used")]
    private static partial void LogCannotListen(ILogger log, int port, string reason);

    [LoggerMessage(Level = LogLevel.Information, Message = "core {CoreId} {Version} at {Host}:{Port}")]
    private static partial void LogHeard(ILogger log, string coreId, string host, int port, string version);

    [LoggerMessage(Level = LogLevel.Information, Message = "selected core: {Selected}")]
    private static partial void LogSelected(ILogger log, string selected);

    [LoggerMessage(Level = LogLevel.Warning, Message = "the core selection {Path} cannot be read ({Reason}); using the configured address")]
    private static partial void LogUnreadable(ILogger log, string path, string reason);
}
