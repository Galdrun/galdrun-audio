using System.Collections.Concurrent;
using System.Globalization;
using System.Text;
using System.Text.Json;
using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Http;
using Multiroom.Server.Providers.Tidal;

namespace Multiroom.Server.Tests;

// Tidal in process: its auth server (the device flow and refresh), the parts
// of api.tidal.com/v1 the provider reads, a CDN and the image server, all on
// one loopback port. The JSON is shaped as Tidal answers, recorded and cut
// down, with the account's data made up.
//
// Tracks are added by the test: one-file ("BTS") manifests over bytes the
// CDN serves with or without ranges, and DASH manifests over segments the CDN
// serves with a delay each, so they arrive out of order.
internal sealed class FakeTidal : IAsyncDisposable
{
    public const string ClientId = "fixture-client";
    public const string ClientSecret = "fixture-secret";
    public const long UserId = 4711;
    public const string Country = "DE";
    public const string PlaylistId = "4261748a-4287-4758-aaab-6d5be3e99e52";
    public const string Cover = "aaaa1111-2222-3333-4444-555566667777";

    private readonly ConcurrentDictionary<string, byte[]> _files = new(StringComparer.Ordinal);
    private readonly ConcurrentDictionary<string, int> _delays = new(StringComparer.Ordinal);
    private readonly ConcurrentDictionary<string, bool> _noRanges = new(StringComparer.Ordinal);
    private readonly ConcurrentDictionary<long, string> _playback = new();
    private readonly WebApplication _app;
    private volatile string? _valid = "access-1";
    private int _pending = int.MaxValue;
    private int _issued = 1;
    private int _refreshes;
    private int _deviceStarts;
    private int _images;

    private FakeTidal(WebApplication app, Uri baseUri)
    {
        _app = app;
        BaseUri = baseUri;
    }

    public Uri BaseUri { get; }

    public TidalEndpoints Endpoints =>
        new(new Uri(BaseUri, "auth/"), new Uri(BaseUri, "api/"), new Uri(BaseUri, "images/"));

    public string RefreshToken { get; private set; } = "refresh-1";

    public string? AccessToken => _valid;

    public bool RefreshRefused { get; set; }

    public int Refreshes => Volatile.Read(ref _refreshes);

    public int DeviceLifetime { get; set; } = 300;

    public string? DeviceError { get; set; }

    public int DeviceStarts => Volatile.Read(ref _deviceStarts);

    public int ImageFetches => Volatile.Read(ref _images);

    // Every form posted to the auth server, and every quality asked for.
    public ConcurrentQueue<Dictionary<string, string>> Forms { get; } = new();

    public ConcurrentQueue<string> Qualities { get; } = new();

    // Per CDN file: every Range header asked for ("" for none).
    public ConcurrentDictionary<string, ConcurrentQueue<string>> Fetches { get; } = new(StringComparer.Ordinal);

    public static async Task<FakeTidal> StartAsync()
    {
        FakeTidal? fake = null;
        var (app, uri) = await FixtureHttp.StartAsync(context => fake!.HandleAsync(context));
        fake = new FakeTidal(app, uri);
        return fake;
    }

    // The person opened the URL and signed in: the next poll is granted.
    public void Approve() => Volatile.Write(ref _pending, 0);

    // The access token stops working, as one does before its announced end.
    public void ExpireAccess() => _valid = null;

    // An account link kept from an earlier run, as TidalSession writes it.
    public void Seed(string directory, DateTimeOffset? expiresAt = null)
    {
        Directory.CreateDirectory(directory);
        File.WriteAllText(Path.Combine(directory, "token.json"), JsonSerializer.Serialize(new Dictionary<string, object>
        {
            ["clientId"] = ClientId,
            ["accessToken"] = _valid ?? "stale",
            ["refreshToken"] = RefreshToken,
            ["expiresAt"] = (expiresAt ?? DateTimeOffset.UtcNow.AddHours(1)).ToUnixTimeSeconds(),
            ["userId"] = UserId,
            ["countryCode"] = Country,
        }));
    }

    public int FetchCount(string file) => Fetches.TryGetValue(file, out var ranges) ? ranges.Count : 0;

    // A one-file track: its URL is cdn/t<id>. `granted` is the tier the
    // playback info answers with, whatever was asked for.
    public void AddBtsTrack(long id, byte[] data, bool ranges = true, string encryption = "NONE",
        string presentation = "FULL", string granted = "LOSSLESS", int? sampleRate = null, int? bitDepth = null)
    {
        var name = $"t{id}";
        _files[name] = data;
        if (!ranges)
            _noRanges[name] = true;
        var manifest = JsonSerializer.Serialize(new
        {
            mimeType = "audio/flac",
            codecs = "flac",
            encryptionType = encryption,
            urls = new[] { new Uri(BaseUri, $"cdn/{name}?token=signed-secret").ToString() },
        });
        AddPlayback(id, "application/vnd.tidal.bts", manifest, presentation, granted, sampleRate, bitDepth);
    }

    // A DASH track: segment 0 is the init segment (cdn/d<id>-init), the rest
    // are media segments 1..n; each served after its delay in milliseconds.
    public void AddDashTrack(long id, byte[][] segments, int[] delays)
    {
        for (var i = 0; i < segments.Length; i++)
        {
            var name = i == 0 ? $"d{id}-init" : $"d{id}-{i}";
            _files[name] = segments[i];
            _delays[name] = delays[i];
        }
        var media = segments.Length - 1;
        var mpd = $"""
            <?xml version="1.0" encoding="UTF-8"?>
            <MPD xmlns="urn:mpeg:dash:schema:mpd:2011" profiles="urn:mpeg:dash:profile:isoff-main:2011" type="static" mediaPresentationDuration="PT0H3M30.000S" minBufferTime="PT3.993S">
              <Period id="0">
                <AdaptationSet id="0" contentType="audio" mimeType="audio/mp4" segmentAlignment="true">
                  <Representation id="FLAC,44100,16" codecs="flac" bandwidth="1024000" audioSamplingRate="44100">
                    <SegmentTemplate timescale="44100" initialization="{new Uri(BaseUri, $"cdn/d{id}-init?token=signed-secret")}" media="{new Uri(BaseUri, $"cdn/d{id}-$Number$?token=signed-secret")}" startNumber="1">
                      <SegmentTimeline>
                        <S d="176128" r="{media - 2}"/>
                        <S d="45056"/>
                      </SegmentTimeline>
                    </SegmentTemplate>
                  </Representation>
                </AdaptationSet>
              </Period>
            </MPD>
            """.Replace("&", "&amp;", StringComparison.Ordinal);
        AddPlayback(id, "application/dash+xml", mpd, "FULL");
    }

    public ValueTask DisposeAsync() => _app.DisposeAsync();

    private void AddPlayback(long id, string mimeType, string manifest, string presentation,
        string granted = "LOSSLESS", int? sampleRate = null, int? bitDepth = null) =>
        _playback[id] = JsonSerializer.Serialize(new
        {
            trackId = id,
            assetPresentation = presentation,
            audioMode = "STEREO",
            audioQuality = granted,
            sampleRate,
            bitDepth,
            manifestMimeType = mimeType,
            manifestHash = "redacted",
            manifest = Convert.ToBase64String(Encoding.UTF8.GetBytes(manifest)),
        });

    private async Task HandleAsync(HttpContext context)
    {
        var path = context.Request.Path.Value ?? "";
        if (path.StartsWith("/auth/", StringComparison.Ordinal))
            await AuthAsync(context, path["/auth/".Length..]);
        else if (path.StartsWith("/api/", StringComparison.Ordinal))
            await ApiAsync(context, path["/api/".Length..]);
        else if (path.StartsWith("/cdn/", StringComparison.Ordinal))
            await CdnAsync(context, path["/cdn/".Length..]);
        else if (path.StartsWith("/images/", StringComparison.Ordinal) && path.EndsWith("/640x640.jpg", StringComparison.Ordinal))
            await ImageAsync(context);
        else
            context.Response.StatusCode = 404;
    }

    private async Task AuthAsync(HttpContext context, string path)
    {
        var form = (await context.Request.ReadFormAsync()).ToDictionary(f => f.Key, f => f.Value.ToString());
        Forms.Enqueue(form);
        if (path == "oauth2/device_authorization")
        {
            Interlocked.Increment(ref _deviceStarts);
            await Json(context, 200, JsonSerializer.Serialize(new
            {
                deviceCode = "device-code-1", userCode = "ABCDE", verificationUri = "link.tidal.test",
                verificationUriComplete = "link.tidal.test/ABCDE", expiresIn = DeviceLifetime, interval = 1,
            }));
            return;
        }
        if (path != "oauth2/token")
        {
            context.Response.StatusCode = 404;
            return;
        }

        switch (form.GetValueOrDefault("grant_type"))
        {
            case "urn:ietf:params:oauth:grant-type:device_code":
                if (form.GetValueOrDefault("device_code") != "device-code-1")
                {
                    await Json(context, 400, """{"error":"invalid_grant"}""");
                    return;
                }
                if (DeviceError is { } error)
                {
                    await Json(context, 400, JsonSerializer.Serialize(new { error }));
                    return;
                }
                if (Volatile.Read(ref _pending) > 0)
                {
                    await Json(context, 400, """
                        {"status":400,"error":"authorization_pending","sub_status":1002,"error_description":"Device Authorization code is not authorized yet"}
                        """);
                    return;
                }
                await Json(context, 200, Issue(withRefresh: true));
                return;

            case "refresh_token":
                Interlocked.Increment(ref _refreshes);
                if (RefreshRefused || form.GetValueOrDefault("refresh_token") != RefreshToken)
                {
                    await Json(context, 400, """
                        {"status":400,"error":"invalid_grant","sub_status":11101,"error_description":"Token could not be verified"}
                        """);
                    return;
                }
                // A refresh answer names no new refresh token and no user.
                await Json(context, 200, Issue(withRefresh: false));
                return;

            default:
                await Json(context, 400, """{"status":400,"error":"unsupported_grant_type"}""");
                return;
        }
    }

    private string Issue(bool withRefresh)
    {
        var access = $"access-{Interlocked.Increment(ref _issued)}";
        _valid = access;
        if (withRefresh)
        {
            RefreshToken = $"refresh-{_issued}";
            return JsonSerializer.Serialize(new
            {
                access_token = access,
                refresh_token = RefreshToken,
                token_type = "Bearer",
                expires_in = 3600,
                user_id = UserId,
                user = new { userId = UserId, countryCode = Country, username = "redacted" },
            });
        }
        return JsonSerializer.Serialize(new { access_token = access, token_type = "Bearer", expires_in = 3600, user_id = UserId });
    }

    private async Task ApiAsync(HttpContext context, string path)
    {
        if (_valid is null || context.Request.Headers.Authorization.ToString() != $"Bearer {_valid}")
        {
            await Json(context, 401, """{"status":401,"subStatus":11003,"userMessage":"The token has expired. (Expired on time)"}""");
            return;
        }
        if (context.Request.Query["countryCode"] != Country)
        {
            await Json(context, 400, """{"status":400,"subStatus":1002,"userMessage":"Country code is required"}""");
            return;
        }

        var parts = path.Split('/');
        var query = context.Request.Query;
        Requests.Enqueue($"{path}?{string.Join('&', query.Where(q => q.Key != "countryCode").Select(q => $"{q.Key}={q.Value}"))}");
        var user = UserId.ToString(CultureInfo.InvariantCulture);
        switch (parts)
        {
            case ["search"]:
                await Json(context, 200, SearchJson);
                return;
            case ["users", var u, "playlists"] when u == user:
                await Json(context, 200, PlaylistsJson);
                return;
            case ["users", var u, "favorites", var kind] when u == user:
                await Json(context, 200, kind switch
                {
                    "albums" => $$"""{"items":[{"created":"2024-01-01T00:00:00.000+0000","item":{{KindOfBlue}}}],"totalNumberOfItems":1}""",
                    "artists" => $$"""{"items":[{"created":"2024-01-01T00:00:00.000+0000","item":{{MilesDavisWithPicture}}}],"totalNumberOfItems":1}""",
                    "tracks" => $$"""{"items":[{"item":{{SoWhat}}},{"item":{{BlueInGreen}}},{"item":{{Freddie}}}],"totalNumberOfItems":3}""",
                    _ => $$"""{"items":[{"created":"2024-01-01T00:00:00.000+0000","item":{{LateNight}}}],"totalNumberOfItems":1}""",
                });
                return;
            case ["playlists", PlaylistId]:
                await Json(context, 200, LateNight);
                return;
            case ["playlists", PlaylistId, "tracks"]:
                await Json(context, 200, $$"""{"items":[{"item":{{SoWhat}},"type":"track"},{"item":{{Freddie}},"type":"track"}],"totalNumberOfItems":2}""");
                return;
            case ["albums", "1000"]:
                await Json(context, 200, KindOfBlue);
                return;
            case ["albums", "2000", "tracks"]:
                await Json(context, 200, $$"""{"items":[{{Chapter(2001, "Kapitel 1.1 - Der Hobbit")}},{{Chapter(2002, "Kapitel 1.2 - Der Hobbit")}},{{Chapter(2003, "Kapitel 2.1 - Der Hobbit")}}],"totalNumberOfItems":3}""");
                return;
            case ["albums", "1000", "tracks"]:
                await Json(context, 200, $$"""{"items":[{{SoWhat}},{{Freddie}}],"totalNumberOfItems":2}""");
                return;
            case ["artists", "3346"]:
                await Json(context, 200, MilesDavisWithPicture);
                return;
            case ["artists", "3346", "albums"] when query["filter"] == "EPSANDSINGLES":
                await Json(context, 200, $$"""{"items":[{{Milestones}}],"totalNumberOfItems":1}""");
                return;
            case ["artists", "3346", "albums"]:
                await Json(context, 200, $$"""{"items":[{{KindOfBlue}}],"totalNumberOfItems":1}""");
                return;
            case ["artists", "3346", "toptracks"]:
                await Json(context, 200, $$"""{"items":[{{SoWhat}},{{Freddie}}],"totalNumberOfItems":2}""");
                return;
            case ["artists", "3346", "similar"]:
                await Json(context, 200, """{"items":[{"id":1566,"name":"John Coltrane","picture":"0a1b2c3d-9999-2222-3333-444455556666"}],"totalNumberOfItems":1}""");
                return;
            case ["pages", "for_you"] when query["deviceType"] == "BROWSER":
                await Json(context, 200, ForYouJson);
                return;
            case ["pages", "data", "abc-123"] when query["albumId"] == "1" && query["deviceType"] == "BROWSER":
                await Json(context, 200, $$"""{"items":[{{KindOfBlue}}],"totalNumberOfItems":1}""");
                return;
            case ["mixes", MixId, "items"]:
                await Json(context, 200, $$"""{"items":[{"item":{{SoWhat}},"type":"track"}],"totalNumberOfItems":1}""");
                return;
            case ["tracks", var id, "playbackinfopostpaywall"]:
                Qualities.Enqueue(context.Request.Query["audioquality"].ToString());
                if (long.TryParse(id, CultureInfo.InvariantCulture, out var trackId) && _playback.TryGetValue(trackId, out var info))
                    await Json(context, 200, info);
                else
                    await Json(context, 404, """{"status":404,"subStatus":2001,"userMessage":"The requested resource could not be found"}""");
                return;
            default:
                await Json(context, 404, """{"status":404,"subStatus":2001,"userMessage":"The requested resource could not be found"}""");
                return;
        }
    }

    private async Task CdnAsync(HttpContext context, string name)
    {
        var range = context.Request.Headers.Range.ToString();
        Fetches.GetOrAdd(name, _ => new ConcurrentQueue<string>()).Enqueue(range);
        if (context.Request.Query["token"] != "signed-secret" || !_files.TryGetValue(name, out var data))
        {
            context.Response.StatusCode = 403;
            return;
        }
        if (_delays.TryGetValue(name, out var delay) && delay > 0)
            await Task.Delay(delay);

        context.Response.ContentType = "application/octet-stream";
        if (range.StartsWith("bytes=", StringComparison.Ordinal) && !_noRanges.ContainsKey(name))
        {
            var bounds = range["bytes=".Length..].Split('-');
            var from = long.Parse(bounds[0], CultureInfo.InvariantCulture);
            var to = bounds[1].Length == 0 ? data.Length - 1 : Math.Min(long.Parse(bounds[1], CultureInfo.InvariantCulture), data.Length - 1);
            context.Response.StatusCode = 206;
            context.Response.Headers.ContentRange = $"bytes {from}-{to}/{data.Length}";
            context.Response.ContentLength = to - from + 1;
            await context.Response.Body.WriteAsync(data.AsMemory((int)from, (int)(to - from + 1)));
            return;
        }
        context.Response.ContentLength = data.Length;
        await context.Response.Body.WriteAsync(data);
    }

    private async Task ImageAsync(HttpContext context)
    {
        Interlocked.Increment(ref _images);
        context.Response.ContentType = "image/jpeg";
        var bytes = Image(context.Request.Path.Value!);
        context.Response.ContentLength = bytes.Length;
        await context.Response.Body.WriteAsync(bytes);
    }

    // 1000 bytes, different for each image path.
    public static byte[] Image(string path)
    {
        var seed = (byte)path.Aggregate(0, (h, c) => h * 31 + c);
        return [0xFF, 0xD8, .. Enumerable.Range(0, 998).Select(i => (byte)(seed + i))];
    }

    private static Task Json(HttpContext context, int status, string json)
    {
        context.Response.StatusCode = status;
        context.Response.ContentType = "application/json";
        return context.Response.WriteAsync(json);
    }

    // A chapter of an audiobook as Tidal lists it: nothing says it is one
    // but its title.
    private static string Chapter(int id, string title) => $$$"""
        {"id":{{{id}}},"title":"{{{title}}}","duration":300,"trackNumber":{{{id - 2000}}},"streamReady":true,
         "artist":{"id":4000,"name":"Gert Heidenreich","type":"MAIN"},
         "album":{"id":2000,"title":"Der Hobbit","cover":"aaaa1111-2222-3333-4444-555566667777"}}
        """;

    private const string MilesDavis = """{"id":3346,"name":"Miles Davis","type":"MAIN"}""";

    private const string MilesDavisWithPicture = """{"id":3346,"name":"Miles Davis","picture":"0a1b2c3d-1111-2222-3333-444455556666"}""";

    private const string Milestones = """
        {"id":1100,"title":"Milestones EP","numberOfTracks":2,"releaseDate":"1958-04-02","type":"EP",
         "cover":"aaaa1111-2222-3333-4444-555566667777","artist":{"id":3346,"name":"Miles Davis","type":"MAIN"}}
        """;

    // Listed, but not to be streamed: Tidal says so itself.
    private const string BlueInGreen = """
        {"id":1003,"title":"Blue in Green","duration":337,"trackNumber":3,"streamReady":false,"allowStreaming":false,
         "artist":{"id":3346,"name":"Miles Davis","type":"MAIN"},
         "album":{"id":1000,"title":"Kind of Blue","cover":"aaaa1111-2222-3333-4444-555566667777"}}
        """;

    private const string LateNight = """
        {"uuid":"4261748a-4287-4758-aaab-6d5be3e99e52","title":"Late night","numberOfTracks":2,
         "squareImage":"bbbb1111-2222-3333-4444-555566667777","image":"cccc1111-2222-3333-4444-555566667777"}
        """;

    public const string MixId = "0012abcd0012abcd";

    // Every request of the API, its query without the country.
    public ConcurrentQueue<string> Requests { get; } = new();

    // The modules the provider reads, one it leaves out, and paths it must
    // refuse: a URL in a page link and in a module's rest.
    private static readonly string ForYouJson = $$$"""
        {"title":"For You","rows":[
          {"modules":[{"type":"FEATURED_PROMOTIONS","title":"Featured","items":[]}]},
          {"modules":[{"type":"ALBUM_LIST","title":"Recommended albums",
            "pagedList":{"items":[{{{KindOfBlue}}}],"totalNumberOfItems":9,"dataApiPath":"pages/data/abc-123?albumId=1"}}]},
          {"modules":[{"type":"MIX_LIST","title":"Your mixes",
            "pagedList":{"items":[{"id":"{{{MixId}}}","title":"My Mix 1","subTitle":"Miles Davis and more","mixType":"DISCOVERY_MIX"}],"totalNumberOfItems":1}}]},
          {"modules":[{"type":"PAGE_LINKS","title":"Genres",
            "pagedList":{"items":[{"title":"Jazz","apiPath":"pages/genre_jazz"},{"title":"Elsewhere","apiPath":"https://evil.example/pages/x"}],"totalNumberOfItems":2}}]},
          {"modules":[{"type":"TRACK_LIST","title":"Popular tracks",
            "pagedList":{"items":[{{{SoWhat}}}],"totalNumberOfItems":50,"dataApiPath":"https://evil.example/pages/data"}}]}
        ]}
        """;

    private const string KindOfBlue = """
        {"id":1000,"title":"Kind of Blue","duration":2759,"numberOfTracks":5,"releaseDate":"1959-08-17",
         "cover":"aaaa1111-2222-3333-4444-555566667777","audioQuality":"LOSSLESS","artist":{"id":3346,"name":"Miles Davis","type":"MAIN"}}
        """;

    private const string SoWhat = """
        {"id":1001,"title":"So What","duration":562,"version":null,"trackNumber":1,"streamReady":true,"audioQuality":"LOSSLESS",
         "artist":{"id":3346,"name":"Miles Davis","type":"MAIN"},"artists":[{"id":3346,"name":"Miles Davis","type":"MAIN"}],
         "album":{"id":1000,"title":"Kind of Blue","cover":"aaaa1111-2222-3333-4444-555566667777"}}
        """;

    private const string Freddie = """
        {"id":1002,"title":"Freddie Freeloader","duration":589,"version":"Remastered","trackNumber":2,"streamReady":true,
         "artists":[{"id":3346,"name":"Miles Davis","type":"MAIN"}],
         "album":{"id":1000,"title":"Kind of Blue","cover":"aaaa1111-2222-3333-4444-555566667777"}}
        """;

    private static readonly string SearchJson = $$"""
        {"artists":{"limit":10,"offset":0,"totalNumberOfItems":1,"items":[{"id":3346,"name":"Miles Davis","picture":"0a1b2c3d-1111-2222-3333-444455556666"}]},
         "albums":{"limit":10,"offset":0,"totalNumberOfItems":1,"items":[{{KindOfBlue}}]},
         "playlists":{"limit":10,"offset":0,"totalNumberOfItems":1,"items":[{{LateNight}}]},
         "tracks":{"limit":10,"offset":0,"totalNumberOfItems":2,"items":[{{SoWhat}},{{Freddie}}]},
         "videos":{"limit":10,"offset":0,"totalNumberOfItems":0,"items":[]},
         "topHit":{"value":{{MilesDavis}},"type":"ARTISTS"}
        }
        """;

    private const string PlaylistsJson = """
        {"limit":10,"offset":0,"totalNumberOfItems":1,"items":[{"uuid":"4261748a-4287-4758-aaab-6d5be3e99e52","title":"Late night",
         "numberOfTracks":2,"squareImage":"bbbb1111-2222-3333-4444-555566667777","image":"cccc1111-2222-3333-4444-555566667777"}]}
        """;
}
