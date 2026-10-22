using System.Net;
using System.Text.Json.Nodes;
using Multiroom.Server.Abstractions;
using Multiroom.Server.Providers;
using Multiroom.Server.Providers.AddIns;

namespace Multiroom.Server.Tests;

// AutoFill and the play history (phase 26), and the web lookup of a
// suggestion (phase 27), on the composed host with the scripted vendor and
// the two catalogues of ApiTests.Ai.
public sealed partial class ApiTests
{
    // The source staged last in living for `title` is heard: the core's transport and
    // sourceStarted, as it sends them.
    private async Task HeardAsync(string title)
    {
        var stage = JsonNode.Parse(_core.All.Last(r => r.Command == "source.stage" &&
            JsonNode.Parse(r.Params)!["title"]!.GetValue<string>() == title).Params)!;
        var source = stage["sourceId"]!.GetValue<string>();
        await _core.EmitAsync(
        [
            """{"event":"zone.transport","zone":"living","state":"playing","timelineNs":100}""",
            $$"""{"event":"zone.sourceStarted","zone":"living","sourceId":"{{source}}","timelineNs":100,"serverTimeNs":1,"startOffsetNs":0}""",
        ]);
    }

    private async Task<JsonNode> AutoFillAsync() => (await Ok(HttpMethod.Get, "zones/living"))["autoFill"]!;

    [Fact]
    public async Task AutoFill_adds_the_songs_the_ai_named_once_and_never_one_the_house_played_lately()
    {
        await using var vendor = await StartWithAiAsync();
        _shelf.File = _stream.File = _dir.Write("song.flac", "song");
        var old = _shelf.Add("Old Song", "Old Artist");
        await Ok(HttpMethod.Put, "zones/living/clients", """{"clients":["roomA"]}""");
        await Ok(HttpMethod.Post, "zones/living/queue/items", Json(new { tracks = new[] { old } }));
        await Ok(HttpMethod.Post, "zones/living/play");
        await HeardAsync("Old Song");
        await Until(async () => (await Ok(HttpMethod.Get, "history"))["total"]!.GetValue<int>() == 1);

        var queued = "ABCD".Select(t => _shelf.Add($"Song {t}", "Some Artist")).ToArray();
        await Ok(HttpMethod.Post, "zones/living/queue/items", Json(new { tracks = queued, replace = true }));
        // The song played lately, from another provider: still not chosen.
        _stream.Add("Old Song - Remastered", "Old Artist");
        for (var i = 1; i <= 5; i++)
            _stream.Add($"New {i}", "New Artist");

        var zone = await Ok(HttpMethod.Patch, "zones/living", """{"autoFill":{"enabled":true,"count":5}}""");
        var settings = zone["autoFill"]!;
        Assert.Equal((true, 3, 5, "ai", "songs that continue this queue", false, 100),
            (settings["enabled"]!.GetValue<bool>(), settings["threshold"]!.GetValue<int>(), settings["count"]!.GetValue<int>(),
                settings["source"]!.GetValue<string>(), settings["request"]!.GetValue<string>(), settings["fallback"]!.GetValue<bool>(),
                settings["avoidRecent"]!.GetValue<int>()));

        using var stream = await EventsAsync(null);
        vendor.Answers.Enqueue(ToolAnswer("suggest_songs", Songs(("Old Song", "Old Artist"), ("New 1", "New Artist"),
            ("New 2", "New Artist"), ("New 3", "New Artist"), ("New 4", "New Artist"), ("New 5", "New Artist"))));
        // Playing from Song A: three left, the threshold.
        await Ok(HttpMethod.Post, "zones/living/play");

        var filled = JsonNode.Parse((await stream.NextAsync(e => e.Type == "queue.autoFilled")).Data)!;
        Assert.Equal(("living", 5, "ai", false), (filled["zone"]!.GetValue<string>(), filled["added"]!.GetValue<int>(),
            filled["source"]!.GetValue<string>(), filled["fallback"]!.GetValue<bool>()));
        Assert.Equal(["Song A", "Song B", "Song C", "Song D", "New 1", "New 2", "New 3", "New 4", "New 5"],
            Titles(await Ok(HttpMethod.Get, "zones/living/queue")));

        var text = UserText(Assert.Single(vendor.Requests));
        Assert.Contains("songs that continue this queue", text, StringComparison.Ordinal);
        Assert.Contains("- Song D — Some Artist", text, StringComparison.Ordinal);
        Assert.Contains("Do not suggest these songs:\n- Old Song — Old Artist", text, StringComparison.Ordinal);

        // Once: the queue is above its threshold now, and a heard source asks nothing.
        await HeardAsync("Song A");
        await Until(async () => (await Ok(HttpMethod.Get, "history"))["total"]!.GetValue<int>() == 2);
        Assert.Single(vendor.Requests);
        var view = await AutoFillAsync();
        Assert.Equal((5, false), (view["last"]!["added"]!.GetValue<int>(), view["filling"]!.GetValue<bool>()));
        Assert.Null(view["waiting"]);

        var history = await Ok(HttpMethod.Get, "history?limit=1");
        Assert.Equal((2, "Song A", "living"), (history["total"]!.GetValue<int>(),
            history["items"]![0]!["track"]!["title"]!.GetValue<string>(), history["items"]![0]!["zone"]!.GetValue<string>()));
        Assert.Equal(0, (await Ok(HttpMethod.Get, "history?zone=kitchen"))["total"]!.GetValue<int>());
        await Refused(HttpMethod.Get, "history?limit=0", HttpStatusCode.BadRequest, "invalidRequest");
        await Refused(HttpMethod.Get, "history?since=1", HttpStatusCode.BadRequest, "invalidRequest");
    }

    [Fact]
    public async Task AutoFill_that_adds_nothing_says_why_waits_for_other_hands_and_falls_back_only_when_asked()
    {
        var queued = new[] { _shelf.Add("Song A", "Some Artist"), _shelf.Add("Song B", "Some Artist") };
        Track[] recommended = [queued[1], _shelf.Add("Rec One", "Some Artist"), _shelf.Add("Rec Two", "Some Artist")];
        await using var vendor = await StartWithAiAsync(more: s =>
            s.AddAddIn("shelfRecs", _ => new ShelfRecommender(() => recommended)));
        _shelf.File = _stream.File = _dir.Write("song.flac", "song");
        await Ok(HttpMethod.Put, "zones/living/clients", """{"clients":["roomA"]}""");
        await Ok(HttpMethod.Post, "zones/living/queue/items", Json(new { tracks = queued }));
        await Ok(HttpMethod.Patch, "zones/living", """{"autoFill":{"enabled":true,"threshold":1,"count":2}}""");

        using var stream = await EventsAsync(null);
        vendor.Answers.Enqueue(TextAnswer("No songs today."));
        await Ok(HttpMethod.Post, "zones/living/play");
        var failed = JsonNode.Parse((await stream.NextAsync(e => e.Type == "queue.autoFilled")).Data)!;
        Assert.Equal((0, "ai"), (failed["added"]!.GetValue<int>(), failed["source"]!.GetValue<string>()));
        Assert.StartsWith("invalidAnswer: ", failed["reason"]!.GetValue<string>(), StringComparison.Ordinal);
        Assert.Equal("nothingAdded", (await AutoFillAsync())["waiting"]!.GetValue<string>());

        // Not again on every change: only when the items change by other hands.
        await HeardAsync("Song A");
        await Task.Delay(300, Ct);
        Assert.Single(vendor.Requests);

        // With fallback, the next fill comes from recommendations, and says why.
        await Ok(HttpMethod.Patch, "zones/living", """{"autoFill":{"fallback":true}}""");
        var fallback = JsonNode.Parse((await stream.NextAsync(e => e.Type == "queue.autoFilled")).Data)!;
        Assert.Equal((2, "recommendations", true), (fallback["added"]!.GetValue<int>(),
            fallback["source"]!.GetValue<string>(), fallback["fallback"]!.GetValue<bool>()));
        Assert.StartsWith("ai failed (invalidAnswer: ", fallback["reason"]!.GetValue<string>(), StringComparison.Ordinal);
        Assert.Equal(["Song A", "Song B", "Rec One", "Rec Two"], Titles(await Ok(HttpMethod.Get, "zones/living/queue")));

        await Ok(HttpMethod.Patch, "zones/living/queue", """{"repeat":"all"}""");
        Assert.Equal("repeatAll", (await AutoFillAsync())["waiting"]!.GetValue<string>());

        // Kept in playback.json, beside the queues.
        var saved = Path.Combine(_dir.Path, "data", "playback.json");
        await Until(async () => File.Exists(saved) &&
            (await File.ReadAllTextAsync(saved, Ct)).Contains("\"autoFill\":{\"living\":{\"enabled\":true,\"threshold\":1,\"count\":2,\"source\":\"ai\"", StringComparison.Ordinal));

        await Refused(HttpMethod.Patch, "zones/living", """{"autoFill":{"count":0}}""", HttpStatusCode.BadRequest, "invalidArgument",
            "autoFill.count: 1 to 50, was 0");
        await Refused(HttpMethod.Patch, "zones/living", """{"autoFill":{"request":" "}}""", HttpStatusCode.BadRequest, "invalidArgument");
        await Refused(HttpMethod.Patch, "zones/living", """{"autoFill":{"source":"magic"}}""", HttpStatusCode.BadRequest, "invalidRequest");
        await Refused(HttpMethod.Patch, "zones/living", """{"autoFill":{"enable":true}}""", HttpStatusCode.BadRequest, "invalidRequest");
        await Refused(HttpMethod.Patch, "zones/nowhere", """{"autoFill":{"enabled":true}}""", HttpStatusCode.NotFound, "notFound");
        await Refused(HttpMethod.Patch, "zones/living", "{}", HttpStatusCode.BadRequest, "invalidArgument");
    }

    [Fact]
    public async Task AutoFill_and_the_history_over_mcp_answer_what_the_routes_answer_and_announce_a_change()
    {
        await using var vendor = await StartWithAiAsync();
        _shelf.File = _dir.Write("song.flac", "song");
        await Ok(HttpMethod.Put, "zones/living/clients", """{"clients":["roomA"]}""");
        await Ok(HttpMethod.Post, "zones/living/queue/items", Json(new { tracks = new[] { _shelf.Add("Song A", "Some Artist") } }));
        await Ok(HttpMethod.Post, "zones/living/play");
        await HeardAsync("Song A");
        await Until(async () => (await Ok(HttpMethod.Get, "history"))["total"]!.GetValue<int>() == 1);
        using var stream = await EventsAsync(null);

        // The external default (read and control) has them; so has the agent.
        await using var mcp = await McpAsync(null);
        var tools = await ToolNames(mcp);
        Assert.Contains("get_autofill", tools);
        Assert.Contains("set_autofill", tools);
        Assert.Contains("get_history", tools);

        var set = await Call(mcp, "set_autofill", new { zone = "living", enabled = false, source = "recommendations", avoidRecent = 500 });
        Assert.Equal(("recommendations", 500), (set["source"]!.GetValue<string>(), set["avoidRecent"]!.GetValue<int>()));
        var changed = JsonNode.Parse((await stream.NextAsync(e => e.Type == "zone.autoFillChanged")).Data)!;
        Assert.Equal("living", changed["zone"]!.GetValue<string>());
        Assert.Equal((await AutoFillAsync()).ToJsonString(), (await Call(mcp, "get_autofill", new { zone = "living" })).ToJsonString());
        Assert.StartsWith("notFound: no zone nowhere", ErrorOf(await CallRaw(mcp, "get_autofill", new { zone = "nowhere" }))
            .Split("': ").Last(), StringComparison.Ordinal);

        var history = await Call(mcp, "get_history");
        var route = await Ok(HttpMethod.Get, "history");
        Assert.Equal(route["items"]!.ToJsonString(), history["items"]!.ToJsonString());
        Assert.Equal((1, false), (history["total"]!.GetValue<int>(), history["cut"]!.GetValue<bool>()));

        vendor.Answers.Enqueue(ToolAnswer("set_autofill", new { zone = "living", enabled = true }));
        vendor.Answers.Enqueue(TextAnswer("AutoFill is on."));
        var reply = await Ok(HttpMethod.Post, "ai/commands", """{"text":"keep the music going by itself","zone":"living"}""");
        Assert.True(reply["actions"]![0]!["ok"]!.GetValue<bool>(), reply.ToJsonString());
        Assert.True((await AutoFillAsync())["enabled"]!.GetValue<bool>());
    }

    // --- web lookup (phase 27) ----------------------------------------------------

    [Fact]
    public async Task A_suggestion_with_web_lookup_looks_up_once_then_names_songs_with_the_notes_and_keeps_the_sources()
    {
        await using var vendor = await FakeOpenAi.StartAsync();
        await StartAsync(sections: $$"""
            , "ai": { "providers": [ { "id": "openrouter", "apiKey": "{{FakeOpenAi.Key}}", "baseUrl": "{{vendor.BaseUri}}" } ] }
            """, services: s => s.AddProvider(_stream.Id, _ => _stream));
        _stream.Add("Brand New Song", "Fresh Artist");
        await Ok(HttpMethod.Patch, "ai/settings", """{"defaultProvider":"openrouter","defaultModel":"model-a","songProviders":["stream"]}""");

        // Off, the default: no request carries a search.
        vendor.Answers.Enqueue(ToolAnswer("suggest_songs", Songs(("Brand New Song", "Fresh Artist"))));
        var plain = await Ok(HttpMethod.Post, "ai/suggestions", """{"request":"songs from the newest album","count":1}""");
        Assert.Empty(plain["sources"]!.AsArray());
        Assert.Null(Assert.Single(vendor.Requests)["plugins"]);

        var settings = await Ok(HttpMethod.Patch, "ai/settings", """{"webLookup":"whenAvailable"}""");
        Assert.Equal("whenAvailable", settings["webLookup"]!.GetValue<string>());
        vendor.Answers.Enqueue("""
            {"choices":[{"index":0,"message":{"role":"assistant","content":"Fresh Artist released Brand New Song in 2026.",
              "annotations":[{"type":"url_citation","url_citation":{"url":"https://example.com/news","title":"News"}}]},
              "finish_reason":"stop"}],"usage":{"prompt_tokens":10,"completion_tokens":5}}
            """);
        vendor.Answers.Enqueue(ToolAnswer("suggest_songs", Songs(("Brand New Song", "Fresh Artist"))));
        var looked = await Ok(HttpMethod.Post, "ai/suggestions", """{"request":"songs from the newest album","count":1}""");
        Assert.Equal(1, looked["found"]!.GetValue<int>());
        Assert.Equal("""[{"url":"https://example.com/news","title":"News"}]""", looked["sources"]!.ToJsonString());
        Assert.Equal(20, looked["usage"]!["inputTokens"]!.GetValue<int>());

        var lookup = vendor.Requests.ElementAt(1);
        Assert.Equal("""[{"id":"web"}]""", lookup["plugins"]!.ToJsonString());
        Assert.Null(lookup["tools"]);
        Assert.Contains("look up on the web", lookup["messages"]![0]!["content"]!.GetValue<string>(), StringComparison.Ordinal);
        var named = vendor.Requests.ElementAt(2);
        Assert.Null(named["plugins"]);
        Assert.Equal("suggest_songs", named["tool_choice"]!["function"]!["name"]!.GetValue<string>());
        Assert.Contains("Notes from a web lookup", UserText(named), StringComparison.Ordinal);
        Assert.Contains("Fresh Artist released Brand New Song in 2026.", UserText(named), StringComparison.Ordinal);

        var status = await Ok(HttpMethod.Get, "status");
        Assert.Equal(1, status["ai"]!["suggestions"]!["webLookups"]!.GetValue<int>());
        Assert.Equal(1, status["ai"]!["providers"]!.AsArray().Single(p => p!["id"]!.GetValue<string>() == "openrouter")!
            ["webLookups"]!.GetValue<int>());
        var providers = await Ok(HttpMethod.Get, "ai/providers");
        Assert.Equal(["reasoning", "webSearch", "transcription"], providers.AsArray().Single(p => p!["id"]!.GetValue<string>() == "openrouter")!
            ["capabilities"]!.AsArray().Select(c => c!.GetValue<string>()));
    }

    // A recommendation add-in that names the tracks it is given, best first.
    private sealed class ShelfRecommender(Func<IReadOnlyList<Track>> tracks) : IRecommendationPlugin
    {
        public string Id => "shelfRecs";

        public string DisplayName => "Shelf";

        public Task<Result<IReadOnlyList<Recommendation>, AddInError>> RecommendAsync(IReadOnlyList<Track> seed, int limit,
            CancellationToken cancellationToken) =>
            Task.FromResult<Result<IReadOnlyList<Recommendation>, AddInError>>(
                tracks().Select((t, i) => new Recommendation(t, 1.0 - i * 0.1, "on the shelf")).ToList());
    }
}
