using Microsoft.Extensions.Logging.Abstractions;
using Multiroom.Server.Abstractions;
using Multiroom.Server.Library;
using Multiroom.Server.Library.Smb;

namespace Multiroom.Server.Tests;

// The indexer, the database and the catalog, over WAV files whose tags the
// tests write. A file that changes in a watched folder is moved into place
// whole, so no pass ever sees it half-written.
public sealed class LibraryTests : IAsyncDisposable
{
    private readonly TempDirectory _dir = new();
    private readonly LibraryDatabase _database;
    private readonly List<LibraryIndexer> _indexers = [];

    public LibraryTests()
    {
        _database = new LibraryDatabase(Path.Combine(_dir.Path, "library.db"));
    }

    public async ValueTask DisposeAsync()
    {
        foreach (var indexer in _indexers)
        {
            await indexer.StopAsync(CancellationToken.None);
            indexer.Dispose();
        }
        _dir.Dispose();
    }

    private static CancellationToken Ct => TestContext.Current.CancellationToken;

    private LibraryCatalog Catalog => new(_database, "local");

    [Fact]
    public async Task A_folder_of_a_few_hundred_files_is_indexed_in_exactly_one_pass_with_a_bounded_queue()
    {
        var music = Folder("music");
        for (var a = 0; a < 10; a++)
        {
            var album = Folder(Path.Combine("music", $"Album {a}"));
            for (var t = 1; t <= 30; t++)
                WriteTrack(Path.Combine(album, $"{t:00}.wav"), $"Song {a}-{t}", $"Artist {a % 3}", $"Album {a}", t);
        }
        await File.WriteAllTextAsync(Path.Combine(music, "notes.txt"), "not audio", Ct);

        var indexer = await Start(TimeSpan.FromMinutes(5), new LibraryFolder(music, LibraryFolderType.Local));
        var deepest = 0;
        await Until(() =>
        {
            deepest = Math.Max(deepest, indexer.Status.QueueDepth);
            return indexer.Status.Passes == 1;
        }, "the first pass");

        Assert.Equal(new IndexerStatus(Scanned: 300, Indexed: 300, Failed: 0, QueueDepth: 0, Passes: 1),
            indexer.Status);
        Assert.InRange(deepest, 0, 1);
        Assert.Equal(10, (await Catalog.BrowseAsync(new("albums", 0, 100), Ct)).Value.Total);
        Assert.Equal(3, (await Catalog.BrowseAsync(new("artists", 0, 100), Ct)).Value.Nodes.Count);
    }

    [Fact]
    public async Task A_changed_file_reindexes_alone_and_a_deleted_file_disappears()
    {
        var music = Folder("music");
        var one = Path.Combine(music, "one.wav");
        var two = Path.Combine(music, "two.wav");
        WriteTrack(one, "Morning Light", "Aurora", "Days");
        WriteTrack(two, "Evening Rain", "Aurora", "Days");
        var indexer = await Start(TimeSpan.FromMinutes(5), new LibraryFolder(music, LibraryFolderType.Local));
        await Until(() => indexer.Status.Passes == 1, "the first pass");
        var id = await IdOf("morning");

        Replace(one, "Midnight Sun", "Aurora", "Days");
        await Until(async () => (await Search("midnight")).Tracks.Count == 1, "the changed title");
        Assert.Equal(id, await IdOf("midnight"));

        Assert.Empty((await Search("morning")).Tracks);
        Assert.Equal(3, indexer.Status.Indexed);

        File.Delete(two);
        await Until(async () => (await Search("evening")).Tracks.Count == 0, "the deleted file to disappear");

        Assert.Equal(3, indexer.Status.Indexed);
        Assert.Equal(["Midnight Sun"], (await Search("aurora")).Tracks.Select(t => t.Title));
    }

    [Fact]
    public async Task A_corrupt_file_is_recorded_by_name_and_not_probed_again_until_it_changes()
    {
        var music = Folder("music");
        WriteTrack(Path.Combine(music, "good.wav"), "Fine");
        var broken = Path.Combine(music, "broken.flac");
        await File.WriteAllBytesAsync(broken, Enumerable.Repeat((byte)0x5A, 4096).ToArray(), Ct);
        var indexer = await Start(TimeSpan.FromMinutes(5), new LibraryFolder(music, LibraryFolderType.Local));
        await Until(() => indexer.Status.Passes == 1, "the first pass");

        Assert.Equal((1L, 1L), (indexer.Status.Indexed, indexer.Status.Failed));
        Assert.False(string.IsNullOrEmpty(ErrorOf(broken)));

        Replace(Path.Combine(music, "later.wav"), "Later");
        await Until(async () => (await Search("later")).Tracks.Count == 1, "the second pass");

        Assert.Equal((2L, 1L), (indexer.Status.Indexed, indexer.Status.Failed));
    }

    [Fact]
    public async Task A_folder_says_what_its_last_pass_found_beside_its_counts_since_start()
    {
        var music = Folder("music");
        WriteTrack(Path.Combine(music, "one.wav"), "One");
        WriteTrack(Path.Combine(music, "two.wav"), "Two");
        await File.WriteAllBytesAsync(Path.Combine(music, "broken.flac"), Enumerable.Repeat((byte)0x5A, 4096).ToArray(), Ct);
        var indexer = await Start(TimeSpan.FromMinutes(5), new LibraryFolder(music, LibraryFolderType.Local));
        await Until(() => indexer.Status.Passes == 1, "the first pass");

        var first = Assert.Single(indexer.Folders);
        Assert.Equal((3L, 2L, 1L), (first.Scanned, first.Indexed, first.Failed));
        Assert.Equal((3, 1, 3, 0, 1, 0), Counts(first.LastPassResult!));

        Assert.True(indexer.Scan(first.Id).IsSuccess);
        await Until(() => indexer.Status.Passes == 2, "the second pass");

        // The counts since start grow with every pass; the pass says what is there.
        var second = Assert.Single(indexer.Folders);
        Assert.Equal((6L, 2L, 1L), (second.Scanned, second.Indexed, second.Failed));
        Assert.Equal((3, 1, 0, 0, 0, 0), Counts(second.LastPassResult!));

        File.Delete(Path.Combine(music, "two.wav"));
        Assert.True(indexer.Scan(first.Id).IsSuccess);
        await Until(() => indexer.Status.Passes >= 3 && indexer.Folders[0].LastPassResult!.Files == 2, "the pass after a delete");
        Assert.Equal(1, indexer.Folders[0].LastPassResult!.Missing);

        static (int, int, int, int, int, int) Counts(LibraryPassResult r) =>
            (r.Files, r.Unreadable, r.Probed, r.Moved, r.Failed, r.Missing);
    }

    [Fact]
    public async Task A_dropped_network_folder_is_retried_on_the_poll_interval_and_never_blocks_a_local_folder()
    {
        var local = Folder("local");
        WriteTrack(Path.Combine(local, "a.wav"), "Alpha");
        var share = Path.Combine(_dir.Path, "share");
        var indexer = await Start(TimeSpan.FromMilliseconds(200),
            new LibraryFolder(share, LibraryFolderType.Network), new LibraryFolder(local, LibraryFolderType.Local));
        var deepest = 0;
        async Task<bool> Found(string text)
        {
            deepest = Math.Max(deepest, indexer.Status.QueueDepth);
            return (await Search(text)).Tracks.Count == 1;
        }

        await Until(() => Found("alpha"), "the local folder's first pass");
        Replace(Path.Combine(local, "b.wav"), "Bravo");
        await Until(() => Found("bravo"), "the local change while the share is down");

        var staging = Folder("staging");
        WriteTrack(Path.Combine(staging, "c.wav"), "Charlie");
        Directory.Move(staging, share);
        await Until(() => Found("charlie"), "the share, on a later poll");

        Assert.InRange(deepest, 0, 2);
        Assert.Equal(0, indexer.Status.Failed);
    }

    [Fact]
    public async Task Search_finds_by_title_artist_album_and_prefix_with_provider_attribution()
    {
        var music = await SeedJazz();

        Assert.Equal(["So What"], (await Search("so what")).Tracks.Select(t => t.Title));
        Assert.Equal(["Naïma"], (await Search("naima")).Tracks.Select(t => t.Title));

        var coltrane = await Search("coltrane");
        Assert.Equal(["Blue Train", "Naïma"], coltrane.Tracks.Select(t => t.Title).Order());
        Assert.Equal(["John Coltrane"], coltrane.Artists.Select(a => a.Name));
        Assert.Equal(["Blue Train", "Giant Steps"], coltrane.Albums.Select(a => a.Title));

        var blu = await Search("blu");
        Assert.Equal(["Blue in Green", "Blue Train", "So What"], blu.Tracks.Select(t => t.Title).Order());
        Assert.Equal(["Blue Train", "Kind of Blue"], blu.Albums.Select(a => a.Title));
        Assert.Empty(blu.Artists);
        Assert.All(blu.Tracks, t => Assert.Equal("local", t.ProviderId));
        Assert.All(blu.Tracks, t => Assert.StartsWith("track:", t.ProviderItemId, StringComparison.Ordinal));
        Assert.Equal(Path.Combine(music, "Miles", "so-what.wav"), Catalog.PathOf(await IdOf("so what")).Value);

        Assert.Equal(["Blue in Green", "So What"], (await Search("miles blue")).Tracks.Select(t => t.Title).Order());
        Assert.Single((await Catalog.SearchAsync("blu", SearchKinds.All, 1, 1, Ct)).Value.Tracks);
        Assert.Equal(CatalogItems.Empty, await Search("  - "));
    }

    [Fact]
    public async Task Browse_opens_artists_albums_and_folders()
    {
        var music = await SeedJazz();

        var root = (await Catalog.BrowseAsync(new(null, 0, 10), Ct)).Value;
        Assert.Equal(["artists", "album-artists", "albums", "genres", "composers", "years", "recent", "folders", "audiobooks"],
            root.Nodes.Select(n => n.NodeId));
        Assert.Equal(9, root.Total);

        var artists = (await Catalog.BrowseAsync(new("artists", 0, 10), Ct)).Value.Nodes;
        Assert.Equal(["John Coltrane", "Miles Davis"], artists.Select(a => a.Title));
        Assert.All(artists, a => Assert.Equal((BrowseKind.Artist, true), (a.Kind, a.Playable)));

        var miles = (await Catalog.BrowseAsync(new(artists[1].NodeId, 0, 10), Ct)).Value;
        Assert.Equal("Miles Davis", miles.Node!.Title);
        Assert.Equal(["Albums", "Tracks"], miles.Sections!.Select(s => s.Title));
        var albumNode = Assert.Single(miles.Sections![0].Nodes);
        Assert.Equal(("Kind of Blue", BrowseKind.Album, "Miles Davis"), (albumNode.Title, albumNode.Kind, albumNode.Subtitle));
        Assert.Equal(["So What", "Blue in Green"], miles.Sections[1].Items.Tracks.Select(t => t.Title).Order().Reverse());
        var album = (await Catalog.BrowseAsync(new(albumNode.NodeId, 0, 10), Ct)).Value;
        Assert.Equal(["So What", "Blue in Green"], album.Items.Tracks.Select(t => t.Title));
        Assert.Equal(("Miles Davis", "Kind of Blue"), (album.Items.Tracks[0].Artist, album.Items.Tracks[0].Album));
        Assert.NotNull(album.Items.Tracks[0].Duration);
        Assert.Equal(new AudioFormat("WAV", 8000, 16, 1, 128), album.Items.Tracks[0].Format);
        Assert.StartsWith("Miles Davis · 2 tracks · 0:00", album.Node!.Subtitle, StringComparison.Ordinal);
        Assert.Equal(2, album.Total);

        var folders = (await Catalog.BrowseAsync(new("folders", 0, 10), Ct)).Value.Nodes;
        Assert.Equal([music], folders.Select(n => n.Title));
        var top = (await Catalog.BrowseAsync(new(folders[0].NodeId, 0, 10), Ct)).Value;
        Assert.Equal(["Coltrane", "Miles"], top.Nodes.Select(n => n.Title));
        Assert.Equal(["Loose"], top.Items.Tracks.Select(t => t.Title));
        var sub = (await Catalog.BrowseAsync(new(top.Nodes[1].NodeId, 0, 10), Ct)).Value;
        Assert.Equal(["Blue in Green", "So What"], sub.Items.Tracks.Select(t => t.Title));

        foreach (var unknown in new[] { "album:999", "artist:x", "folder:" + Path.Combine(music, "nope"), "nonsense" })
            Assert.Equal(ProviderError.NotFound, (await Catalog.BrowseAsync(new(unknown, 0, 10), Ct)).Failure.Code);
    }

    [Fact]
    public async Task The_library_browses_by_genre_composer_and_year_sorts_filters_and_jumps_by_letter_and_shows_its_covers()
    {
        var music = Folder("music");
        var kind = Folder(Path.Combine("music", "Jazz", "Kind of Blue"));
        var giant = Folder(Path.Combine("music", "Jazz", "Giant Steps"));
        var classical = Folder(Path.Combine("music", "Classical"));
        WriteTrack(Path.Combine(kind, "1.wav"), "So What", "Miles Davis", "Kind of Blue", 1, "Jazz", "Miles Davis", 1959, Jpeg(1));
        WriteTrack(Path.Combine(kind, "3.wav"), "Blue in Green", "Miles Davis", "Kind of Blue", 3, "Jazz", "Bill Evans", 1959);
        WriteTrack(Path.Combine(giant, "7.wav"), "Naima", "John Coltrane", "Giant Steps", 7, "jazz", "John Coltrane", 1960);
        await File.WriteAllBytesAsync(Path.Combine(giant, "cover.jpg"), Jpeg(2), Ct);
        WriteTrack(Path.Combine(classical, "2.wav"), "Air", "Bach Orchestra", "Suites", 2, "Classical", "Johann Sebastian Bach", 1985);
        WriteTrack(Path.Combine(music, "elan.wav"), "Élan", "Édith", "Élan", 1, "Chanson", year: 1990);
        var settings = new LibrarySettings([new LibraryFolder(music, LibraryFolderType.Local)], TimeSpan.FromMinutes(5));
        var indexer = await Start(settings.PollInterval, [.. settings.Folders]);
        await Until(() => indexer.Status.Passes == 1, "the first pass");

        async Task<BrowsePage> Browse(BrowseRequest request) => (await Catalog.BrowseAsync(request, Ct)).Value;
        async Task<ProviderError> Refused(BrowseRequest request) => (await Catalog.BrowseAsync(request, Ct)).Failure.Code;
        static List<string> Titles(BrowsePage page) => [.. page.Nodes.Select(n => n.Title)];

        var genres = await Browse(new("genres"));
        Assert.Equal(["Chanson", "Classical", "Jazz"], Titles(genres));
        Assert.Equal(["1 album", "1 album", "2 albums"], genres.Nodes.Select(n => n.Subtitle));
        Assert.Equal([new BrowseLetter("C", 0), new BrowseLetter("J", 2)], genres.Letters);
        Assert.Equal((3, true, "name"), (genres.Total, genres.Filterable, genres.Sort));
        Assert.Equal(["Jazz"], Titles(await Browse(new("genres", Letter: "J"))));
        Assert.Equal(["Jazz"], Titles(await Browse(new("genres", Letter: "D"))));
        Assert.Equal(["Classical"], Titles(await Browse(new("genres", Filter: "clas"))));
        Assert.Equal(ProviderError.InvalidArgument, await Refused(new("genres", Sort: "year")));
        Assert.Equal(ProviderError.InvalidArgument, await Refused(new("genres", Letter: "JA")));
        Assert.Equal(ProviderError.InvalidArgument, await Refused(new("genres", 1, 10, Letter: "J")));

        var jazz = await Browse(new("genre:Jazz"));
        Assert.Equal(("Jazz", BrowseKind.Genre, BrowseLayout.Grid), (jazz.Node!.Title, jazz.Node.Kind, jazz.Layout));
        Assert.Equal(["Giant Steps", "Kind of Blue"], Titles(jazz));
        Assert.Equal(["Giant Steps", "Kind of Blue"], Titles(await Browse(new("genre:Jazz", Sort: "year"))));
        Assert.Equal(ProviderError.InvalidArgument, await Refused(new("genre:Jazz", Sort: "year", Letter: "K")));
        Assert.Equal(ProviderError.NotFound, await Refused(new("genre:Polka")));

        Assert.Equal(["Bill Evans", "Johann Sebastian Bach", "John Coltrane", "Miles Davis"], Titles(await Browse(new("composers"))));
        Assert.Equal(["Blue in Green"], (await Browse(new("composer:Bill Evans"))).Items.Tracks.Select(t => t.Title));
        Assert.Equal(["1990", "1985", "1960", "1959"], Titles(await Browse(new("years"))));
        Assert.Equal(["Kind of Blue"], Titles(await Browse(new("year:1959"))));
        Assert.Equal(ProviderError.InvalidArgument, await Refused(new("years", Filter: "19")));

        var albums = await Browse(new("albums"));
        Assert.Equal(["Élan", "Giant Steps", "Kind of Blue", "Suites"], Titles(albums));
        Assert.Equal(["E", "G", "K", "S"], albums.Letters!.Select(l => l.Letter));
        Assert.Equal(["title", "artist", "year", "added"], albums.Sorts!);
        Assert.Equal(BrowseLayout.Grid, albums.Layout);
        Assert.Equal(["Kind of Blue", "Suites"], Titles(await Browse(new("albums", Letter: "H"))));
        Assert.Equal(["Giant Steps"], Titles(await Browse(new("albums", Filter: "coltrane"))));
        Assert.Equal(["Suites", "Élan", "Giant Steps", "Kind of Blue"], Titles(await Browse(new("albums", Sort: "artist"))));
        Assert.Equal(["Élan", "Suites", "Giant Steps", "Kind of Blue"], Titles(await Browse(new("albums", Sort: "year"))));
        var kindOfBlue = albums.Nodes[2];
        Assert.Equal(("Miles Davis · 1959", true, new ArtworkRef("local", kindOfBlue.NodeId)),
            (kindOfBlue.Subtitle, kindOfBlue.Playable, kindOfBlue.Artwork));
        Assert.Equal(4, (await Browse(new("recent"))).Total);
        Assert.Equal(["Bach Orchestra", "Édith", "John Coltrane", "Miles Davis"], Titles(await Browse(new("album-artists"))));

        Assert.Equal(["So What", "Blue in Green"], (await Catalog.ExpandAsync(kindOfBlue.NodeId, 100, Ct)).Value.Tracks.Select(t => t.Title));
        Assert.Equal(["Naima", "So What", "Blue in Green"], (await Catalog.ExpandAsync("genre:Jazz", 100, Ct)).Value.Tracks.Select(t => t.Title));
        Assert.Equal(["Blue in Green", "Naima", "So What"], (await Catalog.ExpandAsync("folder:" + Path.Combine(music, "Jazz"), 100, Ct))
            .Value.Tracks.Select(t => t.Title).Order(StringComparer.Ordinal));
        var cut = (await Catalog.ExpandAsync("genre:Jazz", 2, Ct)).Value;
        Assert.Equal((2, true), (cut.Tracks.Count, cut.Truncated));
        Assert.Equal(ProviderError.InvalidArgument, (await Catalog.ExpandAsync("albums", 10, Ct)).Failure.Code);
        Assert.Equal(ProviderError.NotFound, (await Catalog.ExpandAsync("nonsense", 10, Ct)).Failure.Code);

        // Covers: embedded in the first track, else beside it, else none.
        var provider = new Providers.Local.LocalFilesProvider(Registry(settings, _database, _dir.Path), Catalog, indexer,
            Path.Combine(_dir.Path, "artwork"));
        var embedded = (await provider.GetArtworkAsync(kindOfBlue.Artwork!, Ct)).Value;
        Assert.Equal("image/jpeg", embedded.ContentType);
        Assert.Equal(Jpeg(1), embedded.Bytes.ToArray());
        Assert.Equal(Jpeg(2), (await provider.GetArtworkAsync(albums.Nodes[1].Artwork!, Ct)).Value.Bytes.ToArray());
        Assert.Equal(ProviderError.NotFound, (await provider.GetArtworkAsync(albums.Nodes[3].Artwork!, Ct)).Failure.Code);
        Assert.Contains("artwork 2 image(s)", (await provider.GetStatusAsync(Ct)).Detail, StringComparison.Ordinal);
    }

    [Fact]
    public async Task A_file_indexed_before_genre_and_composer_were_read_is_probed_again_on_the_next_pass_with_its_id_kept()
    {
        var music = Folder("music");
        WriteTrack(Path.Combine(music, "a.wav"), "Alpha", genre: "Jazz", composer: "Someone");
        var first = await Start(TimeSpan.FromMinutes(5), new LibraryFolder(music, LibraryFolderType.Local));
        await Until(() => first.Status.Passes == 1, "the first pass");
        var id = await IdOf("alpha");
        await first.StopAsync(Ct);
        Count("UPDATE library_file SET tags = 1 RETURNING 1");
        Count("UPDATE track SET genre = NULL, composer = NULL RETURNING 1");

        var second = await Start(TimeSpan.FromMinutes(5), new LibraryFolder(music, LibraryFolderType.Local));
        await Until(() => second.Status.Passes == 1, "the next pass");

        Assert.Equal(1, second.Status.Indexed);
        Assert.Equal(id, await IdOf("alpha"));
        Assert.Equal(["Jazz"], (await Catalog.BrowseAsync(new("genres"), Ct)).Value.Nodes.Select(n => n.Title));
        Assert.Equal(6, Count("SELECT tags FROM library_file"));
    }

    [Fact]
    public async Task A_folder_the_config_no_longer_names_is_forgotten()
    {
        var kept = Folder("kept");
        var dropped = Folder("dropped");
        WriteTrack(Path.Combine(kept, "k.wav"), "Kept");
        WriteTrack(Path.Combine(dropped, "d.wav"), "Dropped");
        var first = await Start(TimeSpan.FromMinutes(5), new LibraryFolder(kept, LibraryFolderType.Local),
            new LibraryFolder(dropped, LibraryFolderType.Local));
        await Until(() => first.Status.Passes == 2, "both folders");
        await first.StopAsync(Ct);

        var second = await Start(TimeSpan.FromMinutes(5), new LibraryFolder(kept, LibraryFolderType.Local));
        await Until(() => second.Status.Passes == 1, "the kept folder");

        Assert.Empty((await Search("dropped")).Tracks);
        Assert.Single((await Search("kept")).Tracks);
        Assert.Equal(0, second.Status.Indexed);
    }

    [Fact]
    public async Task A_renamed_folder_keeps_its_track_ids_and_is_not_probed_again()
    {
        var audio = Folder("audio");
        var songs = Folder(Path.Combine("audio", "songs"));
        WriteTrack(Path.Combine(songs, "one.wav"), "Morning Light", "Aurora");
        WriteTrack(Path.Combine(songs, "two.wav"), "Evening Rain", "Aurora");
        var indexer = await Start(TimeSpan.FromMinutes(5), new LibraryFolder(audio, LibraryFolderType.Local));
        await Until(() => indexer.Status.Passes == 1, "the first pass");
        var morning = await IdOf("morning");
        var evening = await IdOf("evening");

        Directory.Move(songs, Path.Combine(audio, "xyz"));
        await Until(() => Catalog.PathOf(morning) is { IsSuccess: true } found &&
            found.Value == Path.Combine(audio, "xyz", "one.wav"), "the moved file under its old id");

        Assert.Equal(Path.Combine(audio, "xyz", "two.wav"), Catalog.PathOf(evening).Value);
        Assert.Equal((morning, evening), (await IdOf("morning"), await IdOf("evening")));
        Assert.Equal(2, indexer.Status.Indexed);
    }

    [Fact]
    public async Task A_file_moved_to_another_library_folder_keeps_its_track_id()
    {
        var first = Folder("first");
        var second = Folder("second");
        WriteTrack(Path.Combine(first, "song.wav"), "Wanderer");
        var indexer = await Start(TimeSpan.FromMinutes(5), new LibraryFolder(first, LibraryFolderType.Local),
            new LibraryFolder(second, LibraryFolderType.Local));
        await Until(() => indexer.Status.Passes == 2, "both folders");
        var id = await IdOf("wanderer");

        File.Move(Path.Combine(first, "song.wav"), Path.Combine(second, "song.wav"));
        await Until(() => Catalog.PathOf(id) is { IsSuccess: true } found &&
            found.Value == Path.Combine(second, "song.wav"), "the file in the other folder under its old id");

        Assert.Equal(id, await IdOf("wanderer"));
        Assert.Equal(1, indexer.Status.Indexed);
    }

    [Fact]
    public async Task A_copy_is_a_track_of_its_own()
    {
        var music = Folder("music");
        WriteTrack(Path.Combine(music, "song.wav"), "Twin");
        var indexer = await Start(TimeSpan.FromMinutes(5), new LibraryFolder(music, LibraryFolderType.Local));
        await Until(() => indexer.Status.Passes == 1, "the first pass");
        var original = await IdOf("twin");

        File.Copy(Path.Combine(music, "song.wav"), Path.Combine(_dir.Path, "copy.wav"));
        File.Move(Path.Combine(_dir.Path, "copy.wav"), Path.Combine(music, "copy.wav"));
        await Until(async () => (await Search("twin")).Tracks.Count == 2, "the copy");

        Assert.Equal(Path.Combine(music, "song.wav"), Catalog.PathOf(original).Value);
        var copy = (await Search("twin")).Tracks.Single(t => t.ProviderItemId != original).ProviderItemId;
        Assert.Equal(Path.Combine(music, "copy.wav"), Catalog.PathOf(copy).Value);
    }

    [Fact]
    public async Task A_file_that_stays_missing_is_forgotten_and_its_id_is_never_given_again()
    {
        var music = Folder("music");
        WriteTrack(Path.Combine(music, "a.wav"), "Keeper");
        WriteTrack(Path.Combine(music, "b.wav"), "Leaver");
        var indexer = await Start(TimeSpan.FromMinutes(5), new LibraryFolder(music, LibraryFolderType.Local));
        await Until(() => indexer.Status.Passes == 1, "the first pass");
        var leaver = await IdOf("leaver");

        File.Delete(Path.Combine(music, "b.wav"));
        await Until(async () => (await Search("leaver")).Tracks.Count == 0, "the file to be hidden");
        Assert.Equal(ProviderError.NotFound, Catalog.PathOf(leaver).Failure.Code);
        Assert.Equal(2, Count("SELECT COUNT(*) FROM track"));

        // The next pass began after the file went missing, and did not find it.
        Replace(Path.Combine(music, "a.wav"), "Keeper");
        await Until(() => Count("SELECT COUNT(*) FROM track") == 1, "the missing file to be forgotten");

        Replace(Path.Combine(music, "c.wav"), "Newcomer");
        await Until(async () => (await Search("newcomer")).Tracks.Count == 1, "the new file");
        Assert.True(Number(await IdOf("newcomer")) > Number(leaver));
    }

    [Fact]
    public void A_schema_1_library_is_migrated_with_its_track_ids_and_file_links_kept()
    {
        using (var connection = _database.Open())
        {
            using var reader = new StreamReader(typeof(LibraryDatabase).Assembly
                .GetManifestResourceStream("migrations/0001_library.sql")!);
            using var command = connection.CreateCommand();
            command.CommandText = reader.ReadToEnd() + """
                CREATE TABLE schema_version (version INTEGER NOT NULL);
                INSERT INTO schema_version VALUES (1);
                INSERT INTO artist (id, name) VALUES (3, 'Aurora');
                INSERT INTO track (id, title, artist_id) VALUES (7, 'Morning Light', 3);
                INSERT INTO track_search (rowid, title, artist) VALUES (7, 'Morning Light', 'Aurora');
                INSERT INTO library_file (path, folder, directory, size, modified, track_id)
                    VALUES ('C:\m\one.wav', 'C:\m', 'C:\m', 10, 20, 7);
                """;
            command.ExecuteNonQuery();
        }

        Assert.Equal(30, _database.Migrate().Value);

        Assert.Equal(7, Count("SELECT track_id FROM library_file WHERE path = 'C:\\m\\one.wav'"));
        Assert.Equal(3, Count("SELECT artist_id FROM track WHERE id = 7"));
        Assert.Equal(0, Count("SELECT COUNT(*) FROM library_file WHERE fingerprint IS NOT NULL OR missing_since IS NOT NULL"));
        Assert.Equal(8, Count("INSERT INTO track (title) VALUES ('Next') RETURNING id"));
        Assert.Equal(1, Count("PRAGMA foreign_keys"));
    }

    [Fact]
    public void Playlist_entries_and_a_books_parts_survive_the_rebuild_that_lets_a_station_in()
    {
        using (var connection = _database.Open())
        {
            // The schema as it was before stations (0029), with a book and a track in a playlist.
            var assembly = typeof(LibraryDatabase).Assembly;
            var before = assembly.GetManifestResourceNames().Where(n => n.StartsWith("migrations/", StringComparison.Ordinal))
                .Order(StringComparer.Ordinal).TakeWhile(n => !n.StartsWith("migrations/0030", StringComparison.Ordinal))
                .Select(n => new StreamReader(assembly.GetManifestResourceStream(n)!).ReadToEnd());
            using var command = connection.CreateCommand();
            command.CommandText = "PRAGMA foreign_keys = OFF;" + string.Join(Environment.NewLine, before) + """

                CREATE TABLE schema_version (version INTEGER NOT NULL);
                INSERT INTO schema_version VALUES (29);
                INSERT INTO playlist (id, name, position, created) VALUES (1, 'Evening', 0, 0);
                INSERT INTO playlist_item (id, playlist_id, position, provider_id, provider_item_id, title, kind)
                    VALUES (5, 1, 0, 'local', 'book:3', 'The Book', 'group'),
                           (6, 1, 1, 'local', 'track:1', 'Song', 'track');
                INSERT INTO playlist_item_part (item_id, position, provider_id, provider_item_id, title)
                    VALUES (5, 0, 'local', 'track:2', 'Part One'), (5, 1, 'local', 'track:3', 'Part Two');
                PRAGMA foreign_keys = ON;
                """;
            command.ExecuteNonQuery();
        }

        Assert.Equal(30, _database.Migrate().Value);

        var store = new PlaylistStore(_database);
        var entries = store.Entries(1).Value;
        Assert.Equal([(5L, "The Book"), (6L, "Song")], entries.Select(e => (e.Id, e.Title)));
        Assert.Equal(["Part One", "Part Two"], entries[0].Group!.Parts.Select(p => p.Track.Title));
        var station = new RadioStation("webradio", "fip", "FIP");
        Assert.Equal(station, store.AddStation(1, station, 1).Value[1].Station);
        Assert.Equal(2, Count("SELECT COUNT(*) FROM playlist_item_part WHERE item_id = 5"));
        Assert.Equal(1, Count("SELECT COUNT(*) FROM sqlite_master WHERE type = 'index' AND name = 'playlist_item_playlist'"));
        Assert.Equal(1, Count("SELECT COUNT(*) FROM sqlite_master WHERE type = 'index' AND name = 'playlist_item_ref'"));
    }

    [Fact]
    public async Task Migrating_twice_changes_nothing_and_a_newer_schema_is_refused()
    {
        Assert.Equal(30, _database.Migrate().Value);
        Assert.Equal(30, _database.Migrate().Value);

        await using (var connection = _database.Open())
        {
            using var command = connection.CreateCommand();
            command.CommandText = "INSERT INTO schema_version (version) VALUES (99)";
            command.ExecuteNonQuery();
        }

        Assert.Equal(LibraryError.NewerSchema, _database.Migrate().Failure.Code);
    }

    // Two artists, three albums, in subfolders, and one loose track.
    private async Task<string> SeedJazz()
    {
        var music = Folder("music");
        var miles = Folder(Path.Combine("music", "Miles"));
        var coltrane = Folder(Path.Combine("music", "Coltrane"));
        WriteTrack(Path.Combine(miles, "so-what.wav"), "So What", "Miles Davis", "Kind of Blue", 1);
        WriteTrack(Path.Combine(miles, "blue-in-green.wav"), "Blue in Green", "Miles Davis", "Kind of Blue", 3);
        WriteTrack(Path.Combine(coltrane, "blue-train.wav"), "Blue Train", "John Coltrane", "Blue Train", 1);
        WriteTrack(Path.Combine(coltrane, "naima.wav"), "Naïma", "John Coltrane", "Giant Steps", 7);
        WriteTrack(Path.Combine(music, "loose.wav"), "Loose");
        var indexer = await Start(TimeSpan.FromMinutes(5), new LibraryFolder(music, LibraryFolderType.Local));
        await Until(() => indexer.Status.Passes == 1, "the first pass");
        return music;
    }

    private async Task<CatalogItems> Search(string text)
    {
        var found = await Catalog.SearchAsync(text, SearchKinds.All, 0, 50, Ct);
        Assert.True(found.IsSuccess, found.ToString());
        return found.Value;
    }

    // The id of the one track the text finds.
    private async Task<string> IdOf(string text) => Assert.Single((await Search(text)).Tracks).ProviderItemId;

    private static long Number(string trackId) =>
        long.Parse(trackId["track:".Length..], System.Globalization.CultureInfo.InvariantCulture);

    private long Count(string sql)
    {
        using var connection = _database.Open();
        using var command = connection.CreateCommand();
        command.CommandText = sql;
        return (long)command.ExecuteScalar()!;
    }

    private async Task<LibraryIndexer> Start(TimeSpan pollInterval, params LibraryFolder[] folders)
    {
        var indexer = new LibraryIndexer(Registry(new LibrarySettings(folders, pollInterval), _database, _dir.Path), _database,
            NullLogger<LibraryIndexer>.Instance);
        _indexers.Add(indexer);
        await indexer.StartAsync(Ct);
        return indexer;
    }

    // The folders of a test: the settings' own, and none from the API yet. Its
    // SMB sessions are never used, so nothing needs disposing.
    internal static LibraryFolders Registry(LibrarySettings settings, LibraryDatabase database, string directory) =>
        new(settings, database, Path.Combine(directory, "credentials.json"), new SmbSessions(NullLogger<SmbSessions>.Instance));

    private string Folder(string name) => Directory.CreateDirectory(Path.Combine(_dir.Path, name)).FullName;

    private string? ErrorOf(string path)
    {
        using var connection = _database.Open();
        using var command = connection.CreateCommand();
        command.CommandText = "SELECT error FROM library_file WHERE path = $path";
        command.Parameters.AddWithValue("$path", path);
        return command.ExecuteScalar() as string;
    }

    // Written beside the folder, then moved into it whole.
    private void Replace(string path, string title, string? artist = null, string? album = null)
    {
        var staging = Path.Combine(_dir.Path, "staging.wav");
        WriteTrack(staging, title, artist, album);
        File.Move(staging, path, overwrite: true);
    }

    // A tenth of a second of silence, 8 kHz mono, tagged.
    internal static void WriteTrack(string path, string title, string? artist = null, string? album = null,
        int number = 0, string? genre = null, string? composer = null, int year = 0, byte[]? picture = null)
    {
        const int Rate = 8000;
        const int DataBytes = Rate / 10 * 2;
        using (var stream = File.Create(path))
        using (var writer = new BinaryWriter(stream))
        {
            writer.Write("RIFF"u8);
            writer.Write(36 + DataBytes);
            writer.Write("WAVEfmt "u8);
            writer.Write(16);
            writer.Write((short)1);
            writer.Write((short)1);
            writer.Write(Rate);
            writer.Write(Rate * 2);
            writer.Write((short)2);
            writer.Write((short)16);
            writer.Write("data"u8);
            writer.Write(DataBytes);
            writer.Write(new byte[DataBytes]);
        }

        using var file = TagLib.File.Create(path);
        // An ID3v2 tag inside the RIFF file: the one that holds a picture.
        file.GetTag(TagLib.TagTypes.Id3v2, create: true);
        file.Tag.Title = title;
        if (artist is not null)
            file.Tag.Performers = [artist];
        if (album is not null)
            file.Tag.Album = album;
        file.Tag.Track = (uint)number;
        if (genre is not null)
            file.Tag.Genres = [genre];
        if (composer is not null)
            file.Tag.Composers = [composer];
        file.Tag.Year = (uint)year;
        if (picture is not null)
            file.Tag.Pictures = [new TagLib.Picture(new TagLib.ByteVector(picture)) { Type = TagLib.PictureType.FrontCover }];
        file.Save();
    }

    // A few bytes that start as a JPEG does, different for each seed.
    internal static byte[] Jpeg(byte seed) => [0xFF, 0xD8, 0xFF, 0xE0, .. Enumerable.Range(0, 60).Select(i => (byte)(seed + i))];

    private static Task Until(Func<bool> condition, string what) => Until(() => Task.FromResult(condition()), what);

    private static async Task Until(Func<Task<bool>> condition, string what)
    {
        var deadline = DateTime.UtcNow + TimeSpan.FromSeconds(20);
        while (!await condition())
        {
            if (DateTime.UtcNow > deadline)
                Assert.Fail($"timed out waiting for {what}");
            await Task.Delay(20, Ct);
        }
    }
}
