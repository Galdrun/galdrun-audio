using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Logging.Abstractions;
using Multiroom.Server.Playback;
using Multiroom.Server.Playback.Core;
using Multiroom.Server.Providers;

namespace Multiroom.Server.Tests;

// Phases 15 and 16 against FakeCore: what a room asked for is kept, and what
// is sent for it is its effective value -- its zone's volume and mute applied,
// and no drift correction while it is alone in its zone.
public sealed class ZoneSettingsTests : IAsyncDisposable
{
    private static readonly TimeSpan Patience = TimeSpan.FromSeconds(10);

    // living plays at 5 s on its line, with roomA and roomB asked in.
    private const string PlayingStatus = """
        {"server":{"version":"fake","uptimeMs":1,"zones":1,"clients":0,"connected":0,"members":0},"clients":[],
         "zones":[{"zone":"living","state":"playing","transport":"playing","staged":[],"failedSources":[],
           "members":[],"clients":["roomA","roomB"],"startsSent":1,"rejoins":0,"timelineNs":5000000000}]}
        """;

    private readonly FakeCore _core = new();
    private readonly List<CoreLink> _links = [];
    private readonly TempDirectory _dir = new();

    private static CancellationToken Ct => TestContext.Current.CancellationToken;

    public async ValueTask DisposeAsync()
    {
        foreach (var link in _links)
        {
            await link.StopAsync(CancellationToken.None);
            link.Dispose();
        }
        await _core.DisposeAsync();
        _dir.Dispose();
    }

    private async Task<(CoreLink Link, QueueEngineTests.Synchronised Synchronised)> StartAsync()
    {
        var synchronised = new QueueEngineTests.Synchronised();
        var link = Link(synchronised);
        await link.StartAsync(Ct);
        await synchronised.Next.Task.WaitAsync(Patience, Ct);
        return (link, synchronised);
    }

    private CoreLink Link(params ICoreEventSubscriber[] handlers)
    {
        var link = new CoreLink(new CoreLinkOptions("127.0.0.1", _core.Port), handlers,
            NullLogger<CoreLink>.Instance);
        _links.Add(link);
        return link;
    }

    private static async Task Send<T>(CoreLink link, CoreCommand<T> command) =>
        Assert.True((await link.SendAsync(command, Ct)).IsSuccess, command.Name);

    private IReadOnlyList<(string Command, string Params)> SentSince(int mark) =>
        [.. _core.All.Skip(mark).Select(r => (r.Command, r.Params))];

    private static async Task Until(Func<bool> condition)
    {
        using var timeout = new CancellationTokenSource(Patience);
        while (!condition())
            await Task.Delay(10, timeout.Token);
    }

    [Fact]
    public async Task A_zone_volume_lowers_its_rooms_volumes_on_the_line_and_keeps_what_they_asked()
    {
        _core.StatusBody = PlayingStatus;
        var (link, _) = await StartAsync();
        await Send(link, new ZoneSetClients("living", ["roomA", "roomB"]));
        await Send(link, new ClientSetVolume("roomA", 0.8));
        await Send(link, new ClientSetVolume("roomB", 0.7));

        var mark = _core.All.Count;
        Assert.True((await link.SetZoneLevelAsync("living", 0.5, null, Ct)).IsSuccess);
        Assert.Equal(
            [
                ("zones", "{}"),
                ("client.setVolume", """{"client":"roomA","value":0.3,"applyAtTimelineNs":5250000000}"""),
                ("client.setVolume", """{"client":"roomB","value":0.2,"applyAtTimelineNs":5250000000}"""),
            ],
            SentSince(mark));
        Assert.Equal((0.8, 0.7), (link.Desired.Rooms["roomA"].Volume, link.Desired.Rooms["roomB"].Volume));
        Assert.Equal(0.3, link.Desired.Effective("roomA").Volume);

        mark = _core.All.Count;
        Assert.True((await link.SetZoneLevelAsync("living", 1.0, null, Ct)).IsSuccess);
        Assert.Equal(
            [
                ("zones", "{}"),
                ("client.setVolume", """{"client":"roomA","value":0.8,"applyAtTimelineNs":5250000000}"""),
                ("client.setVolume", """{"client":"roomB","value":0.7,"applyAtTimelineNs":5250000000}"""),
            ],
            SentSince(mark));

        // A replay changes nothing and sends nothing.
        mark = _core.All.Count;
        Assert.True((await link.SetZoneLevelAsync("living", 1.0, null, Ct)).IsSuccess);
        Assert.Empty(SentSince(mark));

        // A room's own change inside the zone is sent lowered by the zone's level.
        await link.SetZoneLevelAsync("living", 0.5, null, Ct);
        mark = _core.All.Count;
        await Send(link, new ClientSetVolume("roomA", 0.6));
        Assert.Equal([("client.setVolume", """{"client":"roomA","value":0.1}""")], SentSince(mark));
        Assert.Equal(0.6, link.Desired.Rooms["roomA"].Volume);
    }

    [Fact]
    public async Task A_zone_mute_mutes_only_and_unmuting_leaves_a_room_muted_on_its_own_and_a_moved_room_takes_its_new_zone_s_pair()
    {
        var (link, _) = await StartAsync();
        await Send(link, new ZoneSetClients("living", ["roomA", "roomB"]));
        await Send(link, new ClientSetVolume("roomA", 0.8));
        await Send(link, new ClientSetVolume("roomB", 0.7));
        await link.SetZoneLevelAsync("living", 0.5, null, Ct);

        // Stopped: no position.
        var mark = _core.All.Count;
        Assert.True((await link.SetZoneLevelAsync("living", null, true, Ct)).IsSuccess);
        Assert.Equal(
            [
                ("client.setVolume", """{"client":"roomA","muted":true}"""),
                ("client.setVolume", """{"client":"roomB","muted":true}"""),
            ],
            SentSince(mark));

        await Send(link, new ClientSetVolume("roomB", Muted: true));
        mark = _core.All.Count;
        Assert.True((await link.SetZoneLevelAsync("living", null, false, Ct)).IsSuccess);
        Assert.Equal([("client.setVolume", """{"client":"roomA","muted":false}""")], SentSince(mark));
        Assert.Equal((0.3, false), (link.Desired.Effective("roomA").Volume, link.Desired.Effective("roomA").Muted));
        Assert.Equal((0.2, true), (link.Desired.Effective("roomB").Volume, link.Desired.Effective("roomB").Muted));

        // Into a zone with no level of its own: the room's own pair.
        mark = _core.All.Count;
        await Send(link, new ZoneSetClients("kitchen", ["roomA"]));
        Assert.Equal(
            [
                ("zone.setClients", """{"zone":"kitchen","clients":["roomA"]}"""),
                ("client.setGrouped", """{"client":"roomA","grouped":false}"""),
                ("client.setVolume", """{"client":"roomA","value":0.8}"""),
                ("client.setGrouped", """{"client":"roomB","grouped":false}"""),
            ],
            SentSince(mark));

        // Out of every zone: its own.
        mark = _core.All.Count;
        await Send(link, new ZoneSetClients("living", []));
        Assert.Equal(
            [
                ("zone.setClients", """{"zone":"living","clients":[]}"""),
                ("client.setVolume", """{"client":"roomB","value":0.7}"""),
            ],
            SentSince(mark));
    }

    [Fact]
    public async Task A_reconnect_re_asserts_the_effective_pairs_and_nothing_else()
    {
        var (link, _) = await StartAsync();
        await Send(link, new ZoneCreate("living"));
        await Send(link, new ZoneSetClients("living", ["roomA", "roomB"]));
        await Send(link, new ClientSetVolume("roomA", 0.8));
        await Send(link, new ClientSetVolume("roomB", 0.7, true));
        await link.SetZoneLevelAsync("living", 0.5, null, Ct);

        _core.Drop();
        await Until(() => link.Connections == 2 && link.IsConnected);
        await Until(() => _core.On(2).Count >= 8);

        Assert.Equal(
            [
                ("events.subscribe", "{}"),
                ("status", "{}"),
                ("zone.create", """{"zone":"living"}"""),
                ("zone.setClients", """{"zone":"living","clients":["roomA","roomB"]}"""),
                ("client.setGrouped", """{"client":"roomA","grouped":true}"""),
                ("client.setVolume", """{"client":"roomA","value":0.3,"muted":false}"""),
                ("client.setGrouped", """{"client":"roomB","grouped":true}"""),
                ("client.setVolume", """{"client":"roomB","value":0.2,"muted":true}"""),
            ],
            _core.On(2).Select(r => (r.Command, r.Params)));
    }

    [Fact]
    public async Task A_room_s_old_keys_are_read_or_ignored_and_the_rest_restored()
    {
        var path = Path.Combine(_dir.Path, "playback.json");
        await File.WriteAllTextAsync(path,
            """{"version":2,"zones":[],"queues":[],"rooms":{"roomA":{"bitperfect":"force","volume":0.5},"roomB":{"audioControl":"bitPerfectFallback","driftCorrection":"yes"}}}""", Ct);
        var link = Link();
        await using var services = new ServiceCollection().BuildServiceProvider();
        var store = Store(link, path, services);
        await store.StartAsync(Ct);
        Assert.Null(link.Desired.Rooms["roomA"].AudioControl);
        Assert.Equal(0.5, link.Desired.Rooms["roomA"].Volume);
        // bitPerfectFallback is the old name for auto, and the drift mode is gone.
        Assert.Equal(AudioControl.Auto, link.Desired.Rooms["roomB"].AudioControl);
        await store.StopAsync(Ct);
        Assert.DoesNotContain("bitperfect", await File.ReadAllTextAsync(path, Ct), StringComparison.Ordinal);
    }

    [Fact]
    public async Task A_zone_s_volume_and_mute_outlive_a_restart_and_a_format_1_file_restores_at_full_volume()
    {
        var path = Path.Combine(_dir.Path, "playback.json");
        var link = Link();
        await link.SendAsync(new ZoneSetClients("living", ["roomA"]), Ct);
        await link.SetZoneLevelAsync("living", 0.5, true, Ct);
        await using (var services = new ServiceCollection().BuildServiceProvider())
        {
            var store = Store(link, path, services);
            await store.StartAsync(Ct);
            await store.StopAsync(Ct);
        }

        var restored = Link();
        await using (var services = new ServiceCollection().BuildServiceProvider())
        {
            var store = Store(restored, path, services);
            await store.StartAsync(Ct);
            Assert.Equal(new DesiredZoneLevel(0.5, true), restored.Desired.ZoneLevels["living"]);
            Assert.Equal((0.5, true), (restored.Desired.Effective("roomA").Volume, restored.Desired.Effective("roomA").Muted));
            await store.StopAsync(Ct);
        }

        await File.WriteAllTextAsync(path,
            """{"version":1,"zones":[{"zone":"living","clients":["roomA"]}],"rooms":{},"queues":[]}""", Ct);
        var old = Link();
        await using (var services = new ServiceCollection().BuildServiceProvider())
        {
            var store = Store(old, path, services);
            await store.StartAsync(Ct);
            Assert.Equal(["roomA"], old.Desired.MembersOf("living"));
            Assert.Empty(old.Desired.ZoneLevels);
            await store.StopAsync(Ct);
        }
    }

    [Fact]
    public async Task A_room_s_name_from_the_core_is_kept_through_a_restart_and_forgetting_drops_it_with_its_settings()
    {
        _core.StatusBody = """
            {"server":{"version":"fake","uptimeMs":1,"zones":1,"clients":1,"connected":0,"members":0},
             "clients":[{"clientId":"3f9c2a71e04b8d15","name":"Arbeitsraum","connected":false,"member":false,
               "settings":{"audioControl":"bitPerfectFallback","driftCorrection":true,"audioControlIsDefault":true,"driftCorrectionIsDefault":true},
               "driftCorrection":{"requested":true}}],
             "zones":[]}
            """;
        var path = Path.Combine(_dir.Path, "playback.json");
        var (link, _) = await StartAsync();
        Assert.Equal("Arbeitsraum", link.Desired.Rooms["3f9c2a71e04b8d15"].Name);
        await link.SendAsync(new ZoneSetClients("office", ["3f9c2a71e04b8d15", "roomB"]), Ct);
        await link.SendAsync(new ClientSetVolume("3f9c2a71e04b8d15", 0.3, null), Ct);
        await using (var services = new ServiceCollection().BuildServiceProvider())
        {
            var store = Store(link, path, services);
            await store.StartAsync(Ct);
            await store.StopAsync(Ct);
        }

        // No core yet: the name is the one kept.
        var restored = Link();
        await using (var services = new ServiceCollection().BuildServiceProvider())
        {
            var store = Store(restored, path, services);
            await store.StartAsync(Ct);
            Assert.Equal("Arbeitsraum", restored.Desired.Rooms["3f9c2a71e04b8d15"].Name);
            await store.StopAsync(Ct);
        }

        restored.Desired.Forget("3f9c2a71e04b8d15");
        Assert.False(restored.Desired.Rooms.ContainsKey("3f9c2a71e04b8d15"));
        Assert.Equal(["roomB"], restored.Desired.MembersOf("office"));
        Assert.Null(restored.Desired.ZoneOf("3f9c2a71e04b8d15"));
    }

    private static PlaybackStore Store(CoreLink link, string path, IServiceProvider services,
        EqualizerProfiles? profiles = null)
    {
        var providers = new ProviderRegistry([], services, NullLogger<ProviderRegistry>.Instance);
        var engine = new QueueEngine(link, providers, new QueueEngineTests.FakeMedia(), NullLogger<QueueEngine>.Instance);
        return new PlaybackStore(new PlaybackStoreOptions(path, AutoResume: false), engine, link,
            NullLogger<PlaybackStore>.Instance, equalizerProfiles: profiles);
    }

    [Fact]
    public async Task A_room_s_equalizer_is_re_asserted_only_where_the_core_lacks_it_and_outlives_a_restart_with_the_profiles()
    {
        var equalizer = new CoreEqualizer(true, -6, [new CoreEqBand(EqFilterType.Peak, 1000, 1.41, 3)]);
        var (link, _) = await StartAsync();
        await Send(link, ClientSetEqualizer.For("roomA", equalizer));
        Assert.Equal(equalizer, link.Desired.Rooms["roomA"].Equalizer);

        // A core that forgot it is given it again, exactly.
        _core.Drop();
        await Until(() => link.Connections == 2 && link.IsConnected);
        await Until(() => _core.On(2).Any(r => r.Command == "client.setEqualizer"));
        Assert.Equal("""{"client":"roomA","enabled":true,"preampDb":-6,"bands":[{"type":"peak","frequencyHz":1000,"q":1.41,"gainDb":3}]}""",
            _core.On(2).Single(r => r.Command == "client.setEqualizer").Params);

        // A core that holds it is not.
        _core.StatusBody = """
            {"server":{"version":"fake","uptimeMs":1,"zones":0,"clients":1,"connected":0,"members":0},
             "clients":[{"clientId":"roomA","connected":false,"member":false,
               "settings":{"audioControl":"bitPerfectFallback","driftCorrection":false,"audioControlIsDefault":true,"driftCorrectionIsDefault":true,
                 "equalizer":{"enabled":true,"preampDb":-6,"bands":[{"type":"peak","frequencyHz":1000,"gainDb":3,"q":1.41}]}},
               "driftCorrection":{"requested":false}}],
             "zones":[]}
            """;
        _core.Drop();
        await Until(() => link.Connections == 3 && link.IsConnected);
        await Until(() => _core.On(3).Count >= 2);
        await Task.Delay(200, Ct);
        Assert.DoesNotContain(_core.On(3), r => r.Command == "client.setEqualizer");

        var path = Path.Combine(_dir.Path, "playback.json");
        var profiles = new EqualizerProfiles();
        Assert.True(profiles.Save("Desk", -2, [new CoreEqBand(EqFilterType.LowShelf, 120, 0.7, 4)]).IsSuccess);
        await using (var services = new ServiceCollection().BuildServiceProvider())
        {
            var store = Store(link, path, services, profiles);
            await store.StartAsync(Ct);
            await store.StopAsync(Ct);
        }

        var restored = Link();
        var restoredProfiles = new EqualizerProfiles();
        await using (var services = new ServiceCollection().BuildServiceProvider())
        {
            var store = Store(restored, path, services, restoredProfiles);
            await store.StartAsync(Ct);
            Assert.Equal(equalizer, restored.Desired.Rooms["roomA"].Equalizer);
            Assert.Equal(profiles.Find("desk"), restoredProfiles.Find("Desk"));
            Assert.Equal(5, restoredProfiles.All.Count);
            await store.StopAsync(Ct);
        }
    }

    [Fact]
    public async Task A_room_is_grouped_while_its_zone_names_another_and_until_the_zone_stops()
    {
        var (link, _) = await StartAsync();
        await Send(link, new ZoneCreate("living"));

        // Alone: nothing to send.
        var mark = _core.All.Count;
        await Send(link, new ZoneSetClients("living", ["roomA"]));
        Assert.Equal([("zone.setClients", """{"zone":"living","clients":["roomA"]}""")], SentSince(mark));
        Assert.False(link.Desired.Effective("roomA").Grouped);

        // Both rooms are grouped before the membership names the second.
        mark = _core.All.Count;
        await Send(link, new ZoneSetClients("living", ["roomA", "roomB"]));
        Assert.Equal(
            [
                ("client.setGrouped", """{"client":"roomA","grouped":true}"""),
                ("client.setGrouped", """{"client":"roomB","grouped":true}"""),
                ("zone.setClients", """{"zone":"living","clients":["roomA","roomB"]}"""),
            ],
            SentSince(mark));
        Assert.True(link.Desired.Effective("roomA").Grouped);

        // Back to one room while playing: the one left stays grouped until the
        // zone stops; the one that left is not.
        await _core.EmitAsync("""{"event":"zone.transport","zone":"living","state":"playing","timelineNs":100}""");
        await Until(() => link.Model.Current.Zones["living"].Transport == TransportState.Playing);
        mark = _core.All.Count;
        await Send(link, new ZoneSetClients("living", ["roomA"]));
        Assert.Equal(
            [
                ("zone.setClients", """{"zone":"living","clients":["roomA"]}"""),
                ("client.setGrouped", """{"client":"roomB","grouped":false}"""),
            ],
            SentSince(mark));
        Assert.True(link.Desired.Effective("roomA").Grouped);

        mark = _core.All.Count;
        await _core.EmitAsync("""{"event":"zone.transport","zone":"living","state":"paused","timelineNs":200}""");
        await Until(() => _core.All.Count > mark);
        Assert.Equal([("client.setGrouped", """{"client":"roomA","grouped":false}""")], SentSince(mark));
        Assert.False(link.Desired.Effective("roomA").Grouped);

        // A reconnect to a core that holds nothing sends no grouping for a
        // room alone.
        _core.StatusBody = FakeCore.EmptyStatus;
        _core.Drop();
        await Until(() => _core.On(2).Count >= 4);
        await Task.Delay(200, Ct);
        Assert.Equal(
            [
                ("events.subscribe", "{}"),
                ("status", "{}"),
                ("zone.create", """{"zone":"living"}"""),
                ("zone.setClients", """{"zone":"living","clients":["roomA"]}"""),
            ],
            _core.On(2).Select(r => (r.Command, r.Params)));
    }
}
