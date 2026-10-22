using Microsoft.Extensions.Logging.Abstractions;
using Multiroom.Server.Abstractions;
using Multiroom.Server.Library;

namespace Multiroom.Server.Tests;

// Chapter marks inside one file (phase 32): read at probe time from an MP4's
// Nero chpl or its QuickTime chapter track and from an MP3's CHAP frames,
// kept with the track, and answered with a node's tracks when it is expanded.
public sealed class ChapterTests : IAsyncDisposable
{
    private static readonly (string Title, long StartMs)[] Marks =
        [("Opening", 0), ("The Middle", 60_000), ("The End", 120_000)];

    private readonly TempDirectory _dir = new();
    private readonly LibraryDatabase _database;
    private readonly List<LibraryIndexer> _indexers = [];

    public ChapterTests()
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

    [Fact]
    public async Task Chapters_are_read_from_chpl_a_chapter_track_and_CHAP_frames_and_a_broken_box_is_none()
    {
        var books = Folder("books");
        Mp4Fixture.WriteWithChapters(Path.Combine(books, "chpl.m4b"), 180_000, Marks, Mp4Fixture.ChapterStyle.Chpl);
        Mp4Fixture.WriteWithChapters(Path.Combine(books, "track.m4b"), 180_000, Marks, Mp4Fixture.ChapterStyle.Track);
        Mp4Fixture.WriteWithChapters(Path.Combine(books, "broken.m4b"), 180_000, Marks, Mp4Fixture.ChapterStyle.BrokenChpl);
        WriteMp3(Path.Combine(books, "chap.mp3"), [("Intro", 0), ("Part One", 1_000), ("Part Two", 2_000)]);

        var indexer = await Start(new LibraryFolder(books, LibraryFolderType.Local));
        await Until(() => indexer.Status.Passes == 1);

        Assert.Equal(0, indexer.Status.Failed);
        Assert.Equal(4, indexer.Status.Indexed);
        Assert.Equal(Marks, ChaptersOf("chpl.m4b"));
        Assert.Equal(Marks, ChaptersOf("track.m4b"));
        Assert.Empty(ChaptersOf("broken.m4b"));
        Assert.Equal([("Intro", 0L), ("Part One", 1_000L), ("Part Two", 2_000L)], ChaptersOf("chap.mp3"));

        // Expanded, an audiobook's chapters come with it, each ending where
        // the next starts and the last at the track's end.
        var expanded = (await Catalog.ExpandAsync("folder:" + books, 10, Ct)).Value;
        var chpl = expanded.Tracks.Single(t => t.Title == "chpl");
        Assert.Equal(MediaType.Audiobook, chpl.MediaType);
        Assert.Equal(
            [new Chapter("Opening", 0, 60_000), new Chapter("The Middle", 60_000, 120_000), new Chapter("The End", 120_000, 180_000)],
            expanded.Chapters![chpl.ProviderItemId]);
        Assert.False(expanded.Chapters.ContainsKey(expanded.Tracks.Single(t => t.Title == "broken").ProviderItemId));
    }

    [Fact]
    public async Task A_file_probed_before_chapters_were_read_is_probed_again_with_its_id_kept()
    {
        var books = Folder("books");
        Mp4Fixture.WriteWithChapters(Path.Combine(books, "chpl.m4b"), 180_000, Marks, Mp4Fixture.ChapterStyle.Chpl);
        var first = await Start(new LibraryFolder(books, LibraryFolderType.Local));
        await Until(() => first.Status.Passes == 1);
        var id = Scalar("SELECT track_id FROM library_file");
        await first.StopAsync(Ct);
        Scalar("DELETE FROM track_chapter RETURNING 1");
        Scalar("UPDATE library_file SET tags = 4 RETURNING 1");

        var second = await Start(new LibraryFolder(books, LibraryFolderType.Local));
        await Until(() => second.Status.Passes == 1);
        Assert.Equal(1, second.Status.Indexed);
        Assert.Equal(id, Scalar("SELECT track_id FROM library_file"));
        Assert.Equal(Marks, ChaptersOf("chpl.m4b"));
    }

    // MPEG-1 layer III frames of silence, 128 kbit/s at 44.1 kHz, about
    // three seconds, with ID3v2 CHAP frames in a CTOC's order.
    private static void WriteMp3(string path, (string Title, long StartMs)[] chapters)
    {
        const int FrameBytes = 417;
        byte[] frame = [0xFF, 0xFB, 0x90, 0x64, .. new byte[FrameBytes - 4]];
        File.WriteAllBytes(path, [.. Enumerable.Range(0, 115).SelectMany(_ => frame)]);

        using var file = TagLib.File.Create(path);
        var tag = (TagLib.Id3v2.Tag)file.GetTag(TagLib.TagTypes.Id3v2, create: true);
        tag.Title = "Mp3 Book";
        tag.Genres = ["Hörbuch"];
        foreach (var (title, start) in chapters)
        {
            var chapter = new TagLib.Id3v2.ChapterFrame($"ch{start}", title)
            {
                StartMilliseconds = (uint)start,
                EndMilliseconds = (uint)start + 1_000,
            };
            tag.AddFrame(chapter);
        }
        tag.AddFrame(new TagLib.Id3v2.TableOfContentsFrame("toc")
        {
            IsTopLevel = true,
            IsOrdered = true,
            ChapterIds = [.. chapters.Select(c => $"ch{c.StartMs}")],
        });
        file.Save();
    }

    private LibraryCatalog Catalog => new(_database, "local");

    private List<(string Title, long StartMs)> ChaptersOf(string file)
    {
        using var connection = _database.Open();
        using var command = connection.CreateCommand();
        command.CommandText = """
            SELECT c.title, c.start_ms FROM track_chapter c JOIN library_file f ON f.track_id = c.track_id
            WHERE f.path LIKE '%' || $file ORDER BY c.idx
            """;
        command.Parameters.AddWithValue("$file", file);
        using var reader = command.ExecuteReader();
        var chapters = new List<(string, long)>();
        while (reader.Read())
            chapters.Add((reader.GetString(0), reader.GetInt64(1)));
        return chapters;
    }

    private long? Scalar(string sql)
    {
        using var connection = _database.Open();
        using var command = connection.CreateCommand();
        command.CommandText = sql;
        return command.ExecuteScalar() as long?;
    }

    private async Task<LibraryIndexer> Start(params LibraryFolder[] folders)
    {
        var indexer = new LibraryIndexer(
            LibraryTests.Registry(new LibrarySettings(folders, TimeSpan.FromMinutes(5)), _database, _dir.Path), _database,
            NullLogger<LibraryIndexer>.Instance);
        _indexers.Add(indexer);
        await indexer.StartAsync(Ct);
        return indexer;
    }

    private string Folder(string name) => Directory.CreateDirectory(Path.Combine(_dir.Path, name)).FullName;

    private static async Task Until(Func<bool> condition)
    {
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(10));
        while (!condition())
            await Task.Delay(10, timeout.Token);
    }
}
