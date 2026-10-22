using System.Globalization;
using System.Net;
using System.Net.Http.Headers;
using System.Text.Json;
using System.Text.Json.Nodes;
using Microsoft.Extensions.DependencyInjection;
using Multiroom.Server.Abstractions;
using Multiroom.Server.Ai;
using Multiroom.Server.Ai.Agent;
using Multiroom.Server.Ai.Providers.OpenAi;
using Multiroom.Server.Providers;

namespace Multiroom.Server.Tests;

// Song suggestions (phase 24) and the agent (phase 25) on the composed host,
// with a scripted Chat Completions vendor and two catalogues: "shelf" stands
// in for the library, "stream" for a streaming service.
public sealed partial class ApiTests
{
    private readonly FakeCatalog _shelf = new("shelf");
    private readonly FakeCatalog _stream = new("stream");

    // What a model must never be sent about the seed: ids, providers, paths, URLs.
    private static readonly string[] SeedLeaks = ["shelf", "shelf-0", "providerId", "/", "http"];

    private async Task<FakeOpenAi> StartWithAiAsync(double timeoutSeconds = 5, Action<IServiceCollection>? more = null)
    {
        var vendor = await FakeOpenAi.StartAsync();
        await StartAsync(sections: $$"""
            , "ai": { "providers": [ { "id": "fixture", "baseUrl": "{{vendor.BaseUri}}" } ] }
            """, services: s =>
        {
            s.AddAiProvider(FixtureAi.ProviderId, c => new FixtureAi(c), TimeSpan.FromSeconds(timeoutSeconds));
            s.AddProvider(_shelf.Id, _ => _shelf);
            s.AddProvider(_stream.Id, _ => _stream);
            more?.Invoke(s);
        });
        await Ok(HttpMethod.Put, "ai/providers/fixture/key", $$"""{"key":"{{FakeOpenAi.Key}}"}""");
        var settings = await Ok(HttpMethod.Patch, "ai/settings",
            """{"defaultProvider":"fixture","defaultModel":"model-a","songProviders":["shelf","stream"]}""");
        Assert.Equal(["shelf", "stream"], settings["songProviders"]!.AsArray().Select(p => p!.GetValue<string>()));
        return vendor;
    }

    // A Chat Completions answer that calls one tool.
    private static string ToolAnswer(string tool, object arguments) => new JsonObject
    {
        ["choices"] = new JsonArray(new JsonObject
        {
            ["index"] = 0,
            ["message"] = new JsonObject
            {
                ["role"] = "assistant",
                ["content"] = null,
                ["tool_calls"] = new JsonArray(new JsonObject
                {
                    ["id"] = $"call_{tool}",
                    ["type"] = "function",
                    ["function"] = new JsonObject
                    {
                        ["name"] = tool,
                        ["arguments"] = JsonSerializer.Serialize(arguments, TestJson),
                    },
                }),
            },
            ["finish_reason"] = "tool_calls",
        }),
        ["usage"] = new JsonObject { ["prompt_tokens"] = 10, ["completion_tokens"] = 5 },
    }.ToJsonString();

    private static string TextAnswer(string text) => new JsonObject
    {
        ["choices"] = new JsonArray(new JsonObject
        {
            ["index"] = 0,
            ["message"] = new JsonObject { ["role"] = "assistant", ["content"] = text },
            ["finish_reason"] = "stop",
        }),
        ["usage"] = new JsonObject { ["prompt_tokens"] = 10, ["completion_tokens"] = 5 },
    }.ToJsonString();

    private static object Songs(params (string Title, string Artist)[] songs) =>
        new { songs = songs.Select(s => new { title = s.Title, artist = s.Artist }).ToArray() };

    // The user message of a Chat Completions request.
    private static string UserText(JsonNode request) =>
        request["messages"]!.AsArray().First(m => m!["role"]!.GetValue<string>() == "user")!["content"]!.GetValue<string>();

    // "So What" queued and playing in living, from the shelf.
    private async Task<Track> PlayingAsync()
    {
        _shelf.File = _stream.File = _dir.Write("seed.flac", "seed");
        var seed = _shelf.Add("So What", "Miles Davis", "Kind of Blue");
        await Ok(HttpMethod.Put, "zones/living/clients", """{"clients":["roomA"]}""");
        await Ok(HttpMethod.Post, "zones/living/queue/items", Json(new { tracks = new[] { seed } }));
        await Ok(HttpMethod.Post, "zones/living/play");
        return seed;
    }

    // --- token usage per day ---------------------------------------------------

    [Fact]
    public async Task Ai_usage_is_kept_per_day_by_origin_provider_and_model()
    {
        await using var vendor = await StartWithAiAsync();
        await PlayingAsync();
        _stream.Add("Peace Piece", "Bill Evans");

        vendor.Answers.Enqueue(ToolAnswer("suggest_songs", Songs(("Peace Piece", "Bill Evans"))));
        var suggested = await Ok(HttpMethod.Post, "ai/suggestions", """{"request":"calmer","seed":{"zone":"living"},"count":1}""");
        vendor.Answers.Enqueue(TextAnswer("Nothing to do."));
        await Ok(HttpMethod.Post, "ai/commands", """{"text":"hello"}""");

        var today = DateTime.Now.ToString("yyyy-MM-dd", CultureInfo.InvariantCulture);
        var rows = (await Ok(HttpMethod.Get, $"ai/usage?from={today}&to={today}")).AsArray();
        Assert.Equal(["agent", "manual"], rows.Select(r => r!["origin"]!.GetValue<string>()));
        var manual = rows[1]!;
        Assert.Equal((today, "fixture", "model-a", 1, 0), (manual["day"]!.GetValue<string>(),
            manual["provider"]!.GetValue<string>(), manual["model"]!.GetValue<string>(), manual["calls"]!.GetValue<int>(),
            manual["failures"]!.GetValue<int>()));
        Assert.Equal(suggested["usage"]!["inputTokens"]!.GetValue<int>(), manual["inputTokens"]!.GetValue<int>());
        Assert.Equal(1, rows[0]!["calls"]!.GetValue<int>());

        Assert.Empty((await Ok(HttpMethod.Get, "ai/usage?to=2000-01-01")).AsArray());
        await Refused(HttpMethod.Get, "ai/usage?from=yesterday", null, HttpStatusCode.BadRequest, "invalidRequest");
    }

    // --- song suggestions (phase 24) ------------------------------------------

    [Fact]
    public async Task Suggestions_send_only_the_seeds_names_and_queue_what_was_found_in_the_models_order()
    {
        await using var vendor = await StartWithAiAsync();
        await PlayingAsync();
        _shelf.Add("Blue in Green", "Miles Davis", "Kind of Blue");
        _stream.Add("Blue in Green", "Miles Davis", "Kind of Blue");
        _stream.Add("Naima - Remastered 2003", "John Coltrane");
        _stream.Add("Peace Piece", "Bill Evans");
        _stream.Add("Round Midnight", "Some Cover Band");

        vendor.Answers.Enqueue(ToolAnswer("suggest_songs", Songs(("Blue in Green", "Miles Davis"), ("Naima", "John Coltrane"),
            ("Imaginary Song", "Nobody"), ("Round Midnight", "Miles Davis"))));
        vendor.Answers.Enqueue(ToolAnswer("suggest_songs", Songs(("Peace Piece", "Bill Evans"), ("Another Missing", "Nobody"))));
        // A third round would get this; none is asked.
        vendor.Answers.Enqueue(ToolAnswer("suggest_songs", Songs(("Blue in Green", "Miles Davis"))));

        var answer = await Ok(HttpMethod.Post, "ai/suggestions", """
            {"request":"play calmer songs","seed":{"zone":"living"},"count":4,"queue":{"zone":"living"}}
            """);
        Assert.Equal((3, 3, 2, 3), (answer["found"]!.GetValue<int>(), answer["notFound"]!.GetValue<int>(),
            answer["rounds"]!.GetValue<int>(), answer["queued"]!.GetValue<int>()));
        var songs = answer["songs"]!.AsArray();
        Assert.Equal("shelf", songs[0]!["track"]!["providerId"]!.GetValue<string>());
        Assert.Equal("exact", songs[0]!["match"]!.GetValue<string>());
        Assert.Equal("close", songs[1]!["match"]!.GetValue<string>());
        Assert.Null(songs[3]!["track"]);
        Assert.Equal(["So What", "Blue in Green", "Naima - Remastered 2003", "Peace Piece"],
            Titles(await Ok(HttpMethod.Get, "zones/living/queue")));
        Assert.Equal(20, answer["usage"]!["inputTokens"]!.GetValue<int>());

        Assert.Equal(2, vendor.Requests.Count);
        var first = vendor.Requests.First();
        Assert.Equal("suggest_songs", first["tool_choice"]!["function"]!["name"]!.GetValue<string>());
        var text = UserText(first);
        Assert.Contains("play calmer songs", text, StringComparison.Ordinal);
        Assert.Contains("So What — Miles Davis (album: Kind of Blue)", text, StringComparison.Ordinal);

        Assert.All(SeedLeaks,
            leak => Assert.DoesNotContain(leak, first.ToJsonString(), StringComparison.Ordinal));
        var second = UserText(vendor.Requests.Last());
        Assert.Contains("Do not suggest these songs:", second, StringComparison.Ordinal);
        Assert.Contains("Imaginary Song — Nobody", second, StringComparison.Ordinal);
        Assert.Contains("Blue in Green — Miles Davis", second, StringComparison.Ordinal);

        var stats = (await Ok(HttpMethod.Get, "status"))["ai"]!["suggestions"]!;
        Assert.Equal((1, 2, 6, 3, 3), (stats["requests"]!.GetValue<int>(), stats["rounds"]!.GetValue<int>(),
            stats["songsAsked"]!.GetValue<int>(), stats["found"]!.GetValue<int>(), stats["notFound"]!.GetValue<int>()));
    }

    [Fact]
    public async Task Suggestions_refuse_an_answer_that_is_not_the_tool_and_a_provider_that_times_out_queues_nothing()
    {
        await using var vendor = await StartWithAiAsync(timeoutSeconds: 1);
        await PlayingAsync();
        _stream.Add("Peace Piece", "Bill Evans");

        vendor.Answers.Enqueue(TextAnswer("I would suggest Peace Piece."));
        await Refused(HttpMethod.Post, "ai/suggestions", """{"request":"something similar","seed":{"zone":"living"}}""",
            HttpStatusCode.BadGateway, "invalidAnswer");

        vendor.Hang = true;
        await Refused(HttpMethod.Post, "ai/suggestions",
            """{"request":"something similar","seed":{"queue":"living"},"queue":{"zone":"living","placement":"replace"}}""",
            HttpStatusCode.ServiceUnavailable, "timeout");
        Assert.Equal(["So What"], Titles(await Ok(HttpMethod.Get, "zones/living/queue")));
        vendor.Hang = false;

        await Refused(HttpMethod.Post, "ai/suggestions", """{"request":"x","seed":{"zone":"living","queue":"living"}}""",
            HttpStatusCode.BadRequest, "invalidArgument");
        await Refused(HttpMethod.Post, "ai/suggestions", """{"request":"x","count":0}""", HttpStatusCode.BadRequest,
            "invalidArgument");
        await Refused(HttpMethod.Post, "ai/suggestions", """{"request":"x","seed":{"zone":"nowhere"}}""",
            HttpStatusCode.NotFound, "notFound");
        await Refused(HttpMethod.Patch, "ai/settings", """{"songProviders":["nobody"]}""", HttpStatusCode.NotFound, "notFound");

        // Nothing found is not an error.
        vendor.Answers.Enqueue(ToolAnswer("suggest_songs", Songs(("Imaginary Song", "Nobody"))));
        vendor.Answers.Enqueue(ToolAnswer("suggest_songs", Songs()));
        var none = await Ok(HttpMethod.Post, "ai/suggestions", """{"request":"90s trip-hop","count":1}""");
        Assert.Equal((0, 1, 2), (none["found"]!.GetValue<int>(), none["notFound"]!.GetValue<int>(), none["rounds"]!.GetValue<int>()));
    }

    [Fact]
    public async Task Liked_and_favourite_seed_songs_are_marked_once_and_picked_first()
    {
        await using var vendor = await StartWithAiAsync();
        var tracks = Enumerable.Range(1, 12).Select(i => _shelf.Add($"Song {i}", "Some Artist")).ToArray();
        await Ok(HttpMethod.Post, "zones/living/queue/items", Json(new { tracks }));
        // Song 1 and 2 are older than the queue's last ten; Song 12 is both.
        await Ok(HttpMethod.Post, "likes", Json(new { track = tracks[0] }));
        await Ok(HttpMethod.Post, "favorites", Json(new { track = tracks[1] }));
        await Ok(HttpMethod.Post, "likes", Json(new { track = tracks[11] }));
        await Ok(HttpMethod.Post, "favorites", Json(new { track = tracks[11] }));

        vendor.Answers.Enqueue(ToolAnswer("suggest_songs", Songs()));
        vendor.Answers.Enqueue(ToolAnswer("suggest_songs", Songs()));
        await Ok(HttpMethod.Post, "ai/suggestions", """{"request":"more like these","seed":{"queue":"living"},"count":1}""");

        var seed = UserText(vendor.Requests.First()).Split('\n').Where(l => l.StartsWith("- ", StringComparison.Ordinal)).ToList();
        Assert.Equal(["- Song 1 — Some Artist [liked]", "- Song 2 — Some Artist [liked]", "- Song 5 — Some Artist",
            "- Song 6 — Some Artist", "- Song 7 — Some Artist", "- Song 8 — Some Artist", "- Song 9 — Some Artist",
            "- Song 10 — Some Artist", "- Song 11 — Some Artist", "- Song 12 — Some Artist [liked]"], seed);
        Assert.Contains("[liked] are ones the household liked", vendor.Requests.First()["messages"]![0]!["content"]!.GetValue<string>(),
            StringComparison.Ordinal);

        // The agent's tools name the same tracks.
        await using var mcp = await McpAsync(null);
        var queue = await Call(mcp, "get_queue", new { zone = "living", limit = 100 });
        var items = queue["queue"]!["items"]!.AsArray();
        Assert.Equal([items[0]!["id"]!.GetValue<long>(), items[1]!["id"]!.GetValue<long>(), items[11]!["id"]!.GetValue<long>()],
            queue["preferred"]!.AsArray().Select(i => i!.GetValue<long>()));
        var playlist = (await Ok(HttpMethod.Post, "playlists", """{"name":"Mix"}"""))["id"]!.GetValue<long>();
        await Ok(HttpMethod.Post, $"playlists/{playlist}/entries", Json(new { tracks = tracks[..3] }));
        var entries = await Call(mcp, "get_playlist_entries", new { playlist });
        Assert.Equal(entries["items"]!.AsArray().Take(2).Select(e => e!["id"]!.GetValue<long>()),
            entries["preferred"]!.AsArray().Select(i => i!.GetValue<long>()));
    }

    [Fact]
    public async Task Find_songs_over_mcp_answers_what_the_resolver_answers()
    {
        await using var vendor = await StartWithAiAsync();
        _shelf.Add("Blue in Green", "Miles Davis");
        _stream.Add("Naima - Remastered 2003", "John Coltrane");
        SongSuggestion[] songs = [new("Blue in Green", "Miles Davis"), new("Naima", "John Coltrane"), new("Nothing", "Nobody")];

        var resolved = (await _host!.Services.GetRequiredService<SongResolver>().ResolveAsync(songs, [], Ct)).Value;
        await using var mcp = await McpAsync(null);
        var tool = await Call(mcp, "find_songs", new { songs = songs.Select(s => new { title = s.Title, artist = s.Artist }) });
        Assert.Equal(resolved.Found, tool["found"]!.GetValue<int>());
        Assert.Equal(resolved.Songs.Select(s => s.Track?.ProviderItemId),
            tool["songs"]!.AsArray().Select(s => s!["track"]?["providerItemId"]?.GetValue<string>()));
        Assert.Equal(resolved.Songs.Select(s => s.Reason), tool["songs"]!.AsArray().Select(s => s!["reason"]!.GetValue<string>()));
    }

    // --- the agent (phase 25) ---------------------------------------------------

    // The system prompt a Chat Completions request sent, its first message.
    private static string SystemOf(JsonNode request) => request["messages"]![0]!["content"]!.GetValue<string>();

    [Fact]
    public async Task The_agent_plays_something_similar_through_its_tools_in_the_models_order()
    {
        await using var vendor = await StartWithAiAsync();
        await PlayingAsync();
        var blue = _shelf.Add("Blue in Green", "Miles Davis");
        var peace = _stream.Add("Peace Piece", "Bill Evans");
        using var stream = await EventsAsync(null);

        vendor.Answers.Enqueue(ToolAnswer("list_zones", new { }));
        vendor.Answers.Enqueue(ToolAnswer("find_songs", Songs(("Blue in Green", "Miles Davis"), ("Peace Piece", "Bill Evans"))));
        vendor.Answers.Enqueue(ToolAnswer("add_to_queue", new { zone = "living", tracks = new[] { blue, peace } }));
        vendor.Answers.Enqueue(ToolAnswer("play", new { zone = "living" }));
        vendor.Answers.Enqueue(TextAnswer("Queued Blue in Green and Peace Piece."));

        var reply = await Ok(HttpMethod.Post, "ai/commands",
            """{"text":"play something similar to the current track","zone":"living"}""");
        Assert.Equal("Queued Blue in Green and Peace Piece.", reply["reply"]!.GetValue<string>());
        var actions = reply["actions"]!.AsArray();
        Assert.Equal(["list_zones", "find_songs", "add_to_queue", "play"], actions.Select(a => a!["tool"]!.GetValue<string>()));
        Assert.All(actions, a => Assert.True(a!["ok"]!.GetValue<bool>(), a.ToJsonString()));
        Assert.Null(reply["stoppedBy"]);
        Assert.Equal(["So What", "Blue in Green", "Peace Piece"], Titles(await Ok(HttpMethod.Get, "zones/living/queue")));

        var first = vendor.Requests.First();
        var tools = first["tools"]!.AsArray().Select(t => t!["function"]!["name"]!.GetValue<string>()).ToList();
        Assert.Contains("find_songs", tools);
        Assert.Contains("create_playlist", tools);
        Assert.DoesNotContain("delete_zone", tools);
        // Told by its name, its rooms': the tools take it back to the id.
        Assert.Contains("\"roomA\"", first["messages"]![0]!["content"]!.GetValue<string>(), StringComparison.Ordinal);
        Assert.Contains(vendor.Requests.ElementAt(1)["messages"]!.AsArray(), m => m!["role"]!.GetValue<string>() == "tool");

        // Every vendor call is on the stream, its prompt and answer included.
        var request = JsonNode.Parse((await stream.NextAsync(e => e.Type == "ai.request")).Data)!;
        Assert.True(request["ok"]!.GetValue<bool>());
        Assert.Contains("play something similar to the current track", request["request"]!["messages"]!.ToJsonString(), StringComparison.Ordinal);
        Assert.Contains("find_songs", request["request"]!["tools"]!.AsArray().Select(t => t!.GetValue<string>()));
        Assert.Equal("list_zones", request["answer"]!["toolCalls"]![0]!["name"]!.GetValue<string>());
        Assert.Equal("agent", JsonNode.Parse((await stream.NextAsync(e => e.Type == "ai.toolCalled")).Data)!["caller"]!.GetValue<string>());
        var ended = JsonNode.Parse((await stream.NextAsync(e => e.Type == "ai.commandEnded")).Data)!;
        Assert.Equal((true, 4), (ended["ok"]!.GetValue<bool>(), ended["toolCalls"]!.GetValue<int>()));
    }

    [Fact]
    public async Task The_agent_is_bounded_by_its_tiers_its_limits_its_time_and_its_concurrency()
    {
        await using var vendor = await StartWithAiAsync();

        // A tool outside the tiers is refused, and the command still answers.
        vendor.Answers.Enqueue(ToolAnswer("delete_zone", new { zone = "living" }));
        vendor.Answers.Enqueue(TextAnswer("I may not delete zones."));
        var refused = await Ok(HttpMethod.Post, "ai/commands", """{"text":"delete the living room zone"}""");
        Assert.False(refused["actions"]![0]!["ok"]!.GetValue<bool>());
        Assert.StartsWith("forbidden: delete_zone is a destructive tool", refused["actions"]![0]!["summary"]!.GetValue<string>(),
            StringComparison.Ordinal);
        Assert.Equal("I may not delete zones.", refused["reply"]!.GetValue<string>());

        // A model that cannot do what is asked answers CANNOT:, which is named.
        vendor.Answers.Enqueue(TextAnswer("CANNOT: I cannot change the equalizer."));
        var cannot = await Ok(HttpMethod.Post, "ai/commands", """{"text":"more bass"}""");
        Assert.Equal(("cannot", "I cannot change the equalizer."),
            (cannot["stoppedBy"]!.GetValue<string>(), cannot["reply"]!.GetValue<string>()));

        // A model that loops is stopped at maxToolCalls.
        await Ok(HttpMethod.Patch, "ai/settings", """{"agent":{"maxToolCalls":3}}""");
        for (var i = 0; i < 5; i++)
            vendor.Answers.Enqueue(ToolAnswer("list_zones", new { }));
        var looped = await Ok(HttpMethod.Post, "ai/commands", """{"text":"keep looking"}""");
        Assert.Equal(("maxToolCalls", 3), (looped["stoppedBy"]!.GetValue<string>(), looped["actions"]!.AsArray().Count));
        while (vendor.Answers.TryDequeue(out _))
        {
        }

        // A conversation keeps its turns.
        vendor.Answers.Enqueue(TextAnswer("Which zone?"));
        vendor.Answers.Enqueue(TextAnswer("Done."));
        await Ok(HttpMethod.Post, "ai/commands", """{"text":"play jazz","conversationId":"c1"}""");
        var count = vendor.Requests.Count;
        await Ok(HttpMethod.Post, "ai/commands", """{"text":"the living room","conversationId":"c1"}""");
        var messages = vendor.Requests.ElementAt(count)["messages"]!.ToJsonString();
        Assert.Contains("play jazz", messages, StringComparison.Ordinal);
        Assert.Contains("Which zone?", messages, StringComparison.Ordinal);
        // Only in a conversation may it ask back.
        Assert.Contains("you may ask back", SystemOf(vendor.Requests.ElementAt(count)), StringComparison.Ordinal);
        Assert.DoesNotContain("you may ask back", SystemOf(vendor.Requests.ElementAt(count - 2)), StringComparison.Ordinal);

        // ... and is kept as a chat, titled by its first command.
        var chats = (await Ok(HttpMethod.Get, "ai/chats")).AsArray();
        Assert.Equal(("c1", "play jazz", 2), (chats[0]!["id"]!.GetValue<string>(), chats[0]!["title"]!.GetValue<string>(),
            chats[0]!["turns"]!.GetValue<int>()));
        var chat = await Ok(HttpMethod.Get, "ai/chats/c1");
        Assert.Equal(("Which zone?", "Done."), (chat["turns"]![0]!["reply"]!.GetValue<string>(),
            chat["turns"]![1]!["reply"]!.GetValue<string>()));
        Assert.NotNull(chat["turns"]![0]!["model"]);
        await Ok(HttpMethod.Patch, "ai/chats/c1", """{"title":"Jazz"}""");
        Assert.Equal("Jazz", (await Ok(HttpMethod.Get, "ai/chats/c1"))["title"]!.GetValue<string>());
        await Refused(HttpMethod.Patch, "ai/chats/c1", """{"title":" "}""", HttpStatusCode.BadRequest, "invalidArgument");
        Assert.Equal(HttpStatusCode.NoContent, (await Send(HttpMethod.Delete, "ai/chats/c1")).Status);
        await Refused(HttpMethod.Get, "ai/chats/c1", null, HttpStatusCode.NotFound, "notFound");

        // A provider that never answers costs the command's timeout; the third
        // command at once is busy.
        await Ok(HttpMethod.Patch, "ai/settings", """{"agent":{"timeoutSeconds":2,"maxConcurrent":2}}""");
        vendor.Hang = true;
        var hanging = new[]
        {
            Send(HttpMethod.Post, "ai/commands", """{"text":"one"}"""),
            Send(HttpMethod.Post, "ai/commands", """{"text":"two"}"""),
        };
        await Until(async () => (await Ok(HttpMethod.Get, "status"))["ai"]!["agent"]!["running"]!.GetValue<int>() == 2);
        await Refused(HttpMethod.Post, "ai/commands", """{"text":"three"}""", HttpStatusCode.TooManyRequests, "busy");
        foreach (var answer in await Task.WhenAll(hanging))
        {
            Assert.Equal(HttpStatusCode.ServiceUnavailable, answer.Status);
            Assert.Equal("timeout", answer.Body!["error"]!.GetValue<string>());
        }

        vendor.Hang = false;

        // Who speaks: the one named, else the zone's listener, else the household.
        await Ok(HttpMethod.Post, "users", """{"name":"Alice"}""");
        vendor.Answers.Enqueue(TextAnswer("Done."));
        await Ok(HttpMethod.Post, "ai/commands", """{"text":"like this","user":"alice"}""");
        Assert.Contains("from the user \"Alice\"", SystemOf(vendor.Requests.Last()), StringComparison.Ordinal);
        vendor.Answers.Enqueue(TextAnswer("Done."));
        await Ok(HttpMethod.Post, "ai/commands", """{"text":"like this"}""");
        Assert.DoesNotContain("from the user", SystemOf(vendor.Requests.Last()), StringComparison.Ordinal);
        await Ok(HttpMethod.Patch, "zones/living", """{"user":"Alice"}""");
        vendor.Answers.Enqueue(TextAnswer("Done."));
        await Ok(HttpMethod.Post, "ai/commands", """{"text":"like this","zone":"living"}""");
        Assert.Contains("from the user \"Alice\"", SystemOf(vendor.Requests.Last()), StringComparison.Ordinal);
        await Refused(HttpMethod.Post, "ai/commands", """{"text":"like this","user":"Bob"}""", HttpStatusCode.BadRequest,
            "invalidArgument");

        await Refused(HttpMethod.Post, "ai/commands", """{"text":" "}""", HttpStatusCode.BadRequest, "invalidArgument");
        await Refused(HttpMethod.Post, "ai/commands", """{"text":"x","zone":"nowhere"}""", HttpStatusCode.NotFound, "notFound");
        await Refused(HttpMethod.Patch, "ai/settings", """{"agent":{"maxToolCalls":0}}""", HttpStatusCode.BadRequest,
            "invalidArgument");
        Assert.DoesNotContain("play jazz", await ReadLog(), StringComparison.Ordinal);
    }

    // --- ChatGPT plan and reasoning (phases 28, 29) ----------------------------

    [Fact]
    public async Task A_chatgpt_sign_in_completes_through_the_callback_with_its_client_and_leaks_nothing()
    {
        await using var oauth = await FakeOAuth.StartAsync();
        await using var vendor = await FakeOpenAiResponses.StartAsync();
        vendor.Accepts = t => t == oauth.CurrentAccess;
        var hostFile = Path.Combine(_dir.Path, "data", "ai", ChatGptHost.FileName);
        await StartAsync(sections: $$"""
            , "ai": { "providers": [ { "id": "openai", "baseUrl": "{{vendor.BaseUri}}" } ] }
            """, services: s =>
        {
            // The host's login, pointed at the fake authorization server.
            var registered = s.Single(d => d.ImplementationInstance is AiLoginRegistration { Id: OpenAiProvider.ProviderId });
            s.Remove(registered);
            s.AddAiLogin(OpenAiProvider.ProviderId, h => new ChatGptLogin(h, new ChatGptHost(hostFile), oauth.ChatGpt));
        });
        var answers = new List<string>();

        var chatgpt = (await Ok(HttpMethod.Get, "ai/providers")).AsArray().Single(p => p!["id"]!.GetValue<string>() == "openai")!;
        Assert.Equal(["apiKey", "oauth"], chatgpt["authMethods"]!.AsArray().Select(m => m!.GetValue<string>()));
        Assert.Equal(["reasoning", "webSearch", "transcription"], chatgpt["capabilities"]!.AsArray().Select(m => m!.GetValue<string>()));
        Assert.False(chatgpt["configured"]!.GetValue<bool>());
        await Refused(HttpMethod.Post, "ai/providers/openai/login", """{"redirectUri":"https://gui.example.com/done"}""",
            HttpStatusCode.BadRequest, "invalidArgument");

        // No redirect: this server's callback, which is loopback.
        var start = await Ok(HttpMethod.Post, "ai/providers/openai/login");
        var (code, client) = oauth.ConsentChatGpt(new Uri(start["authorizeUrl"]!.GetValue<string>()));
        var callback = await Ok(HttpMethod.Get,
            $"ai/login/callback?state={Uri.EscapeDataString(start["state"]!.GetValue<string>())}&code={code}&client_id={client}");
        Assert.Equal((true, true, false), (callback["hasToken"]!.GetValue<bool>(), callback["configured"]!.GetValue<bool>(),
            callback["hasKey"]!.GetValue<bool>()));
        answers.Add(callback.ToJsonString());

        var models = await Ok(HttpMethod.Get, "ai/providers/openai/models");
        Assert.Equal(["gpt-plan"], models.AsArray().Select(m => m!["id"]!.GetValue<string>()));
        var signedIn = await Ok(HttpMethod.Get, "ai/providers");
        // Signed in, it still hears speech: on ChatGPT's dictation (ChatGptTests).
        Assert.Equal(["reasoning", "webSearch", "transcription"], signedIn.AsArray().Single(p => p!["id"]!.GetValue<string>() == "openai")!
            ["capabilities"]!.AsArray().Select(m => m!.GetValue<string>()));
        answers.Add(signedIn.ToJsonString());
        answers.Add((await Ok(HttpMethod.Get, "status")).ToJsonString());

        var hostId = JsonElement.Parse(await File.ReadAllTextAsync(hostFile, Ct)).GetProperty("hostId").GetString()!;
        var everything = string.Join('\n', answers) + await ReadLog();
        Assert.All(oauth.Issued.Append(client).Append(hostId),
            secret => Assert.DoesNotContain(secret, everything, StringComparison.Ordinal));
    }

    [Fact]
    public async Task Speech_is_transcribed_by_the_speech_settings_provider_on_its_route()
    {
        await using var vendor = await FakeOpenAi.StartAsync();
        await StartAsync(sections: $$"""
            , "ai": { "providers": [ { "id": "openai", "apiKey": "{{FakeOpenAi.Key}}", "baseUrl": "{{vendor.BaseUri}}" },
                                     { "id": "gemini", "apiKey": "{{FakeOpenAi.Key}}", "baseUrl": "{{vendor.BaseUri}}" } ] }
            """);
        byte[] wav = [.. "RIFF"u8, .. new byte[40]];
        async Task<(HttpStatusCode Status, JsonNode? Body)> Transcribe(string type = "audio/wav")
        {
            using var body = new ByteArrayContent(wav);
            body.Headers.ContentType = new MediaTypeHeaderValue(type);
            using var answer = await Http.PostAsync(new Uri("ai/transcriptions", UriKind.Relative), body, Ct);
            return (answer.StatusCode, JsonNode.Parse(await answer.Content.ReadAsStringAsync(Ct)));
        }

        Assert.Equal("browser", (await Ok(HttpMethod.Get, "ai/settings"))["speech"]!["recognition"]!.GetValue<string>());
        var none = await Transcribe();
        Assert.Equal((HttpStatusCode.Conflict, "notConfigured"), (none.Status, none.Body!["error"]!.GetValue<string>()));

        var speech = (await Ok(HttpMethod.Patch, "ai/settings",
            """{"speech":{"recognition":"server","provider":"openai","model":"whisper-1","language":"de-DE","readReplies":true}}"""))["speech"]!;
        Assert.Equal(("server", "openai", true), (speech["recognition"]!.GetValue<string>(), speech["provider"]!.GetValue<string>(),
            speech["readReplies"]!.GetValue<bool>()));
        Assert.Equal("Turn the kitchen up.", (await Transcribe()).Body!["text"]!.GetValue<string>());
        Assert.Equal([("whisper-1", "de", (long)wav.Length)], vendor.Transcriptions.Select(t => (t.Model, t.Language, t.Bytes)));

        // Gemini hears it as input_audio on a chat turn.
        vendor.Answers.Enqueue("""{"choices":[{"index":0,"message":{"role":"assistant","content":"Lauter."},"finish_reason":"stop"}]}""");
        await Ok(HttpMethod.Patch, "ai/settings", """{"speech":{"provider":"gemini","model":"gemini-2.5-flash"}}""");
        Assert.Equal("Lauter.", (await Transcribe()).Body!["text"]!.GetValue<string>());
        var audio = vendor.Requests.Last()["messages"]![0]!["content"]![1]!["input_audio"]!;
        Assert.Equal((Convert.ToBase64String(wav), "wav"), (audio["data"]!.GetValue<string>(), audio["format"]!.GetValue<string>()));

        var wrong = await Transcribe("audio/webm");
        Assert.Equal((HttpStatusCode.BadRequest, "invalidArgument"), (wrong.Status, wrong.Body!["error"]!.GetValue<string>()));
        await Refused(HttpMethod.Patch, "ai/settings", """{"speech":{"provider":"anthropic"}}""", HttpStatusCode.BadRequest, "invalidArgument");
        await Refused(HttpMethod.Patch, "ai/settings", """{"speech":{"provider":"nobody"}}""", HttpStatusCode.NotFound, "notFound");
        await Refused(HttpMethod.Patch, "ai/settings", """{"speech":{"language":"de DE"}}""", HttpStatusCode.BadRequest, "invalidArgument");
    }

    [Fact]
    public async Task Reasoning_is_set_per_use_won_by_the_request_kept_and_its_thinking_shown_nowhere()
    {
        await using var vendor = await FakeOpenAi.StartAsync();
        await StartAsync(sections: $$"""
            , "ai": { "providers": [ { "id": "openai", "apiKey": "{{FakeOpenAi.Key}}", "baseUrl": "{{vendor.BaseUri}}" },
                                     { "id": "fixture", "apiKey": "{{FakeOpenAi.Key}}", "baseUrl": "{{vendor.BaseUri}}" } ] }
            """, services: s => s.AddAiProvider(FixtureAi.ProviderId, c => new FixtureAi(c), TimeSpan.FromSeconds(5)));

        var initial = await Ok(HttpMethod.Get, "ai/settings");
        Assert.Equal(("default", "default"), (initial["reasoning"]!["agent"]!.GetValue<string>(),
            initial["reasoning"]!["suggestions"]!.GetValue<string>()));
        var patched = await Ok(HttpMethod.Patch, "ai/settings",
            """{"defaultProvider":"openai","defaultModel":"model-a","reasoning":{"agent":"low"}}""");
        Assert.Equal(("low", "default"), (patched["reasoning"]!["agent"]!.GetValue<string>(),
            patched["reasoning"]!["suggestions"]!.GetValue<string>()));
        await Refused(HttpMethod.Patch, "ai/settings", """{"reasoning":{"agent":"extreme"}}""", HttpStatusCode.BadRequest,
            "invalidRequest");

        vendor.Answers.Enqueue("""
            {"choices":[{"index":0,"message":{"role":"assistant","content":"Done.","reasoning_content":"SECRET-THOUGHT"},
              "finish_reason":"stop"}],"usage":{"prompt_tokens":10,"completion_tokens":20,"completion_tokens_details":{"reasoning_tokens":15}}}
            """);
        var reply = await Ok(HttpMethod.Post, "ai/commands", """{"text":"play something"}""");
        Assert.Equal(("Done.", 15), (reply["reply"]!.GetValue<string>(), reply["usage"]!["reasoningTokens"]!.GetValue<int>()));
        var sent = vendor.Requests.Last();
        Assert.Equal(("low", MultiroomAgent.MaxReplyTokens + AiReasoningBudget.Of(AiReasoning.Low)), (sent["reasoning_effort"]!.GetValue<string>(),
            sent["max_completion_tokens"]!.GetValue<int>()));

        await Ok(HttpMethod.Post, "ai/commands", """{"text":"play something","reasoning":"high"}""");
        Assert.Equal("high", vendor.Requests.Last()["reasoning_effort"]!.GetValue<string>());
        await Ok(HttpMethod.Post, "ai/commands", """{"text":"play something","reasoning":"default"}""");
        Assert.Null(vendor.Requests.Last()["reasoning_effort"]);

        await Refused(HttpMethod.Post, "ai/commands", """{"text":"play something","provider":"fixture","model":"model-a","reasoning":"high"}""",
            HttpStatusCode.BadRequest, "invalidArgument", "fixture takes no reasoning setting");

        var status = await Ok(HttpMethod.Get, "status");
        var usage = status["ai"]!["providers"]!.AsArray().Single(p => p!["id"]!.GetValue<string>() == "openai")!;
        Assert.Equal(15, usage["reasoningTokens"]!.GetValue<int>());
        var file = await File.ReadAllTextAsync(Path.Combine(_dir.Path, "data", "ai", "settings.json"), Ct);
        Assert.Contains("\"reasoning\":{\"agent\":\"low\",\"suggestions\":\"default\"}", file, StringComparison.Ordinal);
        Assert.DoesNotContain("SECRET-THOUGHT", reply.ToJsonString() + await ReadLog(), StringComparison.Ordinal);
    }
}
