using System.Text.Json;
using Multiroom.App.Api;

namespace Multiroom.App.Tests;

public sealed class PlaybackTests
{
    internal static PlaybackSnapshot Snapshot() => new([
        new("living", Queue: new("living", [new(1, new Track("local", "track:1", "First")), new(2, new Track("local", "track:2", "Second"), Failure: "Unavailable")], RepeatMode.Off, false, QueueTransport.Playing, Current: 1)),
        new("kitchen", Queue: new("kitchen", [new(3, new RadioStation("webradio", "station:1", "Radio"))], RepeatMode.Off, false, QueueTransport.Playing, Current: 3), StreamTitle: "Live artist")], []);

    [Fact]
    public void Zone_switching_is_local_and_live_station_never_seeks()
    {
        var vm = new PlaybackViewModel(_ => throw new InvalidOperationException("Unexpected network"));
        vm.Apply(Snapshot()); Assert.True(vm.CanSeek); Assert.Equal("First", vm.Title);
        vm.SelectedZone = "kitchen"; Assert.False(vm.CanSeek); Assert.Equal("Live artist", vm.Subtitle); Assert.Equal("LIVE", vm.Duration);
        vm.Apply(new([], [])); Assert.False(vm.HasZone); Assert.Empty(vm.Items);
    }

    [Fact]
    public void Unchanged_snapshot_preserves_rows_and_selection()
    {
        var vm = new PlaybackViewModel(_ => Task.CompletedTask); var snapshot = Snapshot(); vm.Apply(snapshot);
        var first = vm.Items[0]; vm.SelectedItem = first; vm.Apply(snapshot);
        Assert.Same(first, vm.Items[0]); Assert.Same(first, vm.SelectedItem); Assert.True(vm.Items[1].HasFailure); Assert.Equal("", vm.Items[1].Subtitle);
    }

    [Fact]
    public async Task Queue_move_remove_seek_and_transport_have_exact_routes_and_bodies()
    {
        var paths = new List<string>(); var bodies = new List<string?>();
        using var api = new MultiroomClient("http://fake", handler: new FakeHttp(async (r, ct) =>
        { paths.Add(r.Method + " " + r.RequestUri!.PathAndQuery); bodies.Add(r.Content is null ? null : await r.Content.ReadAsStringAsync(ct)); return FakeHttp.Json("{}"); }));
        var vm = new PlaybackViewModel(action => action(api, TestContext.Current.CancellationToken)); vm.Apply(Snapshot()); vm.SelectedItem = vm.Items[1]; vm.SeekSeconds = 12.5m;
        await vm.MoveCommand.ExecuteAsync("up"); await vm.SeekCommand.ExecuteAsync(null); await vm.RemoveCommand.ExecuteAsync(null); await vm.TransportCommand.ExecuteAsync("pause");
        Assert.Equal(["PATCH /api/v1/zones/living/queue/items/2", "POST /api/v1/zones/living/seek", "DELETE /api/v1/zones/living/queue/items/2", "POST /api/v1/zones/living/pause"], paths);
        Assert.Equal("{\"index\":0}", bodies[0]); Assert.Equal("{\"item\":2,\"positionMs\":12500}", bodies[1]); Assert.Null(bodies[^1]);
        vm.SelectedZone = "kitchen"; await vm.SeekCommand.ExecuteAsync(null); Assert.Equal(4, paths.Count);
    }

    [Fact]
    public void Desired_room_state_does_not_override_actual_reason()
    {
        var room = JsonSerializer.Deserialize<RoomView>("""{"client":"room","desired":{"audioControl":"required","volume":0.5},"actual":{"client":"room","connected":true,"member":true,"plan":{"bitPerfect":false,"reason":"resampling","outputFormat":{},"driftCorrected":true}}}""", MultiroomClient.Json)!;
        Assert.Equal("required", room.Desired.AudioControl); Assert.False(room.Actual!.Plan!.BitPerfect); Assert.Equal("resampling", room.Actual.Plan.Reason);
    }

    [Fact]
    public async Task Using_an_existing_zone_never_clears_its_membership()
    {
        var calls = 0;
        using var api = new MultiroomClient("http://fake", handler: new FakeHttp((_, _) => { calls++; return Task.FromResult(FakeHttp.Json("{}")); }));
        var vm = new PlaybackViewModel(action => action(api, TestContext.Current.CancellationToken)); vm.Apply(Snapshot());
        vm.NewZone = "kitchen"; await vm.CreateZoneCommand.ExecuteAsync(null);
        Assert.Equal("kitchen", vm.SelectedZone); Assert.Equal(0, calls);
    }

    [Fact]
    public async Task Membership_edits_survive_refresh_and_send_the_full_set()
    {
        using var api = new MultiroomClient("http://fake", handler: new FakeHttp(async (request, ct) =>
        {
            Assert.Equal(HttpMethod.Put, request.Method); Assert.Equal("/api/v1/zones/living/clients", request.RequestUri!.AbsolutePath);
            Assert.Equal("{\"clients\":[\"roomB\"]}", await request.Content!.ReadAsStringAsync(ct)); return FakeHttp.Json("{}");
        }));
        var snapshot = Snapshot() with { Rooms = [new("roomA", new(Zone: "living")), new("roomB", new())] };
        var vm = new PlaybackViewModel(action => action(api, TestContext.Current.CancellationToken)); vm.Apply(snapshot);
        vm.Membership[0].Assigned = false; vm.Membership[1].Assigned = true; vm.Apply(snapshot);
        Assert.False(vm.Membership[0].Assigned); Assert.True(vm.Membership[1].Assigned);
        await vm.SaveMembershipCommand.ExecuteAsync(null);
    }

    [Fact]
    public async Task Preferences_round_trip_contains_address_zone_and_server_id()
    {
        var directory = Path.Combine(Path.GetTempPath(), "galdrun-gui-" + Guid.NewGuid().ToString("N"));
        var path = Path.Combine(directory, "settings.json");
        try
        {
            var preferences = new Preferences("http://music:4725/api/v1/", "living");
            await preferences.SaveAsync(path, TestContext.Current.CancellationToken);
            Assert.Equal(preferences, await Preferences.LoadAsync(path, TestContext.Current.CancellationToken));
            using var json = JsonDocument.Parse(await File.ReadAllTextAsync(path, TestContext.Current.CancellationToken));
            Assert.Equal(["Address", "ActiveZone", "ServerId"], json.RootElement.EnumerateObject().Select(p => p.Name));
            Assert.Null(json.RootElement.GetProperty("ServerId").GetString()); // a manual address has no id
        }
        finally { if (Directory.Exists(directory)) Directory.Delete(directory, recursive: true); }
    }

    [Theory]
    [InlineData(390, true, false)] [InlineData(900, false, false)] [InlineData(1440, false, true)]
    public void Responsive_layout_is_width_driven(double width, bool compact, bool wide)
    {
        var vm = new ShellViewModel(); vm.SetWidth(width); Assert.Equal(compact, vm.IsCompact); Assert.Equal(wide, vm.IsWide);
    }

    [Theory]
    [InlineData(ProviderState.Ready, true)] [InlineData(ProviderState.Unavailable, false)]
    [InlineData(ProviderState.AuthRequired, false)] [InlineData(ProviderState.Disabled, false)]
    public void Browse_action_respects_capabilities_and_availability(ProviderState state, bool enabled)
    {
        var vm = new ShellViewModel { SelectedProvider = new("fourth", state, "detail", Capabilities: ["browse"]) };
        Assert.Equal(enabled, vm.CanBrowseProvider);
        vm.SelectedProvider = new("fourth", state, "detail", Capabilities: ["search"]); Assert.False(vm.CanBrowseProvider);
    }
}
