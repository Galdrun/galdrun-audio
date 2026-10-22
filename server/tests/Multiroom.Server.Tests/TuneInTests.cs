using System.Net;
using System.Text.Json.Nodes;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Logging.Abstractions;
using Multiroom.Server.Abstractions;
using Multiroom.Server.Library;
using Multiroom.Server.Playback;
using Multiroom.Server.Playback.Core;
using Multiroom.Server.Providers;
using Multiroom.Server.Providers.TuneIn;
using Multiroom.Server.Web.Media;

namespace Multiroom.Server.Tests;

// The TuneIn provider (phase 17) against FakeTuneIn, its stations streamed by
// a FakeStation through a real media endpoint on port 0 and read with the
// headers FFmpeg sends; queued and re-tuned by the queue engine on FakeCore.
public sealed class TuneInTests : IAsyncDisposable
{
    private const string PartnerId = "partner-secret";
    private static readonly TimeSpan Patience = TimeSpan.FromSeconds(10);

    private readonly TempDirectory _dir = new();
    private readonly FakeStation _station = new();
    private readonly MediaHandles _handles = new();
    private readonly MediaEndpoint _endpoint;
    private readonly HttpClient _http = new(new SocketsHttpHandler
    {
        AllowAutoRedirect = false,
        AutomaticDecompression = DecompressionMethods.None,
        UseProxy = false,
    });
    private FakeTuneIn? _tuneIn;
    private TuneInProvider? _provider;

    public TuneInTests()
    {
        _endpoint = new MediaEndpoint(new MediaEndpointOptions(IPAddress.Loopback, 0, null), _handles,
            NullLoggerFactory.Instance);
    }

    private static CancellationToken Ct => TestContext.Current.CancellationToken;

    private TuneInProvider Provider => _provider!;

    public async ValueTask DisposeAsync()
    {
        await _endpoint.StopAsync(CancellationToken.None);
        await _endpoint.DisposeAsync();
        _provider?.Dispose();
        if (_tuneIn is not null)
            await _tuneIn.DisposeAsync();
        await _station.DisposeAsync();
        _http.Dispose();
        _dir.Dispose();
    }

    private async Task StartAsync()
    {
        _tuneIn = await FakeTuneIn.StartAsync(_station);
        _provider = new TuneInProvider(new TuneInSettings(PartnerId, "de-DE"), Path.Combine(_dir.Path, "tunein"),
            _tuneIn.Endpoints, bufferBytes: 32 * 1024);
        await _endpoint.StartAsync(Ct);
    }

    [Fact]
    public async Task Search_answers_stations_with_logos_and_a_logo_is_fetched_once()
    {
        await StartAsync();

        var found = Ok(await Provider.SearchAsync("rock", SearchKinds.All, 0, 10, Ct));
        var station = Assert.Single(found.Stations);
        Assert.Equal(new RadioStation("tunein", "station:s1", "Direct FM", Artwork: new ArtworkRef("tunein", "station:s1")),
            station);
        Assert.Contains(_tuneIn!.Requests, r => r.StartsWith("/Search.ashx?query=rock&types=station&partnerId=partner-secret&locale=de-DE&render=json", StringComparison.Ordinal));

        var logo = Ok(await Provider.GetArtworkAsync(station.Artwork!, Ct));
        Assert.Equal("image/png", logo.ContentType);
        Assert.Equal(FakeTuneIn.Logo, logo.Bytes.ToArray());
        Ok(await Provider.GetArtworkAsync(station.Artwork!, Ct));
        Assert.Single(_tuneIn.Requests, r => r.StartsWith("/Describe.ashx", StringComparison.Ordinal));
        Assert.Equal(ProviderError.NotFound,
            (await Provider.GetArtworkAsync(new ArtworkRef("tunein", "station:../x"), Ct)).Failure.Code);
    }

    [Fact]
    public async Task The_root_and_two_levels_below_it_browse_through_the_contract_and_a_filter_is_refused()
    {
        await StartAsync();

        var root = Ok(await Provider.BrowseAsync(new BrowseRequest(null), Ct));
        Assert.Equal(["Local Radio", "Music"], root.Nodes.Select(n => n.Title));
        Assert.All(root.Nodes, n => Assert.Equal((BrowseKind.Root, false), (n.Kind, n.Playable)));
        Assert.Equal("node:c=music", root.Nodes[1].NodeId);

        var music = Ok(await Provider.BrowseAsync(new BrowseRequest("node:c=music"), Ct));
        Assert.Equal((1, "Music"), (music.Total, music.Node!.Title));
        var rock = Assert.Single(music.Nodes);
        Assert.Equal("node:id=c57943", rock.NodeId);

        // Sectioned: stations with their more link; shows left out; of the
        // related links only TuneIn's own browse link, without render.
        var page = Ok(await Provider.BrowseAsync(new BrowseRequest(rock.NodeId, Limit: 3), Ct));
        Assert.Null(page.Total);
        Assert.Equal(["Stations", "Related"], page.Sections!.Select(s => s.Title));
        var stations = page.Sections![0];
        Assert.Equal(["Direct FM", "Playlist FM", "HLS FM"], stations.Items.Stations.Select(s => s.Name));
        Assert.Equal("node:offset=4&id=c57943&filter=s", stations.More!.NodeId);
        Assert.Equal("node:id=c1", Assert.Single(page.Sections[1].Nodes).NodeId);
        Assert.DoesNotContain(_tuneIn!.Requests, r => r.Contains("%2F", StringComparison.Ordinal) || r.Contains("steal", StringComparison.Ordinal));

        var more = Ok(await Provider.BrowseAsync(new BrowseRequest(stations.More.NodeId, Offset: 1, Limit: 1), Ct));
        Assert.Equal((3, BrowseKind.Stations), (more.Total, more.Node!.Kind));
        Assert.Equal("Sixth FM", Assert.Single(more.Items.Stations).Name);

        Assert.Equal(ProviderError.InvalidArgument,
            (await Provider.BrowseAsync(new BrowseRequest(rock.NodeId, Filter: "x"), Ct)).Failure.Code);
        Assert.Equal(ProviderError.InvalidArgument,
            (await Provider.BrowseAsync(new BrowseRequest(null, Sort: "name"), Ct)).Failure.Code);
        Assert.Equal(ProviderError.InvalidArgument,
            (await Provider.BrowseAsync(new BrowseRequest(rock.NodeId, Offset: 5), Ct)).Failure.Code);
        foreach (var bad in new[] { "node:id=../x", "node:url=http", "node:", "c=music", "node:id=c9" })
            Assert.Equal(ProviderError.NotFound, (await Provider.BrowseAsync(new BrowseRequest(bad), Ct)).Failure.Code);
    }

    [Fact]
    public async Task A_station_resolves_through_a_direct_url_a_pls_or_past_hls_and_one_with_only_hls_is_unavailable_by_name()
    {
        await StartAsync();

        foreach (var (guide, name) in new[] { ("s1", "Direct FM"), ("s2", "Playlist FM"), ("s4", "Mixed FM") })
        {
            var uri = await ServeAsync(new RadioStation("tunein", $"station:{guide}", name));
            using var response = await GetAsync(uri);
            Assert.Equal(HttpStatusCode.OK, response.StatusCode);
            Assert.Null(response.Content.Headers.ContentLength);
            await using var body = await response.Content.ReadAsStreamAsync(Ct);
            var data = new byte[10_000];
            await body.ReadExactlyAsync(data, Ct);
            Assert.Equal(FakeStation.Pattern(9_999), data[^1]);
        }

        var hls = await Provider.ResolveAsync(new RadioStation("tunein", "station:s3", "HLS FM"), Ct);
        Assert.Equal(ProviderError.Unavailable, hls.Failure.Code);
        Assert.Equal("HLS FM: TuneIn offers no stream this server plays", hls.Failure.Detail);
        Assert.Equal(ProviderError.NotFound,
            (await Provider.ResolveAsync(new RadioStation("tunein", "station:s99", "Gone FM"), Ct)).Failure.Code);
        Assert.Equal(ProviderError.NotFound,
            (await Provider.ResolveAsync(new RadioStation("tunein", "station:x", "Bad"), Ct)).Failure.Code);
    }

    [Fact]
    public async Task A_queued_tunein_station_plays_through_the_media_endpoint_re_tunes_on_a_drop_answers_its_title_and_is_queued_from_a_favourite()
    {
        await StartAsync();
        _station.MetaInterval = 8000;
        _station.Title = "Band - Song";
        await using var core = new FakeCore();
        await using var services = new ServiceCollection().BuildServiceProvider();
        await using var providers = new ProviderRegistry(
            [new ProviderRegistration(TuneInProvider.ProviderId, _ => Provider)], services, NullLogger<ProviderRegistry>.Instance);
        await providers.StartAsync(Ct);
        var synchronised = new QueueEngineTests.Synchronised();
        using var link = new CoreLink(new CoreLinkOptions("127.0.0.1", core.Port), [synchronised], NullLogger<CoreLink>.Instance);
        var engine = new QueueEngine(link, providers, _endpoint, NullLogger<QueueEngine>.Instance);
        await link.StartAsync(Ct);
        await synchronised.Next.Task.WaitAsync(Patience, Ct);

        var database = new LibraryDatabase(Path.Combine(_dir.Path, "library.db"));
        Assert.True(database.Migrate().IsSuccess);
        var store = new PlaylistStore(database);
        var station = Assert.Single(Ok(await Provider.SearchAsync("rock", SearchKinds.All, 0, 10, Ct)).Stations);
        var favorite = store.AddFavorite(station).Value;
        var player = new PlaylistPlayer(store, engine, providers);
        Assert.True((await player.EnqueueFavoriteAsync("living", favorite.Id, QueuePlacement.End, Ct)).IsSuccess);
        Assert.True((await engine.PlayAsync("living", null, Ct)).IsSuccess);

        var staged = Staged(core);
        var first = Assert.Single(staged);
        Assert.Equal("Direct FM", first["title"]!.GetValue<string>());
        using (var response = await GetAsync(new Uri(first["mediaUri"]!.GetValue<string>())))
        {
            await using var body = await response.Content.ReadAsStreamAsync(Ct);
            var data = new byte[100_000];
            await body.ReadExactlyAsync(data, Ct);
            Assert.Equal("Band - Song", Provider.CurrentTitle(station.ProviderItemId));
        }

        var sourceId = first["sourceId"]!.GetValue<string>();
        await core.EmitAsync(
            [$$"""{"event":"zone.sourceNeeded","zone":"living","afterSourceId":"{{sourceId}}"}""",
             $$"""{"event":"zone.sourceStarted","zone":"living","sourceId":"{{sourceId}}","timelineNs":0,"serverTimeNs":1,"startOffsetNs":0}"""]);
        await Until(() => engine.GetQueue("living").Value.NowPlaying is not null);
        await core.EmitAsync($$"""{"event":"zone.sourceFailed","zone":"living","sourceId":"{{sourceId}}","reason":"I/O error"}""");
        await Until(() => Staged(core).Count == 2);
        Assert.Equal("Direct FM", Staged(core)[1]["title"]!.GetValue<string>());

        // No stream URL, playlist URL or partner id went to the core.
        Assert.DoesNotContain(core.All, r => r.Params.Contains(_station.Url.Authority, StringComparison.Ordinal) ||
            r.Params.Contains(PartnerId, StringComparison.Ordinal));
        await link.StopAsync(CancellationToken.None);
    }

    private static List<JsonNode> Staged(FakeCore core) =>
        [.. core.All.Where(r => r.Command == "source.stage").Select(r => JsonNode.Parse(r.Params)!)];

    private async Task<Uri> ServeAsync(RadioStation station)
    {
        var resolved = await Provider.ResolveAsync(station, Ct);
        Assert.True(resolved.IsSuccess, resolved.ToString());
        return _endpoint.Publish(resolved.Value);
    }

    // What FFmpeg sends.
    private Task<HttpResponseMessage> GetAsync(Uri uri)
    {
        var request = new HttpRequestMessage(HttpMethod.Get, uri);
        request.Headers.Range = new System.Net.Http.Headers.RangeHeaderValue(0, null);
        request.Headers.Add("Icy-MetaData", "1");
        request.Headers.ConnectionClose = true;
        return _http.SendAsync(request, HttpCompletionOption.ResponseHeadersRead, Ct);
    }

    private static T Ok<T>(Result<T, ProviderError> result)
    {
        Assert.True(result.IsSuccess, result.ToString());
        return result.Value;
    }

    private static async Task Until(Func<bool> condition)
    {
        using var timeout = new CancellationTokenSource(Patience);
        while (!condition())
            await Task.Delay(5, timeout.Token);
    }
}
