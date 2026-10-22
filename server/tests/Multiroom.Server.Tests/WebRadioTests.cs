using System.Net;
using Microsoft.Extensions.Logging.Abstractions;
using Multiroom.Server.Abstractions;
using Multiroom.Server.Library;
using Multiroom.Server.Providers.WebRadio;
using Multiroom.Server.Web.Media;

namespace Multiroom.Server.Tests;

// The webradio provider: stations imported and kept in the library database,
// searched and browsed, and relayed live from a FakeStation through a real
// media endpoint on port 0, read with the headers FFmpeg sends.
public sealed class WebRadioTests : IAsyncDisposable
{
    private const int BufferBytes = 32 * 1024;
    private static readonly TimeSpan Patience = TimeSpan.FromSeconds(10);

    private readonly TempDirectory _dir = new();
    private readonly StationStore _store;
    private readonly WebRadioProvider _provider;
    private readonly MediaHandles _handles = new();
    private readonly MediaEndpoint _endpoint;
    private readonly HttpClient _http = new(new SocketsHttpHandler
    {
        AllowAutoRedirect = false,
        AutomaticDecompression = DecompressionMethods.None,
        UseProxy = false,
    });
    private FakeStation? _station;

    public WebRadioTests()
    {
        var database = new LibraryDatabase(Path.Combine(_dir.Path, "library.db"));
        Assert.True(database.Migrate().IsSuccess);
        _store = new StationStore(database);
        _provider = new WebRadioProvider(_store, Path.Combine(_dir.Path, "artwork"), BufferBytes);
        _endpoint = new MediaEndpoint(new MediaEndpointOptions(IPAddress.Loopback, 0, null), _handles,
            NullLoggerFactory.Instance);
    }

    private static CancellationToken Ct => TestContext.Current.CancellationToken;

    public async ValueTask DisposeAsync()
    {
        await _endpoint.StopAsync(CancellationToken.None);
        await _endpoint.DisposeAsync();
        _provider.Dispose();
        if (_station is not null)
            await _station.DisposeAsync();
        _http.Dispose();
        _dir.Dispose();
    }

    [Fact]
    public void M3u_and_pls_import_with_names_genres_and_logos_and_a_line_that_is_no_url_refuses_the_file()
    {
        var m3u = Ok(StationPlaylist.Parse("""
            #EXTM3U
            #EXTINF:-1 tvg-logo="https://radio.example/logo.png" group-title="Jazz",Jazz, Blues & More
            http://radio.example:8000/jazz

            https://stream.example/news?format=mp3
            """));
        Assert.Equal(
            [
                new NewStation("Jazz, Blues & More", new Uri("http://radio.example:8000/jazz"), Genre: "Jazz",
                    Logo: new Uri("https://radio.example/logo.png")),
                new NewStation("stream.example/news", new Uri("https://stream.example/news?format=mp3")),
            ],
            m3u);

        var pls = Ok(StationPlaylist.Parse("""
            [playlist]
            NumberOfEntries=2
            File2=http://two.example/live
            File1=http://one.example/live
            Title1=Radio Eins
            Version=2
            """));
        Assert.Equal(["Radio Eins", "two.example/live"], pls.Select(s => s.Name));

        var bad = StationPlaylist.Parse("#EXTM3U\nhttp://ok.example/\nC:\\music\\song.mp3\n");
        Assert.Equal(LibraryError.InvalidArgument, bad.Failure.Code);
        Assert.Equal(@"line 3: not an http or https URL: C:\music\song.mp3", bad.Failure.Detail);
        Assert.Equal(LibraryError.InvalidArgument, StationPlaylist.Parse("#EXTM3U\n").Failure.Code);
    }

    [Fact]
    public async Task Stations_are_added_once_by_stream_url_searched_browsed_and_removed()
    {
        var imported = Ok(_store.Import(
        [
            new NewStation("Radio Swiss Jazz", new Uri("http://jazz.example/"), Genre: "Jazz"),
            new NewStation("Deutschlandfunk", new Uri("http://dlf.example/"), new Uri("https://dlf.example/home"), "Nachrichten"),
            new NewStation("Café Olé", new Uri("http://cafe.example/")),
        ]));
        var again = Ok(_store.Add(new NewStation("Another name", new Uri("http://jazz.example/"))));
        Assert.Equal(imported[0], again);
        Assert.Equal(LibraryError.InvalidArgument, _store.Add(new NewStation(" ", new Uri("http://x.example/"))).Failure.Code);
        Assert.Equal(LibraryError.InvalidArgument, _store.Add(new NewStation("File", new Uri("file:///c:/radio.mp3"))).Failure.Code);

        Assert.Equal(["Radio Swiss Jazz"], (await Search("jazz")).Select(s => s.Name));
        Assert.Equal(["Deutschlandfunk"], (await Search("nachr")).Select(s => s.Name));
        Assert.Equal(["Café Olé"], (await Search("cafe ole")).Select(s => s.Name));
        var found = (await Search("deutschlandfunk"))[0];
        Assert.Equal(new RadioStation(WebRadioProvider.ProviderId, $"station:{imported[1].Id}", "Deutschlandfunk",
            "Nachrichten", new Uri("https://dlf.example/home")), found);

        // By name and by genre: every station, and one node per genre.
        var root = (await _provider.BrowseAsync(new(null, 0, 10), Ct)).Value;
        Assert.Equal(["stations", "genre:Jazz", "genre:Nachrichten"], root.Nodes.Select(n => n.NodeId));
        Assert.Equal((BrowseKind.Stations, BrowseKind.Genre, "1 station"), (root.Nodes[0].Kind, root.Nodes[1].Kind, root.Nodes[1].Subtitle));
        Assert.All(root.Nodes, n => Assert.False(n.Playable));
        var all = (await _provider.BrowseAsync(new("stations", 0, 10), Ct)).Value;
        Assert.Equal(["Café Olé", "Deutschlandfunk", "Radio Swiss Jazz"], all.Items.Stations.Select(s => s.Name));
        Assert.Equal((3, true, "name"), (all.Total, all.Filterable, all.Sort));
        Assert.Equal([new BrowseLetter("C", 0), new BrowseLetter("D", 1), new BrowseLetter("R", 2)], all.Letters);
        Assert.Equal(["Deutschlandfunk"], (await _provider.BrowseAsync(new("stations", 1, 1), Ct)).Value.Items.Stations.Select(s => s.Name));
        Assert.Equal(["Radio Swiss Jazz"], (await _provider.BrowseAsync(new("stations", Letter: "P"), Ct)).Value.Items.Stations.Select(s => s.Name));
        Assert.Equal(["Café Olé"], (await _provider.BrowseAsync(new("stations", Filter: "cafe"), Ct)).Value.Items.Stations.Select(s => s.Name));
        Assert.Equal(["Radio Swiss Jazz"], (await _provider.BrowseAsync(new("genre:jazz", 0, 10), Ct)).Value.Items.Stations.Select(s => s.Name));
        Assert.Equal(ProviderError.NotFound, (await _provider.BrowseAsync(new("genre:polka", 0, 10), Ct)).Failure.Code);
        Assert.Equal(ProviderError.NotFound, (await _provider.BrowseAsync(new("genres", 0, 10), Ct)).Failure.Code);
        Assert.Equal(ProviderError.InvalidArgument, (await _provider.BrowseAsync(new("stations", Sort: "added"), Ct)).Failure.Code);
        Assert.Equal(ProviderError.InvalidArgument, (await _provider.BrowseAsync(new("stations", 5, 10, Letter: "D"), Ct)).Failure.Code);
        Assert.Equal(ProviderError.InvalidArgument, (await _provider.BrowseAsync(new(null, Filter: "x"), Ct)).Failure.Code);

        Assert.True(Ok(_store.Remove(imported[1].Id)));
        Assert.Empty(await Search("deutschlandfunk"));
        Assert.Equal(LibraryError.NotFound, _store.Remove(imported[1].Id).Failure.Code);
        var removed = await _provider.ResolveAsync(found, Ct);
        Assert.Equal(ProviderError.NotFound, removed.Failure.Code);
    }

    [Fact]
    public async Task A_served_station_has_no_length_and_no_ranges_and_starts_at_the_stations_first_byte()
    {
        var uri = await ServeAsync(await StationAsync());

        using var response = await GetAsync(uri);

        Assert.Equal(HttpStatusCode.OK, response.StatusCode);
        Assert.Null(response.Content.Headers.ContentLength);
        Assert.Empty(response.Headers.AcceptRanges);
        Assert.True(response.Headers.TransferEncodingChunked);
        Assert.Equal("audio/mpeg", response.Content.Headers.ContentType?.MediaType);
        await using var body = await response.Content.ReadAsStreamAsync(Ct);
        AssertPattern(await ReadAsync(body, 200_000), 0);

        // Upstream: ICY metadata asked for, no compression.
        var upstream = Assert.Single(_station!.Requests);
        Assert.Equal("1", upstream["Icy-MetaData"]);
        Assert.False(upstream.ContainsKey("Accept-Encoding"));
    }

    [Fact]
    public async Task Icy_metadata_is_cut_out_of_the_bytes_and_its_title_kept_for_the_ui()
    {
        var station = await StationAsync();
        _station!.MetaInterval = 8000;
        _station.Title = "Miles Davis - So What";
        var uri = await ServeAsync(station);

        using var response = await GetAsync(uri);
        await using var body = await response.Content.ReadAsStreamAsync(Ct);

        AssertPattern(await ReadAsync(body, 100_000), 0);
        Assert.Equal("Miles Davis - So What", _provider.CurrentTitle(station.ProviderItemId));
    }

    [Fact]
    public async Task A_slow_reader_is_never_cut_and_the_jitter_buffer_stays_inside_its_bound()
    {
        var uri = await ServeAsync(await StationAsync());
        using var response = await GetAsync(uri);
        await using var body = await response.Content.ReadAsStreamAsync(Ct);
        AssertPattern(await ReadAsync(body, 10_000), 0);

        // A paused zone: nothing read for 3 s, while the station sends as fast
        // as it is let.
        long most = 0;
        var clock = System.Diagnostics.Stopwatch.StartNew();
        while (clock.Elapsed < TimeSpan.FromSeconds(3))
        {
            var buffered = Assert.Single(_provider.Buffered);
            Assert.InRange(buffered, 0, _provider.BufferBound);
            most = Math.Max(most, buffered);
            await Task.Delay(10, Ct);
        }
        Assert.True(most >= BufferBytes / 2, $"the buffer held at most {most} bytes");

        // Read on: every byte in order, none lost and none twice.
        AssertPattern(await ReadAsync(body, 500_000), 10_000);
    }

    [Fact]
    public async Task An_upstream_drop_or_end_reaches_the_reader_as_a_dropped_connection()
    {
        foreach (var end in new[] { false, true })
        {
            var station = await StationAsync();
            if (end)
                _station!.EndAfter = 50_000;
            else
                _station!.DropAfter = 50_000;
            var uri = await ServeAsync(station);

            using var response = await GetAsync(uri);
            await using var body = await response.Content.ReadAsStreamAsync(Ct);

            // The station's bytes, then an error, never a clean end: an abort
            // drops what the endpoint had not sent yet, so fewer may arrive.
            var received = new MemoryStream();
            var buffer = new byte[8192];
            await Assert.ThrowsAnyAsync<IOException>(async () =>
            {
                int read;
                while ((read = await body.ReadAsync(buffer, Ct)) > 0)
                    received.Write(buffer, 0, read);
            });
            Assert.InRange(received.Length, 0, 50_000);
            AssertPattern(received.ToArray(), 0);

            await _station.DisposeAsync();
            _station = null;
        }
    }

    [Fact]
    public async Task A_relay_is_read_once_and_one_retired_unopened_lets_its_upstream_go()
    {
        var station = await StationAsync();
        var resolved = (await _provider.ResolveAsync(station, Ct)).Value;
        var uri = await ServeAsync(resolved);
        using (var first = await GetAsync(uri))
        {
            Assert.Equal(HttpStatusCode.OK, first.StatusCode);
            using var second = await GetAsync(uri);
            Assert.Equal(HttpStatusCode.NotFound, second.StatusCode);
        }
        // The reader closed with its GET, and the upstream with it.
        await Until(() => _station!.Open == 0 && _provider.Buffered.Count == 0);

        var unopened = await ServeAsync(station);
        await Until(() => _station!.Open == 1);
        _endpoint.Retire(unopened);
        await Until(() => _station!.Open == 0 && _provider.Buffered.Count == 0);
    }

    [Fact]
    public async Task A_station_that_refuses_or_does_not_answer_fails_to_resolve_by_name()
    {
        var station = await StationAsync();
        _station!.Status = HttpStatusCode.NotFound;
        var refused = await _provider.ResolveAsync(station, Ct);
        Assert.Equal(ProviderError.NotFound, refused.Failure.Code);
        Assert.Equal("Fixture FM: the station answered HTTP 404", refused.Failure.Detail);

        _station.Status = HttpStatusCode.ServiceUnavailable;
        Assert.Equal(ProviderError.Unavailable, (await _provider.ResolveAsync(station, Ct)).Failure.Code);

        var url = _station.Url;
        await _station.DisposeAsync();
        _station = null;
        var gone = await _provider.ResolveAsync(station, Ct);
        Assert.Equal(ProviderError.Unavailable, gone.Failure.Code);
        Assert.DoesNotContain(url.Authority, gone.Failure.Detail, StringComparison.Ordinal);
    }

    [Fact]
    public async Task A_stations_logo_is_its_artwork_fetched_once_and_a_station_without_one_has_none()
    {
        byte[] png = [0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 1, 2, 3];
        var fetches = 0;
        var (app, baseUri) = await FixtureHttp.StartAsync(async context =>
        {
            Interlocked.Increment(ref fetches);
            if (context.Request.Path != "/logo.png")
            {
                context.Response.StatusCode = 404;
                return;
            }
            context.Response.ContentType = "image/png";
            await context.Response.Body.WriteAsync(png);
        });
        await using var _ = app;
        var stations = Ok(_store.Import(
        [
            new NewStation("With Logo", new Uri("http://logo.example/"), Logo: new Uri(baseUri, "logo.png")),
            new NewStation("Lost Logo", new Uri("http://lost.example/"), Logo: new Uri(baseUri, "gone.png")),
            new NewStation("No Logo", new Uri("http://none.example/")),
        ]));

        var listed = (await _provider.BrowseAsync(new(WebRadioProvider.StationsNode, 0, 10), Ct)).Value.Items.Stations;
        var withLogo = listed.Single(s => s.Name == "With Logo");
        Assert.Equal(new ArtworkRef(WebRadioProvider.ProviderId, $"station:{stations[0].Id}"), withLogo.Artwork);
        Assert.Null(listed.Single(s => s.Name == "No Logo").Artwork);

        for (var i = 0; i < 2; i++)
        {
            var artwork = (await _provider.GetArtworkAsync(withLogo.Artwork!, Ct)).Value;
            Assert.Equal("image/png", artwork.ContentType);
            Assert.Equal(png, artwork.Bytes.ToArray());
        }
        Assert.Equal(1, fetches);

        var lost = await _provider.GetArtworkAsync(listed.Single(s => s.Name == "Lost Logo").Artwork!, Ct);
        Assert.Equal(ProviderError.NotFound, lost.Failure.Code);
        Assert.Equal("artwork of Lost Logo: HTTP 404", lost.Failure.Detail);
        var none = await _provider.GetArtworkAsync(new ArtworkRef(WebRadioProvider.ProviderId, $"station:{stations[2].Id}"), Ct);
        Assert.Equal("No Logo has no logo", none.Failure.Detail);
        Assert.Equal(ProviderError.NotFound, (await _provider.GetArtworkAsync(new ArtworkRef("webradio", "x"), Ct)).Failure.Code);
    }

    // --- helpers ------------------------------------------------------------

    private async Task<RadioStation> StationAsync()
    {
        _station = new FakeStation();
        var stored = Ok(_store.Add(new NewStation("Fixture FM", _station.Url)));
        return new RadioStation(WebRadioProvider.ProviderId, $"station:{stored.Id}", stored.Name);
    }

    private async Task<Uri> ServeAsync(RadioStation station)
    {
        var resolved = await _provider.ResolveAsync(station, Ct);
        Assert.True(resolved.IsSuccess, resolved.ToString());
        return await ServeAsync(resolved.Value);
    }

    private async Task<Uri> ServeAsync(ResolvedStream stream)
    {
        if (_endpoint.BaseUri is null)
            await _endpoint.StartAsync(Ct);
        return _endpoint.Publish(stream);
    }

    private async Task<IReadOnlyList<RadioStation>> Search(string text) =>
        (await _provider.SearchAsync(text, SearchKinds.All, 0, 10, Ct)).Value.Stations;

    // What FFmpeg sends.
    private Task<HttpResponseMessage> GetAsync(Uri uri)
    {
        var request = new HttpRequestMessage(HttpMethod.Get, uri);
        request.Headers.Range = new System.Net.Http.Headers.RangeHeaderValue(0, null);
        request.Headers.Add("Icy-MetaData", "1");
        request.Headers.ConnectionClose = true;
        return _http.SendAsync(request, HttpCompletionOption.ResponseHeadersRead, Ct);
    }

    private static async Task<byte[]> ReadAsync(Stream body, int count)
    {
        var data = new byte[count];
        await body.ReadExactlyAsync(data, Ct);
        return data;
    }

    private static void AssertPattern(byte[] data, long from)
    {
        for (var i = 0; i < data.Length; i++)
        {
            if (data[i] != FakeStation.Pattern(from + i))
                Assert.Fail($"byte {from + i} is {data[i]}, not {FakeStation.Pattern(from + i)}");
        }
    }

    private static T Ok<T>(Result<T, LibraryError> result)
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
