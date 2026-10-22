using System.Net;
using System.Text.Json.Nodes;
using Microsoft.Extensions.DependencyInjection;
using Multiroom.Server.Ai;
using Multiroom.Server.Library;
using Multiroom.Server.Providers;
using Multiroom.Server.Web.Api;

namespace Multiroom.Server.Tests;

// AI proposals (phase 38) on the composed host with the scripted vendor:
// "shelf" holds the songs, albums and artists, the library a book.
public sealed partial class ApiTests
{
    // A clock of its own for the runner: a fixed morning, moved by the test.
    private sealed class ProposalClock : TimeProvider
    {
        private long _offset;

        public override DateTimeOffset GetUtcNow() =>
            new DateTimeOffset(2026, 1, 5, 8, 0, 0, TimeSpan.Zero) + TimeSpan.FromTicks(Interlocked.Read(ref _offset));

        public void Advance(TimeSpan by) => Interlocked.Add(ref _offset, by.Ticks);
    }

    private async Task<FakeOpenAi> StartWithProposalsAsync(double timeoutSeconds = 5, TimeProvider? clock = null)
    {
        var book = Directory.CreateDirectory(Path.Combine(_dir.Path, "music", "Other Book")).FullName;
        for (var i = 1; i <= 3; i++)
            LibraryTests.WriteTrack(Path.Combine(book, $"{i}.wav"), $"Part {i}", "Author Bee", "Other Book", i, "Hörbuch");
        _shelf.File = _stream.File = _dir.Write("song.flac", "song");
        foreach (var (title, artist, album) in new[]
                 {
                     ("Liked Song", "Band A", "Liked Album"), ("Alice Song", "Band B", "Alice Album"),
                     ("New Song", "Band C", "New Album"), ("Old Song", "Band D", "Old Album"),
                 })
        {
            _shelf.Add(title, artist, album);
        }
        var vendor = await StartWithAiAsync(timeoutSeconds, s =>
        {
            if (clock is not null)
                s.AddSingleton(sp => ActivatorUtilities.CreateInstance<ProposalRunner>(sp, clock));
        });
        await Ok(HttpMethod.Patch, "ai/settings", """{"songProviders":["shelf","local"]}""");
        var indexer = _host!.Services.GetRequiredService<LibraryIndexer>();
        await Until(() => Task.FromResult(indexer.Status.Passes >= 1));
        return vendor;
    }

    private async Task<JsonNode> SearchOneAsync(string text, string kind) =>
        (await Ok(HttpMethod.Get, $"search?q={Uri.EscapeDataString(text)}&types={kind}"))["items"]![kind]![0]!;

    // A refresh's run to its end, and the lists it left.
    private async Task<JsonNode> ProposedAsync(string query = "")
    {
        JsonNode view = null!;
        await Until(async () => (view = await Ok(HttpMethod.Get, "ai/proposals" + query))["run"] is { } run &&
            run["state"]!.GetValue<string>() != "running");
        return view;
    }

    private static string[] Named(JsonNode view, string kind, string field) =>
        [.. view[kind]!.AsArray().Select(p => p!["item"]![field]!.GetValue<string>())];

    [Fact]
    public async Task A_refresh_proposes_what_resolves_from_the_scopes_marks_and_never_what_is_marked()
    {
        await using var vendor = await StartWithProposalsAsync();
        await Ok(HttpMethod.Post, "users", """{"name":"Alice"}""");
        await Ok(HttpMethod.Post, "likes", $$"""{"track":{{(await SearchOneAsync("Liked Song", "tracks")).ToJsonString()}}}""");
        await Ok(HttpMethod.Post, "likes", $$"""{"album":{{(await SearchOneAsync("Band A Liked Album", "albums")).ToJsonString()}}}""");
        await Ok(HttpMethod.Post, "likes", $$"""{"artist":{{(await SearchOneAsync("Band A", "artists")).ToJsonString()}}}""");
        await Ok(HttpMethod.Post, "likes",
            $$"""{"track":{{(await SearchOneAsync("Alice Song", "tracks")).ToJsonString()}},"user":"Alice"}""");
        await Ok(HttpMethod.Patch, "ai/settings", """{"proposals":{"tracks":1,"albums":1,"artists":1,"audiobooks":1}}""");

        // Off by default, and said so.
        var before = await Ok(HttpMethod.Get, "ai/proposals");
        Assert.Equal((false, true), (before["background"]!["enabled"]!.GetValue<bool>(), before["aiAvailable"]!.GetValue<bool>()));
        Assert.Null(before["run"]);

        void Script()
        {
            vendor.Answers.Enqueue(ToolAnswer("suggest_songs", Songs(("Liked Song", "Band A"), ("New Song", "Band C"))));
            vendor.Answers.Enqueue(ToolAnswer("suggest_albums", new
            {
                albums = new[]
                {
                    new { title = "Liked Album", artist = "Band A", reason = "liked already" },
                    new { title = "New Album", artist = "Band C", reason = "the same warmth" },
                },
            }));
            vendor.Answers.Enqueue(ToolAnswer("suggest_artists", new
            {
                artists = new[] { new { name = "Band A", reason = "liked" }, new { name = "Band C", reason = "close to Band A" } },
            }));
            vendor.Answers.Enqueue(ToolAnswer("suggest_audiobooks", new
            {
                audiobooks = new[]
                {
                    new { title = "Other Book", author = "Author Bee", reason = "a calm voice" },
                    new { title = "Ghost Book", author = "Nobody", reason = "unheard of" },
                },
            }));
        }
        Script();
        using var stream = await EventsAsync(null);
        var started = await Send(HttpMethod.Post, "ai/proposals/refresh");
        Assert.Equal(HttpStatusCode.Accepted, started.Status);
        Assert.Equal(("running", "manual"), (started.Body!["state"]!.GetValue<string>(), started.Body["trigger"]!.GetValue<string>()));
        await stream.NextAsync(e => e.Type == "proposals.running");
        var view = await ProposedAsync();
        Assert.Equal("ok", view["run"]!["state"]!.GetValue<string>());
        Assert.Equal(started.Body["id"]!.GetValue<long>(), view["run"]!["id"]!.GetValue<long>());
        Assert.Equal(4, vendor.Requests.Count);
        Assert.Equal(["New Song"], Named(view, "tracks", "title"));
        Assert.Equal(["New Album"], Named(view, "albums", "title"));
        Assert.Equal("the same warmth", view["albums"]![0]!["reason"]!.GetValue<string>());
        Assert.Equal(["Band C"], Named(view, "artists", "name"));
        Assert.Equal(["Other Book"], Named(view, "audiobooks", "title"));
        Assert.Equal(4, view["run"]!["notFound"]!.GetValue<int>());
        Assert.True(view["run"]!["usage"]!["inputTokens"]!.GetValue<long>() > 0);
        Assert.Contains("ok", JsonNode.Parse((await stream.NextAsync(e => e.Type == "proposals.changed")).Data)!["state"]!
            .GetValue<string>(), StringComparison.Ordinal);
        // The household's seed only, as plain names.
        var albumsAsked = UserText(vendor.Requests.ElementAt(1));
        Assert.Contains("Liked Album — Band A", albumsAsked, StringComparison.Ordinal);
        Assert.DoesNotContain("Alice Song", albumsAsked, StringComparison.Ordinal);
        Assert.DoesNotContain("shelf", albumsAsked, StringComparison.Ordinal);

        // An audiobook proposal plays as a book.
        var book = LikeStore.BookRef(view["audiobooks"]![0]!["item"]!["reference"]!.GetValue<string>());
        var queued = await Ok(HttpMethod.Post, "zones/living/queue/items",
            $$$"""{"node":{"provider":"{{{book.ProviderId}}}","id":{{{Json(book.ProviderItemId)}}}}}""");
        Assert.True(queued["grouped"]!.GetValue<bool>());

        // Alice's run seeds hers and the household's; the household's list stays.
        Script();
        Assert.Equal(HttpStatusCode.Accepted, (await Send(HttpMethod.Post, "ai/proposals/refresh", """{"user":"Alice"}""")).Status);
        var hers = await ProposedAsync("?user=Alice");
        Assert.Equal("ok", hers["run"]!["state"]!.GetValue<string>());
        var aliceAsked = UserText(vendor.Requests.ElementAt(5));
        Assert.Contains("Alice Song", aliceAsked, StringComparison.Ordinal);
        Assert.Contains("Liked Album", aliceAsked, StringComparison.Ordinal);
        Assert.Equal(view["run"]!["id"]!.GetValue<long>(), (await Ok(HttpMethod.Get, "ai/proposals"))["run"]!["id"]!.GetValue<long>());

        // Over MCP: the same view.
        await using var mcp = await McpAsync(null);
        Assert.True(JsonNode.DeepEquals(await Ok(HttpMethod.Get, "ai/proposals"), await Call(mcp, "get_proposals")));
        await Refused(HttpMethod.Get, "ai/proposals?user=bob", HttpStatusCode.BadRequest, "invalidArgument", "no user bob");
    }

    [Fact]
    public async Task A_failed_run_keeps_the_last_good_list_a_running_scope_answers_its_run_and_dismissals_last()
    {
        await using var vendor = await StartWithProposalsAsync(timeoutSeconds: 0.5);
        await Ok(HttpMethod.Post, "likes", $$"""{"track":{{(await SearchOneAsync("Liked Song", "tracks")).ToJsonString()}}}""");
        await Ok(HttpMethod.Patch, "ai/settings", """{"proposals":{"tracks":1,"albums":0,"artists":0,"audiobooks":0}}""");
        vendor.Answers.Enqueue(ToolAnswer("suggest_songs", Songs(("New Song", "Band C"), ("Old Song", "Band D"))));
        await Send(HttpMethod.Post, "ai/proposals/refresh");
        var good = await ProposedAsync();
        Assert.Equal(["New Song"], Named(good, "tracks", "title"));

        // Hung: the second refresh answers the same run, which then times out.
        vendor.Hang = true;
        var first = (await Send(HttpMethod.Post, "ai/proposals/refresh")).Body!;
        var second = (await Send(HttpMethod.Post, "ai/proposals/refresh")).Body!;
        Assert.Equal(first["id"]!.GetValue<long>(), second["id"]!.GetValue<long>());
        var failed = await ProposedAsync();
        Assert.Equal("failed", failed["run"]!["state"]!.GetValue<string>());
        Assert.Contains("timeout", failed["run"]!["error"]!.GetValue<string>(), StringComparison.Ordinal);
        Assert.Equal(["New Song"], Named(failed, "tracks", "title"));
        vendor.Hang = false;

        // Dismissed: gone now, kept gone across a restart, and never proposed again.
        var id = good["tracks"]![0]!["id"]!.GetValue<long>();
        Assert.Equal(HttpStatusCode.NoContent, (await Send(HttpMethod.Delete, $"ai/proposals/{id}")).Status);
        await Refused(HttpMethod.Delete, $"ai/proposals/{id}", HttpStatusCode.NotFound, "notFound");
        Assert.Empty((await Ok(HttpMethod.Get, "ai/proposals"))["tracks"]!.AsArray());
        _http!.Dispose();
        await _host!.StopAsync(Ct);
        _host.Dispose();
        await StartAsync(sections: $$"""
            , "ai": { "providers": [ { "id": "fixture", "baseUrl": "{{vendor.BaseUri}}" } ] }
            """, services: s =>
        {
            s.AddAiProvider(FixtureAi.ProviderId, c => new FixtureAi(c), TimeSpan.FromSeconds(5));
            s.AddProvider(_shelf.Id, _ => _shelf);
            s.AddProvider(_stream.Id, _ => _stream);
        });
        var after = await Ok(HttpMethod.Get, "ai/proposals");
        Assert.Equal(("failed", 0), (after["run"]!["state"]!.GetValue<string>(), after["tracks"]!.AsArray().Count));
        vendor.Answers.Enqueue(ToolAnswer("suggest_songs", Songs(("New Song", "Band C"), ("Old Song", "Band D"))));
        await Send(HttpMethod.Post, "ai/proposals/refresh");
        Assert.Equal(["Old Song"], Named(await ProposedAsync(), "tracks", "title"));
    }

    [Fact]
    public async Task Runs_add_to_the_lists_newest_first_and_past_keep_the_oldest_go()
    {
        await using var vendor = await StartWithProposalsAsync();
        await Ok(HttpMethod.Post, "likes", $$"""{"track":{{(await SearchOneAsync("Liked Song", "tracks")).ToJsonString()}}}""");
        await Refused(HttpMethod.Patch, "ai/settings", """{"proposals":{"keep":0}}""", HttpStatusCode.BadRequest, "invalidArgument",
            "proposals.keep: 1 to 1000, was 0");
        await Ok(HttpMethod.Patch, "ai/settings", """{"proposals":{"tracks":1,"albums":0,"artists":0,"audiobooks":0,"keep":2}}""");
        async Task<string[]> RunAsync(params (string Title, string Artist)[] songs)
        {
            vendor.Answers.Enqueue(ToolAnswer("suggest_songs", Songs(songs)));
            await Send(HttpMethod.Post, "ai/proposals/refresh");
            return Named(await ProposedAsync(), "tracks", "title");
        }

        Assert.Equal(["New Song"], await RunAsync(("New Song", "Band C")));
        // What the lists hold is not proposed again: the next run brings another.
        Assert.Equal(["Old Song", "New Song"], await RunAsync(("New Song", "Band C"), ("Old Song", "Band D")));
        Assert.Contains("New Song", UserText(vendor.Requests.Last()), StringComparison.Ordinal);
        // Past keep, the oldest goes.
        Assert.Equal(["Alice Song", "Old Song"], await RunAsync(("Alice Song", "Band B")));
    }

    [Fact]
    public async Task Background_runs_wait_for_the_debounce_a_changed_seed_and_the_days_limit()
    {
        var clock = new ProposalClock();
        await using var vendor = await StartWithProposalsAsync(clock: clock);
        var runner = _host!.Services.GetRequiredService<ProposalRunner>();
        await Ok(HttpMethod.Patch, "ai/settings", """
            {"proposals":{"enabled":true,"debounceMinutes":10,"intervalHours":1,"maxRunsPerDay":1,"tracks":1,"albums":0,"artists":0,"audiobooks":0}}
            """);
        vendor.Answers.Enqueue(ToolAnswer("suggest_songs", Songs(("New Song", "Band C"))));
        async Task Quiet()
        {
            runner.Wake();
            await Task.Delay(300, Ct);
        }

        // A new like: nothing before the debounce, one run after it.
        await Ok(HttpMethod.Post, "likes", $$"""{"track":{{(await SearchOneAsync("Liked Song", "tracks")).ToJsonString()}}}""");
        await Quiet();
        Assert.Null((await Ok(HttpMethod.Get, "ai/proposals"))["run"]);
        Assert.Empty(vendor.Requests);
        clock.Advance(TimeSpan.FromMinutes(11));
        runner.Wake();
        var ran = await ProposedAsync();
        Assert.Equal("background", ran["run"]!["trigger"]!.GetValue<string>());
        Assert.Single(vendor.Requests);

        // An unchanged seed starts none, even past the interval.
        clock.Advance(TimeSpan.FromHours(2));
        await Quiet();
        Assert.Single(vendor.Requests);

        // A changed seed past the debounce would: the day's one run is spent.
        await Ok(HttpMethod.Post, "likes", $$"""{"track":{{(await SearchOneAsync("Old Song", "tracks")).ToJsonString()}}}""");
        clock.Advance(TimeSpan.FromMinutes(11));
        await Quiet();
        Assert.Single(vendor.Requests);
        Assert.Equal(1, runner.Stats.Runs);
    }

    [Fact]
    public async Task Without_an_ai_a_refresh_is_refused_by_name_and_the_view_says_why()
    {
        await StartAsync();
        var view = await Ok(HttpMethod.Get, "ai/proposals");
        Assert.False(view["aiAvailable"]!.GetValue<bool>());
        Assert.NotNull(view["reason"]);
        await Refused(HttpMethod.Post, "ai/proposals/refresh", HttpStatusCode.Conflict, "notConfigured");
        await Refused(HttpMethod.Patch, "ai/settings", """{"proposals":{"tracks":101}}""", HttpStatusCode.BadRequest,
            "invalidArgument", "proposals.tracks: 0 to 100, was 101");
    }
}
