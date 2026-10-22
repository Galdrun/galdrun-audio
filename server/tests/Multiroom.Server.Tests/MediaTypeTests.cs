using Microsoft.Extensions.Logging.Abstractions;
using Multiroom.Server.Abstractions;
using Multiroom.Server.Library;

namespace Multiroom.Server.Tests;

// What an item is (phase 30): the classifier's rules in their order, the
// indexer's verdict per file and its source, a folder's declared type, the
// audiobooks view, and snapshots that keep the type.
public sealed class MediaTypeTests : IAsyncDisposable
{
    private readonly TempDirectory _dir = new();
    private readonly LibraryDatabase _database;
    private readonly List<LibraryIndexer> _indexers = [];

    public MediaTypeTests()
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

    public static TheoryData<MediaType?, MediaType?, int?, string?, string?, MediaType?, MediaTypeSource?> Rules => new()
    {
        // The owner's folder beats every tag; the provider's own type comes next.
        { MediaType.Music, null, 2, ".m4b", "Audiobook", MediaType.Music, MediaTypeSource.Folder },
        { MediaType.Audiobook, null, null, ".flac", "Jazz", MediaType.Audiobook, MediaTypeSource.Folder },
        { null, MediaType.Music, 2, ".m4b", "Hörbuch", MediaType.Music, MediaTypeSource.Provider },
        // stik beats the container and the genre; a stik that is not 2 says nothing.
        { null, null, 2, ".m4a", "Jazz", MediaType.Audiobook, MediaTypeSource.Tag },
        { null, null, 1, ".m4b", null, MediaType.Audiobook, MediaTypeSource.Extension },
        { null, null, null, ".M4B", null, MediaType.Audiobook, MediaTypeSource.Extension },
        // The genre, folded: case, accents, a list.
        { null, null, null, ".mp3", "Hörbuch", MediaType.Audiobook, MediaTypeSource.Genre },
        { null, null, null, ".mp3", "HORSPIEL", MediaType.Audiobook, MediaTypeSource.Genre },
        { null, null, null, ".mp3", "Fantasy / Audio Book", MediaType.Audiobook, MediaTypeSource.Genre },
        { null, null, null, ".mp3", "Spoken Word", MediaType.Audiobook, MediaTypeSource.Genre },
        { null, null, 1, ".mp3", "Speech", MediaType.Audiobook, MediaTypeSource.Genre },
        // Nothing says anything.
        { null, null, null, ".flac", "Jazz", null, null },
        { null, null, null, null, null, null, null },
    };

    [Theory]
    [MemberData(nameof(Rules))]
    public void The_classifier_takes_the_first_rule_that_answers(MediaType? declared, MediaType? provider, int? kind,
        string? extension, string? genre, MediaType? type, MediaTypeSource? source)
    {
        Assert.Equal(new MediaTypeVerdict(type, source),
            MediaTypeClassifier.Classify(new MediaTypeEvidence(declared, provider, kind, extension, genre)));
        Assert.True(MediaTypeClassifier.IsMusic(new Track("x", "y", "z")));
        Assert.False(MediaTypeClassifier.IsMusic(new Track("x", "y", "z", MediaType: MediaType.Audiobook)));
    }

    [Theory]
    [InlineData(new[] { "Kapitel 1.1 - Der Hobbit", "Kapitel 1.2 - Der Hobbit", "Kapitel 2 - Der Hobbit" }, true)]
    [InlineData(new[] { "Chapter 1", "Chapter 2", "Chapter 3", "Chapter 4", "Credits" }, true)]
    [InlineData(new[] { "Folge 1", "FOLGE 2", "Teil 3" }, true)]
    [InlineData(new[] { "So What", "Freddie Freeloader", "Blue in Green" }, false)]
    [InlineData(new[] { "Chapter 1", "Chapter 2" }, false)]
    [InlineData(new[] { "Chapter 1", "Chapter 2", "Intro", "Outro" }, false)]
    [InlineData(new[] { "Teil eins", "Teil zwei", "Teil drei" }, false)]
    // An album titled "Hörbuch (ungekürzt)" whose tracks are no chapters is not taken: the title alone decides nothing.
    [InlineData(new[] { "Eins", "Zwei", "Drei" }, false)]
    public void A_container_looks_spoken_by_its_chapters_only(string[] tracks, bool spoken) =>
        Assert.Equal(spoken, MediaTypeClassifier.LooksSpoken(tracks));

    [Fact]
    public async Task The_indexer_answers_each_file_with_its_type_and_the_rule_that_gave_it()
    {
        var mixed = Folder("mixed");
        LibraryTests.WriteTrack(Path.Combine(mixed, "song.wav"), "Plain Song", "Band", genre: "Jazz");
        LibraryTests.WriteTrack(Path.Combine(mixed, "hoer.wav"), "Kapitel Eins", "Erzähler", genre: "Hörbuch");
        Mp4Fixture.Write(Path.Combine(mixed, "tagged.m4a"), "Tagged Chapter", mediaKind: 2, genre: "Jazz");
        Mp4Fixture.Write(Path.Combine(mixed, "contained.m4b"), "Contained Chapter");
        Mp4Fixture.Write(Path.Combine(mixed, "song.m4a"), "Mp4 Song", mediaKind: 1);
        var declared = Folder("declared");
        Mp4Fixture.Write(Path.Combine(declared, "remix.m4a"), "Declared Remix", mediaKind: 2);

        var indexer = await Start(new LibraryFolder(mixed, LibraryFolderType.Local),
            new LibraryFolder(declared, LibraryFolderType.Local, MediaType: MediaType.Music));
        await Until(() => indexer.Status.Passes == 2, "both passes");

        Assert.Equal(0, indexer.Status.Failed);
        Assert.Equal((null, null), await TypeOf("Plain Song"));
        Assert.Equal((null, null), await TypeOf("Mp4 Song"));
        Assert.Equal((MediaType.Audiobook, MediaTypeSource.Genre), await TypeOf("Kapitel Eins"));
        Assert.Equal((MediaType.Audiobook, MediaTypeSource.Tag), await TypeOf("Tagged Chapter"));
        Assert.Equal((MediaType.Audiobook, MediaTypeSource.Extension), await TypeOf("Contained Chapter"));
        Assert.Equal((MediaType.Music, MediaTypeSource.Folder), await TypeOf("Declared Remix"));
        Assert.Equal(6, Count("SELECT MIN(tags) FROM library_file"));
    }

    [Fact]
    public async Task A_folders_declared_type_reclassifies_its_tracks_without_a_probe_and_keeps_their_ids()
    {
        var books = Folder("books");
        LibraryTests.WriteTrack(Path.Combine(books, "one.wav"), "Chapter One", "Narrator", "The Book", 1);
        LibraryTests.WriteTrack(Path.Combine(books, "two.wav"), "Chapter Two", "Narrator", "The Book", 2);
        var registry = LibraryTests.Registry(new LibrarySettings([], TimeSpan.FromMinutes(5)), _database, _dir.Path);
        var indexer = await Start(registry);
        var added = registry.Add(LibraryFolderType.Local, books, null, enabled: true);
        Assert.True(added.IsSuccess, added.ToString());
        await Until(() => indexer.Folder(added.Value.Id)?.LastPassResult is not null, "the first pass");
        var id = await IdOf("Chapter One");
        Assert.Equal((null, null), await TypeOf("Chapter One"));

        Assert.True(registry.Update(added.Value.Id, null, null, null, null, (MediaType.Audiobook, true)).IsSuccess);
        await Until(async () => (await TypeOf("Chapter Two")).Type == MediaType.Audiobook, "the reclassification");
        Assert.Equal((MediaType.Audiobook, MediaTypeSource.Folder), await TypeOf("Chapter One"));
        Assert.Equal(id, await IdOf("Chapter One"));
        await Until(() => indexer.Folder(added.Value.Id)?.LastPass is not null && indexer.Status.Passes >= 2, "the next pass");
        Assert.Equal(0, indexer.Folder(added.Value.Id)!.LastPassResult!.Probed);
        Assert.Equal(MediaType.Audiobook, indexer.Folder(added.Value.Id)!.MediaType);

        // Declared none again: the tags decide, and they say nothing.
        Assert.True(registry.Update(added.Value.Id, null, null, null, null, (null, true)).IsSuccess);
        await Until(async () => (await TypeOf("Chapter One")).Type is null, "the tags deciding again");
        Assert.Equal(LibraryError.InvalidArgument,
            registry.Update(added.Value.Id, null, null, null, null, (MediaType.Radio, true)).Failure.Code);
    }

    [Fact]
    public async Task The_audiobooks_view_lists_a_book_over_several_discs_as_one_and_albums_still_list_them()
    {
        var books = Folder("books");
        LibraryTests.WriteTrack(Path.Combine(books, "d2-1.wav"), "Part Three", "Narrator", "Long Book CD 2", 1, "Audiobook");
        LibraryTests.WriteTrack(Path.Combine(books, "d1-2.wav"), "Part Two", "Narrator", "Long Book CD 1", 2, "Audiobook");
        LibraryTests.WriteTrack(Path.Combine(books, "d1-1.wav"), "Part One", "Narrator", "Long Book CD 1", 1, "Audiobook");
        LibraryTests.WriteTrack(Path.Combine(books, "short.wav"), "Only Part", "Narrator", "Short Book", 1, "Audiobook");
        LibraryTests.WriteTrack(Path.Combine(books, "song.wav"), "A Song", "Band", "Record", 1, "Jazz");
        var indexer = await Start(new LibraryFolder(books, LibraryFolderType.Local));
        await Until(() => indexer.Status.Passes == 1, "the first pass");

        var view = (await Catalog.BrowseAsync(new BrowseRequest("audiobooks"), Ct)).Value;
        Assert.Equal(["Long Book", "Short Book"], view.Nodes.Select(n => n.Title));
        var book = view.Nodes[0];
        Assert.True(book.Playable);
        Assert.StartsWith("book:", book.NodeId, StringComparison.Ordinal);

        var expanded = (await Catalog.ExpandAsync(book.NodeId, 100, Ct)).Value;
        Assert.Equal(["Part One", "Part Two", "Part Three"], expanded.Tracks.Select(t => t.Title));
        Assert.All(expanded.Tracks, t => Assert.Equal(MediaType.Audiobook, t.MediaType));
        var page = (await Catalog.BrowseAsync(new BrowseRequest(book.NodeId), Ct)).Value;
        Assert.Equal("Long Book", page.Node!.Title);
        Assert.Equal(3, page.Total);

        var albums = (await Catalog.BrowseAsync(new BrowseRequest("albums", 0, 100), Ct)).Value;
        Assert.Equal(["Long Book CD 1", "Long Book CD 2", "Record", "Short Book"], albums.Nodes.Select(n => n.Title));
        Assert.Equal(ProviderError.InvalidArgument, (await Catalog.ExpandAsync("audiobooks", 10, Ct)).Failure.Code);
        Assert.Equal(ProviderError.NotFound, (await Catalog.ExpandAsync("book:999", 10, Ct)).Failure.Code);
    }

    [Theory]
    [InlineData("Long Book CD 2", "Long Book", 2)]
    [InlineData("Long Book - Disc 3", "Long Book", 3)]
    [InlineData("Long Book (2/5)", "Long Book", 2)]
    [InlineData("Long Book [CD 10]", "Long Book", 10)]
    [InlineData("1984", "1984", null)]
    [InlineData("CD 2", "CD 2", null)]
    public void A_disc_suffix_is_taken_off_a_books_title(string album, string title, int? disc) =>
        Assert.Equal((title, disc), LibraryCatalog.BookTitle(album));

    [Fact]
    public void Playlist_entries_and_favourites_keep_the_type_through_a_restart()
    {
        Assert.True(_database.Migrate().IsSuccess);
        var chapter = new Track("local", "track:1", "Chapter", "Narrator", "Book", TimeSpan.FromMinutes(20),
            MediaType: MediaType.Audiobook);
        var store = new PlaylistStore(_database);
        var playlist = store.CreatePlaylist("Evening").Value;
        Assert.True(store.AddEntries(playlist.Id, [chapter], null).IsSuccess);
        Assert.True(store.AddFavorite(chapter).IsSuccess);

        var again = new PlaylistStore(_database);
        Assert.Equal(MediaType.Audiobook, Assert.Single(again.Entries(playlist.Id).Value).Track!.MediaType);
        Assert.Equal(MediaType.Audiobook, Assert.Single(again.Favorites().Value).Track!.MediaType);
    }

    // --- helpers ------------------------------------------------------------

    private async Task<(MediaType? Type, MediaTypeSource? Source)> TypeOf(string title)
    {
        var track = Assert.Single((await Catalog.SearchAsync($"\"{title}\"", SearchKinds.Tracks, 0, 10, Ct)).Value.Tracks,
            t => t.Title == title);
        return (track.MediaType, track.MediaTypeSource);
    }

    private async Task<string> IdOf(string title) =>
        Assert.Single((await Catalog.SearchAsync(title, SearchKinds.Tracks, 0, 10, Ct)).Value.Tracks, t => t.Title == title)
            .ProviderItemId;

    private long Count(string sql)
    {
        using var connection = _database.Open();
        using var command = connection.CreateCommand();
        command.CommandText = sql;
        return (long)command.ExecuteScalar()!;
    }

    private Task<LibraryIndexer> Start(params LibraryFolder[] folders) =>
        Start(LibraryTests.Registry(new LibrarySettings(folders, TimeSpan.FromMinutes(5)), _database, _dir.Path));

    private async Task<LibraryIndexer> Start(LibraryFolders registry)
    {
        var indexer = new LibraryIndexer(registry, _database, NullLogger<LibraryIndexer>.Instance);
        _indexers.Add(indexer);
        await indexer.StartAsync(Ct);
        return indexer;
    }

    private string Folder(string name) => Directory.CreateDirectory(Path.Combine(_dir.Path, name)).FullName;

    private static Task Until(Func<bool> condition, string what) => Until(() => Task.FromResult(condition()), what);

    private static async Task Until(Func<Task<bool>> condition, string what)
    {
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(10));
        while (!await condition())
        {
            if (timeout.IsCancellationRequested)
                Assert.Fail($"timed out waiting for {what}");
            await Task.Delay(10, CancellationToken.None);
        }
    }
}
