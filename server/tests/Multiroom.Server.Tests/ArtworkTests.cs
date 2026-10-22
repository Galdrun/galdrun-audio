using System.Collections.Concurrent;
using System.Diagnostics;
using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Http;
using Microsoft.Extensions.Logging.Abstractions;
using Multiroom.Server.Abstractions;
using Multiroom.Server.Library;
using Multiroom.Server.Providers.Covers;
using Multiroom.Server.Providers.Local;

namespace Multiroom.Server.Tests;

// The library's art: MusicBrainz resolving albums and artists to their ids
// (by name, or from what Picard tags), the Cover Art Archive, Wikidata and
// fanart.tv answering by those ids, all against a fixture of their answers;
// and the artwork add-in over a library the tests tag: covers kept on the
// server (the files' own, or found), nothing written to the library's
// folders, fanart.tv preferred, an artist's picture kept and served, an
// album without a cover shown by its artist's picture.
public sealed class ArtworkTests : IAsyncDisposable
{
    private const string KindOfBlue = "a1b2c3d4-0000-4000-8000-000000000001";
    private const string KindOfBlueSingle = "a1b2c3d4-0000-4000-8000-000000000002";
    private const string SketchesGroup = "a1b2c3d4-0000-4000-8000-000000000004";
    private const string SketchesRelease = "e0000000-0000-4000-8000-000000000001";
    private const string MilesDavis = "561d854a-6a28-4aa7-8c99-323e6ce46c2a";
    private const string BillEvans = "b6e37d1c-0000-4000-8000-000000000003";
    private const string FanartKey = "fanart-secret";

    private readonly TempDirectory _dir = new();
    private readonly HttpClient _http = new();
    private readonly ConcurrentQueue<(string Path, string Query, long At)> _requests = new();
    private WebApplication? _app;
    private Uri _base = null!;
    private volatile bool _down;
    private LibraryIndexer? _indexer;

    private static CancellationToken Ct => TestContext.Current.CancellationToken;

    private MusicBrainzEndpoints Endpoints => new(new Uri(_base, "mb/"), new Uri(_base, "caa/"),
        new Uri(_base, "wd/"), new Uri(_base, "commons/"));

    private int MusicBrainzRequests => _requests.Count(r => r.Path.StartsWith("/mb/", StringComparison.Ordinal));

    private int Searches => _requests.Count(r => r.Path is "/mb/release-group" or "/mb/artist");

    public async ValueTask DisposeAsync()
    {
        if (_indexer is not null)
        {
            await _indexer.StopAsync(CancellationToken.None);
            _indexer.Dispose();
        }
        if (_app is not null)
        {
            await _app.StopAsync();
            await _app.DisposeAsync();
        }
        _http.Dispose();
        _dir.Dispose();
    }

    [Fact]
    public async Task MusicBrainz_resolves_exactly_one_release_group_and_artist_by_name_and_takes_Picards_ids_as_they_are()
    {
        await StartAsync();
        var musicBrainz = new MusicBrainzSource(_http, Endpoints, TimeSpan.Zero);

        // The single of the same name is left out for the album; the title
        // and the artist match folded; the artist is the credited one.
        var kind = (await musicBrainz.ResolveAlbumAsync(new AlbumQuery("miles davis", "Kind Of Blue"), Ct)).Value;
        Assert.Equal((KindOfBlue, MilesDavis), (kind.ReleaseGroupId, kind.ArtistId));

        // Two albums of one name: no guess. Nothing: no match.
        Assert.False((await musicBrainz.ResolveAlbumAsync(new AlbumQuery("The Twins", "Twin"), Ct)).Value.Identified);
        Assert.False((await musicBrainz.ResolveAlbumAsync(new AlbumQuery("Nobody", "Nothing"), Ct)).Value.Identified);

        // A release alone is looked up for its group and artist; a group
        // tagged is not looked up at all.
        var release = (await musicBrainz.ResolveAlbumAsync(new AlbumQuery("x", "y", ReleaseId: SketchesRelease), Ct)).Value;
        Assert.Equal((SketchesGroup, MilesDavis), (release.ReleaseGroupId, release.ArtistId));
        var asked = MusicBrainzRequests;
        var tagged = (await musicBrainz.ResolveAlbumAsync(new AlbumQuery("x", "y", SketchesGroup, null, BillEvans), Ct)).Value;
        Assert.Equal((SketchesGroup, BillEvans), (tagged.ReleaseGroupId, tagged.ArtistId));
        Assert.Equal(asked, MusicBrainzRequests);

        // An artist alone: its id as given, else by its whole name, one match or none.
        Assert.Equal(BillEvans, (await musicBrainz.ResolveArtistAsync("bill evans", null, Ct)).Value);
        Assert.Null((await musicBrainz.ResolveArtistAsync("Nirvana", null, Ct)).Value);
        Assert.Null((await musicBrainz.ResolveArtistAsync("Bill", null, Ct)).Value);
        asked = MusicBrainzRequests;
        Assert.Equal(MilesDavis, (await musicBrainz.ResolveArtistAsync("Anyone", MilesDavis, Ct)).Value);
        Assert.Equal(asked, MusicBrainzRequests);

        _down = true;
        Assert.Equal(ProviderError.Unavailable, (await musicBrainz.ResolveArtistAsync("Bill Evans", null, Ct)).Failure.Code);
    }

    // Phase 40: a radio song names no album; the album it is on is found by
    // its recording, an album before a single or a compilation.
    [Fact]
    public async Task A_song_named_without_its_album_is_found_on_its_album_and_its_cover_kept_under_its_song_id()
    {
        await StartAsync();
        var musicBrainz = new MusicBrainzSource(_http, Endpoints, TimeSpan.Zero);
        var song = (await musicBrainz.ResolveSongAsync("So What", "Miles Davis", Ct)).Value!;
        Assert.Equal(("Kind of Blue", KindOfBlue, MilesDavis), (song.Album, song.ReleaseGroupId, song.ArtistId));
        Assert.Null((await musicBrainz.ResolveSongAsync("Nothing", "Nobody", Ct)).Value);

        // The client is the test's: the lookup is not disposed.
        var store = new ArtStore(Path.Combine(_dir.Path, "art"));
        var lookup = new ArtworkLookup(store, () => new ArtworkSourceSet(musicBrainz, [musicBrainz]), _http);
        Assert.Equal("added", (await lookup.SongAsync("So What", "Miles Davis", Ct)).Value);
        Assert.Equal(LibraryTests.Jpeg(7), store.Read(Artworks.SongId("So What", "Miles Davis")!).Value.Bytes.ToArray());
        Assert.Equal("present", (await lookup.SongAsync("so what", "Miles Davis", Ct)).Value);
        Assert.Equal("notFound", (await lookup.SongAsync("Nothing", "Nobody", Ct)).Value);
    }

    [Fact]
    public async Task The_Cover_Art_Archive_and_Wikidata_answer_by_id_at_1200_px()
    {
        await StartAsync();
        var musicBrainz = new MusicBrainzSource(_http, Endpoints, TimeSpan.Zero);

        // The release's own cover first, its group's next, without a request.
        var covers = (await musicBrainz.CoversAsync(new AlbumQuery("x", "y", SketchesGroup, SketchesRelease), Ct)).Value;
        Assert.Equal(
        [
            new Uri(_base, $"caa/release/{SketchesRelease}/front-1200").AbsoluteUri,
            new Uri(_base, $"caa/release-group/{SketchesGroup}/front-1200").AbsoluteUri,
        ], covers.Select(c => c.AbsoluteUri));
        Assert.Empty(_requests);

        Assert.Equal([new Uri(_base, "commons/wiki/Special:FilePath/Miles_Davis_1955.jpg?width=1200").AbsoluteUri],
            (await musicBrainz.PicturesAsync(MilesDavis, Ct)).Value.Select(p => p.AbsoluteUri));
        Assert.Empty((await musicBrainz.PicturesAsync("d0000000-0000-4000-8000-000000000001", Ct)).Value);
        Assert.Empty((await musicBrainz.PicturesAsync("../x", Ct)).Value);
    }

    [Fact]
    public async Task Fanart_tv_answers_most_liked_first_by_id_with_its_key_and_names_no_key_in_a_failure()
    {
        await StartAsync();
        var fanart = new FanartTvSource(_http, FanartKey, new Uri(_base, "fanart/"));

        Assert.Equal([$"{_base}fanart-img/kind-2.jpg", $"{_base}fanart-img/kind-1.jpg"],
            (await fanart.CoversAsync(new AlbumQuery("x", "y", KindOfBlue), Ct)).Value.Select(c => c.AbsoluteUri));
        Assert.Empty((await fanart.CoversAsync(new AlbumQuery("x", "y", SketchesGroup), Ct)).Value);
        Assert.Empty((await fanart.CoversAsync(new AlbumQuery("x", "y", ReleaseId: SketchesRelease), Ct)).Value);
        Assert.Equal([$"{_base}fanart-img/evans.jpg"],
            (await fanart.PicturesAsync(BillEvans, Ct)).Value.Select(p => p.AbsoluteUri));
        Assert.Empty((await fanart.PicturesAsync(MilesDavis, Ct)).Value);
        Assert.All(_requests, r => Assert.Equal($"?api_key={FanartKey}", r.Query));

        _down = true;
        var down = await fanart.PicturesAsync(BillEvans, Ct);
        Assert.Equal(ProviderError.Unavailable, down.Failure.Code);
        Assert.DoesNotContain(FanartKey, down.Failure.Detail, StringComparison.Ordinal);
    }

    [Fact]
    public async Task MusicBrainz_is_asked_one_request_at_a_time_at_least_its_spacing_apart()
    {
        await StartAsync();
        var musicBrainz = new MusicBrainzSource(_http, Endpoints, TimeSpan.FromMilliseconds(300));

        await Task.WhenAll(Enumerable.Range(0, 3).Select(_ => musicBrainz.ResolveArtistAsync("Nirvana", null, Ct)));

        var times = _requests.Where(r => r.Path.StartsWith("/mb/", StringComparison.Ordinal)).Select(r => r.At).Order().ToList();
        Assert.Equal(3, times.Count);
        for (var i = 1; i < times.Count; i++)
        {
            var gap = Stopwatch.GetElapsedTime(times[i - 1], times[i]);
            Assert.True(gap >= TimeSpan.FromMilliseconds(250), $"requests {gap.TotalMilliseconds:0} ms apart");
        }
    }

    [Fact]
    public async Task The_add_in_keeps_every_cover_on_the_server_prefers_fanart_tv_and_writes_nothing_to_the_library()
    {
        await StartAsync();
        var music = Directory.CreateDirectory(Path.Combine(_dir.Path, "music")).FullName;
        var kind = Directory.CreateDirectory(Path.Combine(music, "Kind of Blue")).FullName;
        LibraryTests.WriteTrack(Path.Combine(kind, "so-what.wav"), "So What", "Miles Davis", "Kind of Blue", 1);
        LibraryTests.WriteTrack(Path.Combine(kind, "freddie.wav"), "Freddie Freeloader", "Miles Davis", "Kind of Blue", 2);
        var digs = Directory.CreateDirectory(Path.Combine(music, "Everybody Digs")).FullName;
        LibraryTests.WriteTrack(Path.Combine(digs, "peace.wav"), "Peace Piece", "Bill Evans", "Everybody Digs", 1);
        File.WriteAllBytes(Path.Combine(digs, "cover.jpg"), LibraryTests.Jpeg(3));
        var lost = Directory.CreateDirectory(Path.Combine(music, "Lost Tapes")).FullName;
        LibraryTests.WriteTrack(Path.Combine(lost, "lost.wav"), "Lost", "Miles Davis", "Lost Tapes", 1);
        var twin = Directory.CreateDirectory(Path.Combine(music, "Twin")).FullName;
        LibraryTests.WriteTrack(Path.Combine(twin, "one.wav"), "One", "The Twins", "Twin", 1);
        LibraryTests.WriteTrack(Path.Combine(twin, "two.wav"), "Two", "The Twins", "Twin", 2);
        var sketches = Directory.CreateDirectory(Path.Combine(music, "Sketches")).FullName;
        LibraryTests.WriteTrack(Path.Combine(sketches, "concierto.wav"), "Concierto", "Miles Davis", "Sketches of Spain", 1);
        TagPicard(Path.Combine(sketches, "concierto.wav"), SketchesGroup, SketchesRelease, MilesDavis);
        var catalog = await IndexAsync(music);

        var store = new ArtStore(Path.Combine(_dir.Path, "art"));
        var musicBrainz = new MusicBrainzSource(_http, Endpoints, TimeSpan.Zero);
        using var addIn = new ArtworkAddIn(catalog, Folders(music), store, musicBrainz,
            [new FanartTvSource(_http, FanartKey, new Uri(_base, "fanart/")), musicBrainz], new HttpClient());
        async Task<string> Enrich(string title) =>
            (await addIn.EnrichAsync(await TrackAsync(catalog, title), Ct)).Value.GetRawText();

        // fanart.tv's cover before the archive's, kept on the server, the
        // album's folder left alone; the artist's picture from Wikidata, as
        // fanart.tv has none.
        Assert.Equal("""{"cover":"added","picture":"added"}""", await Enrich("so what"));
        Assert.Equal(LibraryTests.Jpeg(21), store.ReadAlbum("Kind of Blue", "Miles Davis").Value.Bytes.ToArray());
        Assert.Equal(LibraryTests.Jpeg(9), store.ReadArtist("miles davis").Value.Bytes.ToArray());
        Assert.Equal(["freddie.wav", "so-what.wav"], Directory.GetFiles(kind).Select(Path.GetFileName).Order());

        // The album's next track finds both kept, and asks nothing.
        var asked = _requests.Count;
        Assert.Equal("""{"cover":"present","picture":"present"}""", await Enrich("freddie"));
        Assert.Equal(asked, _requests.Count);

        // A cover beside the files is copied to the server; the artist is
        // resolved by name, and fanart.tv has its picture.
        Assert.Equal("""{"cover":"kept","picture":"added"}""", await Enrich("peace piece"));
        Assert.Equal(LibraryTests.Jpeg(3), store.ReadAlbum("Everybody Digs", "Bill Evans").Value.Bytes.ToArray());
        Assert.Equal(LibraryTests.Jpeg(22), store.ReadArtist("Bill Evans").Value.Bytes.ToArray());

        // Picard's ids: no search; fanart.tv has nothing, the archive the release's cover.
        var searches = Searches;
        Assert.Equal("""{"cover":"added","picture":"present"}""", await Enrich("concierto"));
        Assert.Equal(LibraryTests.Jpeg(11), store.ReadAlbum("Sketches of Spain", "Miles Davis").Value.Bytes.ToArray());
        Assert.Equal(searches, Searches);

        // No match: nothing kept, and the album's other track asks nothing.
        Assert.Equal("""{"cover":"notFound","picture":"notFound"}""", await Enrich("one"));
        asked = _requests.Count;
        Assert.Equal("""{"cover":"notFound","picture":"notFound"}""", await Enrich("two"));
        Assert.Equal(asked, _requests.Count);

        // The local provider serves what the server keeps, even with the
        // files' own cover gone; the artist's picture for an artist, and for
        // an album of the artist without a cover.
        Assert.Equal("""{"cover":"notFound","picture":"present"}""", await Enrich("lost"));
        File.Delete(Path.Combine(digs, "cover.jpg"));
        var provider = new LocalFilesProvider(Folders(music), catalog, _indexer!, Path.Combine(_dir.Path, "artwork"), store);
        async Task<byte[]> Artwork(ArtworkRef reference) => (await provider.GetArtworkAsync(reference, Ct)).Value.Bytes.ToArray();
        Assert.Equal(LibraryTests.Jpeg(21), await Artwork((await TrackAsync(catalog, "so what")).Artwork!));
        Assert.Equal(LibraryTests.Jpeg(3), await Artwork((await TrackAsync(catalog, "peace piece")).Artwork!));
        var evans = Assert.Single((await catalog.SearchAsync("bill evans", SearchKinds.Artists, 0, 10, Ct)).Value.Artists);
        Assert.Equal(LibraryTests.Jpeg(22), await Artwork(evans.Artwork!));
        Assert.Equal(LibraryTests.Jpeg(9), await Artwork((await TrackAsync(catalog, "lost")).Artwork!));
        Assert.Equal(ProviderError.NotFound,
            (await provider.GetArtworkAsync((await TrackAsync(catalog, "one")).Artwork!, Ct)).Failure.Code);
    }

    [Fact]
    public async Task Without_MusicBrainz_only_Picards_ids_reach_a_source_and_without_a_source_nobody_is_asked()
    {
        await StartAsync();
        var music = Directory.CreateDirectory(Path.Combine(_dir.Path, "music")).FullName;
        LibraryTests.WriteTrack(Path.Combine(music, "peace.wav"), "Peace Piece", "Bill Evans", "Everybody Digs", 1,
            picture: LibraryTests.Jpeg(5));
        LibraryTests.WriteTrack(Path.Combine(music, "so-what.wav"), "So What", "Miles Davis", "Kind of Blue", 1);
        LibraryTests.WriteTrack(Path.Combine(music, "tagged.wav"), "Tagged", "Bill Evans", "Kind of Blue Too", 1);
        TagPicard(Path.Combine(music, "tagged.wav"), KindOfBlue, null, BillEvans);
        var catalog = await IndexAsync(music);
        var store = new ArtStore(Path.Combine(_dir.Path, "art"));
        async Task<string> Enrich(ArtworkAddIn addIn, string title) =>
            (await addIn.EnrichAsync(await TrackAsync(catalog, title), Ct)).Value.GetRawText();

        using (var none = new ArtworkAddIn(catalog, Folders(music), store, null, []))
        {
            Assert.Equal("""{"cover":"kept","picture":"notFound"}""", await Enrich(none, "peace"));
            Assert.Equal("""{"cover":"present","picture":"notFound"}""", await Enrich(none, "peace"));
            Assert.Equal(LibraryTests.Jpeg(5), store.ReadAlbum("Everybody Digs", "Bill Evans").Value.Bytes.ToArray());
            Assert.Equal("""{"cover":"notFound","picture":"notFound"}""", await Enrich(none, "so what"));
            Assert.Empty(_requests);
        }

        using var fanartOnly = new ArtworkAddIn(catalog, Folders(music), store, null,
            [new FanartTvSource(_http, FanartKey, new Uri(_base, "fanart/"))], new HttpClient());
        Assert.Equal("""{"cover":"added","picture":"added"}""", await Enrich(fanartOnly, "tagged"));
        Assert.Equal("""{"cover":"notFound","picture":"notFound"}""", await Enrich(fanartOnly, "so what"));
        Assert.Equal(0, MusicBrainzRequests);
    }

    [Fact]
    public async Task A_service_that_is_down_is_unavailable_and_a_store_that_cannot_be_written_is_the_tracks_failure()
    {
        await StartAsync();
        var music = Directory.CreateDirectory(Path.Combine(_dir.Path, "music")).FullName;
        LibraryTests.WriteTrack(Path.Combine(music, "so-what.wav"), "So What", "Miles Davis", "Kind of Blue", 1);
        var catalog = await IndexAsync(music);
        // A file where the store's directory would go.
        File.WriteAllText(Path.Combine(_dir.Path, "art"), "");
        var musicBrainz = new MusicBrainzSource(_http, Endpoints, TimeSpan.Zero);
        using var addIn = new ArtworkAddIn(catalog, Folders(music), new ArtStore(Path.Combine(_dir.Path, "art")),
            musicBrainz, [musicBrainz], new HttpClient());
        var track = await TrackAsync(catalog, "so what");

        _down = true;
        Assert.Equal(AddInError.Unavailable, (await addIn.EnrichAsync(track, Ct)).Failure.Code);

        _down = false;
        var refused = await addIn.EnrichAsync(track, Ct);
        Assert.Equal(AddInError.Failed, refused.Failure.Code);
        Assert.Contains("cannot be kept", refused.Failure.Detail, StringComparison.Ordinal);
    }

    [Fact]
    public async Task The_indexer_keeps_the_first_MusicBrainz_id_of_each_tag_Picard_writes()
    {
        var music = Directory.CreateDirectory(Path.Combine(_dir.Path, "music")).FullName;
        LibraryTests.WriteTrack(Path.Combine(music, "a.wav"), "A", "Miles Davis", "Sketches of Spain", 1);
        TagPicard(Path.Combine(music, "a.wav"), SketchesGroup.ToUpperInvariant(), SketchesRelease,
            $"{MilesDavis}/{BillEvans}");
        LibraryTests.WriteTrack(Path.Combine(music, "b.wav"), "B", "Miles Davis", "Sketches of Spain", 2);
        TagPicard(Path.Combine(music, "b.wav"), "not an id", null, null);
        var catalog = await IndexAsync(music);

        var a = catalog.ArtworkSubjectOf((await TrackAsync(catalog, "a")).ProviderItemId).Value;
        Assert.Equal((SketchesGroup, SketchesRelease, MilesDavis, MilesDavis),
            (a.MbReleaseGroup, a.MbRelease, a.MbArtist, a.MbAlbumArtist));
        var b = catalog.ArtworkSubjectOf((await TrackAsync(catalog, "b")).ProviderItemId).Value;
        Assert.Equal(((string?)null, (string?)null, (string?)null, (string?)null),
            (b.MbReleaseGroup, b.MbRelease, b.MbArtist, b.MbAlbumArtist));
    }

    [Fact]
    public async Task Settings_set_through_the_api_win_over_the_config_are_kept_and_a_change_forgets_what_was_not_found()
    {
        var path = Path.Combine(_dir.Path, "artwork", "settings.json");
        var config = new ArtworkSettings(true, "config-key");
        var settings = new ArtworkSettingsStore(path, config);
        Assert.Equal(("config-key", true), (settings.FanartTvKey, settings.MusicBrainz));
        var changes = 0;
        settings.Changed += (_, _) => changes++;

        settings.Set(false, null);
        settings.SetFanartTvKey(" api-key ");
        settings.Set(false, null);
        Assert.Equal(2, changes);
        Assert.Equal(ProviderError.InvalidArgument, settings.SetFanartTvKey(" ").Failure.Code);

        // Kept across a restart; removing the key falls back to the config's;
        // fanart.tv switched off keeps its key but is not asked.
        var again = new ArtworkSettingsStore(path, config);
        Assert.Equal(("api-key", false), (again.FanartTvKey, again.MusicBrainz));
        Assert.Equal(new MusicBrainzView(false, ArtworkOrigin.Api), again.Current.MusicBrainz);
        Assert.Equal(new FanartTvView(true, true, ArtworkOrigin.Config, true), again.ClearFanartTvKey().Value.FanartTv);
        Assert.Equal("config-key", again.FanartTvKey);
        Assert.Equal(new FanartTvView(false, true, ArtworkOrigin.Config, false), again.Set(null, false).Value.FanartTv);
        Assert.Null(again.FanartTvKey);
        Assert.Equal(new FanartTvView(true, false, ArtworkOrigin.None, false),
            new ArtworkSettingsStore(Path.Combine(_dir.Path, "none.json"), ArtworkSettings.Default).Current.FanartTv);

        // What was not found, or failed, is asked again; what was found stays.
        var music = Directory.CreateDirectory(Path.Combine(_dir.Path, "music")).FullName;
        foreach (var name in new[] { "found", "missing", "failed" })
            LibraryTests.WriteTrack(Path.Combine(music, $"{name}.wav"), name, "Someone", "Album", 1);
        var catalog = await IndexAsync(music);
        var enrichment = new EnrichmentStore(new LibraryDatabase(Path.Combine(_dir.Path, "library.db")), catalog);
        async Task<string> Id(string title) => (await TrackAsync(catalog, title)).ProviderItemId;
        enrichment.Record(ArtworkAddIn.AddInId, await Id("found"),
            System.Text.Json.JsonSerializer.SerializeToElement(new { cover = "added", picture = "present" }), null);
        enrichment.Record(ArtworkAddIn.AddInId, await Id("missing"),
            System.Text.Json.JsonSerializer.SerializeToElement(new { cover = "kept", picture = "notFound" }), null);
        enrichment.Record(ArtworkAddIn.AddInId, await Id("failed"), null, "Failed: no");

        Assert.Equal(2, enrichment.Forget(ArtworkAddIn.AddInId, ["cover", "picture"], "notFound").Value);
        Assert.Equal(new EnrichmentCounts(1, 0, 2), enrichment.Counts(ArtworkAddIn.AddInId).Value);
    }

    [Fact]
    public async Task Kept_tracks_are_looked_up_first_and_a_library_favourite_shows_its_albums_cover()
    {
        var music = Directory.CreateDirectory(Path.Combine(_dir.Path, "music")).FullName;
        LibraryTests.WriteTrack(Path.Combine(music, "a.wav"), "Alpha", "Someone", "First", 1);
        LibraryTests.WriteTrack(Path.Combine(music, "b.wav"), "Bravo", "Someone", "Second", 1);
        LibraryTests.WriteTrack(Path.Combine(music, "c.wav"), "Charlie", "Someone", "Third", 1);
        var catalog = await IndexAsync(music);
        var database = new LibraryDatabase(Path.Combine(_dir.Path, "library.db"));
        var playlists = new PlaylistStore(database);
        var charlie = await TrackAsync(catalog, "charlie");
        playlists.AddFavorite(charlie);
        new LikeStore(database).Add(await TrackAsync(catalog, "bravo"));

        var pending = new EnrichmentStore(database, catalog).Pending(ArtworkAddIn.AddInId, 10).Value;
        Assert.Equal(["Bravo", "Charlie"], pending.Take(2).Select(t => t.Title).Order());
        Assert.Equal("Alpha", pending[2].Title);

        // The favourite's snapshot keeps no artwork; it is the track's own
        // reference, which shows the album's kept cover.
        var favourite = Assert.Single(playlists.Favorites().Value).Track!;
        Assert.Equal(new ArtworkRef("local", charlie.ProviderItemId), favourite.Artwork);
        var store = new ArtStore(Path.Combine(_dir.Path, "art"));
        store.KeepAlbum("Third", "Someone", new Artwork("image/jpeg", LibraryTests.Jpeg(13)));
        var provider = new LocalFilesProvider(Folders(music), catalog, _indexer!, Path.Combine(_dir.Path, "artwork"), store);
        Assert.Equal(LibraryTests.Jpeg(13), (await provider.GetArtworkAsync(favourite.Artwork!, Ct)).Value.Bytes.ToArray());
    }

    // The ids Picard writes; the album artist's is the artist's here.
    private static void TagPicard(string path, string? releaseGroup, string? release, string? artist)
    {
        using var file = TagLib.File.Create(path);
        file.Tag.MusicBrainzReleaseGroupId = releaseGroup;
        file.Tag.MusicBrainzReleaseId = release;
        file.Tag.MusicBrainzArtistId = artist;
        file.Tag.MusicBrainzReleaseArtistId = artist;
        file.Save();
    }

    private LibraryFolders Folders(string music) =>
        LibraryTests.Registry(new LibrarySettings([new LibraryFolder(music, LibraryFolderType.Local)],
            TimeSpan.FromMinutes(5)), new LibraryDatabase(Path.Combine(_dir.Path, "library.db")), _dir.Path);

    private async Task<LibraryCatalog> IndexAsync(string music)
    {
        var database = new LibraryDatabase(Path.Combine(_dir.Path, "library.db"));
        _indexer = new LibraryIndexer(Folders(music), database, NullLogger<LibraryIndexer>.Instance);
        await _indexer.StartAsync(Ct);
        await Until(() => _indexer.Status.Passes == 1, "the first pass");
        return new LibraryCatalog(database, LocalFilesProvider.ProviderId);
    }

    private static async Task<Track> TrackAsync(LibraryCatalog catalog, string text) =>
        Assert.Single((await catalog.SearchAsync(text, SearchKinds.Tracks, 0, 10, Ct)).Value.Tracks);

    // MusicBrainz (mb/), the Cover Art Archive (caa/), Wikidata (wd/),
    // Commons (commons/) and fanart.tv (fanart/), in recorded shapes. Kind of
    // Blue is an album and a single of one name, with covers on fanart.tv;
    // Twin two albums of one name; Nirvana two artists; Sketches of Spain a
    // release with a cover of its own; fanart.tv knows Bill Evans only.
    // Images: the archive 7 and 11, Commons 9, fanart.tv 21 (covers) and 22.
    private async Task StartAsync()
    {
        (_app, _base) = await FixtureHttp.StartAsync(HandleAsync);
    }

    private async Task HandleAsync(HttpContext context)
    {
        var path = context.Request.Path.Value!;
        var query = context.Request.Query["query"].ToString();
        _requests.Enqueue((path, path.StartsWith("/fanart/", StringComparison.Ordinal) ? context.Request.QueryString.Value! : query,
            Stopwatch.GetTimestamp()));

        string? json = null;
        if ((path.StartsWith("/mb/", StringComparison.Ordinal) || path.StartsWith("/fanart/", StringComparison.Ordinal)) && _down)
        {
            context.Response.StatusCode = StatusCodes.Status503ServiceUnavailable;
            return;
        }
        if (path == "/mb/release-group")
        {
            json = query.Contains("Kind Of Blue", StringComparison.Ordinal)
                ? $$$"""
                  {"release-groups":[
                    {"id":"{{{KindOfBlueSingle}}}","title":"Kind of Blue","primary-type":"Single",
                     "artist-credit":[{"name":"Miles Davis","joinphrase":"","artist":{"id":"{{{MilesDavis}}}","name":"Miles Davis"}}]},
                    {"id":"{{{KindOfBlue}}}","title":"Kind of Blue","primary-type":"Album",
                     "artist-credit":[{"name":"Miles Davis","joinphrase":"","artist":{"id":"{{{MilesDavis}}}","name":"Miles Davis"}}]},
                    {"id":"a1b2c3d4-0000-4000-8000-000000000009","title":"Kind of Blue Sessions","primary-type":"Album",
                     "artist-credit":[{"name":"Miles Davis","joinphrase":"","artist":{"id":"{{{MilesDavis}}}","name":"Miles Davis"}}]}
                  ]}
                  """
                : query.Contains("Kind of Blue", StringComparison.Ordinal)
                    ? $$$"""
                      {"release-groups":[{"id":"{{{KindOfBlue}}}","title":"Kind of Blue","primary-type":"Album",
                       "artist-credit":[{"name":"Miles Davis","joinphrase":"","artist":{"id":"{{{MilesDavis}}}","name":"Miles Davis"}}]}]}
                      """
                    : query.Contains("Twin", StringComparison.Ordinal)
                        ? """
                          {"release-groups":[
                            {"id":"c0000000-0000-4000-8000-000000000001","title":"Twin","primary-type":"Album","artist-credit":[{"name":"The Twins","joinphrase":""}]},
                            {"id":"c0000000-0000-4000-8000-000000000002","title":"Twin","primary-type":"Album","artist-credit":[{"name":"The Twins","joinphrase":""}]}
                          ]}
                          """
                        : """{"release-groups":[]}""";
        }
        else if (path == "/mb/recording")
        {
            json = query.Contains("So What", StringComparison.Ordinal)
                ? $$$"""
                  {"recordings":[{"title":"So What","artist-credit":[{"name":"Miles Davis","joinphrase":"","artist":{"id":"{{{MilesDavis}}}"}}],
                    "releases":[
                      {"release-group":{"id":"a1b2c3d4-0000-4000-8000-000000000010","title":"Jazz Hits","primary-type":"Album","secondary-types":["Compilation"]}},
                      {"release-group":{"id":"{{{KindOfBlueSingle}}}","title":"So What","primary-type":"Single"}},
                      {"release-group":{"id":"{{{KindOfBlue}}}","title":"Kind of Blue","primary-type":"Album"}}]}]}
                  """
                : """{"recordings":[]}""";
        }
        else if (path == $"/mb/release/{SketchesRelease}")
        {
            json = $$$"""
                {"id":"{{{SketchesRelease}}}","release-group":{"id":"{{{SketchesGroup}}}"},
                 "artist-credit":[{"name":"Miles Davis","artist":{"id":"{{{MilesDavis}}}"}}]}
                """;
        }
        else if (path == "/mb/artist")
        {
            json = query.Contains("Nirvana", StringComparison.Ordinal)
                ? """{"artists":[{"id":"d0000000-0000-4000-8000-000000000001","name":"Nirvana"},{"id":"d0000000-0000-4000-8000-000000000002","name":"Nirvana"}]}"""
                : query.Contains("Bill", StringComparison.OrdinalIgnoreCase)
                    ? $$"""{"artists":[{"id":"{{BillEvans}}","name":"Bill Evans"},{"id":"d0000000-0000-4000-8000-000000000003","name":"Bill Evans Trio"}]}"""
                    : """{"artists":[]}""";
        }
        else if (path == $"/mb/artist/{MilesDavis}")
        {
            json = """{"relations":[{"type":"wikidata","url":{"resource":"https://www.wikidata.org/wiki/Q93341"}}]}""";
        }
        else if (path.StartsWith("/mb/artist/", StringComparison.Ordinal))
        {
            json = """{"relations":[]}""";
        }
        else if (path == "/wd/wiki/Special:EntityData/Q93341.json")
        {
            json = """{"entities":{"Q93341":{"claims":{"P18":[{"mainsnak":{"datavalue":{"value":"Miles Davis 1955.jpg"}}}]}}}}""";
        }
        else if (path == $"/fanart/music/albums/{KindOfBlue}")
        {
            json = $$$"""
                {"name":"Kind of Blue","albums":{"{{{KindOfBlue}}}":{"albumcover":[
                  {"id":"1","url":"{{{_base}}}fanart-img/kind-1.jpg","likes":"1"},
                  {"id":"2","url":"{{{_base}}}fanart-img/kind-2.jpg","likes":"5"},
                  {"id":"3","url":"javascript:alert(1)","likes":"9"}]}
                }}
                """;
        }
        else if (path == $"/fanart/music/{BillEvans}")
        {
            json = $$"""{"name":"Bill Evans","artistthumb":[{"id":"7","url":"{{_base}}fanart-img/evans.jpg","likes":"2"}]}""";
        }
        else if (path.StartsWith("/fanart-img/", StringComparison.Ordinal))
        {
            context.Response.ContentType = "image/jpeg";
            await context.Response.Body.WriteAsync(LibraryTests.Jpeg(path.Contains("evans", StringComparison.Ordinal)
                ? (byte)22
                : (byte)21));
            return;
        }
        else if (path == $"/caa/release-group/{KindOfBlue}/front-1200" || path == $"/caa/release/{SketchesRelease}/front-1200"
                 || path.StartsWith("/commons/wiki/Special:FilePath/", StringComparison.Ordinal))
        {
            context.Response.ContentType = "image/jpeg";
            await context.Response.Body.WriteAsync(LibraryTests.Jpeg(path.Contains(SketchesRelease, StringComparison.Ordinal)
                ? (byte)11
                : path.StartsWith("/commons/", StringComparison.Ordinal) ? (byte)9 : (byte)7));
            return;
        }

        if (json is null)
        {
            context.Response.StatusCode = StatusCodes.Status404NotFound;
            return;
        }
        context.Response.ContentType = "application/json";
        await context.Response.WriteAsync(json);
    }

    private static async Task Until(Func<bool> condition, string what)
    {
        var deadline = DateTime.UtcNow + TimeSpan.FromSeconds(20);
        while (!condition())
        {
            if (DateTime.UtcNow > deadline)
                Assert.Fail($"timed out waiting for {what}");
            await Task.Delay(20, Ct);
        }
    }
}
