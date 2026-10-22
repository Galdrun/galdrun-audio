using System.Collections.Concurrent;
using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Http;
using Multiroom.Server.Providers.TuneIn;

namespace Multiroom.Server.Tests;

// TuneIn's OPML API on a free loopback port, answering recorded shapes of
// Browse.ashx, Search.ashx, Tune.ashx and Describe.ashx (render=json). Its
// stations stream from `station`:
//   s1 a direct stream; s2 a .pls naming it; s3 HLS only; s4 HLS, then direct.
// The tree: the root (local, music, podcasts), music (one genre), the genre
// (a sectioned page of stations with its "more" link, shows, and two hostile
// links), and the more link (a flat list). It records every request's path
// and query, and serves a PNG as every station's logo.
internal sealed class FakeTuneIn : IAsyncDisposable
{
    public static readonly byte[] Logo = [0x89, (byte)'P', (byte)'N', (byte)'G', 13, 10, 26, 10, 0, 0, 0, 0];

    private readonly WebApplication _app;
    private readonly FakeStation _station;

    private FakeTuneIn(WebApplication app, Uri baseUri, FakeStation station)
    {
        _app = app;
        BaseUri = baseUri;
        _station = station;
    }

    public Uri BaseUri { get; }

    public TuneInEndpoints Endpoints => new(BaseUri);

    public ConcurrentQueue<string> Requests { get; } = new();

    public static async Task<FakeTuneIn> StartAsync(FakeStation station)
    {
        FakeTuneIn? fake = null;
        var (app, baseUri) = await FixtureHttp.StartAsync(context => fake!.HandleAsync(context));
        fake = new FakeTuneIn(app, baseUri, station);
        return fake;
    }

    public async ValueTask DisposeAsync()
    {
        await _app.StopAsync();
        await _app.DisposeAsync();
    }

    private Task HandleAsync(HttpContext context)
    {
        var request = context.Request;
        Requests.Enqueue(request.Path + request.QueryString);
        var id = request.Query["id"].ToString();
        var c = request.Query["c"].ToString();
        var link = $"http://opml.radiotime.com/Browse.ashx";
        var stream = _station.Url.AbsoluteUri;

        string Station(string guide, string name) =>
            $$"""{"element":"outline","type":"audio","text":"{{name}}","URL":"http://opml.radiotime.com/Tune.ashx?id={{guide}}","guide_id":"{{guide}}","item":"station","image":"http://cdn.example/{{guide}}q.png","subtext":"now on air"}""";

        string? body = (request.Path.Value, id, c) switch
        {
            ("/Browse.ashx", "", "") => $$"""
                {"head":{"title":"Browse","status":"200"},"body":[
                 {"element":"outline","type":"link","text":"Local Radio","URL":"{{link}}?c=local","key":"local"},
                 {"element":"outline","type":"link","text":"Music","URL":"{{link}}?c=music","key":"music"},
                 {"element":"outline","type":"link","text":"Podcasts","URL":"{{link}}?c=podcast","key":"podcast"}]}
                """,
            ("/Browse.ashx", "", "music") => $$"""
                {"head":{"title":"Music","status":"200"},"body":[
                 {"element":"outline","type":"link","text":"Rock","URL":"{{link}}?id=c57943","guide_id":"c57943"}]}
                """,
            ("/Browse.ashx", "c57943", "") when request.Query["offset"].Count == 0 => $$"""
                {"head":{"title":"Rock","status":"200"},"body":[
                 {"element":"outline","text":"Stations","key":"stations","children":[
                  {{Station("s1", "Direct FM")}},{{Station("s2", "Playlist FM")}},{{Station("s3", "HLS FM")}},{{Station("s4", "Mixed FM")}},
                  {"element":"outline","type":"link","text":"More Stations","URL":"{{link}}?offset=4&id=c57943&filter=s","key":"nextStations"}]},
                 {"element":"outline","text":"Shows","key":"shows","children":[
                  {"element":"outline","type":"link","text":"A show","URL":"{{link}}?id=p123","guide_id":"p123","item":"show"},
                  {"element":"outline","type":"link","text":"Latest episodes","URL":"{{link}}?id=c57943&filter=p:topic"},
                  {"element":"outline","type":"link","text":"More Shows","URL":"{{link}}?offset=6&id=c57943&filter=p","key":"nextShows"}]},
                 {"element":"outline","text":"Related","key":"related","children":[
                  {"element":"outline","type":"link","text":"Escape","URL":"{{link}}?id=..%2Fsecret"},
                  {"element":"outline","type":"link","text":"Elsewhere","URL":"http://evil.example/steal?x=1"},
                  {"element":"outline","type":"link","text":"Classic Rock","URL":"{{link}}?id=c1&render=json","guide_id":"c1"}]}]}
                """,
            ("/Browse.ashx", "c57943", "") => $$"""
                {"head":{"title":"Rock","status":"200"},"body":[{{Station("s5", "Fifth FM")}},{{Station("s6", "Sixth FM")}},{{Station("s7", "Seventh FM")}}]}
                """,
            ("/Browse.ashx", _, _) => """{"head":{"status":"400","fault":"Invalid root category"},"body":[]}""",
            ("/Search.ashx", _, _) => $$"""
                {"head":{"title":"Search","status":"200"},"body":[{{Station("s1", "Direct FM")}},
                 {"element":"outline","type":"link","text":"A show","URL":"{{link}}?id=p9","guide_id":"p9","item":"show"}]}
                """,
            ("/Tune.ashx", "s1", _) => $$"""{"head":{"status":"200"},"body":[{"element":"audio","url":"{{stream}}","media_type":"mp3"}]}""",
            ("/Tune.ashx", "s2", _) => $$"""{"head":{"status":"200"},"body":[{"element":"audio","url":"{{BaseUri}}s2.pls","media_type":"mp3"}]}""",
            ("/Tune.ashx", "s3", _) => """{"head":{"status":"200"},"body":[{"element":"audio","url":"http://hls.example/live.m3u8","media_type":"hls"}]}""",
            ("/Tune.ashx", "s4", _) => $$"""
                {"head":{"status":"200"},"body":[{"element":"audio","url":"http://hls.example/live.m3u8","media_type":"hls","is_hls_advanced":"true"},
                 {"element":"audio","url":"{{stream}}","media_type":"aac"}]}
                """,
            ("/Tune.ashx", _, _) => """{"head":{"status":"400","fault":"Invalid station"},"body":[]}""",
            ("/Describe.ashx", "s1", _) => $$"""{"head":{"status":"200"},"body":[{"element":"station","guide_id":"s1","name":"Direct FM","logo":"{{BaseUri}}logo.png"}]}""",
            ("/s2.pls", _, _) => $"[playlist]\nNumberOfEntries=1\nFile1={stream}\nTitle1=Playlist FM\n",
            _ => null,
        };

        if (request.Path.Value == "/logo.png")
        {
            context.Response.ContentType = "image/png";
            return context.Response.Body.WriteAsync(Logo).AsTask();
        }
        if (body is null)
        {
            context.Response.StatusCode = StatusCodes.Status404NotFound;
            return Task.CompletedTask;
        }
        context.Response.ContentType = request.Path.Value!.EndsWith(".pls", StringComparison.Ordinal)
            ? "audio/x-scpls"
            : "application/json";
        return context.Response.WriteAsync(body);
    }
}
