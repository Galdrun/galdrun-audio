using Multiroom.Server.Abstractions;
using Multiroom.Server.Library;
using Multiroom.Server.Providers.Local;

namespace Multiroom.Server.Tests;

// Search answers the tracks the house keeps first: in a playlist, a zone's
// queue, a favourite or liked; through the host for every provider, and in
// the library's own order across its pages.
public sealed partial class ApiTests
{
    [Fact]
    public async Task Unified_search_answers_queued_liked_favourite_and_playlist_tracks_first()
    {
        await StartAsync();
        Track[] tracks = [.. "ABCDE".Select(c => _fake.AddTrack(_dir.Write($"{c}.flac", c.ToString()), $"Blue {c}"))];
        Assert.Equal(["Blue A", "Blue B", "Blue C", "Blue D", "Blue E"], SearchTitles(await Ok(HttpMethod.Get, "search?q=blue")));

        await Ok(HttpMethod.Post, "zones/living/queue/items", $$"""{"tracks":[{{Json(tracks[4])}}]}""");
        await Ok(HttpMethod.Post, "likes", Json(new { track = tracks[3] }));
        await Ok(HttpMethod.Post, "favorites", Json(new { track = tracks[1] }));

        // Kept ones in their provider's order, then the rest.
        Assert.Equal(["Blue B", "Blue D", "Blue E", "Blue A", "Blue C"], SearchTitles(await Ok(HttpMethod.Get, "search?q=blue")));

        var playlist = (await Ok(HttpMethod.Post, "playlists", """{"name":"Mix"}"""))["id"]!.GetValue<long>();
        await Ok(HttpMethod.Post, $"playlists/{playlist}/entries", $$"""{"tracks":[{{Json(tracks[2])}}]}""");
        Assert.Equal(["Blue B", "Blue C", "Blue D", "Blue E", "Blue A"], SearchTitles(await Ok(HttpMethod.Get, "search?q=blue")));
    }

    [Fact]
    public async Task The_library_answers_its_kept_tracks_first_across_its_pages()
    {
        var music = Directory.CreateDirectory(Path.Combine(_dir.Path, "kept")).FullName;
        foreach (var c in "ABCD")
            LibraryTests.WriteTrack(Path.Combine(music, $"{c}.wav"), $"Song {c}", "Someone", "Songs", c - 'A' + 1);
        var database = new LibraryDatabase(Path.Combine(_dir.Path, "kept.db"));
        var indexer = new LibraryIndexer(LibraryTests.Registry(new LibrarySettings(
                [new LibraryFolder(music, LibraryFolderType.Local)], TimeSpan.FromMinutes(5)), database, _dir.Path),
            database, Microsoft.Extensions.Logging.Abstractions.NullLogger<LibraryIndexer>.Instance);
        await indexer.StartAsync(Ct);
        try
        {
            var deadline = DateTime.UtcNow + TimeSpan.FromSeconds(20);
            while (indexer.Status.Passes < 1 && DateTime.UtcNow < deadline)
                await Task.Delay(20, Ct);
            var catalog = new LibraryCatalog(database, LocalFilesProvider.ProviderId);
            async Task<List<string>> Page(int offset) =>
                [.. (await catalog.SearchAsync("song", SearchKinds.Tracks, offset, 2, Ct)).Value.Tracks.Select(t => t.Title)];
            Track Song(char c) => Assert.Single(catalog.SearchAsync($"song {c}", SearchKinds.Tracks, 0, 10, Ct).Result.Value.Tracks);

            var playlists = new PlaylistStore(database);
            playlists.AddEntries(playlists.CreatePlaylist("Mix").Value.Id, [Song('D')], null);
            new LikeStore(database).Add(Song('C'));

            Assert.Equal(["Song C", "Song D"], (await Page(0)).Order());
            Assert.Equal(["Song A", "Song B"], (await Page(2)).Order());
        }
        finally
        {
            await indexer.StopAsync(CancellationToken.None);
            indexer.Dispose();
        }
    }

    private static List<string> SearchTitles(System.Text.Json.Nodes.JsonNode result) =>
        [.. result["items"]!["tracks"]!.AsArray().Select(t => t!["title"]!.GetValue<string>())];
}
