using System.Diagnostics;
using System.Net;
using System.Net.Http.Headers;
using System.Net.Sockets;
using System.Text;
using System.Text.Json.Nodes;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Multiroom.Server.Abstractions;
using Multiroom.Server.Ai;
using Multiroom.Server.Ai.Providers;
using Multiroom.Server.Ai.Providers.Gemini;
using Multiroom.Server.Library;
using Multiroom.Server.Providers;
using Multiroom.Server.Providers.AddIns;
using Multiroom.Server.Providers.Tidal;
using Multiroom.Server.Web.Api;
using Multiroom.Server.Web.Gui;

namespace Multiroom.Server.Tests;

// The outward API on the server as it is composed (ServerHost), on port 0,
// with CoreLink on FakeCore, the fake provider, one provider that never
// answers a search, and a real library database. Every endpoint is asked
// once as it should be and once as it should not.
public sealed partial class ApiTests : IAsyncDisposable
{
    private const string Reason = "the device plays at most 48 kHz; the source is 96 kHz, resampled";
    private static readonly TimeSpan Patience = TimeSpan.FromSeconds(10);

    private readonly TempDirectory _dir = new();
    private readonly FakeCore _core = new();
    private readonly FakeProvider _fake = new();
    private IHost? _host;
    private HttpClient? _http;

    public ApiTests()
    {
        _core.StatusBody = $$"""
            {"server":{"version":"fake","uptimeMs":1,"zones":1,"clients":1,"connected":1,"members":1},
             "clients":[{"clientId":"roomA","connected":true,"member":true,"zone":"living","device":"USB DAC",
               "settings":{"audioControl":"bitPerfectFallback","driftCorrection":true,"audioControlIsDefault":true,"driftCorrectionIsDefault":true,"volume":0.8,"muted":false},
               "driftCorrection":{"requested":true,"active":true},
               "capabilities":{"formats":[{"sampleRate":48000,"channels":2,"sampleType":"s32"}]},
               "output":{"state":"playing","sourceFormat":{"sampleRate":96000,"channels":2,"sampleType":"s24"},
                 "format":{"sampleRate":48000,"channels":2,"sampleType":"s32"},"bitPerfect":false,
                 "reason":"{{Reason}}","plan":"resample 96000 -> 48000","ratio":0.5,"packets":10,"queuedPackets":2,
                 "bufferedMs":120,"connectionDrops":0},
               "sync":{"correctionPpm":1.5,"appliedPpm":1.4,"driftPpm":-3.25,"confidence":0.9,"offsetUs":12.5},
               "playback":{"bufferedMs":110,"underruns":2,"ageMs":40},"rejoins":1,
               "bufferMs":1500,
               "audioDevices":{"configured":"","selected":"usb","devices":[
                 {"id":"onboard","name":"Onboard","isDefault":true},{"id":"usb","name":"USB DAC","isDefault":false} ]} }],
             "zones":[{"zone":"living","state":"idle","transport":"stopped","staged":[],"failedSources":[],
               "members":["roomA"],"clients":["roomA"],"startsSent":0,"rejoins":0}]}
            """;
    }

    private static CancellationToken Ct => TestContext.Current.CancellationToken;

    private HttpClient Http => _http!;

    public async ValueTask DisposeAsync()
    {
        _http?.Dispose();
        if (_host is not null)
        {
            await _host.StopAsync(CancellationToken.None);
            _host.Dispose();
        }
        await _core.DisposeAsync();
        _dir.Dispose();
    }

    // --- status, routes, token ------------------------------------------------

    [Fact]
    public async Task Status_answers_the_core_the_media_endpoint_the_indexer_the_providers_and_the_event_stream()
    {
        await StartAsync();

        var status = await Ok(HttpMethod.Get, "status");

        Assert.False(string.IsNullOrEmpty(status["version"]!.GetValue<string>()));
        Assert.True(status["core"]!["connected"]!.GetValue<bool>());
        Assert.Equal("fake", status["core"]!["version"]!.GetValue<string>());
        Assert.Equal(1, status["core"]!["rooms"]!.GetValue<int>());
        var baseUri = status["media"]!["baseUri"]!.GetValue<string>();
        Assert.StartsWith("http://127.0.0.1:", baseUri, StringComparison.Ordinal);
        // The self-check: the media endpoint answered its probe, and the
        // database is at the latest schema.
        Assert.Equal($"reachable at {baseUri}", status["media"]!["reachability"]!.GetValue<string>());
        Assert.Equal(28, status["library"]!["schemaVersion"]!.GetValue<int>());
        Assert.NotNull(status["library"]!["indexer"]!["queueDepth"]);
        Assert.Equal(["local", "webradio", "tidal", "fake", "slow"],
            status["providers"]!.AsArray().Select(p => p!["id"]!.GetValue<string>()));
        var addIns = status["addIns"]!["registered"]!.AsArray();
        Assert.Equal(["library", "artwork", SleepyAddIn.AddInId], addIns.Select(a => a!["id"]!.GetValue<string>()));
        Assert.Equal(("ready", "recommendation", 5000), (addIns[0]!["state"]!.GetValue<string>(),
            addIns[0]!["kinds"]![0]!.GetValue<string>(), addIns[0]!["timeoutMs"]!.GetValue<int>()));
        Assert.Equal(4, status["addIns"]!["maxInFlight"]!.GetValue<int>());
        Assert.Equal(["artwork"], status["addIns"]!["enrichment"]!.AsArray().Select(e => e!["addIn"]!.GetValue<string>()));
        Assert.Equal(0, status["events"]!["subscribers"]!.GetValue<int>());
    }

    [Fact]
    public async Task Media_unknown_routes_and_wrong_methods_answer_the_error_shape_and_never_route_across()
    {
        await StartAsync();

        await Refused(HttpMethod.Get, "/media/abc", HttpStatusCode.NotFound, "notFound", "no route GET /media/abc");
        await Refused(HttpMethod.Get, "nothing", HttpStatusCode.NotFound, "notFound");
        await Refused(HttpMethod.Delete, "status", HttpStatusCode.MethodNotAllowed, "methodNotAllowed");
    }

    [Fact]
    public async Task A_token_is_required_when_set_and_never_logged()
    {
        const string token = "api-t0ken-never-logged";
        await StartAsync($$""", "token": "{{token}}" """);

        await Refused(HttpMethod.Get, "status", HttpStatusCode.Unauthorized, "unauthorized");
        await Refused(HttpMethod.Get, "/media/x", HttpStatusCode.Unauthorized, "unauthorized");
        Http.DefaultRequestHeaders.Authorization = new AuthenticationHeaderValue("Bearer", "wrong");
        await Refused(HttpMethod.Get, "status", HttpStatusCode.Unauthorized, "unauthorized");
        Http.DefaultRequestHeaders.Authorization = new AuthenticationHeaderValue("Bearer", token);
        await Ok(HttpMethod.Get, "status");

        Assert.DoesNotContain(token, await ReadLog(), StringComparison.Ordinal);
    }

    [Fact]
    public async Task The_gui_serves_its_page_and_forwards_the_api_with_the_token_the_browser_never_sees()
    {
        const string token = "api-t0ken-for-the-gui";
        await StartAsync($$""", "token": "{{token}}" """);
        using var gui = new HttpClient(new SocketsHttpHandler { UseProxy = false })
        {
            BaseAddress = _host!.Services.GetRequiredService<GuiEndpoint>().BaseUri,
        };

        var page = await gui.GetAsync(new Uri("/", UriKind.Relative), Ct);
        Assert.Equal(HttpStatusCode.OK, page.StatusCode);
        Assert.Contains("<title>Galdrun Server</title>", await page.Content.ReadAsStringAsync(Ct), StringComparison.Ordinal);
        Assert.Equal(HttpStatusCode.OK, (await gui.GetAsync(new Uri("/assets/theme.css", UriKind.Relative), Ct)).StatusCode);
        Assert.Equal(HttpStatusCode.NotFound, (await gui.GetAsync(new Uri("/legacy/index.html", UriKind.Relative), Ct)).StatusCode);

        var status = await gui.GetAsync(new Uri("/api/v1/status", UriKind.Relative), Ct);
        Assert.Equal(HttpStatusCode.OK, status.StatusCode);
        Assert.NotNull(JsonNode.Parse(await status.Content.ReadAsStringAsync(Ct))!["version"]);
        // A body goes through as it came, and the API's refusal comes back as it answered.
        using var body = new StringContent("{\"name\":\"*\"}", Encoding.UTF8, "application/json");
        var refused = await gui.PostAsync(new Uri("/api/v1/users", UriKind.Relative), body, Ct);
        Assert.Equal(HttpStatusCode.BadRequest, refused.StatusCode);

        var config = JsonNode.Parse(await gui.GetStringAsync(new Uri("/gui/config", UriKind.Relative), Ct))!;
        Assert.True(config["token"]!.GetValue<bool>());
        Assert.Equal("server", config["logs"]![0]!.GetValue<string>());
        Assert.DoesNotContain(token, config.ToJsonString(), StringComparison.Ordinal);

        // The page's own settings: {} until one is put, then kept whole.
        Assert.Equal("{}", await gui.GetStringAsync(new Uri("/gui/settings", UriKind.Relative), Ct));
        using var settings = new StringContent("{\"zoneBackground\":\"media\"}", Encoding.UTF8, "application/json");
        Assert.Equal(HttpStatusCode.OK, (await gui.PutAsync(new Uri("/gui/settings", UriKind.Relative), settings, Ct)).StatusCode);
        var kept = JsonNode.Parse(await gui.GetStringAsync(new Uri("/gui/settings", UriKind.Relative), Ct))!;
        Assert.Equal("media", kept["zoneBackground"]!.GetValue<string>());
        using var notAnObject = new StringContent("[1]", Encoding.UTF8, "application/json");
        Assert.Equal(HttpStatusCode.BadRequest, (await gui.PutAsync(new Uri("/gui/settings", UriKind.Relative), notAnObject, Ct)).StatusCode);
        Assert.Equal(HttpStatusCode.MethodNotAllowed, (await gui.DeleteAsync(new Uri("/gui/settings", UriKind.Relative), Ct)).StatusCode);
    }

    // --- providers and search -------------------------------------------------

    [Fact]
    public async Task Providers_are_listed_with_their_capabilities_and_browsed_searched_and_asked_for_artwork()
    {
        await StartAsync();
        var track = _fake.AddTrack(_dir.Write("a.flac", "a"), "Blue in Green");

        var fake = (await Ok(HttpMethod.Get, "providers")).AsArray().Single(p => p!["id"]!.GetValue<string>() == "fake")!;
        Assert.Equal("ready", fake["state"]!.GetValue<string>());
        Assert.Equal(["search", "browse", "stream", "liveStream", "artwork"],
            fake["capabilities"]!.AsArray().Select(c => c!.GetValue<string>()));

        var root = await Ok(HttpMethod.Get, "providers/fake/browse");
        Assert.Equal(["tracks", "stations"], root["nodes"]!.AsArray().Select(n => n!["nodeId"]!.GetValue<string>()));
        var tracks = await Ok(HttpMethod.Get, "providers/fake/browse?node=tracks&offset=0&limit=5");
        Assert.Equal(track.ProviderItemId, tracks["items"]!["tracks"]![0]!["providerItemId"]!.GetValue<string>());
        var found = await Ok(HttpMethod.Get, "providers/fake/search?q=blue");
        Assert.Equal("Blue in Green", found["tracks"]![0]!["title"]!.GetValue<string>());

        using (var art = await Http.GetAsync("providers/fake/artwork?reference=cover", Ct))
        {
            Assert.Equal(HttpStatusCode.OK, art.StatusCode);
            Assert.Equal("image/png", art.Content.Headers.ContentType?.MediaType);
            Assert.Equal(FakeProvider.ArtworkBytes, await art.Content.ReadAsByteArrayAsync(Ct));
        }

        await Refused(HttpMethod.Get, "providers/fake/browse?node=nope", HttpStatusCode.NotFound, "notFound");
        await Refused(HttpMethod.Get, "providers/nobody/browse", HttpStatusCode.NotFound, "notFound", "no provider nobody");
        await Refused(HttpMethod.Get, "providers/slow/browse", HttpStatusCode.ServiceUnavailable, "unavailable");
        await Refused(HttpMethod.Get, "providers/fake/browse?limit=0", HttpStatusCode.BadRequest, "invalidRequest",
            "limit: 1 to 100, was 0");
        await Refused(HttpMethod.Get, "providers/fake/browse?nod=x", HttpStatusCode.BadRequest, "invalidRequest",
            "unknown query parameter nod");
        await Refused(HttpMethod.Get, "providers/fake/search", HttpStatusCode.BadRequest, "invalidRequest", "q is required");
        await Refused(HttpMethod.Get, "providers/fake/artwork", HttpStatusCode.BadRequest, "invalidRequest");
        await Refused(HttpMethod.Get, "providers/local/artwork?reference=x", HttpStatusCode.NotFound, "notFound");
        await Refused(HttpMethod.Get, "providers/slow/artwork?reference=x", HttpStatusCode.ServiceUnavailable, "unavailable");
    }

    [Fact]
    public async Task Unified_search_answers_while_one_provider_is_slow_with_its_results_absent_and_the_timeout_named()
    {
        await StartAsync();
        _fake.AddTrack(_dir.Write("a.flac", "a"), "Blue in Green");
        _fake.AddStation("Blue FM", [1, 2, 3]);

        var clock = Stopwatch.StartNew();
        var result = await Ok(HttpMethod.Get, "search?q=blue&limit=10");
        clock.Stop();

        Assert.True(clock.Elapsed < TimeSpan.FromSeconds(3), $"answered in {clock.Elapsed}");
        Assert.Equal("blue", result["query"]!.GetValue<string>());
        Assert.Equal("fake", result["items"]!["tracks"]![0]!["providerId"]!.GetValue<string>());
        Assert.Equal("Blue FM", result["items"]!["stations"]![0]!["name"]!.GetValue<string>());
        Assert.Equal(["local", "webradio", "fake"],
            result["sources"]!.AsArray().Select(s => s!["provider"]!.GetValue<string>()));
        var fake = result["sources"]!.AsArray().Single(s => s!["provider"]!.GetValue<string>() == "fake")!;
        Assert.Equal((1, 1), (fake["tracks"]!.GetValue<int>(), fake["stations"]!.GetValue<int>()));
        var failures = result["failures"]!.AsArray();
        // Tidal is registered without a client id, and says so.
        Assert.Equal(["tidal", "slow"], failures.Select(f => f!["provider"]!.GetValue<string>()));
        Assert.Equal("unavailable", failures[0]!["error"]!.GetValue<string>());
        var failure = failures[1]!;
        Assert.Equal("timeout", failure["error"]!.GetValue<string>());
        Assert.Equal("no answer within 300 ms", failure["message"]!.GetValue<string>());

        await Refused(HttpMethod.Get, "search?q=%20", HttpStatusCode.BadRequest, "invalidRequest", "q is required");
        await Refused(HttpMethod.Get, "search?q=x&limit=101", HttpStatusCode.BadRequest, "invalidRequest");

        // types narrows every provider's answer to the kinds asked for, in any case.
        var stations = await Ok(HttpMethod.Get, "search?q=blue&types=STATIONS");
        Assert.Empty(stations["items"]!["tracks"]!.AsArray());
        Assert.Equal("Blue FM", stations["items"]!["stations"]![0]!["name"]!.GetValue<string>());
        var tracks = await Ok(HttpMethod.Get, "search?q=blue&types=tracks,playlists");
        Assert.Equal("Blue in Green", tracks["items"]!["tracks"]![0]!["title"]!.GetValue<string>());
        Assert.Empty(tracks["items"]!["stations"]!.AsArray());
        Assert.Empty(tracks["items"]!["playlists"]!.AsArray());
        Assert.Single((await Ok(HttpMethod.Get, "search?q=blue&types=all"))["items"]!["stations"]!.AsArray());
        await Refused(HttpMethod.Get, "search?q=x&types=videos", HttpStatusCode.BadRequest, "invalidRequest",
            "types: tracks, albums, artists, playlists, stations or all, was videos");
        await Refused(HttpMethod.Get, "providers/fake/search?q=x&types=1", HttpStatusCode.BadRequest, "invalidRequest");
    }

    // The apps are the config file's: no route sets or answers them.
    [Fact]
    public async Task The_tidal_client_has_no_route()
    {
        await StartAsync();

        foreach (var method in new[] { HttpMethod.Get, HttpMethod.Put, HttpMethod.Delete })
            await Refused(method, "providers/tidal/client", HttpStatusCode.NotFound, "notFound");
    }

    [Fact]
    public async Task The_server_is_named_and_the_server_and_a_room_keep_an_image()
    {
        await StartAsync();
        using var stream = await EventsAsync(null);
        async Task<JsonNode> Next() => JsonNode.Parse((await stream.NextAsync(e =>
            e.Type is "server.changed" or "room.imageChanged")).Data)!;

        var server = await Ok(HttpMethod.Get, "server");
        Assert.Matches("^GALDRUN-SERVER-[0-9]{4}$", server["name"]!.GetValue<string>());
        Assert.False(server["hasImage"]!.GetValue<bool>());
        Assert.Equal("Living room rack", (await Ok(HttpMethod.Patch, "server", """{"name":" Living room rack "}"""))["name"]!
            .GetValue<string>());
        Assert.Equal("Living room rack", (await Next())["name"]!.GetValue<string>());
        await Refused(HttpMethod.Patch, "server", """{"name":"  "}""", HttpStatusCode.BadRequest, "invalidArgument");
        await Refused(HttpMethod.Patch, "server", "{}", HttpStatusCode.BadRequest, "invalidRequest");

        byte[] png = [0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 1, 2, 3];
        byte[] jpeg = [0xFF, 0xD8, 0xFF, 0xE0, 4, 5];
        foreach (var (path, image, type) in new[] { ("server/image", png, "image/png"), ("rooms/roomA/image", jpeg, "image/jpeg") })
        {
            await Refused(HttpMethod.Get, path, HttpStatusCode.NotFound, "notFound");
            Assert.Equal(HttpStatusCode.OK, (await SendBytes(HttpMethod.Put, path, image)).StatusCode);
            Assert.True((await Next())["hasImage"]!.GetValue<bool>());
            using var got = await Http.GetAsync(path, Ct);
            Assert.Equal(type, got.Content.Headers.ContentType!.MediaType);
            Assert.Equal(image, await got.Content.ReadAsByteArrayAsync(Ct));
        }
        Assert.True((await Ok(HttpMethod.Get, "server"))["hasImage"]!.GetValue<bool>());
        await Refused(HttpMethod.Get, "rooms/roomB/image", HttpStatusCode.NotFound, "notFound");

        Assert.Equal(HttpStatusCode.BadRequest, (await SendBytes(HttpMethod.Put, "server/image", "not an image"u8.ToArray())).StatusCode);
        Assert.Equal(HttpStatusCode.BadRequest, (await SendBytes(HttpMethod.Put, "server/image", [])).StatusCode);
        Assert.Equal(HttpStatusCode.BadRequest,
            (await SendBytes(HttpMethod.Put, "server/image", [.. png, .. new byte[ServerProfileStore.MaxImageBytes]])).StatusCode);

        Assert.Equal(HttpStatusCode.NoContent, (await SendBytes(HttpMethod.Delete, "rooms/roomA/image", null)).StatusCode);
        var room = await Next();
        Assert.Equal(("roomA", false), (room["client"]!.GetValue<string>(), room["hasImage"]!.GetValue<bool>()));
        await Refused(HttpMethod.Get, "rooms/roomA/image", HttpStatusCode.NotFound, "notFound");
        Assert.Equal(png, await Http.GetByteArrayAsync("server/image", Ct));

        // Refused changes and removing what is not there publish nothing: the next event is the server's removal.
        Assert.Equal(HttpStatusCode.NoContent, (await SendBytes(HttpMethod.Delete, "rooms/roomA/image", null)).StatusCode);
        Assert.Equal(HttpStatusCode.NoContent, (await SendBytes(HttpMethod.Delete, "server/image", null)).StatusCode);
        var removed = await Next();
        Assert.Equal(("Living room rack", false), (removed["name"]!.GetValue<string>(), removed["hasImage"]!.GetValue<bool>()));
    }

    private async Task<HttpResponseMessage> SendBytes(HttpMethod method, string path, byte[]? bytes)
    {
        using var request = new HttpRequestMessage(method, path);
        if (bytes is not null)
            request.Content = new ByteArrayContent(bytes);
        return await Http.SendAsync(request, Ct);
    }

    // --- zones, transport, queue ----------------------------------------------

    [Fact]
    public async Task A_queue_is_filled_played_edited_and_its_zone_answers_both_halves()
    {
        await StartAsync();
        Track[] tracks = [.. "ABC".Select(c => c.ToString()).Select(t => _fake.AddTrack(_dir.Write($"{t}.flac", t), t))];

        var added = await Ok(HttpMethod.Post, "zones/living/queue/items", $$"""{"tracks":[{{Json(tracks[0])}},{{Json(tracks[1])}}]}""");
        Assert.Equal(["A", "B"], Titles(added));
        added = await Ok(HttpMethod.Post, "zones/living/queue/items", $$"""{"tracks":[{{Json(tracks[2])}}],"index":0}""");
        Assert.Equal(["C", "A", "B"], Titles(added));
        var ids = added["items"]!.AsArray().Select(i => i!["id"]!.GetValue<long>()).ToArray();

        await Ok(HttpMethod.Put, "zones/living/clients", """{"clients":["roomA"]}""");
        var playing = await Ok(HttpMethod.Post, "zones/living/play", $$"""{"item":{{ids[1]}}}""");
        Assert.Equal(ids[1], playing["current"]!.GetValue<long>());
        Assert.Equal("starting", playing["transport"]!.GetValue<string>());
        Assert.Contains(_core.All, r => r.Command == "source.stage" && r.Params.Contains("\"title\":\"A\"", StringComparison.Ordinal));
        Assert.Equal("zone.play", _core.All[^1].Command);

        var zone = await Ok(HttpMethod.Get, "zones/living");
        Assert.Equal(["roomA"], zone["desired"]!["clients"]!.AsArray().Select(c => c!.GetValue<string>()));
        Assert.Equal("playing", zone["actual"]!["state"]!.GetValue<string>());
        Assert.Equal(3, zone["queue"]!["items"]!.AsArray().Count);
        Assert.Equal(["living"], (await Ok(HttpMethod.Get, "zones")).AsArray().Select(z => z!["zone"]!.GetValue<string>()));

        var patched = await Ok(HttpMethod.Patch, "zones/living/queue", """{"repeat":"all","shuffle":true}""");
        Assert.Equal(("all", true), (patched["repeat"]!.GetValue<string>(), patched["shuffle"]!.GetValue<bool>()));
        Assert.Equal(["A", "C", "B"], Titles(await Ok(HttpMethod.Patch, $"zones/living/queue/items/{ids[0]}", """{"index":1}""")));
        Assert.Equal(["A", "B"], Titles(await Ok(HttpMethod.Delete, $"zones/living/queue/items/{ids[0]}")));

        await Ok(HttpMethod.Post, "zones/living/pause");
        Assert.Equal("zone.pause", _core.All[^1].Command);
        await Ok(HttpMethod.Post, "zones/living/seek", $$"""{"item":{{ids[2]}},"positionMs":30500}""");
        Assert.Contains(_core.All, r => r.Command == "source.stage" && r.Params.Contains("\"startOffsetNs\":30500000000", StringComparison.Ordinal));
        await Ok(HttpMethod.Post, "zones/living/next");
        var stopped = await Ok(HttpMethod.Post, "zones/living/stop");
        Assert.Contains(_core.All.TakeLast(2), r => r.Command == "zone.stop");
        Assert.Equal("living", stopped["zone"]!.GetValue<string>());
        Assert.Equal("living", (await Ok(HttpMethod.Get, "zones/living/queue"))["zone"]!.GetValue<string>());

        var deleted = await Http.DeleteAsync("zones/living", Ct);
        Assert.Equal(HttpStatusCode.NoContent, deleted.StatusCode);
        Assert.Contains(_core.All, r => r.Command == "zone.delete" && r.Params.Contains("\"living\"", StringComparison.Ordinal));
        await Refused(HttpMethod.Get, "zones/living/queue", HttpStatusCode.NotFound, "notFound");
        // Its room is not left without a zone.
        var own = Assert.Single((await Ok(HttpMethod.Get, "zones")).AsArray());
        Assert.Equal("roomA", own!["name"]!.GetValue<string>());
        Assert.Equal(["roomA"], own["desired"]!["clients"]!.AsArray().Select(c => c!.GetValue<string>()));
    }

    [Fact]
    public async Task A_queue_is_saved_as_a_playlist_and_a_name_in_use_is_overwritten_only_when_asked()
    {
        await StartAsync();
        Track[] tracks = [.. "AB".Select(c => c.ToString()).Select(t => _fake.AddTrack(_dir.Write($"{t}.flac", t), t))];
        var station = _fake.AddStation("Radio", [1]);
        await Ok(HttpMethod.Post, "zones/living/queue/items", $$"""{"tracks":[{{Json(tracks[0])}},{{Json(tracks[1])}}]}""");
        var queued = await Ok(HttpMethod.Post, "zones/living/queue/items", $$"""{"station":{{Json(station)}}}""");

        var saved = await Ok(HttpMethod.Post, "zones/living/queue/save", """{"name":" Evening "}""");
        Assert.Equal(("Evening", 2, 1), (saved["playlist"]!["name"]!.GetValue<string>(),
            saved["playlist"]!["count"]!.GetValue<int>(), saved["skipped"]!.GetValue<int>()));
        var id = saved["playlist"]!["id"]!.GetValue<long>();
        Assert.Equal(["A", "B"], (await Ok(HttpMethod.Get, $"playlists/{id}/entries")).AsArray()
            .Select(e => e!["item"]!["title"]!.GetValue<string>()));

        await Refused(HttpMethod.Post, "zones/living/queue/save", """{"name":"evening"}""", HttpStatusCode.Conflict,
            "alreadyExists", "a playlist named evening is already there");
        await Ok(HttpMethod.Delete, $"zones/living/queue/items/{queued["items"]![0]!["id"]!.GetValue<long>()}");
        var overwritten = await Ok(HttpMethod.Post, "zones/living/queue/save", """{"name":"evening","overwrite":true}""");
        Assert.Equal((id, 1), (overwritten["playlist"]!["id"]!.GetValue<long>(), overwritten["playlist"]!["count"]!.GetValue<int>()));
        Assert.Single((await Ok(HttpMethod.Get, "playlists")).AsArray());

        // Names are per folder: the same one in a folder is a new playlist there.
        var music = (await Ok(HttpMethod.Post, "playlist-folders", """{"name":"Music"}"""))["id"]!.GetValue<long>();
        var inFolder = await Ok(HttpMethod.Post, "zones/living/queue/save", $$"""{"name":"Evening","folder":{{music}}}""");
        Assert.NotEqual(id, inFolder["playlist"]!["id"]!.GetValue<long>());
        Assert.Equal(music, inFolder["playlist"]!["folder"]!.GetValue<long>());
        await Refused(HttpMethod.Post, "zones/living/queue/save", $$"""{"name":"evening","folder":{{music}}}""",
            HttpStatusCode.Conflict, "alreadyExists");
        await Refused(HttpMethod.Post, "zones/living/queue/save", """{"name":"x","folder":999}""", HttpStatusCode.NotFound, "notFound");

        await Refused(HttpMethod.Post, "zones/living/queue/save", """{"name":" "}""", HttpStatusCode.BadRequest, "invalidArgument");
        await Refused(HttpMethod.Post, "zones/nowhere/queue/save", """{"name":"x"}""", HttpStatusCode.NotFound, "notFound");
    }

    [Fact]
    public async Task Every_room_has_a_zone_named_for_its_rooms_and_an_emptied_zone_goes()
    {
        await StartAsync();
        await Refused(HttpMethod.Put, "zones/nowhere/clients", """{"clients":["roomA"]}""", HttpStatusCode.NotFound,
            "notFound", "no zone nowhere");

        // The core's own zone holds roomA, so it keeps it; roomB gets one of its own.
        await _core.EmitAsync("""{"event":"room.connected","client":"roomB","deviceName":"Speaker","reconnect":false}""");
        string? ownZone = null;
        await Until(async () =>
        {
            ownZone = (await Ok(HttpMethod.Get, "zones")).AsArray()
                .FirstOrDefault(z => z!["name"]!.GetValue<string>() == "roomB")?["zone"]!.GetValue<string>();
            return ownZone is not null;
        });
        Assert.StartsWith("zone-", ownZone, StringComparison.Ordinal);

        // Moved into living: its own zone is empty and goes; living is named for both.
        await Ok(HttpMethod.Put, "zones/living/clients", """{"clients":["roomA","roomB"]}""");
        var zones = (await Ok(HttpMethod.Get, "zones")).AsArray();
        Assert.Equal([("living", "roomA + roomB")],
            zones.Select(z => (z!["zone"]!.GetValue<string>(), z["name"]!.GetValue<string>())));
        Assert.Contains(_core.All, r => r.Command == "zone.delete" && r.Params.Contains(ownZone!, StringComparison.Ordinal));

        // Taken out again: a new zone of its own at living's level, and living
        // is named for roomA alone. roomB's level never changed, so nothing
        // was sent to it.
        await Ok(HttpMethod.Patch, "zones/living", """{"volume":0.4,"muted":true}""");
        var sent = _core.All.Count;
        await Ok(HttpMethod.Put, "zones/living/clients", """{"clients":["roomA"]}""");
        zones = (await Ok(HttpMethod.Get, "zones")).AsArray();
        Assert.Equal(["roomA", "roomB"], zones.Select(z => z!["name"]!.GetValue<string>()).Order(StringComparer.Ordinal));
        Assert.DoesNotContain(zones, z => z!["zone"]!.GetValue<string>() == ownZone);
        var taken = zones.Single(z => z!["name"]!.GetValue<string>() == "roomB")!["desired"]!;
        Assert.Equal((0.4, true), (taken["volume"]!.GetValue<double>(), taken["muted"]!.GetValue<bool>()));
        Assert.DoesNotContain(_core.All.Skip(sent), r => r.Command == "client.setVolume" && r.Params.Contains("roomB", StringComparison.Ordinal));
    }

    [Fact]
    public async Task Zone_and_queue_requests_are_refused_by_name_and_code()
    {
        await StartAsync();
        var track = _fake.AddTrack(_dir.Write("a.flac", "a"), "A");
        var station = _fake.AddStation("Radio", [1]);
        var queued = await Ok(HttpMethod.Post, "zones/living/queue/items", $$"""{"station":{{Json(station)}}}""");
        var radio = queued["items"]![0]!["id"]!.GetValue<long>();

        await Refused(HttpMethod.Get, "zones/nowhere", HttpStatusCode.NotFound, "notFound", "no zone nowhere");
        await Refused(HttpMethod.Post, "zones/nowhere/play", HttpStatusCode.NotFound, "notFound");
        await Refused(HttpMethod.Delete, "zones/nowhere", HttpStatusCode.NotFound, "notFound");
        await Refused(HttpMethod.Delete, "zones/living/queue/items/999", HttpStatusCode.NotFound, "notFound");
        await Refused(HttpMethod.Delete, "zones/living/queue/items/x", HttpStatusCode.NotFound, "notFound", "no item x");
        await Refused(HttpMethod.Post, "zones/living/queue/items", "", HttpStatusCode.BadRequest, "invalidRequest",
            "this request needs a JSON body");
        await Refused(HttpMethod.Post, "zones/living/queue/items", "{nope", HttpStatusCode.BadRequest, "invalidRequest");
        await Refused(HttpMethod.Post, "zones/living/queue/items", "[]", HttpStatusCode.BadRequest, "invalidRequest");
        await Refused(HttpMethod.Post, "zones/living/queue/items",
            """{"tracks":[{"providerId":"fake","providerItemId":"t0","titel":"A"}]}""",
            HttpStatusCode.BadRequest, "invalidRequest", "unknown field tracks.0.titel");
        await Refused(HttpMethod.Post, "zones/living/queue/items",
            """{"tracks":[{"providerId":"fake","providerItemId":"t0"}]}""",
            HttpStatusCode.BadRequest, "invalidRequest", "field tracks.0.title is required");
        await Refused(HttpMethod.Post, "zones/living/queue/items", """{"tracks":"A"}""",
            HttpStatusCode.BadRequest, "invalidRequest", "field tracks has a value of the wrong kind");
        await Refused(HttpMethod.Post, "zones/living/queue/items", $$"""{"tracks":[{{Json(track)}}],"station":{{Json(station)}}}""",
            HttpStatusCode.BadRequest, "invalidRequest", "exactly one of tracks, station, playlist, favorite or node");
        await Refused(HttpMethod.Post, "zones/living/queue/items", $$"""{"tracks":[{{Json(track)}}],"index":9}""",
            HttpStatusCode.BadRequest, "invalidArgument");
        await Refused(HttpMethod.Post, "zones/living/queue/items", """{"tracks":[]}""",
            HttpStatusCode.BadRequest, "invalidRequest", "tracks is empty");
        await Refused(HttpMethod.Patch, "zones/living/queue", "{}", HttpStatusCode.BadRequest, "invalidArgument");
        await Refused(HttpMethod.Patch, "zones/living/queue", """{"repeat":"sometimes"}""", HttpStatusCode.BadRequest,
            "invalidRequest", "field repeat has a value of the wrong kind");
        await Refused(HttpMethod.Patch, $"zones/living/queue/items/{radio}", """{"index":5}""", HttpStatusCode.BadRequest,
            "invalidArgument");
        await Refused(HttpMethod.Post, "zones/living/pause", "{}", HttpStatusCode.BadRequest, "invalidRequest",
            "/api/v1/zones/living/pause takes no body");
        await Refused(HttpMethod.Post, "zones/living/next", HttpStatusCode.NotFound, "notFound");
        await Refused(HttpMethod.Post, "zones/living/seek", $$"""{"item":{{radio}},"positionMs":-1}""",
            HttpStatusCode.BadRequest, "invalidArgument");
        await Refused(HttpMethod.Post, "zones/living/seek", $$"""{"item":{{radio}},"positionMs":1000}""",
            HttpStatusCode.ServiceUnavailable, "unavailable");
        await Refused(HttpMethod.Put, "zones/living/clients", """{"rooms":["roomA"]}""", HttpStatusCode.BadRequest,
            "invalidRequest", "unknown field rooms");
        _core.Refuse("zone.pause", "unavailable", "zone living is not playing");
        await Refused(HttpMethod.Post, "zones/living/pause", HttpStatusCode.ServiceUnavailable, "unavailable",
            "zone.pause: Unavailable: zone living is not playing");
    }

    // --- rooms ----------------------------------------------------------------

    [Fact]
    public async Task A_room_patch_sends_the_volume_control_after_the_audio_control_and_refuses_old_names()
    {
        await StartAsync();

        await Ok(HttpMethod.Patch, "rooms/roomA", """{"volumeControl":"software","audioControl":"auto"}""");
        Assert.Equal(
            [
                ("client.setAudioControl", """{"client":"roomA","audioControl":"auto"}"""),
                ("client.setVolumeControl", """{"client":"roomA","volumeControl":"software"}"""),
            ],
            _core.All.Skip(_core.All.Count - 2).Select(r => (r.Command, r.Params)));

        await Refused(HttpMethod.Patch, "rooms/roomA", """{"volumeControl":"loud"}""", HttpStatusCode.BadRequest, "invalidRequest");
        await Refused(HttpMethod.Patch, "rooms/roomA", """{"audioControl":"force"}""", HttpStatusCode.BadRequest, "invalidRequest");
    }

    [Fact]
    public async Task A_room_patch_sends_the_drift_correction_and_refuses_an_unknown_actuator()
    {
        await StartAsync();

        await Ok(HttpMethod.Patch, "rooms/roomA", """{"driftCorrection":"hardwareClock"}""");
        Assert.Equal(
            ("client.setDriftCorrection", """{"client":"roomA","driftCorrection":"hardwareClock"}"""),
            _core.All.Select(r => (r.Command, r.Params)).Last());

        await Refused(HttpMethod.Patch, "rooms/roomA", """{"driftCorrection":"pll"}""", HttpStatusCode.BadRequest, "invalidRequest");
    }

    [Fact]
    public async Task A_room_answers_both_halves_with_the_bitperfect_reason_verbatim_from_the_core()
    {
        await StartAsync();

        var patched = await Ok(HttpMethod.Patch, "rooms/roomA",
            """{"audioControl":"bitPerfect","outputLatencyMs":20,"volume":0.5,"muted":true}""");
        Assert.Equal(
            [
                ("client.setAudioControl", """{"client":"roomA","audioControl":"bitPerfect"}"""),
                ("client.setOutputLatency", """{"client":"roomA","latencyMs":20}"""),
                ("client.setVolume", """{"client":"roomA","value":0.5,"muted":true}"""),
            ],
            _core.All.Skip(_core.All.Count - 3).Select(r => (r.Command, r.Params)));
        Assert.Equal("bitPerfect", patched["desired"]!["audioControl"]!.GetValue<string>());
        // The settings as the core stores them, its answers applied.
        Assert.Equal(0.5, patched["actual"]!["settings"]!["volume"]!.GetValue<double>());
        Assert.False(patched["actual"]!["settings"]!["audioControlIsDefault"]!.GetValue<bool>());

        var room = (await Ok(HttpMethod.Get, "rooms")).AsArray().Single()!;
        Assert.Equal("clients", _core.All[^1].Command);
        Assert.Equal("roomA", room["client"]!.GetValue<string>());
        var desired = room["desired"]!;
        Assert.Equal("bitPerfect", desired["audioControl"]!.GetValue<string>());
        Assert.Null(desired["driftCorrection"]);
        Assert.Equal(0.5, desired["volume"]!.GetValue<double>());
        Assert.True(desired["muted"]!.GetValue<bool>());
        Assert.Equal(20, desired["outputLatencyMs"]!.GetValue<int>());
        var actual = room["actual"]!;
        Assert.True(actual["connected"]!.GetValue<bool>());
        Assert.Equal("USB DAC", actual["device"]!.GetValue<string>());
        Assert.False(actual["plan"]!["bitPerfect"]!.GetValue<bool>());
        Assert.Equal(Reason, actual["plan"]!["reason"]!.GetValue<string>());
        Assert.Equal(48000, actual["plan"]!["outputFormat"]!["sampleRate"]!.GetValue<int>());
        Assert.Equal(-3.25, actual["sync"]!["driftPpm"]!.GetValue<double>());
        Assert.Equal(0.9, actual["sync"]!["confidence"]!.GetValue<double>());
        Assert.Equal(12.5, actual["sync"]!["offsetUs"]!.GetValue<double>());
        Assert.Equal(120, actual["bufferedMs"]!.GetValue<int>());
        Assert.Equal(2, actual["playback"]!["underruns"]!.GetValue<int>());
        Assert.Equal(40, actual["playback"]!["ageMs"]!.GetValue<int>());
        Assert.Equal(1, actual["rejoins"]!.GetValue<int>());
        // A read asks the core again: the fake's `clients` keeps its own volume.
        Assert.Equal(0.8, actual["settings"]!["volume"]!.GetValue<double>());
        Assert.Equal("usb", actual["audioDevices"]!["selected"]!.GetValue<string>());
        Assert.Equal(2, actual["audioDevices"]!["devices"]!.AsArray().Count);

        // The client keeps the device, so it is sent as asked and recorded nowhere here.
        var switched = await Ok(HttpMethod.Patch, "rooms/roomA", """{"audioDeviceId":"onboard"}""");
        Assert.Equal(("client.setAudioDevice", """{"client":"roomA","deviceId":"onboard"}"""),
            (_core.All[^1].Command, _core.All[^1].Params));
        Assert.Equal("onboard", switched["actual"]!["audioDevices"]!["configured"]!.GetValue<string>());
        Assert.Equal(1500, actual["bufferMs"]!.GetValue<int>());
        await Ok(HttpMethod.Patch, "rooms/roomA", """{"bufferMs":2500}""");
        Assert.Equal(("client.setBuffer", """{"client":"roomA","bufferMs":2500}"""),
            (_core.All[^1].Command, _core.All[^1].Params));
        Assert.Equal("roomA", (await Ok(HttpMethod.Post, "rooms/roomA/rescan"))["client"]!.GetValue<string>());
        Assert.Equal(("client.rescanAudio", """{"client":"roomA"}"""), (_core.All[^1].Command, _core.All[^1].Params));

        Assert.Equal("living", (await Ok(HttpMethod.Post, "rooms/roomA/rejoin"))["zone"]!.GetValue<string>());

        var discovered = await Ok(HttpMethod.Get, "clients/discovered");
        Assert.Equal("", discovered["clients"]![0]!["server"]!.GetValue<string>());
        Assert.False(discovered["clients"]![0]!["connected"]!.GetValue<bool>());
        Assert.Equal("10.0.0.2:4711", (await Ok(HttpMethod.Post, "clients/kitchen/attach"))["server"]!.GetValue<string>());
        Assert.Equal(("client.attach", """{"client":"kitchen"}"""), (_core.All[^1].Command, _core.All[^1].Params));
        await Ok(HttpMethod.Post, "clients/kitchen/attach", """{"server":"10.0.0.3:4711"}""");
        Assert.Equal(("client.attach", """{"client":"kitchen","server":"10.0.0.3:4711"}"""),
            (_core.All[^1].Command, _core.All[^1].Params));
        Assert.Equal("", (await Ok(HttpMethod.Post, "clients/kitchen/detach"))["server"]!.GetValue<string>());
        Assert.Equal(("client.detach", """{"client":"kitchen"}"""), (_core.All[^1].Command, _core.All[^1].Params));
        _core.Refuse("client.attach", "unavailable", "kitchen has not sent a beacon recently");
        await Refused(HttpMethod.Post, "clients/kitchen/attach", HttpStatusCode.ServiceUnavailable, "unavailable");
        Assert.Equal("soxr", (await Ok(HttpMethod.Put, "core/resampler", """{"resampler":"soxr"}"""))["resampler"]!.GetValue<string>());
        var audio = await Ok(HttpMethod.Get, "core/audio");
        Assert.Equal(1000, audio["audio"]!["startDelayMs"]!.GetValue<int>());
        Assert.Equal(64, audio["audio"]!["memoryLimitMb"]!.GetValue<int>());
        Assert.Equal((64, 8, 512, 0), (audio["memoryLimit"]!["mb"]!.GetValue<int>(),
            audio["memoryLimit"]!["minMb"]!.GetValue<int>(), audio["memoryLimit"]!["maxMb"]!.GetValue<int>(),
            audio["memoryLimit"]!["usedMb"]!.GetValue<int>()));
        var timed = await Ok(HttpMethod.Patch, "core/audio", """{"startDelayMs":500}""");
        Assert.Equal(("audio.configure", """{"startDelayMs":500}"""), (_core.All[^1].Command, _core.All[^1].Params));
        Assert.Equal(500, timed["audio"]!["startDelayMs"]!.GetValue<int>());
        await Ok(HttpMethod.Patch, "core/audio", """{"memoryLimitMb":128}""");
        Assert.Equal(("audio.configure", """{"memoryLimitMb":128}"""), (_core.All[^1].Command, _core.All[^1].Params));
        await Refused(HttpMethod.Patch, "core/audio", "{}", HttpStatusCode.BadRequest, "invalidArgument");
        _core.Refuse("audio.configure", "invalidArgument",
            "audio.memoryLimitMb 4 is below the least it may be (8)");
        await Refused(HttpMethod.Patch, "core/audio", """{"memoryLimitMb":4}""", HttpStatusCode.BadRequest,
            "invalidArgument", "audio.memoryLimitMb 4 is below the least it may be (8)");
        var asked = _core.All.Count;
        await Refused(HttpMethod.Patch, "core/audio", """{"memoryLimitMb":-4}""", HttpStatusCode.BadRequest,
            "invalidRequest", "field memoryLimitMb has a value of the wrong kind");
        await Refused(HttpMethod.Patch, "core/audio", """{"memoryLimitMb":"128"}""", HttpStatusCode.BadRequest,
            "invalidRequest", "field memoryLimitMb has a value of the wrong kind");
        Assert.Equal(asked, _core.All.Count);

        await Refused(HttpMethod.Get, "rooms/nobody", HttpStatusCode.NotFound, "notFound", "no room nobody");
        await Refused(HttpMethod.Patch, "rooms/roomA", "{}", HttpStatusCode.BadRequest, "invalidArgument");
        await Refused(HttpMethod.Patch, "rooms/roomA", """{"volume":1.5}""", HttpStatusCode.BadRequest, "invalidArgument",
            "volume 1.5 is outside 0.0..1.0");
        await Refused(HttpMethod.Patch, "rooms/roomA", """{"audioControl":"always"}""", HttpStatusCode.BadRequest, "invalidRequest");
        await Refused(HttpMethod.Put, "core/resampler", """{"resampler":"soxr","x":1}""", HttpStatusCode.BadRequest,
            "invalidRequest", "unknown field x");
        // There is no drift setting, and the
        // old name for auto is read.
        await Refused(HttpMethod.Patch, "rooms/roomA", """{"driftCorrection":"yes"}""", HttpStatusCode.BadRequest, "invalidRequest");
        await Ok(HttpMethod.Patch, "rooms/roomA", """{"audioControl":"bitPerfectFallback"}""");
        Assert.Equal("""{"client":"roomA","audioControl":"auto"}""",
            _core.All.Last(r => r.Command == "client.setAudioControl").Params);
    }

    [Fact]
    public async Task A_zone_s_volume_and_mute_are_patched_over_its_rooms_and_a_room_answers_what_is_sent_for_it()
    {
        await StartAsync();
        await Ok(HttpMethod.Put, "zones/living/clients", """{"clients":["roomA"]}""");
        await Ok(HttpMethod.Patch, "rooms/roomA", """{"volume":0.8}""");
        // Alone in its zone: not grouped, so nothing is sent for it.
        Assert.DoesNotContain(_core.All, r => r.Command == "client.setGrouped");

        using var stream = await EventsAsync(null);
        var zone = await Ok(HttpMethod.Patch, "zones/living", """{"volume":0.5}""");
        Assert.Equal(("client.setVolume", """{"client":"roomA","value":0.3}"""), (_core.All[^1].Command, _core.All[^1].Params));
        Assert.Equal((0.5, false), (zone["desired"]!["volume"]!.GetValue<double>(), zone["desired"]!["muted"]!.GetValue<bool>()));
        var changed = JsonNode.Parse((await stream.NextAsync(e => e.Type == "zone.volumeChanged")).Data)!;
        Assert.Equal(("living", 0.5, false),
            (changed["zone"]!.GetValue<string>(), changed["volume"]!.GetValue<double>(), changed["muted"]!.GetValue<bool>()));

        var desired = (await Ok(HttpMethod.Get, "rooms/roomA"))["desired"]!;
        Assert.Equal((0.8, 0.3), (desired["volume"]!.GetValue<double>(), desired["effectiveVolume"]!.GetValue<double>()));
        Assert.False(desired["effectiveMuted"]!.GetValue<bool>());
        Assert.False(desired["grouped"]!.GetValue<bool>());

        await Refused(HttpMethod.Patch, "zones/living", "{}", HttpStatusCode.BadRequest, "invalidArgument");
        await Refused(HttpMethod.Patch, "zones/living", """{"volume":1.5}""", HttpStatusCode.BadRequest, "invalidArgument",
            "volume 1.5 is outside 0.0..1.0");
        await Refused(HttpMethod.Patch, "zones/living", """{"gain":1}""", HttpStatusCode.BadRequest, "invalidRequest");
        await Refused(HttpMethod.Patch, "zones/attic", """{"muted":true}""", HttpStatusCode.NotFound, "notFound", "no zone attic");
    }

    [Fact]
    public async Task A_room_that_never_set_its_volume_answers_full_volume_in_a_zone_with_a_level()
    {
        await StartAsync();
        await Ok(HttpMethod.Put, "zones/living/clients", """{"clients":["roomA"]}""");
        await Ok(HttpMethod.Patch, "zones/living", """{"volume":0.5}""");

        // Its own volume counts as 1.0 under the zone's: answering null let a
        // UI show the effective 0.5 as the room's own, and raising that made it
        // quieter.
        var desired = (await Ok(HttpMethod.Get, "rooms/roomA"))["desired"]!;
        Assert.Equal((1.0, 0.5), (desired["volume"]!.GetValue<double>(), desired["effectiveVolume"]!.GetValue<double>()));
        desired = (await Ok(HttpMethod.Patch, "rooms/roomA", """{"volume":0.9}"""))["desired"]!;
        Assert.Equal(0.4, desired["effectiveVolume"]!.GetValue<double>(), 9);
    }

    [Fact]
    public async Task A_room_s_max_volume_scales_what_is_sent_so_full_volume_is_that_level()
    {
        await StartAsync();
        await Ok(HttpMethod.Patch, "rooms/roomA", """{"volume":0.8}""");

        // The slider is 60 dB long, so -30 dB is half of it off the position.
        var desired = (await Ok(HttpMethod.Patch, "rooms/roomA", """{"maxVolumeDb":-30}"""))["desired"]!;
        Assert.Equal(-30.0, desired["maxVolumeDb"]!.GetValue<double>());
        Assert.Equal(0.8, desired["volume"]!.GetValue<double>());
        Assert.Equal(0.3, desired["effectiveVolume"]!.GetValue<double>(), 9);
        var sent = JsonNode.Parse(_core.All.Last(r => r.Command == "client.setVolume").Params)!;
        Assert.Equal(0.3, sent["value"]!.GetValue<double>(), 9);

        // The room's own volume keeps the offset, and one below the scale
        // plays its quietest step rather than a mute.
        await Ok(HttpMethod.Patch, "rooms/roomA", """{"volume":0.4}""");
        sent = JsonNode.Parse(_core.All.Last(r => r.Command == "client.setVolume").Params)!;
        Assert.Equal(0.001, sent["value"]!.GetValue<double>(), 9);

        // 0 dB is no limit.
        desired = (await Ok(HttpMethod.Patch, "rooms/roomA", """{"maxVolumeDb":0}"""))["desired"]!;
        Assert.Null(desired["maxVolumeDb"]);
        Assert.Equal(("client.setVolume", """{"client":"roomA","value":0.4}"""), (_core.All[^1].Command, _core.All[^1].Params));

        await Refused(HttpMethod.Patch, "rooms/roomA", """{"maxVolumeDb":3}""", HttpStatusCode.BadRequest, "invalidArgument",
            "maxVolumeDb 3 is outside -60..0");
    }

    [Fact]
    public async Task Cores_lists_what_announced_itself_and_a_selection_names_only_one_of_them()
    {
        await StartAsync();

        var cores = await Ok(HttpMethod.Get, "cores");
        Assert.Null(cores["selected"]);
        Assert.Equal($"127.0.0.1:{_core.Port}", cores["configured"]!.GetValue<string>());
        Assert.Equal($"127.0.0.1:{_core.Port}", cores["connected"]!.GetValue<string>());
        Assert.Empty(cores["cores"]!.AsArray());

        await Refused(HttpMethod.Put, "core/selection", """{"coreId":"galdrun-core-nowhere"}""", HttpStatusCode.NotFound,
            "notFound", "no core galdrun-core-nowhere on the network");
        Assert.Null((await Ok(HttpMethod.Put, "core/selection", """{"coreId":null}"""))["selected"]);
    }

    // --- playlists, favourites, stations ---------------------------------------

    [Fact]
    public async Task Playlists_are_kept_edited_and_queued_with_their_refusals_named()
    {
        await StartAsync();
        var a = _fake.AddTrack(_dir.Write("a.flac", "a"), "A");
        var b = _fake.AddTrack(_dir.Write("b.flac", "b"), "B");
        var lost = new Track("gone", "track:1", "Lost");

        var created = await Send(HttpMethod.Post, "playlists", """{"name":" Evening "}""");
        Assert.Equal(HttpStatusCode.Created, created.Status);
        var id = created.Body!["id"]!.GetValue<long>();
        await Ok(HttpMethod.Post, "playlists", """{"name":"Morning"}""");
        var patched = await Ok(HttpMethod.Patch, $"playlists/{id}", """{"name":"Night","position":1}""");
        Assert.Equal(("Night", 1), (patched["name"]!.GetValue<string>(), patched["position"]!.GetValue<int>()));
        Assert.Equal(["Morning", "Night"], (await Ok(HttpMethod.Get, "playlists")).AsArray().Select(p => p!["name"]!.GetValue<string>()));

        var entries = await Ok(HttpMethod.Post, $"playlists/{id}/entries", $$"""{"tracks":[{{Json(a)}},{{Json(lost)}},{{Json(b)}}]}""");
        var entryIds = entries.AsArray().Select(e => e!["id"]!.GetValue<long>()).ToArray();
        Assert.Equal(["B", "A", "Lost"], Entries(await Ok(HttpMethod.Patch, $"playlists/{id}/entries/{entryIds[2]}", """{"index":0}""")));
        Assert.Equal(["B", "A", "Lost"], Entries(await Ok(HttpMethod.Get, $"playlists/{id}/entries")));

        var queued = await Ok(HttpMethod.Post, "zones/living/queue/items", $$"""{"playlist":{{id}}}""");
        Assert.Equal(["B", "A"], Titles(queued["queue"]!));
        var refused = Assert.Single(queued["refused"]!.AsArray())!;
        Assert.Equal("Lost", refused["entry"]!["item"]!["title"]!.GetValue<string>());
        Assert.Equal("NotFound: no provider gone", refused["reason"]!.GetValue<string>());

        Assert.Equal(["B", "A"], Entries(await Ok(HttpMethod.Delete, $"playlists/{id}/entries/{entryIds[1]}")));
        Assert.Equal(HttpStatusCode.NoContent, (await Send(HttpMethod.Delete, $"playlists/{id}")).Status);

        await Refused(HttpMethod.Get, $"playlists/{id}/entries", HttpStatusCode.NotFound, "notFound");
        await Refused(HttpMethod.Get, "playlists/x/entries", HttpStatusCode.NotFound, "notFound", "no playlist x");
        await Refused(HttpMethod.Post, "playlists", """{"name":" "}""", HttpStatusCode.BadRequest, "invalidArgument");
        await Refused(HttpMethod.Post, "playlists", "{}", HttpStatusCode.BadRequest, "invalidRequest", "field name is required");
        await Refused(HttpMethod.Patch, $"playlists/{id}", "{}", HttpStatusCode.BadRequest, "invalidArgument");
        await Refused(HttpMethod.Delete, $"playlists/{id}", HttpStatusCode.NotFound, "notFound");
        await Refused(HttpMethod.Post, "zones/living/queue/items", """{"playlist":999}""", HttpStatusCode.NotFound, "notFound");
        await Refused(HttpMethod.Post, "zones/living/queue/items", """{"playlist":999,"index":0,"next":true}""",
            HttpStatusCode.BadRequest, "invalidRequest");
    }

    [Fact]
    public async Task Playlist_folders_hold_playlists_and_folders_and_take_them_along_when_removed()
    {
        await StartAsync();
        long Id(JsonNode n) => n["id"]!.GetValue<long>();
        long? Of(JsonNode n, string field) => n[field]?.GetValue<long>();

        var music = Id((await Send(HttpMethod.Post, "playlist-folders", """{"name":" Music "}""")).Body!);
        var rock = await Ok(HttpMethod.Post, "playlist-folders", $$"""{"name":"Rock","parent":{{music}}}""");
        Assert.Equal(("Rock", music), (rock["name"]!.GetValue<string>(), Of(rock, "parent")));
        var list = await Ok(HttpMethod.Post, "playlists", $$"""{"name":"Loud","folder":{{Id(rock)}}}""");
        Assert.Equal(Id(rock), Of(list, "folder"));
        var top = await Ok(HttpMethod.Post, "playlists", """{"name":"Quiet"}""");
        Assert.Null(Of(top, "folder"));

        Assert.Equal(music, Of(await Ok(HttpMethod.Patch, $"playlists/{Id(top)}", $$"""{"folder":{{music}}}"""), "folder"));
        Assert.Null(Of(await Ok(HttpMethod.Patch, $"playlists/{Id(top)}", """{"folder":0}"""), "folder"));
        await Refused(HttpMethod.Patch, $"playlist-folders/{music}", $$"""{"parent":{{Id(rock)}}}""",
            HttpStatusCode.BadRequest, "invalidArgument");
        var renamed = await Ok(HttpMethod.Patch, $"playlist-folders/{Id(rock)}", """{"name":"Hard rock","parent":0}""");
        Assert.Equal(("Hard rock", (long?)null), (renamed["name"]!.GetValue<string>(), Of(renamed, "parent")));
        Assert.Equal(["Hard rock", "Music"], (await Ok(HttpMethod.Get, "playlist-folders")).AsArray().Select(f => f!["name"]!.GetValue<string>()));

        // No two folders in one place share a name, in any case: not made, renamed or moved so.
        await Refused(HttpMethod.Post, "playlist-folders", """{"name":"hard ROCK"}""", HttpStatusCode.Conflict, "alreadyExists",
            "a folder named hard ROCK is already there");
        await Refused(HttpMethod.Patch, $"playlist-folders/{music}", """{"name":"Hard rock"}""", HttpStatusCode.Conflict, "alreadyExists");
        var inner = Id(await Ok(HttpMethod.Post, "playlist-folders", $$"""{"name":"Hard rock","parent":{{music}}}"""));
        await Refused(HttpMethod.Patch, $"playlist-folders/{inner}", """{"parent":0}""", HttpStatusCode.Conflict, "alreadyExists");

        // Removed, a folder takes every folder within it and every playlist in any of them; the
        // other playlists close up their positions.
        await Ok(HttpMethod.Patch, $"playlists/{Id(list)}", $$"""{"folder":{{inner}}}""");
        await Ok(HttpMethod.Post, "playlists", $$"""{"name":"Deep","folder":{{inner}}}""");
        Assert.Equal(HttpStatusCode.NoContent, (await Send(HttpMethod.Delete, $"playlist-folders/{music}")).Status);
        Assert.Equal(["Hard rock"], (await Ok(HttpMethod.Get, "playlist-folders")).AsArray().Select(f => f!["name"]!.GetValue<string>()));
        Assert.Equal([("Quiet", 0)], (await Ok(HttpMethod.Get, "playlists")).AsArray()
            .Select(p => (p!["name"]!.GetValue<string>(), p["position"]!.GetValue<int>())));
        await Refused(HttpMethod.Get, $"playlists/{Id(list)}/entries", HttpStatusCode.NotFound, "notFound");

        await Refused(HttpMethod.Post, "playlists", """{"name":"X","folder":999}""", HttpStatusCode.NotFound, "notFound");
        await Refused(HttpMethod.Post, "playlist-folders", """{"name":" "}""", HttpStatusCode.BadRequest, "invalidArgument");
        await Refused(HttpMethod.Patch, $"playlist-folders/{Id(rock)}", "{}", HttpStatusCode.BadRequest, "invalidArgument");
        await Refused(HttpMethod.Delete, $"playlist-folders/{music}", HttpStatusCode.NotFound, "notFound");
    }

    [Fact]
    public async Task Favourites_are_added_once_listed_queued_and_removed()
    {
        await StartAsync();
        var a = _fake.AddTrack(_dir.Write("a.flac", "a"), "A");
        var radio = _fake.AddStation("Radio", [1]);

        var favorite = await Send(HttpMethod.Post, "favorites", $$"""{"track":{{Json(a)}}}""");
        Assert.Equal(HttpStatusCode.Created, favorite.Status);
        var id = favorite.Body!["id"]!.GetValue<long>();
        Assert.Equal(id, (await Ok(HttpMethod.Post, "favorites", $$"""{"track":{{Json(a)}}}"""))["id"]!.GetValue<long>());
        var station = (await Ok(HttpMethod.Post, "favorites", $$"""{"station":{{Json(radio)}}}"""))["id"]!.GetValue<long>();
        Assert.Equal(["A", "Radio"], (await Ok(HttpMethod.Get, "favorites")).AsArray().Select(f => f!["title"]!.GetValue<string>()));

        Assert.Equal(["A"], Titles(await Ok(HttpMethod.Post, "zones/living/queue/items", $$"""{"favorite":{{id}}}""")));
        Assert.Equal(["Radio", "A"], Titles(await Ok(HttpMethod.Post, "zones/living/queue/items", $$"""{"favorite":{{station}},"next":true}""")));

        Assert.Equal(HttpStatusCode.NoContent, (await Send(HttpMethod.Delete, $"favorites/{id}")).Status);
        await Refused(HttpMethod.Delete, $"favorites/{id}", HttpStatusCode.NotFound, "notFound");
        await Refused(HttpMethod.Post, "favorites", $$"""{"track":{{Json(a)}},"station":{{Json(radio)}}}""",
            HttpStatusCode.BadRequest, "invalidArgument", "exactly one of track, station or node");
        await Refused(HttpMethod.Post, "zones/living/queue/items", $$"""{"favorite":{{id}}}""", HttpStatusCode.NotFound, "notFound");
    }

    [Fact]
    public async Task Likes_are_added_once_listed_by_kind_answered_in_an_items_status_and_removed()
    {
        await StartAsync();
        using var stream = await EventsAsync(null);
        var a = _fake.AddTrack(_dir.Write("a.flac", "a"), "A");
        var radio = _fake.AddStation("Radio", [1]);
        var album = new Album("fake", "album:1", "Kind of Blue", "Miles Davis", 1959, new ArtworkRef("fake", "cover:1"));
        var artist = new Artist("fake", "artist:1", "Miles Davis");

        var liked = await Send(HttpMethod.Post, "likes", $$"""{"track":{{Json(a)}}}""");
        Assert.Equal(HttpStatusCode.Created, liked.Status);
        var id = liked.Body!["id"]!.GetValue<long>();
        Assert.Equal("A", liked.Body["item"]!["title"]!.GetValue<string>());
        var added = JsonNode.Parse((await stream.NextAsync(e => e.Type == "like.changed")).Data)!;
        Assert.Equal(id, added["id"]!.GetValue<long>());
        Assert.Equal(id, (await Ok(HttpMethod.Post, "likes", $$"""{"track":{{Json(a)}}}"""))["id"]!.GetValue<long>());
        var albumLike = (await Ok(HttpMethod.Post, "likes", $$"""{"album":{{Json(album)}}}"""))["id"]!.GetValue<long>();
        await Ok(HttpMethod.Post, "likes", $$"""{"artist":{{Json(artist)}}}""");
        await Ok(HttpMethod.Post, "likes", $$"""{"station":{{Json(radio)}}}""");

        var all = await Ok(HttpMethod.Get, "likes");
        Assert.Equal(4, all.AsArray().Count);
        var albums = await Ok(HttpMethod.Get, "likes?kind=album");
        // Phase 40: its picture by its artworkId, never its provider's reference.
        Assert.Equal(("album", "Kind of Blue", 1959, Artworks.AlbumId("Kind of Blue", "Miles Davis")), (
            albums[0]!["item"]!["kind"]!.GetValue<string>(), albums[0]!["item"]!["title"]!.GetValue<string>(),
            albums[0]!["item"]!["year"]!.GetValue<int>(), albums[0]!["item"]!["artworkId"]!.GetValue<string>()));
        Assert.Null(albums[0]!["item"]!["artwork"]);
        Assert.Single(albums.AsArray());
        await Refused(HttpMethod.Get, "likes?kind=song", HttpStatusCode.BadRequest, "invalidRequest");
        await Refused(HttpMethod.Get, "likes?sort=name", HttpStatusCode.BadRequest, "invalidRequest");

        var favorite = (await Ok(HttpMethod.Post, "favorites", $$"""{"track":{{Json(a)}}}"""))["id"]!.GetValue<long>();
        var status = await Ok(HttpMethod.Post, "items/status", $$"""
            {"items":[
                {"kind":"track","providerId":"fake","providerItemId":"{{a.ProviderItemId}}"},
                {"kind":"album","providerId":"fake","providerItemId":"album:1"},
                {"kind":"track","providerId":"fake","providerItemId":"unknown"}]}
            """);
        Assert.Equal((true, id, true, favorite), (status[0]!["liked"]!.GetValue<bool>(), status[0]!["likeId"]!.GetValue<long>(),
            status[0]!["favorite"]!.GetValue<bool>(), status[0]!["favoriteId"]!.GetValue<long>()));
        Assert.Equal((true, albumLike, false), (status[1]!["liked"]!.GetValue<bool>(), status[1]!["likeId"]!.GetValue<long>(),
            status[1]!["favorite"]!.GetValue<bool>()));
        Assert.Equal(("unknown", false, false), (status[2]!["providerItemId"]!.GetValue<string>(),
            status[2]!["liked"]!.GetValue<bool>(), status[2]!["favorite"]!.GetValue<bool>()));

        Assert.Equal(HttpStatusCode.NoContent, (await Send(HttpMethod.Delete, $"likes/{id}")).Status);
        var removed = JsonNode.Parse((await stream.NextAsync(e => e.Type == "like.changed" &&
            e.Data.Contains("removed", StringComparison.Ordinal))).Data)!;
        Assert.Equal(id, removed["id"]!.GetValue<long>());
        await Refused(HttpMethod.Delete, $"likes/{id}", HttpStatusCode.NotFound, "notFound");
        Assert.False((await Ok(HttpMethod.Post, "items/status",
            $$"""{"items":[{"kind":"track","providerId":"fake","providerItemId":"{{a.ProviderItemId}}"}]}"""))[0]!["liked"]!
            .GetValue<bool>());

        await Refused(HttpMethod.Post, "likes", $$"""{"track":{{Json(a)}},"album":{{Json(album)}}}""",
            HttpStatusCode.BadRequest, "invalidArgument", "exactly one of track, album, artist, station or node");
        await Refused(HttpMethod.Post, "likes", "{}", HttpStatusCode.BadRequest, "invalidArgument");
        await Refused(HttpMethod.Post, "items/status", """{"items":[]}""", HttpStatusCode.BadRequest, "invalidArgument");
        Assert.Equal(HttpStatusCode.BadRequest, (await Send(HttpMethod.Post, "items/status",
            """{"items":[{"kind":"song","providerId":"fake","providerItemId":"x"}]}""")).Status);
    }

    [Fact]
    public async Task Radio_title_changes_publish_metadata_invalidations_and_answer_the_current_song()
    {
        await StartAsync();
        using var stream = await EventsAsync(null);
        var station = _fake.AddStation("Radio", [1]);
        await Ok(HttpMethod.Post, "zones/living/queue/items", $$"""{"station":{{Json(station)}}}""");
        await Ok(HttpMethod.Post, "zones/living/play");
        _fake.RadioTitle = "Artist - Song";
        await stream.NextAsync(e => e.Type == "zone.metadataChanged" && e.Data.Contains("Artist - Song", StringComparison.Ordinal));
        var zone = await Ok(HttpMethod.Get, "zones/living");
        var first = zone["radioTrack"]!;
        Assert.Equal("Song", first["title"]!.GetValue<string>());
        Assert.Equal("Artist", first["artist"]!.GetValue<string>());
        Assert.Null(first["album"]);
        Assert.Equal(station.ProviderItemId, zone["queue"]!["items"]![0]!["item"]!["providerItemId"]!.GetValue<string>());
        _fake.RadioTitle = "Artist - Next song";
        await stream.NextAsync(e => e.Type == "zone.metadataChanged" && e.Data.Contains("Next song", StringComparison.Ordinal));
        var next = (await Ok(HttpMethod.Get, "zones/living"))["radioTrack"]!;
        Assert.NotEqual(first["providerItemId"]!.GetValue<string>(), next["providerItemId"]!.GetValue<string>());
        _fake.RadioTitle = null;
        await stream.NextAsync(e => e.Type == "zone.metadataChanged" && !e.Data.Contains("streamTitle", StringComparison.Ordinal));
        Assert.Null((await Ok(HttpMethod.Get, "zones/living"))["radioTrack"]);
    }

    [Fact]
    public async Task Radio_song_likes_keep_metadata_without_a_library_track_and_are_idempotent()
    {
        await StartAsync();
        const string body = """{"metadata":{"title":" Song ","artist":" Artist ","album":" Album "}}""";
        var added = await Send(HttpMethod.Post, "likes", body);
        Assert.Equal(HttpStatusCode.Created, added.Status);
        var like = added.Body!; var track = like["item"]!; var id = like["id"]!.GetValue<long>();
        Assert.Equal("track", track["kind"]!.GetValue<string>());
        // Phase 40: a song heard on the radio still names its picture, by its album.
        Assert.Equal(Artworks.AlbumId("Album", "Artist"), track["artworkId"]!.GetValue<string>());
        Assert.Equal("radio-metadata", track["providerId"]!.GetValue<string>());
        Assert.Equal(("Song", "Artist", "Album"), (track["title"]!.GetValue<string>(), track["artist"]!.GetValue<string>(), track["album"]!.GetValue<string>()));
        Assert.Equal(id, (await Ok(HttpMethod.Post, "likes", """{"metadata":{"title":"song","artist":"artist","album":"album"}}"""))["id"]!.GetValue<long>());
        var status = await Ok(HttpMethod.Post, "items/status", $$"""{"items":[{"kind":"track","providerId":"radio-metadata","providerItemId":"{{track["providerItemId"]!.GetValue<string>()}}"}]}""");
        Assert.True(status[0]!["liked"]!.GetValue<bool>());
        Assert.False(status[0]!["favorite"]!.GetValue<bool>());
        Assert.Equal("Album", (await Ok(HttpMethod.Get, "likes?kind=track"))[0]!["item"]!["album"]!.GetValue<string>());
        await Refused(HttpMethod.Post, "likes", """{"metadata":{"title":" "}}""", HttpStatusCode.BadRequest, "invalidArgument");
        await Refused(HttpMethod.Post, "likes", """{"metadata":{"title":"Song"},"artist":{"providerId":"fake","providerItemId":"artist:1","name":"Artist"}}""", HttpStatusCode.BadRequest, "invalidArgument");
        Assert.Equal(HttpStatusCode.NoContent, (await Send(HttpMethod.Delete, $"likes/{id}")).Status);
    }

    [Fact]
    public async Task Stations_are_added_imported_and_removed_without_their_stream_url_ever_answered()
    {
        await StartAsync();

        var added = await Send(HttpMethod.Post, "stations",
            """{"name":"Jazz","streamUrl":"http://user:secret@radio.example/jazz","genre":"jazz"}""");
        Assert.Equal(HttpStatusCode.Created, added.Status);
        Assert.Equal("webradio", added.Body!["providerId"]!.GetValue<string>());
        Assert.DoesNotContain("radio.example", added.Text, StringComparison.Ordinal);
        var jazz = added.Body["providerItemId"]!.GetValue<string>();

        var imported = await Send(HttpMethod.Post, "stations/import",
            """{"playlist":"#EXTM3U\n#EXTINF:-1,Blues\nhttp://radio.example/blues\n"}""");
        Assert.Equal(HttpStatusCode.Created, imported.Status);
        Assert.Equal("Blues", imported.Body![0]!["name"]!.GetValue<string>());
        Assert.Equal(["Blues", "Jazz"], (await Ok(HttpMethod.Get, "providers/webradio/browse?node=stations"))["items"]!["stations"]!
            .AsArray().Select(s => s!["name"]!.GetValue<string>()));

        Assert.Equal(HttpStatusCode.NoContent, (await Send(HttpMethod.Delete, $"stations/{jazz}")).Status);
        await Refused(HttpMethod.Delete, $"stations/{jazz}", HttpStatusCode.NotFound, "notFound");
        await Refused(HttpMethod.Delete, "stations/5", HttpStatusCode.NotFound, "notFound", "no station 5");
        await Refused(HttpMethod.Post, "stations", """{"name":"X","streamUrl":"ftp://radio.example/x"}""",
            HttpStatusCode.BadRequest, "invalidArgument");
        await Refused(HttpMethod.Post, "stations", """{"name":"X","url":"http://radio.example/x"}""",
            HttpStatusCode.BadRequest, "invalidRequest", "unknown field url");
        await Refused(HttpMethod.Post, "stations/import", """{"playlist":"#EXTM3U\nnot a url\n"}""",
            HttpStatusCode.BadRequest, "invalidArgument");
    }

    [Fact]
    public async Task Station_folders_hold_stations_and_folders_and_take_them_along_when_removed()
    {
        await StartAsync();
        long Id(JsonNode n) => n["id"]!.GetValue<long>();
        long? Of(JsonNode n, string field) => n[field]?.GetValue<long>();
        async Task<List<(string, long?)>> Listed() => [.. (await Ok(HttpMethod.Get, "stations")).AsArray()
            .Select(e => (e!["station"]!["name"]!.GetValue<string>(), Of(e, "folder")))];

        var news = Id(await Ok(HttpMethod.Post, "station-folders", """{"name":"News"}"""));
        var local = Id(await Ok(HttpMethod.Post, "station-folders", $$"""{"name":"Local","parent":{{news}}}"""));
        var jazz = (await Ok(HttpMethod.Post, "stations", """{"name":"Jazz","streamUrl":"http://radio.example/jazz"}"""))["providerItemId"]!
            .GetValue<string>();
        await Ok(HttpMethod.Post, "stations", $$"""{"name":"City","streamUrl":"http://radio.example/city","folder":{{local}}}""");
        await Ok(HttpMethod.Post, "stations/import", $$"""{"playlist":"#EXTM3U\n#EXTINF:-1,World\nhttp://radio.example/world\n","folder":{{news}}}""");
        Assert.Equal([("City", local), ("Jazz", null), ("World", news)], await Listed());

        var moved = await Ok(HttpMethod.Patch, $"stations/{jazz}", $$"""{"folder":{{news}}}""");
        Assert.Equal(("Jazz", news), (moved["station"]!["name"]!.GetValue<string>(), Of(moved, "folder")));
        Assert.DoesNotContain("radio.example", (await Send(HttpMethod.Get, "stations")).Text, StringComparison.Ordinal);
        await Refused(HttpMethod.Patch, $"stations/{jazz}", """{"folder":999}""", HttpStatusCode.NotFound, "notFound", "no station folder 999");
        await Refused(HttpMethod.Patch, "stations/station:999", """{"folder":0}""", HttpStatusCode.NotFound, "notFound");
        await Refused(HttpMethod.Patch, $"station-folders/{news}", $$"""{"parent":{{local}}}""", HttpStatusCode.BadRequest, "invalidArgument");
        await Refused(HttpMethod.Post, "station-folders", """{"name":"news"}""", HttpStatusCode.Conflict, "alreadyExists");
        Assert.Null(Of(await Ok(HttpMethod.Patch, $"station-folders/{local}", """{"name":"Town","parent":0}"""), "parent"));

        // Removed, a folder takes every folder within it and every station in any of them.
        await Ok(HttpMethod.Patch, $"station-folders/{local}", $$"""{"parent":{{news}}}""");
        Assert.Equal(HttpStatusCode.NoContent, (await Send(HttpMethod.Delete, $"station-folders/{news}")).Status);
        Assert.Empty((await Ok(HttpMethod.Get, "station-folders")).AsArray());
        Assert.Empty(await Listed());
        Assert.Empty((await Ok(HttpMethod.Get, "search?q=jazz&types=all"))["items"]!["stations"]!.AsArray());
    }

    // --- the media browser ----------------------------------------------------

    [Fact]
    public async Task Providers_answer_their_roots_browse_with_options_and_a_node_is_queued_by_reference_or_in_place_of_the_queue()
    {
        var album = Directory.CreateDirectory(Path.Combine(_dir.Path, "music", "Days")).FullName;
        LibraryTests.WriteTrack(Path.Combine(album, "1.wav"), "One", "Aurora", "Days", 1, "Pop");
        LibraryTests.WriteTrack(Path.Combine(album, "2.wav"), "Two", "Aurora", "Days", 2, "Pop");
        await StartAsync();
        var track = _fake.AddTrack(_dir.Write("a.flac", "a"), "Solo");
        var indexer = _host!.Services.GetRequiredService<Library.LibraryIndexer>();
        await Until(() => Task.FromResult(indexer.Status.Passes >= 1));

        var providers = (await Ok(HttpMethod.Get, "providers")).AsArray();
        JsonNode Provider(string id) => providers.Single(p => p!["id"]!.GetValue<string>() == id)!;
        Assert.Equal(["artists", "album-artists", "albums", "genres", "composers", "years", "recent", "folders", "audiobooks"],
            Provider("local")["root"]!.AsArray().Select(n => n!["nodeId"]!.GetValue<string>()));
        Assert.Equal("stations", Provider("webradio")["root"]![0]!["nodeId"]!.GetValue<string>());
        Assert.Null(Provider("slow")["root"]);
        Assert.Contains("expand", Provider("local")["capabilities"]!.AsArray().Select(c => c!.GetValue<string>()));

        var albums = await Ok(HttpMethod.Get, "providers/local/browse?node=albums&sort=title&letter=D&limit=5");
        var days = albums["nodes"]![0]!;
        Assert.Equal(("Days", "album", "Aurora", true), (days["title"]!.GetValue<string>(), days["kind"]!.GetValue<string>(),
            days["subtitle"]!.GetValue<string>(), days["playable"]!.GetValue<bool>()));
        Assert.Equal(("grid", 1, "title"), (albums["layout"]!.GetValue<string>(), albums["total"]!.GetValue<int>(),
            albums["sort"]!.GetValue<string>()));
        Assert.Equal("D", albums["letters"]![0]!["letter"]!.GetValue<string>());
        var id = days["nodeId"]!.GetValue<string>();
        Assert.Equal(["One", "Two"], (await Ok(HttpMethod.Get, $"providers/local/browse?node={id}"))["items"]!["tracks"]!
            .AsArray().Select(t => t!["title"]!.GetValue<string>()));
        await Refused(HttpMethod.Get, "providers/local/browse?node=albums&sort=colour", HttpStatusCode.BadRequest, "invalidArgument",
            "albums sorts by title, artist, year, added, not colour");
        await Refused(HttpMethod.Get, "providers/local/browse?node=years&filter=19", HttpStatusCode.BadRequest, "invalidArgument");
        await Refused(HttpMethod.Get, "providers/local/browse?node=albums&order=title", HttpStatusCode.BadRequest, "invalidRequest",
            "unknown query parameter order");

        await Ok(HttpMethod.Post, "zones/living/queue/items", $$"""{"tracks":[{{Json(track)}}]}""");
        var queued = await Ok(HttpMethod.Post, "zones/living/queue/items", $$$"""{"node":{"provider":"local","id":"{{{id}}}"}}""");
        Assert.Equal((2, false), (queued["queued"]!.GetValue<int>(), queued["truncated"]!.GetValue<bool>()));
        Assert.Empty(queued["refused"]!.AsArray());
        Assert.Equal(["Solo", "One", "Two"], Titles(queued["queue"]!));
        var replaced = await Ok(HttpMethod.Post, "zones/living/queue/items", $$"""{"node":{"provider":"local","id":"genre:Pop"},"replace":true}""");
        Assert.Equal(["One", "Two"], Titles(replaced["queue"]!));
        Assert.Equal(["Solo"], Titles(await Ok(HttpMethod.Post, "zones/living/queue/items",
            $$"""{"tracks":[{{Json(track)}}],"replace":true}""")));

        await Refused(HttpMethod.Post, "zones/living/queue/items", """{"node":{"provider":"local","id":"albums"}}""",
            HttpStatusCode.BadRequest, "invalidArgument");
        await Refused(HttpMethod.Post, "zones/living/queue/items", """{"node":{"provider":"local","id":"album:999"}}""",
            HttpStatusCode.NotFound, "notFound");
        await Refused(HttpMethod.Post, "zones/living/queue/items", """{"node":{"provider":"fake","id":"tracks"}}""",
            HttpStatusCode.ServiceUnavailable, "unavailable");
        await Refused(HttpMethod.Post, "zones/living/queue/items", $$"""{"node":{"provider":"local","id":"{{id}}"},"next":true,"replace":true}""",
            HttpStatusCode.BadRequest, "invalidRequest");
        // A node goes in before the item at `index` (a drop onto a queue row).
        Assert.Equal(["One", "Two", "Solo"], Titles((await Ok(HttpMethod.Post, "zones/living/queue/items",
            $$"""{"node":{"provider":"local","id":"{{id}}"},"index":0}"""))["queue"]!));
        await Refused(HttpMethod.Post, "zones/living/queue/items", $$"""{"node":{"provider":"local","id":"{{id}}"},"index":0,"replace":true}""",
            HttpStatusCode.BadRequest, "invalidRequest");
        await Refused(HttpMethod.Post, "zones/living/queue/items", """{"node":{"provider":"local"}}""",
            HttpStatusCode.BadRequest, "invalidRequest", "field node.id is required");
    }

    // --- add-ins --------------------------------------------------------------

    [Fact]
    public async Task More_like_this_answers_a_track_or_a_playlist_ranked_and_explained_while_an_add_in_sleeps()
    {
        var music = Directory.CreateDirectory(Path.Combine(_dir.Path, "music")).FullName;
        LibraryTests.WriteTrack(Path.Combine(music, "so-what.wav"), "So What", "Miles Davis", "Kind of Blue", 1, "Jazz", year: 1959);
        LibraryTests.WriteTrack(Path.Combine(music, "blue.wav"), "Blue in Green", "Miles Davis", "Kind of Blue", 3, "Jazz", year: 1959);
        LibraryTests.WriteTrack(Path.Combine(music, "runaway.wav"), "Runaway", "Aurora", "All My Demons", 1, "Pop", year: 2015);
        await StartAsync();
        var indexer = _host!.Services.GetRequiredService<Library.LibraryIndexer>();
        await Until(() => Task.FromResult(indexer.Status.Passes >= 1));
        var soWhat = (await Ok(HttpMethod.Get, "providers/local/search?q=so%20what"))["tracks"]![0]!;

        var clock = Stopwatch.StartNew();
        var answer = await Ok(HttpMethod.Post, "recommendations", $$"""{"track":{{soWhat.ToJsonString()}},"limit":5}""");
        clock.Stop();

        Assert.True(clock.Elapsed < TimeSpan.FromSeconds(3), $"answered in {clock.Elapsed}");
        var item = Assert.Single(answer["items"]!.AsArray())!;
        Assert.Equal(("Blue in Green", 1.0), (item["track"]!["title"]!.GetValue<string>(), item["score"]!.GetValue<double>()));
        Assert.Equal(("library", "same artist Miles Davis, genre Jazz, era 1959"),
            (item["sources"]![0]!["addIn"]!.GetValue<string>(), item["sources"]![0]!["reason"]!.GetValue<string>()));
        Assert.Equal(1, answer["sources"]![0]!["tracks"]!.GetValue<int>());
        var failure = Assert.Single(answer["failures"]!.AsArray())!;
        Assert.Equal((SleepyAddIn.AddInId, "timeout", "no answer within 300 ms"), (failure["addIn"]!.GetValue<string>(),
            failure["error"]!.GetValue<string>(), failure["message"]!.GetValue<string>()));

        var playlist = (await Ok(HttpMethod.Post, "playlists", """{"name":"Evening"}"""))["id"]!.GetValue<long>();
        await Ok(HttpMethod.Post, $"playlists/{playlist}/entries", $$"""{"tracks":[{{soWhat.ToJsonString()}}]}""");
        var fromPlaylist = await Ok(HttpMethod.Post, "recommendations", $$"""{"playlist":{{playlist}}}""");
        Assert.Equal(["Blue in Green"], fromPlaylist["items"]!.AsArray().Select(i => i!["track"]!["title"]!.GetValue<string>()));
        var sleepy = (await Ok(HttpMethod.Get, "status"))["addIns"]!["registered"]!.AsArray()
            .Single(a => a!["id"]!.GetValue<string>() == SleepyAddIn.AddInId)!;
        Assert.Equal((2, 2), (sleepy["calls"]!.GetValue<int>(), sleepy["timeouts"]!.GetValue<int>()));

        var empty = (await Ok(HttpMethod.Post, "playlists", """{"name":"Empty"}"""))["id"]!.GetValue<long>();
        await Refused(HttpMethod.Post, "recommendations", $$"""{"playlist":{{empty}}}""", HttpStatusCode.BadRequest,
            "invalidArgument", $"playlist {empty} is empty");
        await Refused(HttpMethod.Post, "recommendations", """{"playlist":999}""", HttpStatusCode.NotFound, "notFound");
        await Refused(HttpMethod.Post, "recommendations", "{}", HttpStatusCode.BadRequest, "invalidRequest",
            "exactly one of track or playlist");
        await Refused(HttpMethod.Post, "recommendations", $$"""{"track":{{soWhat.ToJsonString()}},"playlist":{{playlist}}}""",
            HttpStatusCode.BadRequest, "invalidRequest", "exactly one of track or playlist");
        await Refused(HttpMethod.Post, "recommendations", $$"""{"playlist":{{playlist}},"limit":0}""",
            HttpStatusCode.BadRequest, "invalidRequest", "limit: 1 to 100, was 0");
        await Refused(HttpMethod.Post, "recommendations", """{"seed":{}}""", HttpStatusCode.BadRequest, "invalidRequest",
            "unknown field seed");
        await Refused(HttpMethod.Get, "recommendations", HttpStatusCode.MethodNotAllowed, "methodNotAllowed");
    }

    // --- library folders ------------------------------------------------------

    [Fact]
    public async Task An_smb_folder_is_added_listed_edited_scanned_and_removed_and_its_password_is_never_answered()
    {
        using var nas = new FakeNas(Directory.CreateDirectory(Path.Combine(_dir.Path, "nas")).FullName);
        Directory.CreateDirectory(Path.Combine(nas.Root, "Jazz"));
        Directory.CreateDirectory(Path.Combine(nas.Root, "Rock"));
        LibraryTests.WriteTrack(Path.Combine(nas.Root, "Jazz", "blue.wav"), "Blue Nas Song");
        await StartAsync();
        var login = $"\"host\":\"{FakeNas.Host}\",\"port\":{nas.Port},\"user\":\"{FakeNas.User}\",\"password\":\"{FakeNas.Password}\"";
        var answers = new List<string>();
        async Task<JsonNode> Answered(HttpMethod method, string path, string? json = null)
        {
            var answer = await Send(method, path, json);
            answers.Add(answer.Text);
            Assert.True(answer.Status is HttpStatusCode.OK or HttpStatusCode.Created or HttpStatusCode.Accepted,
                $"{method} {path}: {(int)answer.Status} {answer.Text}");
            return answer.Body!;
        }

        var shares = await Answered(HttpMethod.Post, "library/smb/shares", $$"""{{{login}}}""");
        Assert.Contains("music", shares.AsArray().Select(s => s!.GetValue<string>()));
        var directories = await Answered(HttpMethod.Post, "library/smb/directories", $$"""{{{login}},"share":"music"}""");
        Assert.Equal(["Jazz", "Rock"], directories.AsArray().Select(s => s!.GetValue<string>()));
        await Refused(HttpMethod.Post, "library/smb/shares",
            $$"""{"host":"{{FakeNas.Host}}","port":{{nas.Port}},"user":"{{FakeNas.User}}","password":"nope"}""",
            HttpStatusCode.Forbidden, "authFailed");
        await Refused(HttpMethod.Post, "library/smb/shares", """{"host":"nas:445"}""", HttpStatusCode.BadRequest, "invalidArgument");

        using var stream = await EventsAsync(null);
        var added = await Send(HttpMethod.Post, "library/folders", $$"""{"type":"smb",{{login}},"share":"music","path":"Jazz"}""");
        answers.Add(added.Text);
        Assert.Equal(HttpStatusCode.Created, added.Status);
        var id = added.Body!["id"]!.GetValue<string>();
        Assert.True(added.Body["hasPassword"]!.GetValue<bool>());
        Assert.Equal($"smb://{FakeNas.Host}:{nas.Port}/music/Jazz", added.Body["path"]!.GetValue<string>());
        Assert.Equal(id, JsonNode.Parse((await stream.NextAsync(e => e.Type == "library.folderChanged")).Data)!["id"]!.GetValue<string>());
        await Until(async () => (await Answered(HttpMethod.Get, "providers/local/search?q=blue"))["tracks"]!.AsArray().Count == 1);

        var folders = (await Answered(HttpMethod.Get, "library/folders")).AsArray();
        Assert.Equal(["config:0", id], folders.Select(f => f!["id"]!.GetValue<string>()));
        Assert.Equal(("config", "api"), (folders[0]!["origin"]!.GetValue<string>(), folders[1]!["origin"]!.GetValue<string>()));
        Assert.Equal("ready", folders[1]!["state"]!.GetValue<string>());
        Assert.Equal(1, folders[1]!["indexed"]!.GetValue<long>());

        await Answered(HttpMethod.Patch, $"library/folders/{id}", """{"password":"wrong-one"}""");
        Assert.Equal(id, (await Answered(HttpMethod.Post, $"library/folders/{id}/scan"))["id"]!.GetValue<string>());
        await Until(async () => (await Answered(HttpMethod.Get, "library/folders"))[1]!["state"]!.GetValue<string>() == "authFailed");
        Assert.Single((await Answered(HttpMethod.Get, "providers/local/search?q=blue"))["tracks"]!.AsArray());
        await Answered(HttpMethod.Patch, $"library/folders/{id}", $$"""{"password":"{{FakeNas.Password}}"}""");
        await Until(async () => (await Answered(HttpMethod.Get, "library/folders"))[1]!["state"]!.GetValue<string>() == "ready");

        await Refused(HttpMethod.Patch, "library/folders/config:0", """{"enabled":false}""", HttpStatusCode.BadRequest,
            "invalidArgument");
        await Refused(HttpMethod.Post, "library/folders", $$"""{"type":"smb",{{login}},"share":"MUSIC","path":"jazz/sub"}""",
            HttpStatusCode.BadRequest, "invalidArgument");
        await Refused(HttpMethod.Post, "library/folders", """{"type":"local","path":"relative"}""", HttpStatusCode.BadRequest,
            "invalidArgument");
        await Refused(HttpMethod.Post, "library/folders", """{"type":"local","path":"C:\\x","host":"nas"}""",
            HttpStatusCode.BadRequest, "invalidArgument");
        await Refused(HttpMethod.Post, "library/folders", """{"type":"smb","host":"nas","share":"m","secret":"x"}""",
            HttpStatusCode.BadRequest, "invalidRequest", "unknown field secret");

        Assert.Equal(HttpStatusCode.NoContent, (await Send(HttpMethod.Delete, $"library/folders/{id}")).Status);
        await Until(async () => (await Answered(HttpMethod.Get, "providers/local/search?q=blue"))["tracks"]!.AsArray().Count == 0);
        await Refused(HttpMethod.Delete, $"library/folders/{id}", HttpStatusCode.NotFound, "notFound");
        await Refused(HttpMethod.Post, "library/folders/99/scan", HttpStatusCode.NotFound, "notFound");

        Assert.All(answers, a => Assert.DoesNotContain(FakeNas.Password, a, StringComparison.Ordinal));
        Assert.DoesNotContain(FakeNas.Password, await ReadLog(), StringComparison.Ordinal);
        Assert.DoesNotContain("wrong-one", await ReadLog(), StringComparison.Ordinal);
    }

    // --- AI providers (phase 21) -----------------------------------------------

    // A vendor that speaks Chat Completions, added from a test assembly: a
    // subclass, and one line to register it.
    private sealed class FixtureAi(AiProviderContext context) : OpenAiCompatibleProvider(context)
    {
        public const string ProviderId = "fixture";

        public override string Id => ProviderId;

        public override string DisplayName => "Fixture AI";

        protected override Uri DefaultBaseUrl { get; } = new("http://127.0.0.1:9/v1/");
    }

    [Fact]
    public async Task Ai_providers_are_listed_their_keys_set_and_removed_and_their_models_listed_without_a_key_leaking()
    {
        const string wrong = "sk-wrong-never-logged";
        await using var vendor = await FakeOpenAi.StartAsync();
        await StartAsync(sections: $$"""
            , "ai": { "providers": [ { "id": "fixture", "baseUrl": "{{vendor.BaseUri}}" },
                                     { "id": "anthropic", "apiKey": "sk-config-never-answered" } ] }
            """, services: s => s.AddAiProvider(FixtureAi.ProviderId, c => new FixtureAi(c), TimeSpan.FromSeconds(5)));

        var providers = (await Ok(HttpMethod.Get, "ai/providers")).AsArray();
        Assert.Equal(["openai", "anthropic", "gemini", "openrouter", "groq", "mistral", "ollama", "fixture"],
            providers.Select(p => p!["id"]!.GetValue<string>()));
        var anthropic = providers[1]!;
        Assert.Equal((true, true, "config"), (anthropic["hasKey"]!.GetValue<bool>(),
            anthropic["configured"]!.GetValue<bool>(), anthropic["origin"]!.GetValue<string>()));
        var ollama = providers[6]!;
        Assert.Equal(["none"], ollama["authMethods"]!.AsArray().Select(m => m!.GetValue<string>()));
        Assert.True(ollama["configured"]!.GetValue<bool>());
        var fixture = providers[7]!;
        Assert.Equal((false, false, "none", true), (fixture["configured"]!.GetValue<bool>(),
            fixture["hasKey"]!.GetValue<bool>(), fixture["origin"]!.GetValue<string>(), fixture["enabled"]!.GetValue<bool>()));
        Assert.Null(fixture["authorised"]);
        await Refused(HttpMethod.Get, "ai/providers/fixture/models", HttpStatusCode.Conflict, "notConfigured");

        // A wrong key: the vendor's refusal is ours to summarise, never to relay.
        var set = await Ok(HttpMethod.Put, "ai/providers/fixture/key", $$"""{"key":"{{wrong}}"}""");
        Assert.Equal((true, "api"), (set["hasKey"]!.GetValue<bool>(), set["origin"]!.GetValue<string>()));
        var refused = await Send(HttpMethod.Get, "ai/providers/fixture/models");
        Assert.Equal(HttpStatusCode.Forbidden, refused.Status);
        Assert.Equal("authFailed", refused.Body!["error"]!.GetValue<string>());
        var afterRefusal = (await Ok(HttpMethod.Get, "ai/providers")).AsArray()[7]!;
        Assert.Equal((false, true), (afterRefusal["authorised"]!.GetValue<bool>(), afterRefusal["reachable"]!.GetValue<bool>()));

        await Ok(HttpMethod.Put, "ai/providers/fixture/key", $$"""{"key":"{{FakeOpenAi.Key}}"}""");
        var models = await Ok(HttpMethod.Get, "ai/providers/fixture/models?refresh=true");
        Assert.Equal(["model-a", "model-b"], models.AsArray().Select(m => m!["id"]!.GetValue<string>()));
        var ready = (await Ok(HttpMethod.Get, "ai/providers")).AsArray()[7]!;
        Assert.Equal((true, true), (ready["authorised"]!.GetValue<bool>(), ready["reachable"]!.GetValue<bool>()));
        vendor.RateLimited = true;
        await Refused(HttpMethod.Get, "ai/providers/fixture/models?refresh=true", HttpStatusCode.TooManyRequests, "rateLimited");
        vendor.RateLimited = false;

        await Refused(HttpMethod.Put, "ai/providers/anthropic/key", """{"key":"other"}""", HttpStatusCode.BadRequest,
            "invalidArgument", "the key of anthropic is from the config file; change it there");
        await Refused(HttpMethod.Put, "ai/providers/ollama/key", """{"key":"k"}""", HttpStatusCode.BadRequest, "invalidArgument");
        await Refused(HttpMethod.Put, "ai/providers/nobody/key", """{"key":"k"}""", HttpStatusCode.NotFound, "notFound");
        await Refused(HttpMethod.Put, "ai/providers/fixture/key", """{"key":" "}""", HttpStatusCode.BadRequest, "invalidArgument");
        await Refused(HttpMethod.Put, "ai/providers/fixture/key", """{"key":"k","secret":1}""", HttpStatusCode.BadRequest,
            "invalidRequest", "unknown field secret");
        await Refused(HttpMethod.Get, "ai/providers/fixture/models?refresh=yes", HttpStatusCode.BadRequest, "invalidRequest");

        var removed = await Ok(HttpMethod.Delete, "ai/providers/fixture/key");
        Assert.Equal((false, "none"), (removed["hasKey"]!.GetValue<bool>(), removed["origin"]!.GetValue<string>()));
        Assert.Null(removed["authorised"]);

        var status = await Ok(HttpMethod.Get, "status");
        var usage = status["ai"]!["providers"]!.AsArray().Single(p => p!["id"]!.GetValue<string>() == "fixture")!;
        Assert.Equal((4, 3), (usage["calls"]!.GetValue<int>(), usage["failures"]!.GetValue<int>()));

        var everything = string.Join('\n', (await Ok(HttpMethod.Get, "ai/providers")).ToJsonString(),
            status.ToJsonString(), refused.Text, await ReadLog());
        Assert.DoesNotContain(wrong, everything, StringComparison.Ordinal);
        Assert.DoesNotContain(FakeOpenAi.Key, everything, StringComparison.Ordinal);
        Assert.DoesNotContain("sk-config-never-answered", everything, StringComparison.Ordinal);
    }

    [Fact]
    public async Task Ai_settings_name_the_default_and_switch_providers_off_and_are_kept()
    {
        await StartAsync();

        var initial = await Ok(HttpMethod.Get, "ai/settings");
        Assert.Null(initial["defaultProvider"]);
        Assert.Empty(initial["disabledProviders"]!.AsArray());

        var patched = await Ok(HttpMethod.Patch, "ai/settings",
            """{"defaultProvider":"ollama","defaultModel":" llama3.2 ","disabledProviders":["groq","groq"]}""");
        Assert.Equal(("ollama", "llama3.2"), (patched["defaultProvider"]!.GetValue<string>(),
            patched["defaultModel"]!.GetValue<string>()));
        Assert.Equal(["groq"], patched["disabledProviders"]!.AsArray().Select(p => p!.GetValue<string>()));
        var kept = await Ok(HttpMethod.Patch, "ai/settings", """{"defaultModel":""}""");
        Assert.Equal("ollama", kept["defaultProvider"]!.GetValue<string>());
        Assert.Null(kept["defaultModel"]);

        var groq = (await Ok(HttpMethod.Get, "ai/providers")).AsArray().Single(p => p!["id"]!.GetValue<string>() == "groq")!;
        Assert.False(groq["enabled"]!.GetValue<bool>());
        await Refused(HttpMethod.Get, "ai/providers/groq/models", HttpStatusCode.Conflict, "notConfigured",
            "groq is switched off in the AI settings");
        await Refused(HttpMethod.Patch, "ai/settings", """{"defaultProvider":"nobody"}""", HttpStatusCode.NotFound, "notFound");
        await Refused(HttpMethod.Patch, "ai/settings", """{"disabledProviders":["nobody"]}""", HttpStatusCode.NotFound, "notFound");

        var file = await File.ReadAllTextAsync(Path.Combine(_dir.Path, "data", "ai", "settings.json"), Ct);
        Assert.Contains("\"defaultProvider\":\"ollama\"", file, StringComparison.Ordinal);
    }

    // --- AI logins (phase 22) -------------------------------------------------

    // A vendor with a browser login, added from a test assembly: a subclass
    // listing oauth, and its login registered with one line.
    private sealed class FixtureLoginAi(AiProviderContext context) : OpenAiCompatibleProvider(context)
    {
        public const string ProviderId = "fixture-login";

        public override string Id => ProviderId;

        public override string DisplayName => "Fixture login";

        public override IReadOnlyList<AiAuthMethod> AuthMethods => [AiAuthMethod.ApiKey, AiAuthMethod.OAuth];

        protected override Uri DefaultBaseUrl { get; } = new("http://127.0.0.1:9/v1/");
    }

    [Fact]
    public async Task Ai_logins_complete_through_the_app_or_the_callback_without_the_token_and_leak_nothing()
    {
        const string token = "api-t0ken-for-logins";
        await using var oauth = await FakeOAuth.StartAsync();
        await using var vendor = await FakeOpenAi.StartAsync();
        vendor.Accepts = t => t == oauth.CurrentAccess;
        await StartAsync($$""", "token": "{{token}}" """, $$"""
            , "ai": { "providers": [ { "id": "fixture-login", "baseUrl": "{{vendor.BaseUri}}" } ] }
            """, s => s.AddAiProvider(FixtureLoginAi.ProviderId, c => new FixtureLoginAi(c), TimeSpan.FromSeconds(5))
                .AddAiLogin(FixtureLoginAi.ProviderId, h => new GoogleLogin(h, oauth.Google)));
        Http.DefaultRequestHeaders.Authorization = new AuthenticationHeaderValue("Bearer", token);
        var answers = new List<string>();
        var listed = (await Ok(HttpMethod.Get, "ai/providers")).AsArray();
        Assert.Equal(["apiKey", "oauth"], listed.Single(p => p!["id"]!.GetValue<string>() == FixtureLoginAi.ProviderId)!
            ["authMethods"]!.AsArray().Select(m => m!.GetValue<string>()));

        await Refused(HttpMethod.Post, "ai/providers/fixture-login/login", HttpStatusCode.Conflict, "notConfigured");
        var client = await Ok(HttpMethod.Put, "ai/providers/fixture-login/client",
            $$"""{"clientId":"{{FakeOAuth.ClientId}}","clientSecret":"{{FakeOAuth.ClientSecret}}"}""");
        Assert.Equal((true, false), (client["hasClient"]!.GetValue<bool>(), client["hasToken"]!.GetValue<bool>()));
        answers.Add(client.ToJsonString());
        await Refused(HttpMethod.Put, "ai/providers/openai/client", """{"clientId":"x"}""", HttpStatusCode.BadRequest,
            "invalidArgument", "openai takes no OAuth client");
        await Refused(HttpMethod.Post, "ai/providers/groq/login", HttpStatusCode.BadRequest, "invalidArgument",
            "groq has no browser login");
        await Refused(HttpMethod.Post, "ai/providers/fixture-login/login", """{"redirectUri":"not a url"}""",
            HttpStatusCode.BadRequest, "invalidArgument");

        // A browser on this machine: the redirect is this server's callback,
        // which takes no bearer token. Google adds parameters of its own.
        var start = await Ok(HttpMethod.Post, "ai/providers/fixture-login/login");
        var callback = new Uri(Http.BaseAddress!, "ai/login/callback");
        Assert.Equal(callback.AbsoluteUri, start["redirectUri"]!.GetValue<string>());
        var code = oauth.Consent(new Uri(start["authorizeUrl"]!.GetValue<string>()));
        using var browser = new HttpClient(new SocketsHttpHandler { UseProxy = false });
        var state = start["state"]!.GetValue<string>();
        var redirect = new Uri($"{callback.AbsoluteUri}?state={state}&code={Uri.EscapeDataString(code)}&scope=x&authuser=0");
        using (var landed = await browser.GetAsync(redirect, Ct))
        {
            var text = await landed.Content.ReadAsStringAsync(Ct);
            Assert.True(landed.StatusCode == HttpStatusCode.OK, text);
            Assert.True(JsonNode.Parse(text)!["hasToken"]!.GetValue<bool>());
            answers.Add(text);
        }
        using (var again = await browser.GetAsync(redirect, Ct))
            Assert.Equal(HttpStatusCode.BadRequest, again.StatusCode);
        using (var other = await browser.GetAsync(new Uri(Http.BaseAddress!, "ai/providers"), Ct))
            Assert.Equal(HttpStatusCode.Unauthorized, other.StatusCode);

        // The app's own page: it posts the code it was brought.
        var second = await Ok(HttpMethod.Post, "ai/providers/fixture-login/login", """{"redirectUri":"http://app.local/done"}""");
        Assert.Equal("http://app.local/done", second["redirectUri"]!.GetValue<string>());
        var complete = $$"""{"state":"{{second["state"]}}","code":"{{oauth.Consent(new Uri(second["authorizeUrl"]!.GetValue<string>()))}}"}""";
        var done = await Ok(HttpMethod.Post, "ai/login/complete", complete);
        Assert.Equal((true, false, "api"), (done["hasToken"]!.GetValue<bool>(), done["hasKey"]!.GetValue<bool>(),
            done["origin"]!.GetValue<string>()));
        answers.Add(done.ToJsonString());
        await Refused(HttpMethod.Post, "ai/login/complete", complete, HttpStatusCode.BadRequest, "invalidArgument",
            "state: unknown or already used; start the login again");
        await Refused(HttpMethod.Post, "ai/login/complete", """{"state":"s"}""", HttpStatusCode.BadRequest, "invalidRequest");

        // The vendor is asked with the token.
        var models = await Ok(HttpMethod.Get, "ai/providers/fixture-login/models?refresh=true");
        Assert.Equal(2, models.AsArray().Count);
        var status = await Ok(HttpMethod.Get, "status");
        Assert.Equal((0, 16), (status["ai"]!["pendingLogins"]!.GetValue<int>(), status["ai"]!["maxPendingLogins"]!.GetValue<int>()));
        answers.Add(status.ToJsonString());
        answers.Add((await Ok(HttpMethod.Get, "ai/providers")).ToJsonString());

        // Logout: forgotten and revoked; the client stays until removed.
        var loggedOut = await Ok(HttpMethod.Delete, "ai/providers/fixture-login/key");
        Assert.Equal((false, true), (loggedOut["hasToken"]!.GetValue<bool>(), loggedOut["hasClient"]!.GetValue<bool>()));
        Assert.NotEmpty(oauth.RevokedTokens);
        var removed = await Ok(HttpMethod.Delete, "ai/providers/fixture-login/client");
        Assert.False(removed["hasClient"]!.GetValue<bool>());

        var everything = string.Join('\n', answers) + await ReadLog();
        Assert.All(oauth.Issued, secret => Assert.DoesNotContain(secret, everything, StringComparison.Ordinal));
        Assert.DoesNotContain(FakeOAuth.ClientSecret, everything, StringComparison.Ordinal);
        Assert.DoesNotContain(token, everything, StringComparison.Ordinal);
    }

    // --- events ---------------------------------------------------------------

    [Fact]
    public async Task The_event_stream_forwards_queue_core_and_indexer_events_in_order_and_replays_after_an_id()
    {
        await StartAsync();
        var track = _fake.AddTrack(_dir.Write("a.flac", "a"), "A");

        using (var stream = await EventsAsync(null))
        {
            await Ok(HttpMethod.Post, "zones/living/queue/items", $$"""{"tracks":[{{Json(track)}}]}""");
            var changed = await stream.NextAsync(e => e.Type == "queue.changed");
            Assert.Equal("A", JsonNode.Parse(changed.Data)!["items"]![0]!["title"]!.GetValue<string>());

            await _core.EmitAsync("""{"event":"room.connected","client":"roomB","deviceName":"Speaker","reconnect":false}""");
            var connected = await stream.NextAsync(e => e.Type == "room.connected");
            Assert.Equal("roomB", JsonNode.Parse(connected.Data)!["client"]!.GetValue<string>());
            Assert.True(connected.Id > changed.Id);

            await Ok(HttpMethod.Delete, "zones/living/queue/items/" +
                JsonNode.Parse(changed.Data)!["items"]![0]!["id"]!.GetValue<long>());
            Assert.Equal("queue.changed", (await stream.NextAsync(_ => true)).Type);
        }

        // From before the queue was touched: everything since is replayed, the
        // indexer's first progress included.
        using var replay = await EventsAsync(0);
        var types = new List<string>();
        while (types.LastOrDefault() != "room.connected")
            types.Add((await replay.NextAsync(_ => true)).Type);
        Assert.Contains("indexer.progress", types);
        Assert.Contains("core.synchronised", types);
        Assert.True(types.IndexOf("queue.changed") < types.IndexOf("room.connected"));

        await Refused(HttpMethod.Get, "events?since=1", HttpStatusCode.BadRequest, "invalidRequest",
            "unknown query parameter since");
    }

    // --- system metrics (phase 18) ----------------------------------------------

    [Fact]
    public async Task System_answers_this_server_the_core_and_every_room_and_streams_them()
    {
        _core.StatusBody = """
            {"server":{"version":"fake","uptimeMs":1,"zones":0,"clients":2,"connected":1,"members":0,
               "system":{"cpuPercent":3.5,"systemCpuPercent":20.0,"memoryBytes":16777216,
                 "systemMemoryTotalBytes":8589934592,"systemMemoryAvailableBytes":4294967296,"uptimeMs":60000}},
             "clients":[{"clientId":"roomA","connected":true,"member":false,
               "settings":{"audioControl":"bitPerfectFallback","driftCorrection":true,"audioControlIsDefault":true,"driftCorrectionIsDefault":true},
               "driftCorrection":{"requested":true},
               "system":{"memoryBytes":14680064,"privateMemoryBytes":10485760,"threads":12,"handles":240,
                 "uptimeMs":5000,"systemUptimeMs":172800000,"temperatureC":51.5,
                 "wifiRssiDbm":-67,"wifiChannel":11,"wifiBssid":"aa:bb:cc:dd:ee:ff","wifiRxRateKbps":72200,"networkLink":"wifi",
                 "cpuArchitecture":"armv6l","cpuModel":"ARMv6-compatible processor rev 7 (v6l)",
                 "machineModel":"Raspberry Pi Model B Plus Rev 1.2","cpuCores":1,"cpuMhz":700,
                 "internalMemoryTotalBytes":327680,"internalMemoryAvailableBytes":65536,
                 "psramTotalBytes":4194304,"psramAvailableBytes":2097152},"systemAgeMs":700},
              {"clientId":"roomB","connected":false,"member":false,
               "settings":{"audioControl":"bitPerfectFallback","driftCorrection":true,"audioControlIsDefault":true,"driftCorrectionIsDefault":true},
               "driftCorrection":{"requested":true}}],
             "zones":[]}
            """;
        await StartAsync();

        // Rates need two samples, so the second one is waited for.
        JsonNode system = null!;
        await Until(async () => (system = await Ok(HttpMethod.Get, "system"))["server"]!["cpuPercent"] is not null);
        var server = system["server"]!;
        Assert.InRange(server["cpuPercent"]!.GetValue<double>(), 0.0, 100.0);
        Assert.True(server["workingSetBytes"]!.GetValue<ulong>() > 0);
        Assert.True(server["managedHeapBytes"]!.GetValue<ulong>() > 0);
        Assert.True(server["threads"]!.GetValue<int>() > 0);
        Assert.NotNull(server["uptimeMs"]);
        Assert.True(server["privateMemoryBytes"]!.GetValue<ulong>() > 0);
        Assert.True(server["handles"]!.GetValue<int>() > 0);
        Assert.InRange(server["systemCpuPercent"]!.GetValue<double>(), 0.0, 100.0);
        Assert.True(server["systemMemoryAvailableBytes"]!.GetValue<ulong>() <= server["systemMemoryTotalBytes"]!.GetValue<ulong>());
        Assert.True(server["systemUptimeMs"]!.GetValue<ulong>() >= server["uptimeMs"]!.GetValue<ulong>());
        Assert.True(server["gen0Collections"]!.GetValue<int>() >= server["gen2Collections"]!.GetValue<int>());
        // What the machine is, as the core names it.
        Assert.Equal((ulong)Environment.ProcessorCount, server["cpuCores"]!.GetValue<ulong>());
        Assert.False(string.IsNullOrWhiteSpace(server["cpuArchitecture"]!.GetValue<string>()));
        if (OperatingSystem.IsWindows())
        {
            Assert.False(string.IsNullOrWhiteSpace(server["cpuModel"]!.GetValue<string>()));
            Assert.True(server["cpuMhz"]!.GetValue<ulong>() > 0);
        }

        Assert.Equal(16777216UL, system["core"]!["memoryBytes"]!.GetValue<ulong>());
        Assert.Equal(3.5, system["core"]!["cpuPercent"]!.GetValue<double>());
        var rooms = system["rooms"]!.AsArray();
        var roomA = rooms.Single(r => r!["client"]!.GetValue<string>() == "roomA")!;
        Assert.Equal(51.5, roomA["metrics"]!["temperatureC"]!.GetValue<double>());
        Assert.Equal(12UL, roomA["metrics"]!["threads"]!.GetValue<ulong>());
        Assert.Equal(240UL, roomA["metrics"]!["handles"]!.GetValue<ulong>());
        Assert.Equal(10485760UL, roomA["metrics"]!["privateMemoryBytes"]!.GetValue<ulong>());
        Assert.Equal(172800000UL, roomA["metrics"]!["systemUptimeMs"]!.GetValue<ulong>());
        Assert.Equal(-67, roomA["metrics"]!["wifiRssiDbm"]!.GetValue<int>());
        Assert.Equal("wifi", roomA["metrics"]!["networkLink"]!.GetValue<string>());
        Assert.Equal(11UL, roomA["metrics"]!["wifiChannel"]!.GetValue<ulong>());
        Assert.Equal("aa:bb:cc:dd:ee:ff", roomA["metrics"]!["wifiBssid"]!.GetValue<string>());
        Assert.Equal(327680UL, roomA["metrics"]!["internalMemoryTotalBytes"]!.GetValue<ulong>());
        Assert.Equal(65536UL, roomA["metrics"]!["internalMemoryAvailableBytes"]!.GetValue<ulong>());
        Assert.Equal(4194304UL, roomA["metrics"]!["psramTotalBytes"]!.GetValue<ulong>());
        Assert.Equal(2097152UL, roomA["metrics"]!["psramAvailableBytes"]!.GetValue<ulong>());
        Assert.Equal(72200UL, roomA["metrics"]!["wifiRxRateKbps"]!.GetValue<ulong>());
        Assert.Equal("armv6l", roomA["metrics"]!["cpuArchitecture"]!.GetValue<string>());
        Assert.Equal("ARMv6-compatible processor rev 7 (v6l)", roomA["metrics"]!["cpuModel"]!.GetValue<string>());
        Assert.Equal("Raspberry Pi Model B Plus Rev 1.2", roomA["metrics"]!["machineModel"]!.GetValue<string>());
        Assert.Equal(1UL, roomA["metrics"]!["cpuCores"]!.GetValue<ulong>());
        Assert.Equal(700UL, roomA["metrics"]!["cpuMhz"]!.GetValue<ulong>());
        Assert.Null(roomA["metrics"]!["cpuPercent"]); // what the client did not say stays unsaid
        Assert.Equal(700UL, roomA["ageMs"]!.GetValue<ulong>());
        Assert.Null(roomA["reason"]);
        var roomB = rooms.Single(r => r!["client"]!.GetValue<string>() == "roomB")!;
        Assert.Null(roomB["metrics"]);
        Assert.Equal("notConnected", roomB["reason"]!.GetValue<string>());

        var actual = (await Ok(HttpMethod.Get, "rooms/roomA"))["actual"]!;
        Assert.Equal(14680064UL, actual["system"]!["memoryBytes"]!.GetValue<ulong>());
        Assert.Equal(700UL, actual["systemAgeMs"]!.GetValue<ulong>());

        using var stream = await EventsAsync(null);
        var metrics = JsonNode.Parse((await stream.NextAsync(e => e.Type == "system.metrics")).Data)!;
        Assert.Equal(16777216UL, metrics["core"]!["memoryBytes"]!.GetValue<ulong>());
        Assert.Equal(2, metrics["rooms"]!.AsArray().Count);
    }

    [Fact]
    public async Task Rooms_sync_streams_every_connected_rooms_sync_and_buffer_named_as_in_rooms()
    {
        // The constructor's roomA, and roomB, which is not connected.
        _core.StatusBody = _core.StatusBody.Replace("\"clients\":[{", """
            "clients":[{"clientId":"roomB","connected":false,"member":false,
               "settings":{"audioControl":"bitPerfectFallback","driftCorrection":true,"audioControlIsDefault":true,"driftCorrectionIsDefault":true},
               "driftCorrection":{"requested":true}},{
            """, StringComparison.Ordinal);
        await StartAsync();

        using var stream = await EventsAsync(null);
        var sync = JsonNode.Parse((await stream.NextAsync(e => e.Type == "rooms.sync")).Data)!;
        var roomA = sync["rooms"]!.AsArray().Single()!;
        Assert.Equal("roomA", roomA["client"]!.GetValue<string>());
        Assert.Equal(-3.25, roomA["sync"]!["driftPpm"]!.GetValue<double>());
        Assert.Equal(1.4, roomA["sync"]!["appliedPpm"]!.GetValue<double>());
        Assert.Equal(12.5, roomA["sync"]!["offsetUs"]!.GetValue<double>());
        Assert.Null(roomA["sync"]!["standardErrorPpm"]); // not measured: absent
        Assert.Equal(120UL, roomA["bufferedMs"]!.GetValue<ulong>());
        Assert.Equal(110UL, roomA["playback"]!["bufferedMs"]!.GetValue<ulong>());
        Assert.Equal(2UL, roomA["playback"]!["underruns"]!.GetValue<ulong>());
        Assert.Equal(1UL, roomA["rejoins"]!.GetValue<ulong>());

        // The same names as the room's actual half.
        var actual = (await Ok(HttpMethod.Get, "rooms/roomA"))["actual"]!;
        Assert.Equal(roomA["sync"]!["driftPpm"]!.GetValue<double>(), actual["sync"]!["driftPpm"]!.GetValue<double>());
        Assert.Equal(roomA["bufferedMs"]!.GetValue<ulong>(), actual["bufferedMs"]!.GetValue<ulong>());
        Assert.Equal(roomA["rejoins"]!.GetValue<ulong>(), actual["rejoins"]!.GetValue<ulong>());
    }

    [Fact]
    public async Task System_answers_the_cores_audio_memory_and_each_zones_share()
    {
        _core.StatusBody = _core.StatusBody
            .Replace("\"members\":1}", "\"members\":1,\"memoryUsedBytes\":3145728,\"memoryLimitBytes\":67108864}", StringComparison.Ordinal)
            .Replace("\"startsSent\":0,\"rejoins\":0}", "\"startsSent\":0,\"rejoins\":0,\"blocks\":96,\"memoryBytes\":3145728,\"blocksRefused\":2}",
                StringComparison.Ordinal);
        await StartAsync();

        JsonNode system = null!;
        await Until(async () => (system = await Ok(HttpMethod.Get, "system"))["coreMemory"] is not null);
        var memory = system["coreMemory"]!;
        Assert.Equal((3145728UL, 67108864UL), (memory["usedBytes"]!.GetValue<ulong>(), memory["limitBytes"]!.GetValue<ulong>()));
        var zone = memory["zones"]!.AsArray().Single()!;
        Assert.Equal(("living", 3145728UL, 96UL, 2UL), (zone["zone"]!.GetValue<string>(), zone["bytes"]!.GetValue<ulong>(),
            zone["blocks"]!.GetValue<ulong>(), zone["blocksRefused"]!.GetValue<ulong>()));
        Assert.Equal(3145728UL, (await Ok(HttpMethod.Get, "zones/living"))["actual"]!["memoryBytes"]!.GetValue<ulong>());
    }

    [Fact]
    public async Task An_unstable_room_is_answered_as_its_health_on_the_room_and_the_zone_and_streamed()
    {
        _core.StatusBody = _core.StatusBody.Replace("\"rejoins\":1,", "\"rejoins\":1,\"unstable\":false,\"backoffMs\":0,",
            StringComparison.Ordinal);
        await StartAsync();
        Assert.Equal("ok", (await Ok(HttpMethod.Get, "rooms/roomA"))["actual"]!["health"]!["state"]!.GetValue<string>());

        using var stream = await EventsAsync(null);
        var asked = _core.All.Count;
        _core.StatusBody = _core.StatusBody.Replace("\"unstable\":false,\"backoffMs\":0,",
            "\"unstable\":true,\"backoffMs\":4000,\"episodeRejoins\":3,\"unstableSinceNs\":5,", StringComparison.Ordinal);
        await _core.EmitAsync("""{"event":"room.unstable","client":"roomA","zone":"living","trigger":"evicted","rejoins":3,"backoffMs":4000,"detail":"stranded"}""");
        var streamed = JsonNode.Parse((await stream.NextAsync(e => e.Type == "room.unstable")).Data)!;
        Assert.Equal(("roomA", 3), (streamed["client"]!.GetValue<string>(), streamed["rejoins"]!.GetValue<int>()));

        var health = (await Ok(HttpMethod.Get, "rooms/roomA"))["actual"]!["health"]!;
        Assert.Equal(("unstable", 3, 4000, "evicted", "stranded"), (health["state"]!.GetValue<string>(),
            health["rejoins"]!.GetValue<int>(), health["backoffMs"]!.GetValue<int>(), health["trigger"]!.GetValue<string>(),
            health["detail"]!.GetValue<string>()));
        Assert.NotNull(health["since"]);
        var zone = (await Ok(HttpMethod.Get, "zones/living"))["actual"]!["health"]!["roomA"]!;
        Assert.Equal("unstable", zone["state"]!.GetValue<string>());

        // Nothing is done about it: the core is only read.
        Assert.All(_core.All.Skip(asked), r => Assert.Contains(r.Command, (string[])["status", "clients", "zones"]));

        _core.StatusBody = _core.StatusBody.Replace("\"unstable\":true,\"backoffMs\":4000,\"episodeRejoins\":3,\"unstableSinceNs\":5,",
            "\"unstable\":false,\"backoffMs\":0,", StringComparison.Ordinal);
        await _core.EmitAsync("""{"event":"room.stable","client":"roomA","zone":"living","rejoins":3,"unstableForMs":30000}""");
        await stream.NextAsync(e => e.Type == "room.stable");
        Assert.Equal("ok", (await Ok(HttpMethod.Get, "rooms/roomA"))["actual"]!["health"]!["state"]!.GetValue<string>());
    }

    [Fact]
    public async Task System_leaves_the_core_absent_for_a_core_that_predates_it_and_says_why_a_room_has_none()
    {
        await StartAsync(); // the constructor's status: no system blocks anywhere

        // The first sample may predate CoreLink's first connection, and so name no room.
        JsonNode system = null!;
        await Until(async () => (system = await Ok(HttpMethod.Get, "system"))["rooms"]!.AsArray().Count > 0);
        Assert.True(system["server"]!["workingSetBytes"]!.GetValue<ulong>() > 0);
        Assert.Null(system["core"]);
        var roomA = system["rooms"]!.AsArray().Single()!;
        Assert.Equal("roomA", roomA["client"]!.GetValue<string>());
        Assert.Null(roomA["metrics"]);
        Assert.Equal("noReports", roomA["reason"]!.GetValue<string>());
        Assert.Null((await Ok(HttpMethod.Get, "rooms/roomA"))["actual"]!["system"]);
    }

    [Fact]
    public async Task A_subscriber_that_stops_reading_costs_a_bounded_queue_and_gets_events_lost_never_a_stalled_server()
    {
        await StartAsync();
        var events = _host!.Services.GetRequiredService<ApiEvents>();

        using var socket = new Socket(AddressFamily.InterNetwork, SocketType.Stream, ProtocolType.Tcp) { ReceiveBufferSize = 4096 };
        await socket.ConnectAsync(IPAddress.Loopback, Http.BaseAddress!.Port, Ct);
        await socket.SendAsync(Encoding.ASCII.GetBytes("GET /api/v1/events HTTP/1.1\r\nHost: x\r\n\r\n"), Ct);
        var deadline = DateTime.UtcNow + Patience;
        while (events.Status.Subscribers == 0)
        {
            Assert.True(DateTime.UtcNow < deadline, "the subscriber never arrived");
            await Task.Delay(10, Ct);
        }

        var payload = new { text = new string('x', 2000) };
        var clock = Stopwatch.StartNew();
        for (var i = 0; i < 3000; i++)
        {
            events.Publish("test.flood", payload);
            Assert.True(events.Status.Held <= ApiEvents.SubscriberCapacity);
        }
        clock.Stop();

        Assert.True(clock.Elapsed < TimeSpan.FromSeconds(5), $"publishing took {clock.Elapsed}");
        Assert.True(events.Status.Dropped > 0);
        Assert.True((await Ok(HttpMethod.Get, "status"))["core"]!["connected"]!.GetValue<bool>());

        // Reading on: the marker comes before the first event after the gap.
        var seen = new StringBuilder();
        var buffer = new byte[64 * 1024];
        deadline = DateTime.UtcNow + Patience;
        while (!seen.ToString().Contains("event: events.lost\ndata: {\"count\":", StringComparison.Ordinal))
        {
            Assert.True(DateTime.UtcNow < deadline, "no events.lost arrived");
            var n = await socket.ReceiveAsync(buffer, Ct);
            Assert.True(n > 0, "the stream closed");
            seen.Append(Encoding.UTF8.GetString(buffer, 0, n));
            if (seen.Length > 1_000_000)
                seen.Remove(0, seen.Length - 1000);
        }
    }

    // --- helpers --------------------------------------------------------------

    // `api` is added to the api section, `sections` to the file's top level.
    private async Task StartAsync(string api = "", string sections = "", Action<IServiceCollection>? services = null)
    {
        Directory.CreateDirectory(Path.Combine(_dir.Path, "music"));
        await File.WriteAllBytesAsync(Path.Combine(_dir.Path, "music", "broken.flac"), [1, 2, 3], Ct);
        var config = _dir.Write("server.json", $$"""
            { "dataDirectory": "data", "artwork": { "musicBrainz": false }, "core": { "port": {{_core.Port}}, "discoveryPort": 0 }, "media": { "port": 0 },
              "api": { "port": 0, "searchTimeout": "00:00:00.300" {{api}} }, "gui": { "port": 0 },
              "library": { "folders": [ { "path": "music" } ] } {{sections}} }
            """);
        var builder = ServerHost.CreateBuilder(["--config", config], TextWriter.Null).Value;
        builder.Services.AddProvider(FakeProvider.ProviderId, _ => _fake);
        builder.Services.AddProvider(SlowProvider.ProviderId, _ => new SlowProvider());
        // A second add-in is this one line (phase 12).
        builder.Services.AddAddIn(SleepyAddIn.AddInId, _ => new SleepyAddIn(), TimeSpan.FromMilliseconds(300));
        services?.Invoke(builder.Services);
        _host = builder.Build();
        await _host.StartAsync(Ct);

        var endpoint = _host.Services.GetRequiredService<ApiEndpoint>();
        _http = new HttpClient(new SocketsHttpHandler { UseProxy = false }) { BaseAddress = endpoint.BaseUri };
        var link = _host.Services.GetRequiredService<Playback.Core.CoreLink>();
        var deadline = DateTime.UtcNow + Patience;
        while (link.Model.Current.Rooms.Count == 0)
        {
            Assert.True(DateTime.UtcNow < deadline, "CoreLink never synchronised with FakeCore");
            await Task.Delay(10, Ct);
        }
    }

    private static async Task Until(Func<Task<bool>> condition)
    {
        var deadline = DateTime.UtcNow + Patience;
        while (!await condition())
        {
            Assert.True(DateTime.UtcNow < deadline, "timed out");
            await Task.Delay(20, Ct);
        }
    }

    private sealed record Answered(HttpStatusCode Status, string Text, JsonNode? Body);

    private async Task<Answered> Send(HttpMethod method, string path, string? json = null)
    {
        using var request = new HttpRequestMessage(method, path);
        if (json is not null)
            request.Content = new StringContent(json, Encoding.UTF8, "application/json");
        using var response = await Http.SendAsync(request, Ct);
        var text = await response.Content.ReadAsStringAsync(Ct);
        return new Answered(response.StatusCode, text, text.Length == 0 ? null : JsonNode.Parse(text));
    }

    private async Task<JsonNode> Ok(HttpMethod method, string path, string? json = null)
    {
        var answer = await Send(method, path, json);
        Assert.True(answer.Status is HttpStatusCode.OK or HttpStatusCode.Created,
            $"{method} {path}: {(int)answer.Status} {answer.Text}");
        return answer.Body!;
    }

    private Task Refused(HttpMethod method, string path, HttpStatusCode status, string error, string? message = null) =>
        Refused(method, path, null, status, error, message);

    // An error answer: its status, its code, and, when given, its message.
    private async Task Refused(HttpMethod method, string path, string? json, HttpStatusCode status, string error,
        string? message = null)
    {
        var answer = await Send(method, path, json);
        Assert.True(answer.Status == status, $"{method} {path}: {(int)answer.Status} {answer.Text}");
        Assert.Equal(error, answer.Body!["error"]!.GetValue<string>());
        if (message is not null)
            Assert.Equal(message, answer.Body["message"]!.GetValue<string>());
    }

    private async Task<string> ReadLog()
    {
        await using var file = new FileStream(Path.Combine(_dir.Path, "data", "server.log"), FileMode.Open,
            FileAccess.Read, FileShare.ReadWrite);
        using var reader = new StreamReader(file);
        return await reader.ReadToEndAsync(Ct);
    }

    private static string Json(object value) =>
        System.Text.Json.JsonSerializer.Serialize(value, value.GetType(), TestJson);

    private static readonly System.Text.Json.JsonSerializerOptions TestJson = new()
    {
        PropertyNamingPolicy = System.Text.Json.JsonNamingPolicy.CamelCase,
        DefaultIgnoreCondition = System.Text.Json.Serialization.JsonIgnoreCondition.WhenWritingNull,
    };

    private static List<string> Titles(JsonNode queue) =>
        [.. queue["items"]!.AsArray().Select(i => i!["title"]!.GetValue<string>())];

    private static List<string> Entries(JsonNode entries) =>
        [.. entries.AsArray().Select(e => e!["item"]!["title"]!.GetValue<string>())];

    private async Task<EventReader> EventsAsync(long? lastEventId)
    {
        var request = new HttpRequestMessage(HttpMethod.Get, "events");
        if (lastEventId is { } id)
            request.Headers.Add("Last-Event-ID", id.ToString(System.Globalization.CultureInfo.InvariantCulture));
        var response = await Http.SendAsync(request, HttpCompletionOption.ResponseHeadersRead, Ct);
        Assert.Equal("text/event-stream", response.Content.Headers.ContentType?.MediaType);
        return new EventReader(request, response, await response.Content.ReadAsStreamAsync(Ct));
    }

    private sealed record ServerEvent(long? Id, string Type, string Data);

    private sealed class EventReader(HttpRequestMessage request, HttpResponseMessage response, Stream body) : IDisposable
    {
        private readonly StreamReader _reader = new(body);

        public async Task<ServerEvent> NextAsync(Func<ServerEvent, bool> wanted)
        {
            using var timeout = CancellationTokenSource.CreateLinkedTokenSource(Ct);
            timeout.CancelAfter(Patience);
            while (true)
            {
                long? id = null;
                string? type = null;
                var data = "";
                while (await _reader.ReadLineAsync(timeout.Token) is { Length: > 0 } line)
                {
                    if (line.StartsWith("id: ", StringComparison.Ordinal))
                        id = long.Parse(line[4..], System.Globalization.CultureInfo.InvariantCulture);
                    else if (line.StartsWith("event: ", StringComparison.Ordinal))
                        type = line[7..];
                    else if (line.StartsWith("data: ", StringComparison.Ordinal))
                        data = line[6..];
                }
                if (type is not null && wanted(new ServerEvent(id, type, data)))
                    return new ServerEvent(id, type, data);
            }
        }

        public void Dispose()
        {
            _reader.Dispose();
            response.Dispose();
            request.Dispose();
        }
    }

    // A recommendation add-in that never answers and ignores its cancellation.
    private sealed class SleepyAddIn : IRecommendationPlugin
    {
        public const string AddInId = "sleepy";

        public string Id => AddInId;

        public string DisplayName => "Sleepy";

        public Task<Result<IReadOnlyList<Recommendation>, AddInError>> RecommendAsync(IReadOnlyList<Track> seed,
            int limit, CancellationToken cancellationToken) =>
            new TaskCompletionSource<Result<IReadOnlyList<Recommendation>, AddInError>>().Task;
    }

    // A search provider that never answers and ignores its cancellation.
    private sealed class SlowProvider : IProvider, ISearchProvider
    {
        public const string ProviderId = "slow";

        public string Id => ProviderId;

        public string DisplayName => "Slow";

        public ProviderCapabilities Capabilities => ProviderCapabilities.Search;

        public Task<ProviderStatus> GetStatusAsync(CancellationToken cancellationToken) =>
            Task.FromResult(new ProviderStatus(ProviderState.Ready));

        public Task<Result<CatalogItems, ProviderError>> SearchAsync(string text, SearchKinds kinds, int offset, int limit,
            CancellationToken cancellationToken) =>
            new TaskCompletionSource<Result<CatalogItems, ProviderError>>().Task;
    }
}
