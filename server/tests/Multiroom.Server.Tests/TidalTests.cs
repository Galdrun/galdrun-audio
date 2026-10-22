using System.Net;
using System.Net.Http.Headers;
using System.Security.AccessControl;
using System.Security.Principal;
using Microsoft.Extensions.Logging.Abstractions;
using Multiroom.Server.Abstractions;
using Multiroom.Server.Providers.Tidal;
using Multiroom.Server.Web.Media;

namespace Multiroom.Server.Tests;

// The Tidal provider against FakeTidal: device login and silent refresh,
// the catalog normalized into the domain model, tracks reassembled into the
// bounded cache and served by a real media endpoint with FFmpeg's headers,
// and cover art. Nothing here touches the network.
public sealed class TidalTests : IAsyncDisposable
{
    private static readonly TimeSpan Patience = TimeSpan.FromSeconds(10);

    // The pieces a one-file track is fetched in (TrackCache.ChunkBytes).
    private const int Chunk = 1024 * 1024;

    private readonly TempDirectory _dir = new();
    private readonly MediaHandles _handles = new();
    private readonly MediaEndpoint _endpoint;
    private readonly HttpClient _http = new(new SocketsHttpHandler
    {
        AllowAutoRedirect = false,
        AutomaticDecompression = DecompressionMethods.None,
        UseProxy = false,
    });
    private readonly List<TidalProvider> _providers = [];
    private FakeTidal? _tidal;

    public TidalTests()
    {
        _endpoint = new MediaEndpoint(new MediaEndpointOptions(IPAddress.Loopback, 0, null), _handles,
            NullLoggerFactory.Instance);
    }

    private static CancellationToken Ct => TestContext.Current.CancellationToken;

    private string Directory => Path.Combine(_dir.Path, "tidal");

    public async ValueTask DisposeAsync()
    {
        await _endpoint.StopAsync(CancellationToken.None);
        await _endpoint.DisposeAsync();
        foreach (var provider in _providers)
            provider.Dispose();
        if (_tidal is not null)
            await _tidal.DisposeAsync();
        _http.Dispose();
        _dir.Dispose();
    }

    [Fact]
    public async Task Device_login_links_the_account_and_keeps_owner_only_tokens_for_the_same_application()
    {
        var tidal = await TidalAsync();
        var provider = Provider(seed: false);

        var waiting = await provider.GetStatusAsync(Ct);
        Assert.Equal(ProviderState.AuthRequired, waiting.State);
        Assert.StartsWith("sign in with POST /api/v1/providers/tidal/login", waiting.Detail, StringComparison.Ordinal);
        Assert.Empty(tidal.Forms);
        var refused = await provider.SearchAsync("miles", SearchKinds.All, 0, 10, Ct);
        Assert.Equal(ProviderError.Unavailable, refused.Failure.Code);
        Assert.Contains("not authorized", refused.Failure.Detail, StringComparison.Ordinal);

        await AuthorizeAsync(provider);
        var ready = await provider.GetStatusAsync(Ct);
        Assert.StartsWith("user 4711 (DE), quality Lossless;", ready.Detail, StringComparison.Ordinal);
        Assert.True((await provider.SearchAsync("miles", SearchKinds.All, 0, 10, Ct)).IsSuccess);

        var exchange = Assert.Single(tidal.Forms, f => f.ContainsKey("grant_type"));
        Assert.Equal("urn:ietf:params:oauth:grant-type:device_code", exchange["grant_type"]);
        Assert.Equal("device-code-1", exchange["device_code"]);
        Assert.All(tidal.Forms, f => Assert.Equal((FakeTidal.ClientId, FakeTidal.ClientSecret), (f["client_id"], f["client_secret"])));
        Assert.Equal(1, tidal.DeviceStarts);

        // The tokens are kept for this account only, and are in no status.
        var file = Path.Combine(Directory, "token.json");
        Assert.Contains(tidal.RefreshToken, await File.ReadAllTextAsync(file, Ct), StringComparison.Ordinal);
        Assert.DoesNotContain(tidal.AccessToken!, ready.Detail, StringComparison.Ordinal);
        Assert.DoesNotContain(FakeTidal.ClientSecret, ready.Detail, StringComparison.Ordinal);
        if (OperatingSystem.IsWindows())
        {
            var acl = new FileInfo(file).GetAccessControl();
            Assert.True(acl.AreAccessRulesProtected);
            var rule = Assert.Single(acl.GetAccessRules(true, true, typeof(SecurityIdentifier)).Cast<FileSystemAccessRule>());
            Assert.Equal(WindowsIdentity.GetCurrent().User, rule.IdentityReference);
        }
        else
        {
            Assert.Equal(UnixFileMode.UserRead | UnixFileMode.UserWrite, File.GetUnixFileMode(file));
        }

        // A later run reads the link and asks for none.
        var again = Provider(seed: false);
        Assert.Equal(ProviderState.Ready, (await again.GetStatusAsync(Ct)).State);
        Assert.Equal(1, tidal.DeviceStarts);
    }

    [Fact]
    public async Task Without_a_client_id_login_is_unavailable()
    {
        var tidal = await TidalAsync();
        var provider = new TidalProvider(
            new TidalSettings(null, null, TidalQuality.Lossless, 64 * 1024 * 1024, 1024 * 1024),
            Directory, NullLogger.Instance, tidal.Endpoints);
        provider.Start();
        _providers.Add(provider);

        var waiting = await provider.GetStatusAsync(Ct);
        Assert.Equal(ProviderState.AuthRequired, waiting.State);
        Assert.StartsWith("no Tidal client id; set providers.tidal.clientId in the config file", waiting.Detail, StringComparison.Ordinal);
        Assert.Equal(ProviderError.Unavailable, (await provider.SearchAsync("miles", SearchKinds.All, 0, 10, Ct)).Failure.Code);
        Assert.Equal(0, tidal.DeviceStarts);
        Assert.Equal(ProviderError.Unavailable, (await provider.StartLoginAsync(Ct)).Failure.Code);
    }

    [Fact]
    public async Task A_link_kept_for_another_client_id_is_not_used_and_one_without_a_secret_signs_in_bare()
    {
        var tidal = await TidalAsync();
        tidal.Seed(Directory);
        var provider = new TidalProvider(
            new TidalSettings("other-client", null, TidalQuality.Lossless, 64 * 1024 * 1024, 1024 * 1024),
            Directory, NullLogger.Instance, tidal.Endpoints);
        provider.Start();
        _providers.Add(provider);

        Assert.Equal(ProviderState.AuthRequired, (await provider.GetStatusAsync(Ct)).State);
        await AuthorizeAsync(provider);
        var form = tidal.Forms.Last();
        Assert.Equal("other-client", form["client_id"]);
        Assert.False(form.ContainsKey("client_secret"));
    }

    [Fact]
    public async Task The_playback_client_renews_a_new_link_at_once_and_a_kept_one_at_its_first_use()
    {
        var tidal = await TidalAsync();
        var provider = Provider(seed: false, playback: true);
        await AuthorizeAsync(provider);

        // Signed in by the device client, renewed straight away by the playback one.
        var forms = tidal.Forms.Where(f => f.ContainsKey("grant_type")).ToList();
        Assert.Equal(["urn:ietf:params:oauth:grant-type:device_code", "refresh_token"], forms.Select(f => f["grant_type"]));
        Assert.Equal((FakeTidal.ClientId, FakeTidal.ClientSecret), (forms[0]["client_id"], forms[0]["client_secret"]));
        Assert.Equal((PlaybackId, PlaybackSecret), (forms[1]["client_id"], forms[1]["client_secret"]));
        var file = Path.Combine(Directory, "token.json");
        Assert.Contains($"\"renewedBy\":\"{PlaybackId}\"", await File.ReadAllTextAsync(file, Ct), StringComparison.Ordinal);
        Assert.Contains(tidal.AccessToken!, await File.ReadAllTextAsync(file, Ct), StringComparison.Ordinal);
        Assert.DoesNotContain(PlaybackSecret, (await provider.GetStatusAsync(Ct)).Detail, StringComparison.Ordinal);

        // A later run with the same client uses the token as it is.
        var refreshes = tidal.Refreshes;
        Assert.True((await Provider(seed: false, playback: true).SearchAsync("miles", SearchKinds.All, 0, 10, Ct)).IsSuccess);
        Assert.Equal(refreshes, tidal.Refreshes);

        // A link from before the playback client is renewed by it at its first use.
        var kept = await TidalAsync();
        var old = Provider();
        Assert.True((await old.SearchAsync("miles", SearchKinds.All, 0, 10, Ct)).IsSuccess);
        Assert.Equal(0, kept.Refreshes);
        Assert.True((await Provider(seed: false, playback: true).SearchAsync("miles", SearchKinds.All, 0, 10, Ct)).IsSuccess);
        var renewal = Assert.Single(kept.Forms, f => f.GetValueOrDefault("grant_type") == "refresh_token");
        Assert.Equal(PlaybackId, renewal["client_id"]);
        Assert.Contains($"\"renewedBy\":\"{PlaybackId}\"", await File.ReadAllTextAsync(file, Ct), StringComparison.Ordinal);
    }

    [Fact]
    public async Task Search_and_browse_answer_tracks_albums_artists_and_playlists_in_the_domain_model()
    {
        var tidal = await TidalAsync();
        var provider = Provider();

        var found = (await provider.SearchAsync("miles davis", SearchKinds.All, 0, 10, Ct)).Value;
        var cover = new ArtworkRef("tidal", FakeTidal.Cover);
        Assert.Equal(
            [
                new Track("tidal", "track:1001", "So What", "Miles Davis", "Kind of Blue", TimeSpan.FromSeconds(562), cover,
                    new AudioFormat("FLAC", 44100, 16, 2), MediaType.Music, MediaTypeSource.Provider),
                new Track("tidal", "track:1002", "Freddie Freeloader (Remastered)", "Miles Davis", "Kind of Blue",
                    TimeSpan.FromSeconds(589), cover, MediaType: MediaType.Music, MediaTypeSource: MediaTypeSource.Provider),
            ],
            found.Tracks);
        Assert.Equal([new Album("tidal", "album:1000", "Kind of Blue", "Miles Davis", 1959, cover)], found.Albums);
        Assert.Equal([new Artist("tidal", "artist:3346", "Miles Davis", new ArtworkRef("tidal", "0a1b2c3d-1111-2222-3333-444455556666"))],
            found.Artists);
        Assert.Empty(found.Stations);
        var list = Assert.Single(found.Playlists);
        Assert.Equal(($"playlist:{FakeTidal.PlaylistId}", "Late night", BrowseKind.Playlist, true),
            (list.NodeId, list.Title, list.Kind, list.Playable));
        Assert.Contains(tidal.Requests, r => r.Contains("types=ARTISTS,ALBUMS,TRACKS,PLAYLISTS", StringComparison.Ordinal));

        // Only the kinds asked for are asked of Tidal and answered.
        var playlists = (await provider.SearchAsync("late", SearchKinds.Playlists, 0, 10, Ct)).Value;
        Assert.Equal((0, 0, 0, 1), (playlists.Tracks.Count, playlists.Albums.Count, playlists.Artists.Count, playlists.Playlists.Count));
        Assert.Contains(tidal.Requests, r => r.StartsWith("search?query=late&types=PLAYLISTS&", StringComparison.Ordinal));
        var before = tidal.Requests.Count;
        Assert.Same(CatalogItems.Empty, (await provider.SearchAsync("late", SearchKinds.Stations, 0, 10, Ct)).Value);
        Assert.Equal(before, tidal.Requests.Count);

        var root = (await provider.BrowseAsync(new(null, 0, 10), Ct)).Value;
        Assert.Equal(["page:pages/for_you", "page:pages/explore_new_music", "collection", "page:pages/my_collection_my_mixes",
            "page:pages/genre_page", "page:pages/moods"], root.Nodes.Select(n => n.NodeId));
        Assert.All(root.Nodes, n => Assert.Equal(BrowseKind.Root, n.Kind));

        var playlist = (await provider.BrowseAsync(new($"playlist:{FakeTidal.PlaylistId}", 0, 10), Ct)).Value;
        Assert.Equal(["track:1001", "track:1002"], playlist.Items.Tracks.Select(t => t.ProviderItemId));
        Assert.Equal(("Late night", "2 tracks", BrowseKind.Playlist, true),
            (playlist.Node!.Title, playlist.Node.Subtitle, playlist.Node.Kind, playlist.Node.Playable));
        var album = (await provider.BrowseAsync(new("album:1000", 0, 10), Ct)).Value;
        Assert.Equal(["So What", "Freddie Freeloader (Remastered)"], album.Items.Tracks.Select(t => t.Title));
        Assert.Equal(("Kind of Blue", "Miles Davis · 1959 · 5 tracks", 2), (album.Node!.Title, album.Node.Subtitle, album.Total));
        Assert.Equal(cover, album.Node.Artwork);
        var mix = (await provider.BrowseAsync(new($"mix:{FakeTidal.MixId}", 0, 10), Ct)).Value;
        Assert.Equal(["So What"], mix.Items.Tracks.Select(t => t.Title));

        Assert.Equal(ProviderError.NotFound, (await provider.BrowseAsync(new("album:9", 0, 10), Ct)).Failure.Code);
        Assert.Equal(ProviderError.NotFound, (await provider.BrowseAsync(new("genres", 0, 10), Ct)).Failure.Code);
        Assert.Equal(ProviderError.NotFound, (await provider.BrowseAsync(new("playlist:../../users", 0, 10), Ct)).Failure.Code);
    }

    [Fact]
    public async Task The_collection_an_artist_and_tidals_own_pages_browse_as_sections_each_paged_on_its_own()
    {
        var tidal = await TidalAsync();
        var provider = Provider();

        var collection = (await provider.BrowseAsync(new("collection", 0, 10), Ct)).Value;
        Assert.Equal(["Playlists", "Albums", "Artists", "Tracks", "Your playlists"], collection.Sections!.Select(s => s.Title));
        Assert.Equal([$"playlist:{FakeTidal.PlaylistId}"], collection.Sections![0].Nodes.Select(n => n.NodeId));
        Assert.Equal((BrowseKind.Album, BrowseLayout.Grid), (collection.Sections[1].Nodes[0].Kind, collection.Sections[1].Layout));
        Assert.Equal(["artist:3346"], collection.Sections[2].Nodes.Select(n => n.NodeId));
        Assert.Equal(["So What", "Blue in Green", "Freddie Freeloader (Remastered)"], collection.Sections[3].Items.Tracks.Select(t => t.Title));
        Assert.Equal(("collection/tracks", true, 3), (collection.Sections[3].More!.NodeId, collection.Sections[3].More!.Playable,
            collection.Sections[3].Total));

        var byName = (await provider.BrowseAsync(new("collection/albums", 0, 10, Sort: "name"), Ct)).Value;
        Assert.Equal(["added", "name"], byName.Sorts!);
        Assert.Equal(("name", 1), (byName.Sort, byName.Total));
        Assert.Contains(tidal.Requests, r => r.StartsWith("users/4711/favorites/albums?", StringComparison.Ordinal) &&
            r.Contains("order=NAME", StringComparison.Ordinal) && r.Contains("orderDirection=ASC", StringComparison.Ordinal));
        Assert.Equal(ProviderError.InvalidArgument, (await provider.BrowseAsync(new("collection/albums", Sort: "year"), Ct)).Failure.Code);
        Assert.Equal(ProviderError.InvalidArgument, (await provider.BrowseAsync(new("collection/own", Sort: "name"), Ct)).Failure.Code);
        Assert.Equal(ProviderError.InvalidArgument, (await provider.BrowseAsync(new("album:1000", Sort: "name"), Ct)).Failure.Code);
        Assert.Equal(ProviderError.InvalidArgument, (await provider.BrowseAsync(new("collection/albums", Filter: "blue"), Ct)).Failure.Code);
        Assert.Equal(ProviderError.InvalidArgument, (await provider.BrowseAsync(new("collection/albums", Letter: "K"), Ct)).Failure.Code);

        var artist = (await provider.BrowseAsync(new("artist:3346", 0, 5), Ct)).Value;
        Assert.Equal(("Miles Davis", BrowseKind.Artist), (artist.Node!.Title, artist.Node.Kind));
        Assert.Equal(["Top tracks", "Albums", "Singles & EPs", "Similar artists"], artist.Sections!.Select(s => s.Title));
        Assert.Equal(["So What", "Freddie Freeloader (Remastered)"], artist.Sections![0].Items.Tracks.Select(t => t.Title));
        Assert.Equal(["album:1000"], artist.Sections[1].Nodes.Select(n => n.NodeId));
        Assert.Equal(["album:1100"], artist.Sections[2].Nodes.Select(n => n.NodeId));
        Assert.Equal(["artist:1566"], artist.Sections[3].Nodes.Select(n => n.NodeId));
        Assert.Equal(["artist:3346/toptracks", "artist:3346/albums", "artist:3346/singles", "artist:3346/similar"],
            artist.Sections.Select(s => s.More!.NodeId));
        var singles = (await provider.BrowseAsync(new("artist:3346/singles", 0, 10), Ct)).Value;
        Assert.Equal(["Milestones EP"], singles.Nodes.Select(n => n.Title));
        Assert.Equal(1, singles.Total);
        Assert.Equal(ProviderError.InvalidArgument, (await provider.BrowseAsync(new("artist:3346", 10, 5), Ct)).Failure.Code);

        var forYou = (await provider.BrowseAsync(new("page:pages/for_you", 0, 10), Ct)).Value;
        Assert.Equal("For You", forYou.Node!.Title);
        var sections = forYou.Sections!;
        Assert.Equal(["Recommended albums", "Your mixes", "Genres", "Popular tracks"], sections.Select(s => s.Title));
        var albums = forYou.Sections![0];
        Assert.Equal((9, "data:pages/data/abc-123?albumId=1", BrowseLayout.Grid), (albums.Total, albums.More!.NodeId, albums.Layout));
        Assert.Equal(("Kind of Blue", "Miles Davis · 1959 · 5 tracks"), (albums.Nodes[0].Title, albums.Nodes[0].Subtitle));
        Assert.Equal(($"mix:{FakeTidal.MixId}", "Miles Davis and more", true),
            (forYou.Sections[1].Nodes[0].NodeId, forYou.Sections[1].Nodes[0].Subtitle, forYou.Sections[1].Nodes[0].Playable));
        Assert.Equal(["page:pages/genre_jazz"], forYou.Sections[2].Nodes.Select(n => n.NodeId));
        Assert.Equal(["So What"], forYou.Sections[3].Items.Tracks.Select(t => t.Title));
        Assert.Null(forYou.Sections[3].More);

        var rest = (await provider.BrowseAsync(new(albums.More.NodeId, 0, 10), Ct)).Value;
        Assert.Equal(["album:1000"], rest.Nodes.Select(n => n.NodeId));
        Assert.Equal(1, rest.Total);
        foreach (var hostile in new[] { "page:https://evil.example/pages/x", "page:pages/../users/4711", "data:pages/x?a b=1", "page:" })
            Assert.Equal(ProviderError.NotFound, (await provider.BrowseAsync(new(hostile, 0, 10), Ct)).Failure.Code);
        Assert.DoesNotContain(tidal.Requests, r => r.Contains("evil", StringComparison.Ordinal) || r.Contains("..", StringComparison.Ordinal));
    }

    [Fact]
    public async Task Albums_playlists_mixes_an_artist_and_the_collections_tracks_expand_and_a_page_does_not()
    {
        await TidalAsync();
        var provider = Provider();

        Assert.Equal(["So What", "Freddie Freeloader (Remastered)"],
            (await provider.ExpandAsync("album:1000", 100, Ct)).Value.Tracks.Select(t => t.Title));
        Assert.Equal(2, (await provider.ExpandAsync($"playlist:{FakeTidal.PlaylistId}", 100, Ct)).Value.Tracks.Count);
        Assert.Single((await provider.ExpandAsync($"mix:{FakeTidal.MixId}", 100, Ct)).Value.Tracks);
        Assert.Equal(2, (await provider.ExpandAsync("artist:3346", 100, Ct)).Value.Tracks.Count);

        var favourites = (await provider.ExpandAsync("collection/tracks", 100, Ct)).Value;
        Assert.Equal(["So What", "Freddie Freeloader (Remastered)"], favourites.Tracks.Select(t => t.Title));
        Assert.Equal(("Blue in Green", "Tidal does not stream it to this account"),
            (Assert.Single(favourites.Refused).Track.Title, favourites.Refused[0].Reason));
        Assert.False(favourites.Truncated);

        // An album whose track titles are chapters is a book, said by the provider.
        var book = (await provider.ExpandAsync("album:2000", 100, Ct)).Value.Tracks;
        Assert.Equal(3, book.Count);
        Assert.All(book, t => Assert.Equal((MediaType.Audiobook, MediaTypeSource.Provider), (t.MediaType, t.MediaTypeSource)));
        Assert.All((await provider.ExpandAsync("album:1000", 100, Ct)).Value.Tracks, t => Assert.Equal(MediaType.Music, t.MediaType));

        var cut = (await provider.ExpandAsync("album:1000", 1, Ct)).Value;
        Assert.Equal((1, true), (cut.Tracks.Count, cut.Truncated));

        Assert.Equal(ProviderError.InvalidArgument, (await provider.ExpandAsync("page:pages/for_you", 100, Ct)).Failure.Code);
        Assert.Equal(ProviderError.InvalidArgument, (await provider.ExpandAsync("collection/albums", 100, Ct)).Failure.Code);
        Assert.Equal(ProviderError.NotFound, (await provider.ExpandAsync("nonsense", 100, Ct)).Failure.Code);
    }

    [Fact]
    public async Task An_access_token_that_expires_mid_browse_is_refreshed_silently_and_a_refused_refresh_asks_again()
    {
        var tidal = await TidalAsync();
        var provider = Provider();
        Assert.True((await provider.BrowseAsync(new("collection/own", 0, 10), Ct)).IsSuccess);

        tidal.ExpireAccess();
        var page = await provider.BrowseAsync(new($"playlist:{FakeTidal.PlaylistId}", 0, 10), Ct);
        Assert.True(page.IsSuccess, page.ToString());
        Assert.Equal(1, tidal.Refreshes);
        Assert.Contains(tidal.AccessToken!, await File.ReadAllTextAsync(Path.Combine(Directory, "token.json"), Ct),
            StringComparison.Ordinal);

        // A token that has run out is refreshed before it is used.
        var expired = await TidalAsync();
        var later = Provider(expiresAt: DateTimeOffset.UtcNow.AddSeconds(-10));
        Assert.True((await later.SearchAsync("so what", SearchKinds.All, 0, 10, Ct)).IsSuccess);
        Assert.Equal(1, expired.Refreshes);

        // Refused: the link is forgotten and a new browser login is required.
        expired.RefreshRefused = true;
        expired.ExpireAccess();
        var refused = await later.SearchAsync("so what", SearchKinds.All, 0, 10, Ct);
        Assert.Equal(ProviderError.Unavailable, refused.Failure.Code);
        Assert.False(File.Exists(Path.Combine(Directory, "token.json")));
        Assert.Contains("sign in with POST", (await later.GetStatusAsync(Ct)).Detail, StringComparison.Ordinal);
        Assert.Equal(0, expired.DeviceStarts);
        Assert.Equal(ProviderState.AuthRequired, (await later.GetStatusAsync(Ct)).State);
    }

    [Fact]
    public async Task A_dash_track_arriving_out_of_order_is_reassembled_byte_stable_and_served_seekable()
    {
        var tidal = await TidalAsync();
        // An init segment and five media segments of different lengths; the
        // first ones arrive last.
        int[] lengths = [1200, 70_000, 65_536, 90_001, 3, 40_000];
        byte[][] segments = [.. lengths.Select((n, s) => Bytes(n, s))];
        tidal.AddDashTrack(2001, segments, [200, 300, 0, 0, 0, 0]);
        byte[] whole = [.. segments.SelectMany(s => s)];
        var provider = Provider();

        var resolved = (await provider.ResolveAsync(Track(2001), Ct)).Value;
        Assert.Equal(StreamShape.KnownLength, resolved.Shape);
        Assert.Equal(whole.Length, resolved.Length);
        Assert.Equal("audio/mp4", resolved.ContentType);
        Assert.Equal(new AudioFormat("FLAC", 44100, 16, 2), resolved.Format);
        Assert.Equal(["LOSSLESS"], tidal.Qualities);

        var uri = await ServeAsync(resolved);
        foreach (var offset in new long[] { 0, 1199, 1200, 71_199, 150_000, whole.Length - 128 })
        {
            using var response = await GetAsync(uri, offset);
            Assert.Equal(HttpStatusCode.PartialContent, response.StatusCode);
            Assert.Equal($"bytes {offset}-{whole.Length - 1}/{whole.Length}", response.Content.Headers.ContentRange?.ToString());
            Assert.Equal(whole[(int)offset..], await response.Content.ReadAsByteArrayAsync(Ct));
        }

        // Complete on disk, byte for byte, and answered from there: a second
        // staging fetches nothing.
        var tracks = Path.Combine(Directory, "tracks");
        await Until(() => Task.FromResult(System.IO.Directory.GetFiles(tracks, "*.mp4").Length == 1));
        Assert.Equal(whole, await File.ReadAllBytesAsync(Path.Combine(tracks, "2001-LOSSLESS.LOSSLESS.mp4"), Ct));
        var fetched = segments.Select((_, i) => tidal.FetchCount(i == 0 ? "d2001-init" : $"d2001-{i}")).Sum();
        Assert.Equal(whole, await ReadAllAsync(await ServeAsync((await provider.ResolveAsync(Track(2001), Ct)).Value)));
        Assert.Equal(fetched, segments.Select((_, i) => tidal.FetchCount(i == 0 ? "d2001-init" : $"d2001-{i}")).Sum());
        Assert.Single(tidal.Qualities);

        // Nothing outlives the run: disposing deletes the files, and a later
        // run deletes what a crash left and fetches the track again.
        provider.Dispose();
        Assert.Empty(System.IO.Directory.GetFiles(tracks));
        var leftover = Path.Combine(tracks, "2001-LOSSLESS.LOSSLESS.mp4");
        await File.WriteAllBytesAsync(leftover, whole, Ct);
        var restarted = Provider();
        Assert.False(File.Exists(leftover));
        var again = (await restarted.ResolveAsync(Track(2001), Ct)).Value;
        Assert.Equal(new AudioFormat("FLAC", 44100, 16, 2), again.Format);
        Assert.Equal(whole, await ReadAllAsync(await ServeAsync(again)));
        Assert.Equal(2, tidal.Qualities.Count);
    }

    [Fact]
    public async Task A_one_file_track_is_fetched_in_ranges_or_whole_when_its_server_takes_none()
    {
        var tidal = await TidalAsync();
        var data = Bytes(3 * Chunk + 12_345, 7);
        tidal.AddBtsTrack(3001, data);
        tidal.AddBtsTrack(3002, data[..500_000], ranges: false);
        var provider = Provider();

        var ranged = (await provider.ResolveAsync(Track(3001), Ct)).Value;
        Assert.Equal(data.Length, ranged.Length);
        Assert.Equal("audio/flac", ranged.ContentType);
        using (var response = await GetAsync(await ServeAsync(ranged), 2 * Chunk + 5))
            Assert.Equal(data[(2 * Chunk + 5)..], await response.Content.ReadAsByteArrayAsync(Ct));
        // A probe of one byte, then each 1 MiB piece once: first and last first.
        var asked = tidal.Fetches["t3001"].ToList();
        Assert.Equal("bytes=0-0", asked[0]);
        Assert.Equal([$"bytes=0-{Chunk - 1}", $"bytes={3 * Chunk}-{data.Length - 1}"],
            asked[1..3].Order(StringComparer.Ordinal));
        Assert.Equal(5, asked.Count);

        var whole = (await provider.ResolveAsync(Track(3002), Ct)).Value;
        Assert.Equal(data[..500_000], await ReadAllAsync(await ServeAsync(whole)));
        Assert.Equal(2, tidal.FetchCount("t3002"));
    }

    [Fact]
    public async Task A_track_says_the_tier_tidal_granted_also_from_the_cache_and_a_lower_one_shows_on_status()
    {
        var tidal = await TidalAsync();
        tidal.AddBtsTrack(3101, Bytes(5000, 3), granted: "HIGH");
        tidal.AddBtsTrack(3102, Bytes(5000, 4), sampleRate: 48000, bitDepth: 24);
        var provider = Provider();

        var aac = new AudioFormat("AAC", Channels: 2, Bitrate: 320);
        var high = (await provider.ResolveAsync(Track(3101), Ct)).Value;
        Assert.Equal(aac, high.Format);
        Assert.Contains("quality Lossless (Tidal grants HIGH)", (await provider.GetStatusAsync(Ct)).Detail, StringComparison.Ordinal);
        var named = (await provider.ResolveAsync(Track(3102), Ct)).Value;
        Assert.Equal(new AudioFormat("FLAC", 48000, 24, 2), named.Format);
        await ReadAllAsync(await ServeAsync(high));
        await ReadAllAsync(await ServeAsync(named));

        await Until(() => Task.FromResult(System.IO.Directory.GetFiles(Path.Combine(Directory, "tracks"), "*.flac").Length == 2));
        Assert.True(File.Exists(Path.Combine(Directory, "tracks", "3101-LOSSLESS.HIGH.flac")));
        Assert.True(File.Exists(Path.Combine(Directory, "tracks", "3102-LOSSLESS.LOSSLESS-48000-24.flac")));

        // From the cache, without asking Tidal again.
        Assert.Equal(aac, (await provider.ResolveAsync(Track(3101), Ct)).Value.Format);
        Assert.Equal(new AudioFormat("FLAC", 48000, 24, 2), (await provider.ResolveAsync(Track(3102), Ct)).Value.Format);
        Assert.Equal(2, tidal.Qualities.Count);
    }

    [Fact]
    public async Task A_track_tidal_does_not_have_fails_by_name_and_an_encrypted_or_preview_one_is_unavailable()
    {
        var tidal = await TidalAsync();
        tidal.AddBtsTrack(4002, Bytes(1000, 1), encryption: "OLD_AES");
        tidal.AddBtsTrack(4003, Bytes(1000, 2), presentation: "PREVIEW");
        var provider = Provider();

        var missing = await provider.ResolveAsync(Track(4001, "Gone Song"), Ct);
        Assert.Equal(ProviderError.NotFound, missing.Failure.Code);
        Assert.StartsWith("Gone Song: ", missing.Failure.Detail, StringComparison.Ordinal);

        var encrypted = await provider.ResolveAsync(Track(4002, "Locked"), Ct);
        Assert.Equal(ProviderError.Unavailable, encrypted.Failure.Code);
        Assert.Equal("Locked: the stream is encrypted (OLD_AES)", encrypted.Failure.Detail);

        var preview = await provider.ResolveAsync(Track(4003, "Teaser"), Ct);
        Assert.Equal(ProviderError.Unavailable, preview.Failure.Code);
        Assert.Equal(ProviderError.NotFound, (await provider.ResolveAsync(new Track("tidal", "/etc/passwd", "x"), Ct)).Failure.Code);

        // Nothing was cached, and no signed URL is in any message.
        Assert.Equal((0, 0L, 0), TrackHeld(provider));
        Assert.DoesNotContain("signed-secret", encrypted.Failure.Detail + preview.Failure.Detail, StringComparison.Ordinal);
    }

    [Fact]
    public async Task The_cache_evicts_the_least_recently_played_track_not_in_use_and_refuses_when_all_is_in_use()
    {
        var tidal = await TidalAsync();
        foreach (var id in new long[] { 1, 2, 3 })
            tidal.AddBtsTrack(id, Bytes(100_000, (int)id));
        var provider = Provider(cacheBytes: 250_000);

        async Task<ResolvedStream> Play(long id)
        {
            var resolved = await provider.ResolveAsync(Track(id), Ct);
            Assert.True(resolved.IsSuccess, resolved.ToString());
            return resolved.Value;
        }

        (await Play(1)).Release();
        (await Play(2)).Release();
        (await Play(1)).Release();
        Assert.Equal(2, tidal.FetchCount("t1"));
        // 3 does not fit beside 1 and 2: 2 was played least recently.
        var three = await Play(3);
        Assert.Equal((2, 200_000L, 1), TrackHeld(provider));
        (await Play(1)).Release();
        Assert.Equal(2, tidal.FetchCount("t1"));
        var two = await Play(2);
        Assert.Equal(4, tidal.FetchCount("t2"));
        // 2 and 3 are in use: 1 goes, and nothing more fits.
        var full = await provider.ResolveAsync(Track(1), Ct);
        Assert.Equal(ProviderError.Unavailable, full.Failure.Code);
        Assert.Contains("do not fit the track cache", full.Failure.Detail, StringComparison.Ordinal);
        Assert.Equal((2, 200_000L, 2), TrackHeld(provider));
        Assert.Contains("track cache 2 track(s), 2 in use, 200000 of 250000 bytes", (await provider.GetStatusAsync(Ct)).Detail,
            StringComparison.Ordinal);

        two.Release();
        three.Release();
        (await Play(1)).Release();
        Assert.True(System.IO.Directory.GetFiles(Path.Combine(Directory, "tracks")).Length <= 2);
    }

    [Fact]
    public async Task Artwork_is_fetched_once_kept_within_its_bound_and_only_for_tidal_image_ids()
    {
        var tidal = await TidalAsync();
        var provider = Provider(artworkCacheBytes: 2_500);
        var cover = new ArtworkRef("tidal", FakeTidal.Cover);

        var first = (await provider.GetArtworkAsync(cover, Ct)).Value;
        var second = (await provider.GetArtworkAsync(cover, Ct)).Value;
        Assert.Equal("image/jpeg", first.ContentType);
        var expected = FakeTidal.Image("/images/aaaa1111/2222/3333/4444/555566667777/640x640.jpg");
        Assert.Equal(expected, first.Bytes.ToArray());
        Assert.Equal(expected, second.Bytes.ToArray());
        Assert.Equal(1, tidal.ImageFetches);

        foreach (var id in new[] { "11111111-0000-0000-0000-000000000000", "22222222-0000-0000-0000-000000000000" })
            Assert.True((await provider.GetArtworkAsync(new ArtworkRef("tidal", id), Ct)).IsSuccess);
        var files = System.IO.Directory.GetFiles(Path.Combine(Directory, "artwork"));
        Assert.Equal(2, files.Length);
        Assert.True(files.Sum(f => new FileInfo(f).Length) <= 2_500);

        Assert.Equal(ProviderError.NotFound, (await provider.GetArtworkAsync(new ArtworkRef("tidal", "../../token"), Ct)).Failure.Code);
        Assert.Equal(ProviderError.NotFound, (await provider.GetArtworkAsync(new ArtworkRef("local", FakeTidal.Cover), Ct)).Failure.Code);
    }

    // --- helpers ------------------------------------------------------------

    private async Task<FakeTidal> TidalAsync()
    {
        if (_tidal is not null)
            await _tidal.DisposeAsync();
        _tidal = await FakeTidal.StartAsync();
        return _tidal;
    }

    private const string PlaybackId = "fixture-playback";
    private const string PlaybackSecret = "fixture-playback-secret";

    private TidalProvider Provider(bool seed = true, DateTimeOffset? expiresAt = null, long cacheBytes = 64 * 1024 * 1024,
        long artworkCacheBytes = 1024 * 1024, bool playback = false)
    {
        if (seed)
            _tidal!.Seed(Directory, expiresAt);
        var provider = new TidalProvider(
            new TidalSettings(FakeTidal.ClientId, FakeTidal.ClientSecret, TidalQuality.Lossless, cacheBytes, artworkCacheBytes,
                playback ? PlaybackId : null, playback ? PlaybackSecret : null),
            Directory, NullLogger.Instance, _tidal!.Endpoints);
        provider.Start();
        _providers.Add(provider);
        return provider;
    }

    [Fact]
    public async Task Pending_device_login_is_reused()
    {
        var tidal = await TidalAsync();
        var provider = Provider(seed: false);
        var first = (await provider.StartLoginAsync(Ct)).Value;
        var repeated = await Task.WhenAll(provider.StartLoginAsync(Ct), provider.StartLoginAsync(Ct));
        Assert.All(repeated, result => Assert.Equal(first, result.Value));
        Assert.Equal("https://link.tidal.test/ABCDE", first.AuthorizeUrl.AbsoluteUri);
        Assert.Equal("ABCDE", first.UserCode);
        Assert.Equal(1, tidal.DeviceStarts);
        await Until(() => Task.FromResult(tidal.Forms.Any(f => f.ContainsKey("device_code"))));
        Assert.DoesNotContain("device-code-1", (await provider.GetStatusAsync(Ct)).Detail, StringComparison.Ordinal);
        tidal.Approve();
        await Until(async () => (await provider.GetStatusAsync(Ct)).State == ProviderState.Ready);
        Assert.Equal(1, tidal.DeviceStarts);
    }

    [Fact]
    public async Task Expired_or_denied_device_login_can_be_started_again()
    {
        var tidal = await TidalAsync();
        var provider = Provider(seed: false);
        tidal.DeviceLifetime = 1;
        Assert.True((await provider.StartLoginAsync(Ct)).IsSuccess);
        await Until(() => Task.FromResult(provider.Login is null));
        Assert.Contains("expired", (await provider.GetStatusAsync(Ct)).Detail, StringComparison.Ordinal);
        tidal.DeviceLifetime = 300;
        tidal.DeviceError = "access_denied";
        Assert.True((await provider.StartLoginAsync(Ct)).IsSuccess);
        await Until(() => Task.FromResult(provider.Login is null));
        Assert.Contains("refused", (await provider.GetStatusAsync(Ct)).Detail, StringComparison.Ordinal);
        tidal.DeviceError = null;
        await AuthorizeAsync(provider);
        Assert.Equal(3, tidal.DeviceStarts);
    }

    private async Task AuthorizeAsync(TidalProvider provider)
    {
        var login = await provider.StartLoginAsync(Ct);
        Assert.True(login.IsSuccess, login.ToString());
        _tidal!.Approve();
        await Until(async () => (await provider.GetStatusAsync(Ct)).State == ProviderState.Ready);
        await Until(() => Task.FromResult(provider.Login is null));
    }

    private static Track Track(long id, string title = "Fixture") => new("tidal", $"track:{id}", title);

    private static (int, long, int) TrackHeld(TidalProvider provider)
    {
        var detail = provider.GetStatusAsync(CancellationToken.None).GetAwaiter().GetResult().Detail;
        var part = detail[(detail.IndexOf("track cache ", StringComparison.Ordinal) + "track cache ".Length)..];
        var words = part.Split([' ', ','], StringSplitOptions.RemoveEmptyEntries);
        return (int.Parse(words[0], System.Globalization.CultureInfo.InvariantCulture),
            long.Parse(words[5], System.Globalization.CultureInfo.InvariantCulture),
            int.Parse(words[2], System.Globalization.CultureInfo.InvariantCulture));
    }

    private async Task<Uri> ServeAsync(ResolvedStream stream)
    {
        if (_endpoint.BaseUri is null)
            await _endpoint.StartAsync(Ct);
        return _endpoint.Publish(stream);
    }

    // What FFmpeg sends.
    private Task<HttpResponseMessage> GetAsync(Uri uri, long offset)
    {
        var request = new HttpRequestMessage(HttpMethod.Get, uri);
        request.Headers.Range = new RangeHeaderValue(offset, null);
        request.Headers.Add("Icy-MetaData", "1");
        request.Headers.ConnectionClose = true;
        return _http.SendAsync(request, Ct);
    }

    private async Task<byte[]> ReadAllAsync(Uri uri)
    {
        using var response = await GetAsync(uri, 0);
        Assert.Equal(HttpStatusCode.PartialContent, response.StatusCode);
        return await response.Content.ReadAsByteArrayAsync(Ct);
    }

    private static byte[] Bytes(int count, int seed) =>
        [.. Enumerable.Range(0, count).Select(i => (byte)((i * 7 + seed * 13 + i / 251) & 0xFF))];

    private static async Task Until(Func<Task<bool>> condition)
    {
        using var timeout = new CancellationTokenSource(Patience);
        while (!await condition())
            await Task.Delay(10, timeout.Token);
    }
}
