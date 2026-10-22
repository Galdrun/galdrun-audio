using System.Net;
using System.Net.Sockets;
using System.Text;
using Microsoft.Extensions.Logging.Abstractions;
using Multiroom.Server.Abstractions;
using Multiroom.Server.Playback.Core;

namespace Multiroom.Server.Tests;

public sealed class CoreDiscoveryTests : IDisposable
{
    private static readonly TimeSpan Patience = TimeSpan.FromSeconds(10);

    private readonly TempDirectory _dir = new();

    private static CancellationToken Ct => TestContext.Current.CancellationToken;

    public void Dispose() => _dir.Dispose();

    private CoreDiscovery NewDiscovery() =>
        new(new CoreDiscoveryOptions(0, Path.Combine(_dir.Path, "core.json")), NullLogger<CoreDiscovery>.Instance);

    // What the core's makeCoreBeacon writes (ServerCore test
    // core_beacon_carries_what_the_high_end_server_needs pins the same names).
    private static string Beacon(string coreId, int controlPort) =>
        $$"""{"type":"coreBeacon","protocolVersion":2,"coreId":"{{coreId}}","coreVersion":"0.1.0","controlPort":{{controlPort}}}""";

    private static async Task AnnounceAsync(CoreDiscovery discovery, string coreId, int controlPort)
    {
        using var udp = new UdpClient();
        var port = await discovery.Port.WaitAsync(Patience, Ct);
        await udp.SendAsync(Encoding.UTF8.GetBytes(Beacon(coreId, controlPort)),
            new IPEndPoint(IPAddress.Loopback, port), Ct);
        var deadline = DateTime.UtcNow + Patience;
        while (discovery.Find(coreId) is null)
        {
            Assert.True(DateTime.UtcNow < deadline, $"core {coreId} was never heard");
            await Task.Delay(20, Ct);
        }
    }

    [Fact]
    public void Only_a_core_beacon_with_its_four_mandatory_fields_is_a_core()
    {
        var core = CoreDiscovery.Parse(Encoding.UTF8.GetBytes(Beacon("galdrun-core-nas-1", 4713)), IPAddress.Loopback);
        Assert.NotNull(core);
        Assert.Equal(("galdrun-core-nas-1", "127.0.0.1", 4713, "0.1.0"), (core.CoreId, core.Host, core.ControlPort, core.Version));

        // A client's beacon arrives on another port, but is not a core wherever it lands.
        Assert.Null(CoreDiscovery.Parse(Encoding.UTF8.GetBytes(
            """{"type":"clientBeacon","protocolVersion":2,"clientId":"a","controlPort":4720}"""), IPAddress.Loopback));
        Assert.Null(CoreDiscovery.Parse("not json"u8.ToArray(), IPAddress.Loopback));
        Assert.Null(CoreDiscovery.Parse(Encoding.UTF8.GetBytes(Beacon("", 4713)), IPAddress.Loopback));
        Assert.Null(CoreDiscovery.Parse(Encoding.UTF8.GetBytes(Beacon("a", 0)), IPAddress.Loopback));
        Assert.Null(CoreDiscovery.Parse(
            """{"type":"coreBeacon","coreId":"a","controlPort":4713}"""u8.ToArray(), IPAddress.Loopback));

        // An unknown key is a newer core, not a broken one.
        Assert.NotNull(CoreDiscovery.Parse(
            """{"type":"coreBeacon","protocolVersion":3,"coreId":"a","controlPort":4713,"extra":true}"""u8.ToArray(),
            IPAddress.Loopback));
    }

    [Fact]
    public async Task A_selected_core_is_kept_by_id_across_a_restart()
    {
        using (var discovery = NewDiscovery())
        {
            await discovery.StartAsync(Ct);
            Assert.Null(discovery.SelectedCoreId);

            // Only a core on the network can be chosen.
            var unknown = discovery.Select("galdrun-core-nowhere");
            Assert.False(unknown.IsSuccess);
            Assert.Equal(CoreError.NotFound, unknown.Failure.Code);

            await AnnounceAsync(discovery, "galdrun-core-nas-1", 4713);
            Assert.Equal("galdrun-core-nas-1", Assert.Single(discovery.Cores).CoreId);
            Assert.True(discovery.Select("galdrun-core-nas-1").IsSuccess);
            await discovery.StopAsync(CancellationToken.None);
        }

        using var restarted = NewDiscovery();
        Assert.Equal("galdrun-core-nas-1", restarted.SelectedCoreId);

        // Null goes back to the configured address, and is kept too.
        Assert.True(restarted.Select(null).IsSuccess);
        using var again = NewDiscovery();
        Assert.Null(again.SelectedCoreId);
    }

    [Fact]
    public async Task The_link_moves_to_a_newly_selected_core()
    {
        await using var configured = new FakeCore();
        await using var discovered = new FakeCore();
        using var discovery = NewDiscovery();
        await discovery.StartAsync(Ct);
        using var link = new CoreLink(new CoreLinkOptions("127.0.0.1", configured.Port), [],
            NullLogger<CoreLink>.Instance, discovery);
        await link.StartAsync(Ct);
        try
        {
            await WaitUntilAsync(() => link.IsConnected && link.Port == configured.Port);

            await AnnounceAsync(discovery, "galdrun-core-other", discovered.Port);
            Assert.True(discovery.Select("galdrun-core-other").IsSuccess);

            await WaitUntilAsync(() => link.IsConnected && link.Port == discovered.Port);
            await WaitUntilAsync(() => discovered.All.Any(r => r.Command == "status"));
        }
        finally
        {
            await link.StopAsync(CancellationToken.None);
            await discovery.StopAsync(CancellationToken.None);
        }
    }

    private static async Task WaitUntilAsync(Func<bool> condition)
    {
        var deadline = DateTime.UtcNow + Patience;
        while (!condition())
        {
            Assert.True(DateTime.UtcNow < deadline, "timed out");
            await Task.Delay(20, Ct);
        }
    }
}
