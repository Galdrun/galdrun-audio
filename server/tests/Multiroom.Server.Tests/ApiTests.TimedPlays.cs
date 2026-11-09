using System.Net;
using System.Text.Json.Nodes;
using Microsoft.Extensions.DependencyInjection;
using Multiroom.Server.Abstractions;
using Multiroom.Server.Playback;

namespace Multiroom.Server.Tests;

// Timed plays: a playlist started and stopped in rooms by a weekly schedule.
// The scheduler's own loop runs on the real clock; the blocks here lie three
// days away, and each tick is driven by hand.
public sealed partial class ApiTests
{
    [Fact]
    public async Task A_timed_play_starts_its_playlist_in_its_block_and_stops_it_after_unless_the_rooms_played_already()
    {
        await StartAsync();
        var scheduler = _host!.Services.GetRequiredService<TimedPlays>();
        Track[] tracks = [.. "ABC".Select(c => c.ToString()).Select(t => _fake.AddTrack(_dir.Write($"{t}.flac", t), t))];
        var playlist = (await Ok(HttpMethod.Post, "playlists", """{"name":"Morning"}"""))["id"]!.GetValue<long>();
        await Ok(HttpMethod.Post, $"playlists/{playlist}/entries", Json(new { tracks }));
        await Ok(HttpMethod.Put, "zones/living/clients", """{"clients":["roomA"]}""");

        var day = DateTime.Today.AddDays(3);
        var dayName = day.DayOfWeek.ToString().ToLowerInvariant();
        await Refused(HttpMethod.Post, "timed-plays", """{"name":" "}""", HttpStatusCode.BadRequest, "invalidArgument");
        await Refused(HttpMethod.Post, "timed-plays",
            $$"""{"name":"x","blocks":[{"day":"{{dayName}}","start":600,"end":660},{"day":"{{dayName}}","start":630,"end":700}]}""",
            HttpStatusCode.BadRequest, "invalidArgument", $"blocks on {day.DayOfWeek} overlap");

        var created = await Ok(HttpMethod.Post, "timed-plays", $$"""
            {"name":" Wake up ","rooms":["roomA"],"playlist":{{playlist}},"volume":0.4,"repeat":"all","fadeInSeconds":0,"fadeOutSeconds":0,
             "blocks":[{"day":"{{dayName}}","start":600,"end":660,"resume":true}]}
            """);
        var id = created["id"]!.GetValue<long>();
        Assert.Equal("Wake up", created["definition"]!["name"]!.GetValue<string>());
        Assert.Single((await Ok(HttpMethod.Get, "timed-plays")).AsArray());

        // Before the block nothing; inside it the playlist starts, once.
        await scheduler.TickAsync(day.AddHours(9.9), Ct);
        Assert.DoesNotContain(_core.All, r => r.Command == "zone.play");
        await scheduler.TickAsync(day.AddHours(10.5), Ct);
        var zone = await Ok(HttpMethod.Get, "zones/living");
        Assert.Equal(["A", "B", "C"], Titles(zone["queue"]!));
        Assert.Equal(("starting", "all"), (zone["queue"]!["transport"]!.GetValue<string>(),
            zone["queue"]!["repeat"]!.GetValue<string>()));
        Assert.Equal(0.4, zone["desired"]!["volume"]!.GetValue<double>());
        Assert.Equal("living", (await Ok(HttpMethod.Get, $"timed-plays/{id}"))["run"]!["zone"]!.GetValue<string>());
        var plays = _core.All.Count(r => r.Command == "zone.play");
        await scheduler.TickAsync(day.AddHours(10.6), Ct);
        Assert.Equal(plays, _core.All.Count(r => r.Command == "zone.play"));

        // After it the zone stops, and where it stopped is kept.
        await scheduler.TickAsync(day.AddHours(11), Ct);
        await Until(() => Task.FromResult(_core.All.Any(r => r.Command == "zone.stop")));
        await Until(async () => (await Ok(HttpMethod.Get, $"timed-plays/{id}"))["resume"] is not null);
        var stopped = await Ok(HttpMethod.Get, $"timed-plays/{id}");
        Assert.Null(stopped["run"]);
        Assert.Equal(0, stopped["resume"]!["index"]!.GetValue<int>());

        // Its only room plays already: neither started nor, afterwards, stopped.
        await Ok(HttpMethod.Post, "zones/living/play");
        var stops = _core.All.Count(r => r.Command == "zone.stop");
        await scheduler.TickAsync(day.AddHours(10.5), Ct);
        Assert.Null((await Ok(HttpMethod.Get, $"timed-plays/{id}"))["run"]);
        await scheduler.TickAsync(day.AddHours(11), Ct);
        Assert.Equal(stops, _core.All.Count(r => r.Command == "zone.stop"));

        Assert.Equal(HttpStatusCode.NoContent, (await Send(HttpMethod.Delete, $"timed-plays/{id}")).Status);
        await Refused(HttpMethod.Get, $"timed-plays/{id}", HttpStatusCode.NotFound, "notFound");
    }

    [Fact]
    public async Task A_timed_play_starts_in_rooms_whose_queue_played_to_its_end()
    {
        await StartAsync();
        var scheduler = _host!.Services.GetRequiredService<TimedPlays>();
        var track = _fake.AddTrack(_dir.Write("A.flac", "A"), "A");
        var playlist = (await Ok(HttpMethod.Post, "playlists", """{"name":"Morning"}"""))["id"]!.GetValue<long>();
        await Ok(HttpMethod.Post, $"playlists/{playlist}/entries", Json(new { tracks = new[] { track } }));
        await Ok(HttpMethod.Put, "zones/living/clients", """{"clients":["roomA"]}""");

        // The queue plays its one track to the end: "playing", nothing current.
        await Ok(HttpMethod.Post, "zones/living/queue/items", Json(new { tracks = new[] { track } }));
        await Ok(HttpMethod.Post, "zones/living/play");
        await HeardAsync("A");
        var source = JsonNode.Parse(_core.All.Last(r => r.Command == "source.stage").Params)!["sourceId"]!.GetValue<string>();
        await _core.EmitAsync($$"""{"event":"zone.ended","zone":"living","afterSourceId":"{{source}}"}""");
        await Until(async () => (await Ok(HttpMethod.Get, "zones/living"))["queue"]!["current"] is null);

        var day = DateTime.Today.AddDays(3);
        var id = (await Ok(HttpMethod.Post, "timed-plays", $$"""
            {"name":"Wake","rooms":["roomA"],"playlist":{{playlist}},"fadeInSeconds":0,
             "blocks":[{"day":"{{day.DayOfWeek.ToString().ToLowerInvariant()}}","start":600,"end":660}]}
            """))["id"]!.GetValue<long>();
        await scheduler.TickAsync(day.AddHours(10.5), Ct);
        Assert.NotNull((await Ok(HttpMethod.Get, $"timed-plays/{id}"))["run"]);
    }

    [Fact]
    public async Task Timed_plays_are_made_read_and_deleted_over_mcp_and_every_change_is_an_event()
    {
        await StartAsync();
        using var stream = await EventsAsync(null);
        await using var mcp = await McpAsync(await NamedClient("all", "read", "control", "modify", "destructive"));

        var created = await Call(mcp, "create_timed_play", new
        {
            definition = new
            {
                name = "Evening",
                rooms = OnlyRoomA,
                blocks = new[] { new { day = "friday", start = 1080, end = 1440 } },
            },
        });
        var id = created["id"]!.GetValue<long>();
        // Resume is a block's default, 3 s fades the timed play's.
        Assert.True(created["definition"]!["blocks"]![0]!["resume"]!.GetValue<bool>());
        Assert.Equal((3, 3), (created["definition"]!["fadeInSeconds"]!.GetValue<int>(), created["definition"]!["fadeOutSeconds"]!.GetValue<int>()));
        Assert.Equal(id, JsonNode.Parse((await stream.NextAsync(e => e.Type == "timedPlay.changed")).Data)!["id"]!.GetValue<long>());
        Assert.True(JsonNode.DeepEquals(await Ok(HttpMethod.Get, "timed-plays"), await Call(mcp, "list_timed_plays")));

        var updated = await Call(mcp, "update_timed_play", new { id, definition = new { name = "Late", enabled = false } });
        Assert.Equal(("Late", false), (updated["definition"]!["name"]!.GetValue<string>(),
            updated["definition"]!["enabled"]!.GetValue<bool>()));
        Assert.Contains("overlap", ErrorOf(await CallRaw(mcp, "update_timed_play", new
        {
            id,
            definition = new
            {
                name = "x",
                blocks = new[] { new { day = "friday", start = 0, end = 60 }, new { day = "friday", start = 30, end = 90 } },
            },
        })), StringComparison.Ordinal);

        await Call(mcp, "delete_timed_play", new { id });
        Assert.True(JsonNode.Parse((await stream.NextAsync(e => e.Type == "timedPlay.changed" &&
            e.Data.Contains("removed", StringComparison.Ordinal))).Data)!["removed"]!.GetValue<bool>());
        Assert.Empty((await Ok(HttpMethod.Get, "timed-plays")).AsArray());
    }

    [Fact]
    public async Task A_sleep_timer_overrides_timed_plays_and_stops_its_zone_when_it_runs_out()
    {
        var clock = new ManualClock();
        await StartAsync(services: s => s.AddSingleton(new SleepTimers(clock)));
        var scheduler = _host!.Services.GetRequiredService<TimedPlays>();
        using var stream = await EventsAsync(null);
        await using var mcp = await McpAsync(await NamedClient("all", "read", "control"));
        var track = _fake.AddTrack(_dir.Write("A.flac", "A"), "A");
        var playlist = (await Ok(HttpMethod.Post, "playlists", """{"name":"Morning"}"""))["id"]!.GetValue<long>();
        await Ok(HttpMethod.Post, $"playlists/{playlist}/entries", Json(new { tracks = new[] { track } }));
        await Ok(HttpMethod.Put, "zones/living/clients", """{"clients":["roomA"]}""");

        // Its fade out is a playback setting, 30 s unless set.
        Assert.Equal(30, (await Ok(HttpMethod.Get, "settings/playback"))["sleepFadeSeconds"]!.GetValue<int>());
        await Refused(HttpMethod.Patch, "settings/playback", """{"sleepFadeSeconds":601}""", HttpStatusCode.BadRequest, "invalidRequest");
        Assert.Equal(5, (await Ok(HttpMethod.Patch, "settings/playback", """{"sleepFadeSeconds":5}"""))["sleepFadeSeconds"]!.GetValue<int>());

        await Refused(HttpMethod.Put, "zones/living/sleep-timer", """{"minutes":721}""", HttpStatusCode.BadRequest, "invalidArgument");
        var set = await Ok(HttpMethod.Put, "zones/living/sleep-timer", """{"minutes":60}""");
        Assert.Equal((true, 3600), (set["active"]!.GetValue<bool>(), set["remainingSeconds"]!.GetValue<int>()));
        Assert.Equal("living", JsonNode.Parse((await stream.NextAsync(e => e.Type == "zone.sleepTimerChanged")).Data)!["zone"]!.GetValue<string>());
        Assert.True((await Ok(HttpMethod.Get, "zones/living"))["sleepTimer"]!["active"]!.GetValue<bool>());

        // While it runs, a timed play does not start in its rooms.
        var day = DateTime.Today.AddDays(3);
        var id = (await Ok(HttpMethod.Post, "timed-plays", $$"""
            {"name":"Wake","rooms":["roomA"],"playlist":{{playlist}},"fadeInSeconds":0,
             "blocks":[{"day":"{{day.DayOfWeek.ToString().ToLowerInvariant()}}","start":600,"end":660}]}
            """))["id"]!.GetValue<long>();
        await scheduler.TickAsync(day.AddHours(10.5), Ct);
        Assert.Null((await Ok(HttpMethod.Get, $"timed-plays/{id}"))["run"]);

        // Run out, it stops the zone and is off.
        await Ok(HttpMethod.Post, "zones/living/queue/items", Json(new { tracks = new[] { track } }));
        await Ok(HttpMethod.Post, "zones/living/play");
        var stops = _core.All.Count(r => r.Command == "zone.stop");
        clock.Advance(TimeSpan.FromMinutes(61));
        await scheduler.TickAsync(day.AddHours(10.6), Ct);
        await Until(() => Task.FromResult(_core.All.Count(r => r.Command == "zone.stop") > stops));
        Assert.False((await Ok(HttpMethod.Get, "zones/living/sleep-timer"))["active"]!.GetValue<bool>());

        // Over MCP: set, then off with 0.
        Assert.Equal(1800, (await Call(mcp, "set_sleep_timer", new { zone = "living", minutes = 30 }))["remainingSeconds"]!.GetValue<int>());
        Assert.False((await Call(mcp, "set_sleep_timer", new { zone = "living", minutes = 0 }))["active"]!.GetValue<bool>());
    }

    private sealed class ManualClock : TimeProvider
    {
        private DateTimeOffset _now = DateTimeOffset.UtcNow;

        public override DateTimeOffset GetUtcNow() => _now;

        public void Advance(TimeSpan by) => _now += by;
    }

    [Fact]
    public void Blocks_that_touch_at_midnight_are_one_stretch()
    {
        var monday = new DateTime(2026, 11, 9, 0, 0, 0, DateTimeKind.Local);
        var definition = new Library.TimedPlayDefinition("Night", Rooms: ["roomA"], Playlist: 1, Blocks:
        [
            new(DayOfWeek.Monday, 22 * 60, 24 * 60),
            new(DayOfWeek.Tuesday, 0, 2 * 60),
            new(DayOfWeek.Wednesday, 0, 60, Enabled: false),
        ]);
        Assert.NotNull(TimedPlays.ActiveBlock(definition, monday.AddHours(23).AddMinutes(59).AddSeconds(59)));
        Assert.NotNull(TimedPlays.ActiveBlock(definition, monday.AddDays(1)));
        Assert.Null(TimedPlays.ActiveBlock(definition, monday.AddDays(1).AddHours(2)));
        Assert.Null(TimedPlays.ActiveBlock(definition, monday.AddDays(2).AddMinutes(30)));
        Assert.Null(TimedPlays.ActiveBlock(definition with { Enabled = false }, monday.AddHours(23)));
    }
}
