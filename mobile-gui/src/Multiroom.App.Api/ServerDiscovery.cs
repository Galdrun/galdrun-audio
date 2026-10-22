using System.Buffers.Binary;
using System.Diagnostics;
using System.Net;
using System.Net.NetworkInformation;
using System.Net.Sockets;
using System.Runtime.CompilerServices;
using System.Text;

namespace Multiroom.App.Api;

// One high-end server as its mDNS records presented it: the instance name
// ("Galdrun on <host>._galdrun._tcp.local") is the stable id to remember, the
// SRV and A records say where it answers right now, and the TXT adds its
// version and whether a bearer token is needed.
public sealed record DiscoveredServer(string Instance, string Host, IPAddress Address, int Port,
    string? Version = null, bool TokenRequired = false)
{
    // DNS names are case-insensitive; the instance is everything before the
    // service type, and the announcer's "Galdrun on " is not part of a name.
    public string Name => DisplayName(Instance);

    // The friendly name of any instance id, also for ids that stopped answering.
    public static string DisplayName(string instance)
    {
        var name = instance.EndsWith("." + ServerDiscovery.ServiceType, StringComparison.OrdinalIgnoreCase)
            ? instance[..^(ServerDiscovery.ServiceType.Length + 1)] : instance;
        return name.StartsWith(Branding.InstancePrefix, StringComparison.Ordinal) ? name[Branding.InstancePrefix.Length..] : name;
    }

    // The TXT's path is always the outward /api/v1 the client normalizes to.
    public Uri BaseUri => new($"http://{Address}:{Port}/");
    public string Endpoint => $"{Address}:{Port}";
    public string Detail
    {
        get
        {
            var parts = new List<string> { Host };
            if (Version is { } version) parts.Add($"version {version}");
            if (TokenRequired) parts.Add("API token required");
            return string.Join(" · ", parts);
        }
    }
}

// Browses for the server's mDNS service (the server's ServiceAnnouncer) the
// way any app may, without platform APIs or a multicast entitlement: a
// legacy query (RFC 6762 section 6.7) sent from an ephemeral port to the
// mDNS group, which the responder answers unicast with the PTR, SRV, TXT and
// A records in one datagram. No group is joined, so nothing multicast is
// received here, and nothing is sent to beyond the local link.
public static class ServerDiscovery
{
    public const string ServiceType = Branding.ServiceType;

    private static readonly IPEndPoint Group = new(IPAddress.Parse("224.0.0.251"), 5353);
    private const ushort TypeA = 1;
    private const ushort TypePtr = 12;
    private const ushort TypeTxt = 16;
    private const ushort TypeSrv = 33;

    // Every server that answers within the window, each as soon as it
    // answers. One instance name is reported at most once per browse.
    public static async IAsyncEnumerable<DiscoveredServer> BrowseAsync(TimeSpan window,
        [EnumeratorCancellation] CancellationToken ct = default)
    {
        using var socket = new Socket(AddressFamily.InterNetwork, SocketType.Dgram, ProtocolType.Udp);
        var id = (ushort)Random.Shared.Next(ushort.MaxValue + 1);
        var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        socket.Bind(new IPEndPoint(IPAddress.Any, 0));

        using var sending = CancellationTokenSource.CreateLinkedTokenSource(ct);
        var sender = SendQueriesAsync(socket, id, sending.Token);
        try
        {
            var buffer = new byte[9000];
            var start = Stopwatch.GetTimestamp();
            while (true)
            {
                var remaining = window - Stopwatch.GetElapsedTime(start);
                if (remaining <= TimeSpan.Zero) break;
                using var timeout = CancellationTokenSource.CreateLinkedTokenSource(ct);
                timeout.CancelAfter(remaining);
                int received;
                try
                {
                    received = await socket.ReceiveAsync(buffer.AsMemory(), SocketFlags.None, timeout.Token)
                        .ConfigureAwait(false);
                }
                catch (OperationCanceledException) when (!ct.IsCancellationRequested) { break; }
                catch (SocketException) { continue; }
                foreach (var server in ParseResponse(buffer.AsSpan(0, received), id))
                    if (seen.Add(server.Instance))
                        yield return server;
            }
        }
        finally
        {
            sending.Cancel();
            try { await sender.ConfigureAwait(false); }
            catch (Exception e) when (e is OperationCanceledException or SocketException or ObjectDisposedException) { }
        }
    }

    // Three rounds, a second and then two apart (RFC 6762 section 5.2's
    // idea): a datagram can be lost, and the server announces itself only
    // twice at its own startup.
    private static async Task SendQueriesAsync(Socket socket, ushort id, CancellationToken ct)
    {
        var query = Query(id);
        foreach (var delay in new[] { TimeSpan.Zero, TimeSpan.FromSeconds(1), TimeSpan.FromSeconds(3) })
        {
            await Task.Delay(delay, ct).ConfigureAwait(false);
            foreach (var address in MulticastAddresses())
                try
                {
                    socket.SetSocketOption(SocketOptionLevel.IP, SocketOptionName.MulticastInterface, address.GetAddressBytes());
                    await socket.SendToAsync(query, SocketFlags.None, Group, ct).ConfigureAwait(false);
                }
                catch (SocketException) { } // that interface has no route: the others still go out
        }
    }

    // The IPv4 interfaces that are up and can multicast, one address each,
    // so every network the device is on is asked once per round.
    private static List<IPAddress> MulticastAddresses()
    {
        var addresses = new List<IPAddress>();
        foreach (var nic in NetworkInterface.GetAllNetworkInterfaces())
        {
            if (nic.OperationalStatus != OperationalStatus.Up || !nic.SupportsMulticast ||
                nic.NetworkInterfaceType == NetworkInterfaceType.Loopback || !nic.Supports(NetworkInterfaceComponent.IPv4))
                continue;
            var address = nic.GetIPProperties().UnicastAddresses.Select(u => u.Address)
                .FirstOrDefault(a => a.AddressFamily == AddressFamily.InterNetwork);
            if (address is not null) addresses.Add(address);
        }
        return addresses;
    }

    // The legacy query: one PTR question for the service type, its class
    // carrying the unicast-response bit (RFC 6762 section 5.4) for
    // responders that look at it on a query from any port.
    internal static byte[] Query(ushort id)
    {
        var packet = new List<byte>();
        WriteUInt16(packet, id);
        WriteUInt16(packet, 0x0000); // a plain query
        WriteUInt16(packet, 1);
        WriteUInt16(packet, 0); WriteUInt16(packet, 0); WriteUInt16(packet, 0);
        WriteName(packet, ServiceType);
        WriteUInt16(packet, TypePtr);
        WriteUInt16(packet, 0x8001); // IN with the unicast-response bit
        return [.. packet];
    }

    // Whole servers from one response datagram: a PTR of the service type
    // whose instance also has an SRV here, and an A for the SRV's target.
    // Records with TTL 0 are goodbyes and dropped; anything malformed ends
    // the datagram's contribution silently, the way the announcer drops
    // unparseable questions -- the port is open to the whole network.
    internal static IEnumerable<DiscoveredServer> ParseResponse(ReadOnlySpan<byte> packet, ushort id)
    {
        if (packet.Length < 12 || BinaryPrimitives.ReadUInt16BigEndian(packet) != id ||
            (BinaryPrimitives.ReadUInt16BigEndian(packet[2..]) & 0x8000) == 0)
            return [];
        var offset = 12;
        for (var i = 0; i < BinaryPrimitives.ReadUInt16BigEndian(packet[4..]); i++)
        {
            if (!TryReadName(packet, ref offset, out _) || offset + 4 > packet.Length) return [];
            offset += 4;
        }
        var pointers = new List<string>();
        var services = new Dictionary<string, (string Host, int Port)>(StringComparer.OrdinalIgnoreCase);
        var texts = new Dictionary<string, (string? Version, bool Token)>(StringComparer.OrdinalIgnoreCase);
        var addresses = new Dictionary<string, IPAddress>(StringComparer.OrdinalIgnoreCase);
        var records = BinaryPrimitives.ReadUInt16BigEndian(packet[6..]) + BinaryPrimitives.ReadUInt16BigEndian(packet[8..]) +
            BinaryPrimitives.ReadUInt16BigEndian(packet[10..]);
        for (var i = 0; i < records; i++)
        {
            if (!TryReadName(packet, ref offset, out var name) || offset + 10 > packet.Length) return [];
            var type = BinaryPrimitives.ReadUInt16BigEndian(packet[offset..]);
            var ttl = BinaryPrimitives.ReadUInt32BigEndian(packet[(offset + 4)..]);
            var length = BinaryPrimitives.ReadUInt16BigEndian(packet[(offset + 8)..]);
            offset += 10;
            if (offset + length > packet.Length) return [];
            var end = offset + length;
            var position = offset;
            if (ttl != 0)
            {
                if (type == TypePtr && name.Equals(ServiceType, StringComparison.OrdinalIgnoreCase))
                {
                    if (TryReadName(packet, ref position, out var instance) && position <= end) pointers.Add(instance);
                }
                else if (type == TypeSrv && length >= 8)
                {
                    position += 4; // priority and weight
                    var port = BinaryPrimitives.ReadUInt16BigEndian(packet[position..]);
                    position += 2;
                    if (TryReadName(packet, ref position, out var host) && position <= end) services[name] = (host, port);
                }
                else if (type == TypeTxt)
                {
                    string? version = null;
                    var token = false;
                    while (position < end)
                    {
                        var entryLength = packet[position++];
                        if (position + entryLength > end) break;
                        var entry = Encoding.UTF8.GetString(packet.Slice(position, entryLength));
                        position += entryLength;
                        var equals = entry.IndexOf('=');
                        if (equals < 0) continue;
                        if (entry[..equals].Equals("version", StringComparison.Ordinal)) version = entry[(equals + 1)..];
                        else if (entry[..equals].Equals("auth", StringComparison.Ordinal))
                            token = entry[(equals + 1)..].Equals("token", StringComparison.Ordinal);
                    }
                    texts[name] = (version, token);
                }
                else if (type == TypeA && length == 4)
                    addresses[name] = new IPAddress(packet.Slice(offset, 4));
            }
            offset = end;
        }
        var servers = new List<DiscoveredServer>();
        foreach (var instance in pointers)
        {
            if (!services.TryGetValue(instance, out var service)) continue;
            if (!addresses.TryGetValue(service.Host, out var address)) continue;
            texts.TryGetValue(instance, out var text);
            servers.Add(new DiscoveredServer(instance, service.Host, address, service.Port, text.Version, text.Token));
        }
        return servers;
    }

    // Labels up to the root, following compression pointers (RFC 1035
    // 4.1.4), with a bound on the jumps so a pointer loop is a refusal.
    private static bool TryReadName(ReadOnlySpan<byte> packet, ref int offset, out string name)
    {
        name = "";
        var labels = new List<string>();
        var position = offset;
        var jumped = false;
        for (var jumps = 0; ;)
        {
            if (position >= packet.Length) return false;
            var length = packet[position];
            if (length == 0) { position++; break; }
            if ((length & 0xC0) == 0xC0)
            {
                if (position + 1 >= packet.Length || ++jumps > 16) return false;
                if (!jumped) offset = position + 2;
                jumped = true;
                position = ((length & 0x3F) << 8) | packet[position + 1];
                continue;
            }
            if ((length & 0xC0) != 0 || position + 1 + length > packet.Length) return false;
            labels.Add(Encoding.UTF8.GetString(packet.Slice(position + 1, length)));
            position += 1 + length;
        }
        if (!jumped) offset = position;
        name = string.Join('.', labels);
        return true;
    }

    private static void WriteName(List<byte> packet, string name)
    {
        foreach (var label in name.Split('.'))
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
}
