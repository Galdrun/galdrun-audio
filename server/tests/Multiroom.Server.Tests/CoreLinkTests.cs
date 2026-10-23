using System.Threading.Channels;
using Microsoft.Extensions.Logging.Abstractions;
using Multiroom.Server.Abstractions;
using Multiroom.Server.Playback.Core;

namespace Multiroom.Server.Tests;

public sealed class CoreLinkTests : IAsyncDisposable
{
    private static readonly TimeSpan Patience = TimeSpan.FromSeconds(10);

    private readonly FakeCore _core = new();
    private readonly Recorder _events = new();
    private readonly CoreLink _link;

    public CoreLinkTests()
    {
        _link = new CoreLink(new CoreLinkOptions("127.0.0.1", _core.Port), [_events], NullLogger<CoreLink>.Instance);
    }

    private static CancellationToken Ct => TestContext.Current.CancellationToken;

    public async ValueTask DisposeAsync()
    {
        await _link.StopAsync(CancellationToken.None);
        _link.Dispose();
        await _core.DisposeAsync();
    }

    private async Task StartAsync()
    {
        await _link.StartAsync(Ct);
        await _events.WaitForAsync<CoreSynchronised>();
    }

    [Fact]
    public async Task Room_power_state_survives_status_updates_and_changes_live_then_clears_on_disconnect()
    {
        _core.StatusBody = UnderrunStatus(0).Replace("\"member\":true",
            "\"member\":true,\"stream\":{\"ready\":true,\"powerState\":\"on\"}", StringComparison.Ordinal);
        await StartAsync();
        Assert.Equal("on", _link.Model.Current.Rooms["roomA"].PowerState);
        foreach (var power in new[] { "standby", "standbyManual", "on" })
        {
            await _core.EmitAsync($$$"""{"event":"room.powerState","client":"roomA","powerState":"{{{power}}}"}""");
            await _events.WaitForAsync<RoomPowerState>(e => e.PowerState == power);
            Assert.Equal(power, _link.Model.Current.Rooms["roomA"].PowerState);
        }
        await _core.EmitAsync("""{"event":"room.disconnected","client":"roomA","reason":"test"}""");
        await _events.WaitForAsync<RoomDisconnected>();
        Assert.Null(_link.Model.Current.Rooms["roomA"].PowerState);
        _core.StatusBody = UnderrunStatus(0);
        Assert.True((await _link.SendAsync(new GetStatus(), Ct)).IsSuccess);
        Assert.Null(_link.Model.Current.Rooms["roomA"].PowerState); // older client
    }

    [Fact]
    public async Task An_announced_deep_sleep_and_its_wake_address_outlast_the_disconnect()
    {
        await StartAsync();
        await _core.EmitAsync("""{"event":"room.powerState","client":"roomA","powerState":"sleep","wakeOnLan":"02:00:00:00:00:01"}""");
        await _events.WaitForAsync<RoomPowerState>(e => e.PowerState == "sleep");
        await _core.EmitAsync("""{"event":"room.disconnected","client":"roomA","reason":"test"}""");
        await _events.WaitForAsync<RoomDisconnected>();
        var room = _link.Model.Current.Rooms["roomA"];
        Assert.Equal(("sleep", "02:00:00:00:00:01"), (room.PowerState, room.WakeOnLan));
        await _core.EmitAsync("""{"event":"room.connected","client":"roomA","deviceName":"d","reconnect":true}""");
        await _events.WaitForAsync<RoomConnected>();
        room = _link.Model.Current.Rooms["roomA"];
        Assert.Equal((null, null), (room.PowerState, room.WakeOnLan));
    }

    [Fact]
    public async Task A_sleeping_room_is_woken_at_its_kept_address_by_a_core_that_restarted()
    {
        await StartAsync();
        await _core.EmitAsync("""{"event":"room.powerState","client":"roomA","powerState":"sleep","wakeOnLan":"02:00:00:00:00:01"}""");
        await _events.WaitForAsync<RoomPowerState>(e => e.PowerState == "sleep");
        await _core.EmitAsync("""{"event":"room.disconnected","client":"roomA","reason":"test"}""");
        await _events.WaitForAsync<RoomDisconnected>();
        Assert.Equal("02:00:00:00:00:01", _link.Desired.Rooms["roomA"].WakeOnLan);
        Assert.DoesNotContain(_core.All, r => r.Command == "client.wake");

        // A core that knows nothing of the room, as one does after a restart.
        _core.Drop();
        await _events.WaitForAsync<CoreSynchronised>(s => s.Reconnected);
        Assert.Contains(_core.All, r => r.Command == "client.wake" &&
            r.Params == """{"client":"roomA","wakeOnLan":"02:00:00:00:00:01"}""");

        await _core.EmitAsync("""{"event":"room.connected","client":"roomA","deviceName":"d","reconnect":true}""");
        await _events.WaitForAsync<RoomConnected>();
        Assert.Null(_link.Desired.Rooms["roomA"].WakeOnLan);
    }

    [Fact]
    public async Task Every_command_round_trips_typed_with_the_core_s_exact_field_names()
    {
        await StartAsync();

        async Task<T> Send<T>(CoreCommand<T> command, string wireParams)
        {
            var result = await _link.SendAsync(command, Ct);
            Assert.True(result.IsSuccess, $"{command.Name}: {result}");
            // A membership command may be followed by the rooms' grouping.
            var sent = _core.All.Last(r => r.Command == command.Name);
            Assert.Equal(wireParams, sent.Params);
            return result.Value;
        }

        Assert.Equal("fake", (await Send(new GetStatus(), "{}")).Server.Version);
        Assert.Empty((await Send(new GetClients(), "{}")).Clients);
        Assert.Empty((await Send(new GetZones(), "{}")).Zones);
        Assert.Equal(new ZoneCreated("living", true), await Send(new ZoneCreate("living"), """{"zone":"living"}"""));
        Assert.Equal(["roomA", "roomB"],
            (await Send(new ZoneSetClients("living", ["roomA", "roomB"]), """{"zone":"living","clients":["roomA","roomB"]}""")).Added);
        Assert.Equal(new SourceStaged("living", "1", true),
            await Send(new SourceStage("living", "1", "http://127.0.0.1:4714/media/h1", "t1", "Träumerei"),
                """{"zone":"living","sourceId":"1","mediaUri":"http://127.0.0.1:4714/media/h1","trackId":"t1","title":"Träumerei"}"""));
        await Send(new SourceStage("living", "2", "http://127.0.0.1:4714/media/h2", StartOffsetNs: 5_000_000),
            """{"zone":"living","sourceId":"2","mediaUri":"http://127.0.0.1:4714/media/h2","startOffsetNs":5000000}""");
        Assert.Equal(new StagedCleared("living", 0), await Send(new SourceClearStaged("living"), """{"zone":"living"}"""));
        Assert.Equal(new ZonePlayed("living", ZoneState.Playing, TransportState.Starting),
            await Send(new ZonePlay("living"), """{"zone":"living"}"""));
        Assert.Equal(new ZonePaused("living", true, 7), await Send(new ZonePause("living"), """{"zone":"living"}"""));
        Assert.Equal(new ZoneSought("living", "1", 1_000_000_000, 100, 150),
            await Send(new ZoneSeek("living", 1_000_000_000), """{"zone":"living","positionNs":1000000000}"""));
        Assert.Equal(new ZoneStopped("living", true, 9), await Send(new ZoneStop("living"), """{"zone":"living"}"""));
        Assert.Equal(new GroupedSet("roomA", true, true),
            await Send(new ClientSetGrouped("roomA", true), """{"client":"roomA","grouped":true}"""));
        Assert.Equal(new AudioControlSet("roomA", AudioControl.Auto, true),
            await Send(new ClientSetAudioControl("roomA", AudioControl.Auto),
                """{"client":"roomA","audioControl":"auto"}"""));
        Assert.Equal(new VolumeSet("roomA", true, 0.5, false),
            await Send(new ClientSetVolume("roomA", 0.5, false), """{"client":"roomA","value":0.5,"muted":false}"""));
        Assert.Equal(new VolumeSet("roomA", true, Muted: true),
            await Send(new ClientSetVolume("roomA", Muted: true), """{"client":"roomA","muted":true}"""));
        Assert.Equal(new ClientRejoined("roomA", "living", 3), await Send(new ClientRejoin("roomA"), """{"client":"roomA"}"""));
        Assert.Equal(new ClientWoken("roomA", "02:00:00:00:00:01"), await Send(new ClientWake("roomA"), """{"client":"roomA"}"""));
        Assert.Equal(new ClientPowerSettings("roomA", true, "on", 900, 3600, true),
            await Send(new ClientSetPower("roomA", 900, 3600), """{"client":"roomA","standbyAfterSeconds":900,"deepSleepAfterSeconds":3600}"""));
        Assert.Equal(new ClientPowerSettings("roomA", true, "on", 900, 3600, true),
            await Send(new ClientPower("roomA", "deepSleep"), """{"client":"roomA","action":"deepSleep"}"""));
        Assert.Equal(new OutputLatencySet("roomA", 20, true, false),
            await Send(new ClientSetOutputLatency("roomA", 20), """{"client":"roomA","latencyMs":20}"""));
        Assert.Equal(new AudioDeviceSet("roomA", "", true),
            await Send(new ClientSetAudioDevice("roomA", ""), """{"client":"roomA","deviceId":""}"""));
        Assert.Equal(new AudioRescanRequested("roomA"), await Send(new ClientRescanAudio("roomA"), """{"client":"roomA"}"""));
        Assert.Equal(new ResamplerConfigured("soxr", true, true),
            await Send(new ResamplerConfigure("soxr"), """{"resampler":"soxr"}"""));
        Assert.Equal(new ZoneDeleted("living", true), await Send(new ZoneDelete("living"), """{"zone":"living"}"""));
        Assert.Equal(new Subscription(false, true), await Send(new EventsUnsubscribe(), "{}"));
        Assert.Equal(new Subscription(true, true), await Send(new EventsSubscribe(), "{}"));
    }

    [Fact]
    public async Task A_core_error_comes_back_with_the_core_s_code_unchanged()
    {
        _core.Refuse("zone.play", "notFound", "no zone attic");
        await StartAsync();

        var result = await _link.SendAsync(new ZonePlay("attic"), Ct);

        Assert.False(result.IsSuccess);
        Assert.Equal(CoreError.NotFound, result.Failure.Code);
        Assert.Equal("no zone attic", result.Failure.Detail);
        Assert.Equal("notFound", ApiError.From(result.Failure).Error);
    }

    [Theory]
    [InlineData(InputAction.StandbyOff, "standbyOff")]
    [InlineData(InputAction.StandbyOn, "standbyOn")]
    [InlineData(InputAction.ToggleStandby, "toggleStandby")]
    [InlineData(InputAction.DeepSleep, "deepSleep")]
    public async Task Power_bindings_round_trip_through_the_core(InputAction action, string name)
    {
        await StartAsync();
        var result = await _link.SendAsync(new ClientSetInputBindings("roomA", "power",
            new Dictionary<string, CoreInputBinding> { ["click"] = new(action) }), Ct);
        Assert.True(result.IsSuccess, result.ToString());
        Assert.Equal(action, result.Value.Bindings["click"].Action);
        Assert.Equal("{\"client\":\"roomA\",\"inputId\":\"power\",\"bindings\":{\"click\":{\"action\":\"" + name + "\"}}}", _core.All[^1].Params);
    }

    [Fact]
    public async Task Status_is_read_into_the_model_and_what_was_not_measured_stays_absent()
    {
        _core.StatusBody = """
            {"server":{"version":"0.29.0","uptimeMs":5000,"resampler":"soxr","zones":1,"clients":2,"connected":1,"members":1},
             "clients":[
              {"clientId":"roomA","connected":true,"session":"00ff","device":"USB DAC",
               "capabilities":{"formats":[{"sampleRate":96000,"channels":2,"sampleType":"int24"}],"preferredSampleRate":96000,
                 "hardwareVolume":{"available":true,"control":"PCM","steps":256,"minDb":-102,"maxDb":4,"hasMute":true}},
               "volumeControl":{"configured":"auto","effective":"hardware"},
               "stream":{"ready":true,"format":{"sampleRate":96000,"channels":2,"sampleType":"int24"}},
               "zone":"living",
               "settings":{"audioControl":"bitPerfectFallback","driftCorrection":true,"audioControlIsDefault":true,"driftCorrectionIsDefault":false,"volume":0.8},
               "member":true,
               "driftCorrection":{"requested":true,"active":false,"disabledBy":"bitperfect available"},
               "output":{"state":"streaming","sourceFormat":{"sampleRate":96000,"channels":2,"sampleType":"int24"},
                 "format":{"sampleRate":96000,"channels":2,"sampleType":"int24"},"bitPerfect":true,
                 "reason":"the device plays the source format","plan":"bitperfect 96000/2/int24","ratio":1,
                 "packets":10,"queuedPackets":2,"bufferedMs":1500,"connectionDrops":0},
               "start":"playing",
               "sync":{"offsetUs":12.5,"correctionPpm":0,"appliedPpm":0},
               "playback":{"bufferedMs":1490,"underruns":0,"ageMs":20},
               "rejoins":0},
              {"clientId":"roomB","connected":false,"zone":"living",
               "settings":{"audioControl":"bitPerfect","driftCorrection":false,"audioControlIsDefault":false,"driftCorrectionIsDefault":true},
               "member":false,"driftCorrection":{"requested":false},"rejoins":1}],
             "zones":[
              {"zone":"living","state":"playing","transport":"playing","timelineNs":2000,
               "sourceFormat":{"sampleRate":96000,"channels":2,"sampleType":"int24"},
               "source":{"sourceId":"1","mediaUri":"http://127.0.0.1:4714/media/h1","trackId":"t1","timelineStartNs":0,"ended":false},
               "staged":[{"sourceId":"2","mediaUri":"http://127.0.0.1:4714/media/h2"}],
               "failedSources":[{"sourceId":"0","reason":"HTTP 404"}],
               "members":["roomA"],"clients":["roomA","roomB"],"startsSent":1,"rejoins":0}]}
            """;

        await StartAsync();

        var model = _link.Model.Current;
        var a = model.Rooms["roomA"];
        Assert.True(a.Connected);
        Assert.True(a.Member);
        Assert.Equal("USB DAC", a.Device);
        Assert.Equal(new CoreHardwareVolume(true, "PCM", 256, -102, 4, true), a.Capabilities!.HardwareVolume);
        Assert.Equal(new CoreVolumeControl(VolumeControl.Auto, VolumeControl.Hardware), a.VolumeControl);
        Assert.Equal(new CorePlan(true, "the device plays the source format", new AudioFormatInfo(96000, 2, "int24"), false,
            new AudioFormatInfo(96000, 2, "int24"), "bitperfect 96000/2/int24"), a.Plan);
        Assert.Equal(0.8, a.Settings!.Volume);
        Assert.Null(a.Settings.Muted);
        Assert.Equal(12.5, a.Sync!.OffsetUs);
        Assert.Null(a.Sync.DriftPpm);
        Assert.Equal(1500UL, a.BufferedMs);
        var b = model.Rooms["roomB"];
        Assert.False(b.Connected);
        Assert.Null(b.Plan);
        Assert.Null(b.Sync);
        Assert.Equal(AudioControl.BitPerfect, b.Settings!.AudioControl);
        var living = model.Zones["living"];
        Assert.Equal(TransportState.Playing, living.Transport);
        Assert.Equal("1", living.Source!.SourceId);
        Assert.Null(living.Source.Title);
        Assert.Equal("2", Assert.Single(living.Staged).SourceId);
        Assert.Equal(new CoreFailedSource("0", "HTTP 404"), Assert.Single(living.FailedSources));
        Assert.Equal(["roomA", "roomB"], living.Clients);
    }

    [Fact]
    public async Task A_scripted_event_sequence_updates_the_model_in_order()
    {
        _core.StatusBody = """
            {"server":{"version":"fake","uptimeMs":1,"zones":1,"clients":1,"connected":1,"members":0},
             "clients":[{"clientId":"roomA","connected":true,"zone":"living",
               "settings":{"audioControl":"bitPerfectFallback","driftCorrection":false,"audioControlIsDefault":true,"driftCorrectionIsDefault":true},
               "member":false,"driftCorrection":{"requested":false}}],
             "zones":[{"zone":"living","state":"idle","transport":"stopped",
               "staged":[{"sourceId":"s1","mediaUri":"http://127.0.0.1:4714/media/h1","title":"One"}],
               "failedSources":[],"members":[],"clients":["roomA"],"startsSent":0,"rejoins":0}]}
            """;
        await StartAsync();

        await _core.EmitAsync([
            """{"event":"zone.transport","zone":"living","state":"starting","timelineNs":0}""",
            """{"event":"room.joined","client":"roomA","zone":"living","bitPerfect":false,"reason":"resampled to the device","outputFormat":{"sampleRate":48000,"channels":2,"sampleType":"int32"},"driftCorrected":true}""",
            """{"event":"zone.sourceStarted","zone":"living","sourceId":"s1","timelineNs":100,"serverTimeNs":555,"startOffsetNs":0}""",
            """{"event":"zone.transport","zone":"living","state":"playing","timelineNs":100}""",
            """{"event":"zone.sourceNeeded","zone":"living","afterSourceId":"s1","remainingNs":5000}""",
            """{"event":"zone.sourceFailed","zone":"living","sourceId":"s2","reason":"HTTP 404"}""",
            """{"event":"room.disconnected","client":"roomA","reason":"closed","zone":"living"}""",
            """{"event":"zone.ended","zone":"living","afterSourceId":"s1"}""",
        ]);
        await _events.WaitForAsync<ZoneEnded>();

        Assert.Equal(
            [typeof(ZoneTransport), typeof(RoomJoined), typeof(SourceStarted), typeof(ZoneTransport),
                typeof(SourceNeeded), typeof(SourceFailed), typeof(RoomDisconnected), typeof(ZoneEnded)],
            _events.Seen.SkipWhile(e => e is not ZoneTransport).Select(e => e.GetType()));
        var model = _link.Model.Current;
        var living = model.Zones["living"];
        Assert.Equal(ZoneState.Ended, living.State);
        Assert.Equal(TransportState.Playing, living.Transport);
        Assert.Equal(100UL, living.TimelineNs);
        Assert.Equal(new CoreSource("s1", true, "http://127.0.0.1:4714/media/h1", Title: "One", TimelineStartNs: 100),
            living.Source);
        Assert.Empty(living.Staged);
        Assert.Equal(new CoreFailedSource("s2", "HTTP 404"), Assert.Single(living.FailedSources));
        // Joined, then disconnected: the order is what leaves it out.
        Assert.Empty(living.Members);
        var room = model.Rooms["roomA"];
        Assert.False(room.Connected);
        Assert.False(room.Member);
        Assert.Null(room.Plan);
        Assert.Equal("living", room.Zone);
    }

    private static string UnstableStatus(bool unstable, ulong rejoins, ulong episode, ulong backoffMs) => $$"""
        {"server":{"version":"fake","uptimeMs":1,"zones":1,"clients":1,"connected":1,"members":1},
         "clients":[{"clientId":"roomA","connected":true,"zone":"living",
           "settings":{"audioControl":"bitPerfectFallback","driftCorrection":false,"audioControlIsDefault":true,"driftCorrectionIsDefault":true},
           "member":true,"driftCorrection":{"requested":false},"rejoins":{{rejoins}},
           "system":{"wifiRssiDbm":-78},
           "unstable":{{(unstable ? "true" : "false")}},"backoffMs":{{backoffMs}}{{(unstable ? $$""","episodeRejoins":{{episode}},"unstableSinceNs":123""" : "")}}}],
         "zones":[{"zone":"living","state":"playing","transport":"playing","staged":[],"failedSources":[],
           "members":["roomA"],"clients":["roomA"],"startsSent":1,"rejoins":{{rejoins}}}]}
        """;

    [Fact]
    public async Task An_unstable_room_is_known_from_its_events_and_from_status_and_fell_behind_counts_nothing()
    {
        _core.StatusBody = UnstableStatus(false, 2, 0, 0);
        await StartAsync();
        var ok = _link.Model.Current.Rooms["roomA"].Health;
        Assert.Equal(new CoreRoomHealth(RoomHealthState.Ok, 0, WifiRssiDbm: -78), ok);

        await _core.EmitAsync([
            """{"event":"room.fellBehind","client":"roomA","zone":"living","trigger":"leftBehind","timelineNs":5}""",
            """{"event":"room.unstable","client":"roomA","zone":"living","trigger":"evicted","rejoins":3,"backoffMs":4000,"detail":"stranded at 5 s"}""",
        ]);
        await _events.WaitForAsync<RoomUnstable>();
        var room = _link.Model.Current.Rooms["roomA"];
        Assert.Equal(2UL, room.Rejoins);
        var unstable = room.Health!;
        Assert.Equal((RoomHealthState.Unstable, 3UL, 4000UL, "evicted", "stranded at 5 s", -78),
            (unstable.State, unstable.Rejoins, unstable.BackoffMs!.Value, unstable.Trigger, unstable.Detail, unstable.WifiRssiDbm!.Value));
        Assert.NotNull(unstable.Since);

        // The next status, mid-episode: its count, the episode's beginning kept.
        _core.StatusBody = UnstableStatus(true, 7, 5, 8000);
        Assert.True((await _link.SendAsync(new GetStatus(), Ct)).IsSuccess);
        room = _link.Model.Current.Rooms["roomA"];
        Assert.Equal(7UL, room.Rejoins);
        Assert.Equal(unstable with { Rejoins = 5, BackoffMs = 8000 }, room.Health);

        await _core.EmitAsync("""{"event":"room.stable","client":"roomA","zone":"living","rejoins":5,"unstableForMs":40000}""");
        await _events.WaitForAsync<RoomStable>();
        Assert.Equal(ok, _link.Model.Current.Rooms["roomA"].Health);

        await _core.EmitAsync("""{"event":"room.left","client":"roomA","zone":"living","cause":"removed"}""");
        await _events.WaitForAsync<RoomLeft>();
        Assert.Null(_link.Model.Current.Rooms["roomA"].Health);

        // A reconnect to a core that says unstable: unstable, with no event,
        // and no beginning this server could know.
        _core.StatusBody = UnstableStatus(true, 9, 4, 2000);
        _core.Drop();
        await _events.WaitForAsync<CoreSynchronised>(s => s.Reconnected);
        Assert.Equal(new CoreRoomHealth(RoomHealthState.Unstable, 4, BackoffMs: 2000, WifiRssiDbm: -78),
            _link.Model.Current.Rooms["roomA"].Health);
    }

    private sealed class Clock : TimeProvider
    {
        public DateTimeOffset Now { get; set; } = new(2026, 1, 5, 8, 0, 0, TimeSpan.Zero);

        public override DateTimeOffset GetUtcNow() => Now;
    }

    private static string UnderrunStatus(ulong underruns, bool unstable = false) => $$"""
        {"server":{"version":"fake","uptimeMs":1,"zones":1,"clients":1,"connected":1,"members":1},
         "clients":[{"clientId":"roomA","connected":true,"zone":"living",
           "settings":{"audioControl":"bitPerfectFallback","driftCorrection":false,"audioControlIsDefault":true,"driftCorrectionIsDefault":true},
           "member":true,"driftCorrection":{"requested":false},"rejoins":0,
           "playback":{"bufferedMs":100,"underruns":{{underruns}},"ageMs":10},
           "unstable":{{(unstable ? "true" : "false")}},"backoffMs":0{{(unstable ? ",\"episodeRejoins\":3" : "")}}}],
         "zones":[{"zone":"living","state":"playing","transport":"playing","staged":[],"failedSources":[],
           "members":["roomA"],"clients":["roomA"],"startsSent":1,"rejoins":0}]}
        """;

    [Fact]
    public async Task A_room_that_keeps_underrunning_is_degraded_until_its_window_is_quiet_and_unstable_wins()
    {
        var clock = new Clock();
        await using var core = new FakeCore();
        var events = new Recorder();
        using var link = new CoreLink(new CoreLinkOptions("127.0.0.1", core.Port), [events], NullLogger<CoreLink>.Instance,
            time: clock);
        core.StatusBody = UnderrunStatus(5);
        await link.StartAsync(Ct);
        await events.WaitForAsync<CoreSynchronised>();
        async Task<CoreRoomHealth> At(ulong underruns, double seconds, bool unstable = false)
        {
            clock.Now += TimeSpan.FromSeconds(seconds);
            core.StatusBody = UnderrunStatus(underruns, unstable);
            Assert.True((await link.SendAsync(new GetStatus(), Ct)).IsSuccess);
            return link.Model.Current.Rooms["roomA"].Health!;
        }

        // The first report's count is history, not news.
        Assert.Equal((RoomHealthState.Ok, 0), (link.Model.Current.Rooms["roomA"].Health!.State, link.Model.Current.Rooms["roomA"].Health!.RecentUnderruns));
        Assert.Equal((RoomHealthState.Ok, 2), ((await At(7, 1)).State, (await At(7, 1)).RecentUnderruns));
        var degraded = await At(8, 10);
        Assert.Equal((RoomHealthState.Degraded, 3), (degraded.State, degraded.RecentUnderruns));
        var since = degraded.Since;
        Assert.Equal(clock.Now, since);
        Assert.Equal(since, (await At(8, 5)).Since); // still in the window, the same episode

        // Unstable is the core's, and wins; the underruns stay counted.
        var unstable = await At(9, 1, unstable: true);
        Assert.Equal((RoomHealthState.Unstable, 4), (unstable.State, unstable.RecentUnderruns));

        // Thirty quiet seconds: ok again.
        var quiet = await At(9, 31);
        Assert.Equal((RoomHealthState.Ok, 0, (DateTimeOffset?)null), (quiet.State, quiet.RecentUnderruns, quiet.Since));
        // A client that started again counts from its new number.
        Assert.Equal(0, (await At(1, 1)).RecentUnderruns);
        await link.StopAsync(CancellationToken.None);
    }

    [Fact]
    public async Task What_a_room_reports_arrives_as_events_and_updates_the_model()
    {
        await StartAsync();

        await _core.EmitAsync([
            """{"event":"room.capabilities","client":"roomA","device":"USB DAC 2","capabilities":{"formats":[{"sampleRate":48000,"channels":2,"sampleType":"int16"}],"accessMode":"shared","hardwareVolume":{"available":false}}}""",
            """{"event":"room.audioDevices","client":"roomA","audioDevices":{"configured":"","selected":"usb","devices":[{"id":"usb","name":"USB","isDefault":true}]}}""",
            """{"event":"room.bufferConfig","client":"roomA","bufferMs":3000}""",
            """{"event":"room.volumeControl","client":"roomA","volumeControl":{"configured":"hardware","effective":"disabled","reason":"no hardware volume"}}""",
        ]);
        await _events.WaitForAsync<RoomVolumeControl>();

        var room = _link.Model.Current.Rooms["roomA"];
        Assert.Equal("USB DAC 2", room.Device);
        Assert.Equal(new CoreHardwareVolume(false), room.Capabilities!.HardwareVolume);
        Assert.Equal("shared", room.Capabilities.AccessMode);
        Assert.Equal("usb", room.AudioDevices!.Selected);
        Assert.Equal(3000U, room.BufferMs);
        Assert.Equal(new CoreVolumeControl(VolumeControl.Hardware, VolumeControl.Disabled, "no hardware volume"),
            room.VolumeControl);
    }

    [Fact]
    public async Task A_dropped_connection_reconnects_resubscribes_reads_status_and_reasserts_only_the_difference()
    {
        await StartAsync();
        Assert.True((await _link.SendAsync(new ZoneCreate("living"), Ct)).IsSuccess);
        Assert.True((await _link.SendAsync(new ZoneCreate("kitchen"), Ct)).IsSuccess);
        Assert.True((await _link.SendAsync(new ZoneSetClients("living", ["roomA"]), Ct)).IsSuccess);
        Assert.True((await _link.SendAsync(new ClientSetAudioControl("roomA", AudioControl.BitPerfect), Ct)).IsSuccess);
        Assert.True((await _link.SendAsync(new ClientSetVolume("roomA", 0.4), Ct)).IsSuccess);
        Assert.True((await _link.SendAsync(new ClientSetOutputLatency("roomA", 20), Ct)).IsSuccess);

        // The core comes back with living and roomA's settings, but without
        // kitchen, with living's membership lost, and with no latency for roomA.
        _core.StatusBody = """
            {"server":{"version":"fake","uptimeMs":1,"zones":1,"clients":1,"connected":0,"members":0},
             "clients":[{"clientId":"roomA","connected":false,
               "settings":{"audioControl":"bitPerfect","driftCorrection":false,"audioControlIsDefault":false,"driftCorrectionIsDefault":false,"volume":0.4},
               "member":false,"driftCorrection":{"requested":false}}],
             "zones":[{"zone":"living","state":"idle","transport":"stopped","staged":[],"failedSources":[],
               "members":[],"clients":[],"startsSent":0,"rejoins":0}]}
            """;
        _core.Drop();
        await _events.WaitForAsync<CoreSynchronised>(s => s.Reconnected);

        Assert.Equal(2, _link.Connections);
        Assert.Equal(
            [
                ("events.subscribe", "{}"),
                ("status", "{}"),
                ("zone.create", """{"zone":"kitchen"}"""),
                ("zone.setClients", """{"zone":"living","clients":["roomA"]}"""),
                ("client.setOutputLatency", """{"client":"roomA","latencyMs":20}"""),
            ],
            _core.On(2).Select(r => (r.Command, r.Params)));
    }

    [Fact]
    public async Task Events_lost_causes_the_same_resync_on_the_same_connection()
    {
        await StartAsync();
        Assert.True((await _link.SendAsync(new ZoneCreate("kitchen"), Ct)).IsSuccess);
        var before = _core.On(1).Count;

        await _core.EmitAsync("""{"event":"events.lost","count":3}""");
        Assert.Equal(new EventsLost(3), await _events.WaitForAsync<EventsLost>());
        await _events.WaitForAsync<CoreSynchronised>(s => !s.Reconnected);

        Assert.Equal(1, _link.Connections);
        Assert.Equal(
            [("status", "{}"), ("zone.create", """{"zone":"kitchen"}""")],
            _core.On(1).Skip(before).Select(r => (r.Command, r.Params)));
    }

    [Fact]
    public async Task A_command_sent_while_disconnected_is_asserted_on_connect()
    {
        var result = await _link.SendAsync(new ClientSetVolume("roomA", 0.3), Ct);
        Assert.Equal(CoreError.NotConnected, result.Failure.Code);

        await StartAsync();

        Assert.Equal(
            [("events.subscribe", "{}"), ("status", "{}"), ("client.setVolume", """{"client":"roomA","value":0.3}""")],
            _core.On(1).Select(r => (r.Command, r.Params)));
    }

    [Fact]
    public async Task A_handler_that_never_returns_costs_a_bounded_queue_and_dropped_events_not_the_reader()
    {
        await using var core = new FakeCore();
        var stuck = new Stuck();
        using var link = new CoreLink(new CoreLinkOptions("127.0.0.1", core.Port), [stuck], NullLogger<CoreLink>.Instance);
        await link.StartAsync(Ct);
        // It takes CoreSynchronised and never returns; the queue is empty behind it.
        await stuck.Entered.Task.WaitAsync(Patience, Ct);

        const int count = CoreLink.EventQueueCapacity + 144;
        await core.EmitAsync(Enumerable.Range(0, count).Select(i =>
            $$"""{"event":"room.connected","client":"c{{i}}","deviceName":"d","reconnect":false}"""));

        // The reader took every line in, and still answers.
        await Until(() => link.Model.Current.Rooms.ContainsKey($"c{count - 1}"));
        Assert.True((await link.SendAsync(new GetZones(), Ct)).IsSuccess);
        Assert.Equal(count, link.Model.Current.Rooms.Count);
        Assert.Equal(CoreLink.EventQueueCapacity, link.QueuedEvents);
        Assert.Equal(144, link.DroppedEvents);

        await link.StopAsync(CancellationToken.None);
    }

    private static async Task Until(Func<bool> condition)
    {
        using var timeout = new CancellationTokenSource(Patience);
        while (!condition())
            await Task.Delay(10, timeout.Token);
    }

    private sealed class Recorder : ICoreEventSubscriber
    {
        private readonly Channel<CoreEvent> _pending = Channel.CreateUnbounded<CoreEvent>();
        private readonly List<CoreEvent> _seen = [];

        public IReadOnlyList<CoreEvent> Seen
        {
            get
            {
                lock (_seen)
                    return [.. _seen];
            }
        }

        public ValueTask HandleAsync(CoreEvent coreEvent, CancellationToken cancellationToken)
        {
            lock (_seen)
                _seen.Add(coreEvent);
            _pending.Writer.TryWrite(coreEvent);
            return ValueTask.CompletedTask;
        }

        // The next event of this type, past any seen by an earlier call.
        public async Task<T> WaitForAsync<T>(Func<T, bool>? match = null)
            where T : CoreEvent
        {
            using var timeout = new CancellationTokenSource(Patience);
            await foreach (var coreEvent in _pending.Reader.ReadAllAsync(timeout.Token))
            {
                if (coreEvent is T wanted && (match?.Invoke(wanted) ?? true))
                    return wanted;
            }
            throw new InvalidOperationException("the event channel closed");
        }
    }

    private sealed class Stuck : ICoreEventSubscriber
    {
        public TaskCompletionSource Entered { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);

        public async ValueTask HandleAsync(CoreEvent coreEvent, CancellationToken cancellationToken)
        {
            Entered.TrySetResult();
            await Task.Delay(Timeout.Infinite, cancellationToken);
        }
    }

    // The core sends room.left before it forgets a room, and the event can
    // arrive after the forget's answer: it must not bring the room back.
    [Fact]
    public void A_room_left_event_does_not_bring_back_a_forgotten_room()
    {
        var model = new CoreModel();
        model.Apply(new RoomLeft("gone", "zone-a", "removed"));
        Assert.False(model.Current.Rooms.ContainsKey("gone"));
    }
}
