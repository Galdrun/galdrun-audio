using System.Net;
using System.Text.Json;
using Multiroom.App.Api;

namespace Multiroom.App.Tests;

public sealed class RoomsTests
{
    private static CancellationToken Ct => TestContext.Current.CancellationToken;
    internal static RoomView Room() => JsonSerializer.Deserialize<RoomView>("""
        {"client":"USB / DAC","desired":{"zone":"living","audioControl":"bitPerfect","volume":0.8,"muted":false,"outputLatencyMs":20,"effectiveVolume":0.4,"effectiveMuted":true,"grouped":true},
        "actual":{"client":"USB / DAC","connected":true,"member":true,"zone":"living","device":"USB DAC","capabilities":{"formats":[{"sampleRate":48000}]},
        "settings":{"volume":0.4,"muted":true,"audioControl":"bitPerfect","driftCorrection":false,"outputLatencyMs":20},
        "plan":{"bitPerfect":false,"reason":"core reason EXACT","outputFormat":{"sampleRate":48000},"driftCorrected":false},
        "sync":{"driftPpm":-3.25,"offsetUs":12.5},"bufferedMs":120,"playback":{"underruns":2,"ageMs":40},"rejoins":1,"audioDevices":{"selected":"usb"}}}
        """, MultiroomClient.Json)!;

    [Fact]
    public void Desired_effective_and_actual_remain_distinct_and_edits_survive_snapshots()
    {
        var vm = new RoomsViewModel(_ => Task.CompletedTask);
        var room = Room(); vm.Apply(new([new("living")], [room]));
        Assert.Contains("Audio control: bitPerfect", vm.Desired);
        Assert.Contains($"Effective volume: {0.4}", vm.Desired);
        Assert.Contains("Reason: core reason EXACT", vm.Actual);
        Assert.Contains("Bit-perfect: False", vm.Actual);
        Assert.Contains("sampleRate: 48000", vm.Actual); Assert.Contains("underruns: 2", vm.Actual);
        vm.Volume = 35; vm.Muted = false;
        vm.Apply(new([new("living")], [room with { Desired = room.Desired with { Volume = 0.1 } }]));
        Assert.Equal(35, vm.Volume); Assert.False(vm.Muted);
        vm.Apply(new([], [new("Offline", new())])); Assert.Null(vm.Volume);
        Assert.Equal("No actual room state reported.", vm.Actual);
    }

    [Fact]
    public async Task Each_room_control_sends_only_its_setting_and_reloads_authoritative_state()
    {
        var bodies = new List<string>(); var paths = new List<string>();
        using var api = new MultiroomClient("http://fake", handler: new FakeHttp(async (r, ct) =>
        {
            paths.Add($"{r.Method} {r.RequestUri!.AbsolutePath}");
            if (r.Method == HttpMethod.Get) return BrowserTests.Reply(new[] { Room() });
            if (r.Content is not null) bodies.Add(await r.Content.ReadAsStringAsync(ct));
            else Assert.EndsWith("/rejoin", r.RequestUri.AbsolutePath);
            return FakeHttp.Json("{}");
        }));
        var vm = new RoomsViewModel(a => a(api, Ct)); vm.Apply(new([new("living")], [Room()]));
        vm.AudioControl = AudioControl.Auto; vm.Volume = 25; vm.Muted = true; vm.Latency = 42;
        foreach (var setting in new[] { "audioControl", "volume", "mute", "latency" }) await vm.SaveSettingCommand.ExecuteAsync(setting);
        await vm.RejoinCommand.ExecuteAsync(null);
        Assert.Equal(new[] { "{\"audioControl\":\"auto\"}", "{\"volume\":0.25}", "{\"muted\":true}", "{\"outputLatencyMs\":42}" }, bodies);
        Assert.Equal(10, paths.Count); Assert.Equal("PATCH /api/v1/rooms/USB%20%2F%20DAC", paths[0]);
        Assert.Equal("POST /api/v1/rooms/USB%20%2F%20DAC/rejoin", paths[8]);
        Assert.Equal("GET /api/v1/rooms", paths[9]);
        vm.Latency = 1.5m; await Assert.ThrowsAsync<ArgumentException>(() => vm.SaveSettingCommand.ExecuteAsync("latency"));
        Assert.Equal(10, paths.Count);
    }

    [Fact]
    public async Task Drift_correction_is_offered_where_the_client_reports_it_and_sent_alone()
    {
        var bodies = new List<string>();
        using var api = new MultiroomClient("http://fake", handler: new FakeHttp(async (r, ct) =>
        {
            if (r.Method == HttpMethod.Get) return BrowserTests.Reply(new[] { Room() });
            bodies.Add(await r.Content!.ReadAsStringAsync(ct));
            return FakeHttp.Json("{}");
        }));
        var vm = new RoomsViewModel(a => a(api, Ct)); vm.Apply(new([], [Room()]));
        Assert.False(vm.HasDriftCorrection); Assert.Null(vm.DriftCorrection);

        var trimmed = JsonSerializer.Deserialize<RoomView>("""
            {"client":"kitchen","desired":{"zone":"living","audioControl":"bitPerfect","grouped":true},
            "actual":{"client":"kitchen","connected":true,"member":true,
            "plan":{"bitPerfect":true,"reason":"the client offers the source format","outputFormat":{"sampleRate":44100},"driftCorrected":true,"driftActuator":"hardwareClock"},
            "driftCorrection":{"requested":true,"active":true,"actuator":"hardwareClock","mode":"hardwareClock","hardwareClockAvailable":true}}}
            """, MultiroomClient.Json)!;
        vm.Apply(new([], [trimmed]));
        Assert.True(vm.HasDriftCorrection);
        Assert.Equal(DriftCorrection.HardwareClock, vm.DriftCorrection);
        Assert.Contains("Bit-perfect: True · hardware clock synchronized", vm.Actual);

        vm.DriftCorrection = DriftCorrection.Resampling;
        await vm.SaveSettingCommand.ExecuteAsync("driftCorrection");
        Assert.Equal(new[] { "{\"driftCorrection\":\"resampling\"}" }, bodies);
    }

    [Fact]
    public async Task Refused_setting_refreshes_desired_state_without_changing_actual_reason()
    {
        var requested = false;
        using var api = new MultiroomClient("http://fake", handler: new FakeHttp((r, _) =>
        {
            if (r.Method == HttpMethod.Patch) { requested = true; return Task.FromResult(FakeHttp.Json("{\"error\":\"notConnected\",\"message\":\"secret\"}", HttpStatusCode.ServiceUnavailable)); }
            Assert.True(requested); return Task.FromResult(BrowserTests.Reply(new[] { Room() with { Desired = new(AudioControl: "auto") } }));
        }));
        var vm = new RoomsViewModel(a => a(api, Ct)); vm.Apply(new([], [Room()])); vm.AudioControl = AudioControl.Auto;
        var error = await Assert.ThrowsAsync<ApiException>(() => vm.SaveSettingCommand.ExecuteAsync("audioControl"));
        Assert.DoesNotContain("secret", error.Message); Assert.Contains("Audio control: auto", vm.Desired);
        Assert.Contains("Reason: core reason EXACT", vm.Actual);
    }

    [Fact]
    public async Task Assignment_uses_a_fresh_full_zone_membership_and_unassignment_preserves_other_rooms()
    {
        var bodies = new List<string>();
        using var api = new MultiroomClient("http://fake", handler: new FakeHttp(async (r, ct) =>
        {
            if (r.Method == HttpMethod.Put)
            { Assert.Equal("/api/v1/zones/living/clients", r.RequestUri!.AbsolutePath); bodies.Add(await r.Content!.ReadAsStringAsync(ct)); return FakeHttp.Json("{}"); }
            return r.RequestUri!.AbsolutePath.EndsWith("zones", StringComparison.Ordinal)
                ? BrowserTests.Reply(new[] { new ZoneView("living", new(["other", "arrived elsewhere", "USB / DAC"])) })
                : BrowserTests.Reply(new[] { Room() });
        }));
        var vm = new RoomsViewModel(a => a(api, Ct)); vm.Apply(new([new("living", new([]))], [Room()]));
        await vm.AssignCommand.ExecuteAsync("assign"); await vm.AssignCommand.ExecuteAsync("remove");
        Assert.Equal(new[] { "{\"clients\":[\"other\",\"arrived elsewhere\",\"USB / DAC\"]}", "{\"clients\":[\"other\",\"arrived elsewhere\"]}" }, bodies);
    }

    [Fact]
    public async Task Diagnostics_uses_only_outward_status_rooms_and_zones()
    {
        var paths = new List<string>();
        using var api = new MultiroomClient("http://fake", handler: new FakeHttp((r, _) =>
        {
            var path = r.RequestUri!.AbsolutePath; paths.Add(path);
            return Task.FromResult(path.EndsWith("zones", StringComparison.Ordinal) ? FakeHttp.Json("[]") :
                path.EndsWith("rooms", StringComparison.Ordinal) ? BrowserTests.Reply(new[] { Room() }) : FakeHttp.Json("""
                {"version":"server-v","core":{"connected":true,"version":"core-v","connections":2,"zones":1,"rooms":1,"queuedEvents":3,"droppedEvents":4},
                "media":{"reachability":"reachable"},"library":{"indexer":{"queueDepth":5}},"providers":[{"id":"fourth","state":"ready","detail":"ready"}],
                "addIns":{"registered":[{"id":"library","state":"ready"}]},"events":{"subscribers":1,"held":2,"dropped":3}}
                """));
        }));
        var vm = new RoomsViewModel(a => a(api, Ct)); await vm.RefreshAsync(diagnostics: true);
        Assert.Equal(new[] { "/api/v1/zones", "/api/v1/rooms", "/api/v1/status" }, paths);
        foreach (var value in new[] { "server-v", "core-v", "queueDepth: 5", "fourth", "id: library", "reachable", "3 dropped", "USB / DAC" }) Assert.Contains(value, vm.Diagnostics);
    }

    [Fact]
    public async Task A_room_is_shown_by_its_name_renamed_when_connected_and_forgotten_after_a_confirmation()
    {
        var sent = new List<string>();
        using var api = new MultiroomClient("http://fake", handler: new FakeHttp(async (r, ct) =>
        {
            if (r.Method == HttpMethod.Get)
                return r.RequestUri!.AbsolutePath.EndsWith("/zones", StringComparison.Ordinal)
                    ? BrowserTests.Reply(Array.Empty<ZoneView>()) : BrowserTests.Reply(new[] { Room() });
            sent.Add($"{r.Method} {r.RequestUri!.AbsolutePath} {(r.Content is null ? "" : await r.Content.ReadAsStringAsync(ct))}");
            return r.Method == HttpMethod.Delete ? new HttpResponseMessage(HttpStatusCode.NoContent) : FakeHttp.Json("{}");
        }));
        var vm = new RoomsViewModel(a => a(api, Ct));
        var named = Room() with { Name = "Arbeitsraum" };
        vm.Apply(new([new("living")], [named]));
        Assert.Equal("Arbeitsraum", vm.SelectedRoom!.DisplayName);
        Assert.Equal("Arbeitsraum", vm.RoomName);
        Assert.Equal("Client id: USB / DAC", vm.RoomId);
        Assert.True(vm.CanRename); Assert.False(vm.CanForget);

        vm.RoomName = "Wohnzimmer";
        await vm.SaveSettingCommand.ExecuteAsync("name");
        Assert.Equal("PATCH /api/v1/rooms/USB%20%2F%20DAC {\"name\":\"Wohnzimmer\"}", sent[^1]);

        // A connected room is not forgotten; one that is away needs two clicks.
        await vm.ForgetCommand.ExecuteAsync(null);
        Assert.Single(sent);
        var away = named with { Actual = named.Actual! with { Connected = false } };
        vm.Apply(new([], [new("Offline", new()), away]));
        vm.SelectedRoom = vm.Rooms[1];
        Assert.True(vm.CanForget); Assert.False(vm.CanRename);
        await vm.ForgetCommand.ExecuteAsync(null);
        Assert.Single(sent); Assert.True(vm.ConfirmForget);
        Assert.Equal("Confirm: forget this room", vm.ForgetLabel);
        await vm.ForgetCommand.ExecuteAsync(null);
        Assert.Equal("DELETE /api/v1/rooms/USB%20%2F%20DAC ", sent[^1]);
        Assert.False(vm.ConfirmForget);

        // A room without a name is shown by its id.
        Assert.Equal("Offline", new RoomView("Offline", new()).DisplayName);
    }
}
