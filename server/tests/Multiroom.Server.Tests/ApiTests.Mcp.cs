using System.Net;
using System.Net.Http.Headers;
using System.Text.Json;
using System.Text.Json.Nodes;
using Microsoft.Extensions.DependencyInjection;
using ModelContextProtocol.Client;
using ModelContextProtocol.Protocol;
using Multiroom.Server.Web.Api;
using Multiroom.Server.Web.Media;

namespace Multiroom.Server.Tests;

// The Galdrun MCP server (phase 23), over the SDK's own client against the
// composed host: each caller lists exactly the tools its tiers allow and is
// refused the others by name; a named client's token sees its tiers and opens
// nothing else; a wrong token is 401; and every tool answers what the matching
// REST route answers for the same state.
public sealed partial class ApiTests
{
    private const string McpToken = "api-t0ken-for-mcp";

    // What configures the house or holds a secret is never a tool.
    private static readonly string[] NeverTools = ["library", "folder", "credential", "login", "key", "resampler", "device", "binding", "setting"];

    private static readonly string[] ToolCalledFields = ["caller", "tool", "ok", "ms"];

    private static readonly string[] OnlyRoomA = ["roomA"];

    private async Task<McpClient> McpAsync(string? token)
    {
        var transport = new HttpClientTransport(new HttpClientTransportOptions
        {
            Endpoint = new Uri(Http.BaseAddress!, "mcp"),
            TransportMode = HttpTransportMode.StreamableHttp,
            AdditionalHeaders = token is null ? null : new Dictionary<string, string> { ["Authorization"] = $"Bearer {token}" },
        });
        return await McpClient.CreateAsync(transport, cancellationToken: Ct);
    }

    private static async Task<List<string>> ToolNames(McpClient client) =>
        [.. (await client.ListToolsAsync(cancellationToken: Ct)).Select(t => t.Name).Order(StringComparer.Ordinal)];

    // The tool's answer as JSON; a tool error fails the test with its text.
    private static async Task<JsonNode> Call(McpClient client, string tool, object? arguments = null)
    {
        var result = await CallRaw(client, tool, arguments);
        var text = ((TextContentBlock)result.Content[0]).Text;
        Assert.True(result.IsError != true, $"{tool}: {text}");
        return JsonNode.Parse(text)!;
    }

    private static async Task<CallToolResult> CallRaw(McpClient client, string tool, object? arguments = null)
    {
        var args = arguments is null
            ? new Dictionary<string, object?>()
            : JsonSerializer.SerializeToNode(arguments)!.AsObject().ToDictionary(p => p.Key, p => (object?)p.Value);
        return await client.CallToolAsync(tool, args, cancellationToken: Ct);
    }

    private static string ErrorOf(CallToolResult result)
    {
        Assert.True(result.IsError, "the tool answered");
        return ((TextContentBlock)result.Content[0]).Text;
    }

    private async Task<string> NamedClient(string name, params string[] tiers)
    {
        var created = await Ok(HttpMethod.Post, "ai/mcp/clients", JsonSerializer.Serialize(new { name, tiers }));
        Assert.Equal(name, created["client"]!["name"]!.GetValue<string>());
        return created["token"]!.GetValue<string>();
    }

    [Fact]
    public async Task Mcp_lists_the_tools_of_each_callers_tiers_and_refuses_the_others_by_name()
    {
        await StartAsync($$""", "token": "{{McpToken}}" """);
        Http.DefaultRequestHeaders.Authorization = new AuthenticationHeaderValue("Bearer", McpToken);

        // The external default: read and control.
        await using var external = await McpAsync(McpToken);
        var listed = await ToolNames(external);
        Assert.Contains("search", listed);
        Assert.Contains("play", listed);
        Assert.DoesNotContain("create_playlist", listed);
        Assert.DoesNotContain("delete_zone", listed);
        Assert.Equal("forbidden: delete_zone is a destructive tool, which external is not allowed",
            ErrorOf(await CallRaw(external, "delete_zone", new { zone = "living" })));

        // Each tool is in exactly one tier: one client per tier partitions them.
        var perTier = new Dictionary<string, List<string>>();
        foreach (var tier in new[] { "read", "control", "modify", "destructive" })
        {
            await using var only = await McpAsync(await NamedClient($"only-{tier}", tier));
            perTier[tier] = await ToolNames(only);
            Assert.NotEmpty(perTier[tier]);
        }
        await using var all = await McpAsync(await NamedClient("all", "read", "control", "modify", "destructive"));
        var everything = await ToolNames(all);
        Assert.Equal(everything, perTier.Values.SelectMany(t => t).Order(StringComparer.Ordinal));
        Assert.Equal(everything.Count, everything.Distinct().Count());
        Assert.All(NeverTools, never => Assert.DoesNotContain(everything, t => t.Contains(never, StringComparison.Ordinal)));

        // A named client's tiers, and its token opens nothing but /mcp.
        var reader = await NamedClient("reader", "read");
        await using (var client = await McpAsync(reader))
        {
            Assert.Equal(perTier["read"], await ToolNames(client));
            Assert.StartsWith("forbidden: play is a control tool, which reader is not allowed",
                ErrorOf(await CallRaw(client, "play", new { zone = "living" })), StringComparison.Ordinal);
        }
        using (var other = new HttpClient(new SocketsHttpHandler { UseProxy = false }) { BaseAddress = Http.BaseAddress })
        {
            other.DefaultRequestHeaders.Authorization = new AuthenticationHeaderValue("Bearer", reader);
            Assert.Equal(HttpStatusCode.Unauthorized, (await other.GetAsync("status", Ct)).StatusCode);
            other.DefaultRequestHeaders.Authorization = new AuthenticationHeaderValue("Bearer", "vxm_wrong");
            using var refused = await other.PostAsync("mcp", new StringContent("{}"), Ct);
            Assert.Equal(HttpStatusCode.Unauthorized, refused.StatusCode);
        }
        await Assert.ThrowsAnyAsync<Exception>(() => McpAsync("vxm_wrong"));

        // The list answers no token; the file keeps none either; one is answered once.
        var clients = await Send(HttpMethod.Get, "ai/mcp/clients");
        Assert.DoesNotContain(reader, clients.Text, StringComparison.Ordinal);
        Assert.DoesNotContain(reader, await File.ReadAllTextAsync(Path.Combine(_dir.Path, "data", "ai", "mcp.json"), Ct),
            StringComparison.Ordinal);
        await Refused(HttpMethod.Post, "ai/mcp/clients", """{"name":"reader","tiers":["read"]}""", HttpStatusCode.BadRequest,
            "invalidArgument");
        await Refused(HttpMethod.Post, "ai/mcp/clients", """{"name":"x","tiers":[]}""", HttpStatusCode.BadRequest,
            "invalidArgument");
        await Refused(HttpMethod.Post, "ai/mcp/clients", """{"name":"x","tiers":["admin"]}""", HttpStatusCode.BadRequest,
            "invalidRequest");
        Assert.Equal(HttpStatusCode.NoContent, (await Send(HttpMethod.Delete, "ai/mcp/clients/reader")).Status);
        await Refused(HttpMethod.Delete, "ai/mcp/clients/reader", HttpStatusCode.NotFound, "notFound");
        await Assert.ThrowsAnyAsync<Exception>(() => McpAsync(reader));

        // The external tiers are the user's.
        var patched = await Ok(HttpMethod.Patch, "ai/mcp", """{"externalTiers":["read"]}""");
        Assert.Equal(["read"], patched["externalTiers"]!.AsArray().Select(t => t!.GetValue<string>()));
        await using (var narrowed = await McpAsync(McpToken))
            Assert.Equal(perTier["read"], await ToolNames(narrowed));

        // Route isolation: the media listener has no /api/v1/mcp.
        var media = _host!.Services.GetRequiredService<MediaEndpoint>().BaseUri!;
        using var plain = new HttpClient(new SocketsHttpHandler { UseProxy = false });
        Assert.Equal(HttpStatusCode.NotFound, (await plain.GetAsync(new Uri(media, "/api/v1/mcp"), Ct)).StatusCode);
        Assert.DoesNotContain(reader, await ReadLog(), StringComparison.Ordinal);
    }

    [Fact]
    public async Task Mcp_takes_a_zone_by_its_id_its_name_or_any_of_its_rooms()
    {
        await StartAsync($$""", "token": "{{McpToken}}" """);
        Http.DefaultRequestHeaders.Authorization = new AuthenticationHeaderValue("Bearer", McpToken);
        await _core.EmitAsync("""{"event":"room.connected","client":"roomB","deviceName":"Speaker","reconnect":false}""");
        await Until(async () => (await Ok(HttpMethod.Get, "zones")).AsArray().Count == 2);
        await Ok(HttpMethod.Put, "zones/living/clients", """{"clients":["roomA","roomB"]}""");

        await using var mcp = await McpAsync(McpToken);
        foreach (var name in new[] { "living", "roomA + roomB", "RoomB", " roomA " })
        {
            var zone = await Call(mcp, "set_zone_volume", new { zone = name, volume = 0.5 });
            Assert.Equal(("living", "roomA + roomB"), (zone["zone"]!.GetValue<string>(), zone["name"]!.GetValue<string>()));
        }
        Assert.Contains("no zone kitchen", ErrorOf(await CallRaw(mcp, "set_zone_volume", new { zone = "kitchen", volume = 0.5 })),
            StringComparison.Ordinal);
    }

    [Fact]
    public async Task Mcp_tools_answer_what_the_matching_routes_answer()
    {
        await StartAsync();
        var a = _fake.AddTrack(_dir.Write("a.flac", "a"), "Blue in Green");
        var b = _fake.AddTrack(_dir.Write("b.flac", "b"), "So What");
        _fake.AddStation("Blue FM", [1, 2, 3]);
        await using var mcp = await McpAsync(await NamedClient("all", "read", "control", "modify", "destructive"));
        using var stream = await EventsAsync(null);

        static void Same(JsonNode? rest, JsonNode? tool, string what) =>
            Assert.True(JsonNode.DeepEquals(rest, tool), $"{what}\nREST: {rest?.ToJsonString()}\ntool: {tool?.ToJsonString()}");

        Same(await Ok(HttpMethod.Get, "search?q=blue&limit=10"), await Call(mcp, "search", new { query = "blue", limit = 10 }), "search");
        Same(await Ok(HttpMethod.Get, "providers"), await Call(mcp, "list_providers"), "providers");
        Same(await Ok(HttpMethod.Get, "providers/fake/browse?node=tracks&limit=5"),
            await Call(mcp, "browse", new { provider = "fake", node = "tracks", limit = 5 }), "browse");

        // Control: a zone, its queue, transport, modes, volumes.
        var tracks = JsonSerializer.SerializeToNode(new[] { a, b }, TestJson);
        Same((await Ok(HttpMethod.Put, "zones/living/clients", """{"clients":["roomA"]}""")),
            (await Call(mcp, "set_zone_rooms", new { zone = "living", rooms = OnlyRoomA }))["queue"], "set_zone_rooms");
        var added = await Call(mcp, "add_to_queue", new { zone = "living", tracks });
        Assert.Equal(2, added["queued"]!.GetValue<int>());
        Same(await Ok(HttpMethod.Get, "zones/living/queue"), (await Call(mcp, "get_queue", new { zone = "living" }))["queue"], "get_queue");
        await Call(mcp, "play", new { zone = "living" });
        var moded = await Call(mcp, "set_queue_mode", new { zone = "living", repeat = "all", shuffle = false });
        Same(await Ok(HttpMethod.Get, "zones/living/queue"), moded["queue"], "set_queue_mode");
        Assert.Equal("all", (await Ok(HttpMethod.Get, "zones/living/queue"))["repeat"]!.GetValue<string>());
        var zone = await Call(mcp, "set_zone_volume", new { zone = "living", volume = 0.5 });
        Same(await Ok(HttpMethod.Get, "zones/living"), zone, "set_zone_volume");
        Same((await Ok(HttpMethod.Get, "zones")).AsArray(), (await Call(mcp, "list_zones"))["items"], "list_zones");
        var room = await Call(mcp, "set_room_volume", new { room = "roomA", volume = 0.4 });
        Assert.Equal(0.4, room["desired"]!["volume"]!.GetValue<double>());
        Same(await Ok(HttpMethod.Get, "rooms"), await Call(mcp, "list_rooms"), "list_rooms");
        // A room's equalizer: a profile, on or off with its bands kept, or whole.
        Same(await Ok(HttpMethod.Get, "equalizer/profiles"), await Call(mcp, "list_equalizer_profiles"), "list_equalizer_profiles");
        Assert.Contains(": invalidArgument: room roomA has no equalizer yet",
            ErrorOf(await CallRaw(mcp, "set_room_equalizer", new { room = "roomA", enabled = false })), StringComparison.Ordinal);
        var boosted = await Call(mcp, "set_room_equalizer", new { room = "roomA", profile = "bass boost" });
        Same((await Ok(HttpMethod.Get, "rooms/roomA"))["desired"], boosted["desired"], "set_room_equalizer");
        Assert.Equal(-7.2, boosted["desired"]!["equalizer"]!["preampDb"]!.GetValue<double>());
        var off = (await Call(mcp, "set_room_equalizer", new { room = "roomA", enabled = false }))["desired"]!["equalizer"]!;
        Assert.Equal((false, 10), (off["enabled"]!.GetValue<bool>(), off["bands"]!.AsArray().Count));
        var whole = new { enabled = true, preampDb = -2, bands = new[] { new { type = "lowShelf", frequencyHz = 120, q = 0.7, gainDb = 2 } } };
        Assert.Single((await Call(mcp, "set_room_equalizer", new { room = "roomA", equalizer = whole }))["desired"]!["equalizer"]!["bands"]!.AsArray());
        Assert.Contains(": invalidArgument: exactly one of profile, enabled or equalizer",
            ErrorOf(await CallRaw(mcp, "set_room_equalizer", new { room = "roomA", profile = "Flat", enabled = true })), StringComparison.Ordinal);
        Assert.Equal("Desk", (await Call(mcp, "save_equalizer_profile", new { name = "Desk", preampDb = -2, bands = whole.bands }))
            ["name"]!.GetValue<string>());
        Same(await Ok(HttpMethod.Get, "equalizer/profiles"), await Call(mcp, "list_equalizer_profiles"), "save_equalizer_profile");

        // A station by its stream URL, once.
        var station = await Call(mcp, "add_station", new { name = "Jazz Radio", streamUrl = "https://radio.example/jazz.mp3", genre = "Jazz" });
        Assert.Equal("Jazz Radio", station["name"]!.GetValue<string>());
        Assert.Equal(station["providerItemId"]!.GetValue<string>(),
            (await Call(mcp, "add_station", new { name = "Again", streamUrl = "https://radio.example/jazz.mp3" }))["providerItemId"]!.GetValue<string>());
        Assert.Contains("the stream URL must be an absolute http or https URL",
            ErrorOf(await CallRaw(mcp, "add_station", new { name = "Local", streamUrl = "file:///etc/passwd" })), StringComparison.Ordinal);

        var queue = await Ok(HttpMethod.Get, "zones/living/queue");
        var second = queue["items"]![1]!["id"]!.GetValue<long>();
        await Call(mcp, "move_queue_item", new { zone = "living", item = second, index = 0 });
        Assert.Equal(second, (await Ok(HttpMethod.Get, "zones/living/queue"))["items"]![0]!["id"]!.GetValue<long>());

        // Modify: playlists, favourites, likes; the views as the routes list them.
        var playlist = await Call(mcp, "create_playlist", new { name = "Evening" });
        var id = playlist["id"]!.GetValue<long>();
        Same(await Ok(HttpMethod.Get, "playlists"), await Call(mcp, "list_playlists"), "list_playlists");
        await Call(mcp, "add_to_playlist", new { playlist = id, tracks });
        Same(await Ok(HttpMethod.Get, $"playlists/{id}/entries"),
            (await Call(mcp, "get_playlist_entries", new { playlist = id }))["items"], "get_playlist_entries");
        var favorite = await Call(mcp, "add_favorite", new { track = JsonSerializer.SerializeToNode(a, TestJson) });
        Same(await Ok(HttpMethod.Get, "favorites"), (await Call(mcp, "list_favorites"))["items"], "list_favorites");
        await Call(mcp, "like", new { track = JsonSerializer.SerializeToNode(b, TestJson) });
        Same(await Ok(HttpMethod.Get, "likes"), (await Call(mcp, "list_likes"))["items"], "list_likes");
        Assert.Equal("So What", JsonNode.Parse((await stream.NextAsync(e => e.Type == "like.changed")).Data)!
            ["item"]!["title"]!.GetValue<string>());
        Same(await Ok(HttpMethod.Post, "recommendations", JsonSerializer.Serialize(new { track = a, limit = 5 }, TestJson)),
            await Call(mcp, "recommend", new { tracks = new[] { JsonSerializer.SerializeToNode(a, TestJson) }, limit = 5 }), "recommend");

        // A module's failure is the tool's error, with its code.
        // The SDK puts "An error occurred invoking '<tool>': " before it.
        Assert.EndsWith(": notFound: no queue for zone nowhere", ErrorOf(await CallRaw(mcp, "get_queue", new { zone = "nowhere" })),
            StringComparison.Ordinal);
        Assert.Contains(": invalidArgument: exactly one of", ErrorOf(await CallRaw(mcp, "add_to_queue", new { zone = "living" })),
            StringComparison.Ordinal);

        // Destructive.
        var entries = await Ok(HttpMethod.Get, $"playlists/{id}/entries");
        await Call(mcp, "remove_playlist_entry", new { playlist = id, entry = entries[0]!["id"]!.GetValue<long>() });
        Assert.Single((await Ok(HttpMethod.Get, $"playlists/{id}/entries")).AsArray());
        await Call(mcp, "delete_playlist", new { playlist = id });
        Assert.Empty((await Ok(HttpMethod.Get, "playlists")).AsArray());
        await Call(mcp, "remove_favorite", new { favorite = favorite["id"]!.GetValue<long>() });
        Assert.Empty((await Ok(HttpMethod.Get, "favorites")).AsArray());
        var like = (await Ok(HttpMethod.Get, "likes"))[0]!["id"]!.GetValue<long>();
        await Call(mcp, "unlike", new { like });
        Assert.Empty((await Ok(HttpMethod.Get, "likes")).AsArray());
        Assert.EndsWith(": notFound: no like " + like, ErrorOf(await CallRaw(mcp, "unlike", new { like })), StringComparison.Ordinal);
        var replaced = await Call(mcp, "replace_queue", new { zone = "living", tracks = new[] { JsonSerializer.SerializeToNode(b, TestJson) } });
        Assert.Equal(1, replaced["queue"]!["total"]!.GetValue<int>());
        var only = (await Ok(HttpMethod.Get, "zones/living/queue"))["items"]![0]!["id"]!.GetValue<long>();
        await Call(mcp, "remove_queue_item", new { zone = "living", item = only });
        Assert.Empty((await Ok(HttpMethod.Get, "zones/living/queue"))["items"]!.AsArray());
        await Call(mcp, "replace_queue", new { zone = "living", tracks = new[] { JsonSerializer.SerializeToNode(b, TestJson) } });
        var cleared = await Call(mcp, "clear_queue", new { zone = "living" });
        Assert.Equal(0, cleared["total"]!.GetValue<int>());
        Assert.Empty((await Ok(HttpMethod.Get, "zones/living/queue"))["items"]!.AsArray());
        Assert.EndsWith(": notFound: no queue for zone nowhere", ErrorOf(await CallRaw(mcp, "clear_queue", new { zone = "nowhere" })),
            StringComparison.Ordinal);
        await Call(mcp, "delete_zone", new { zone = "living" });
        await Refused(HttpMethod.Get, "zones/living/queue", HttpStatusCode.NotFound, "notFound");

        // Every call is on the stream, with no arguments.
        var called = JsonNode.Parse((await stream.NextAsync(e => e.Type == "ai.toolCalled" && e.Data.Contains("\"delete_zone\"", StringComparison.Ordinal))).Data)!;
        Assert.Equal(("all", "delete_zone", true), (called["caller"]!.GetValue<string>(), called["tool"]!.GetValue<string>(),
            called["ok"]!.GetValue<bool>()));
        Assert.Equal(ToolCalledFields, called.AsObject().Select(p => p.Key));
    }
}
