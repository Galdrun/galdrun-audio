using System.Net;
using System.Text.Json.Nodes;
using Microsoft.Extensions.DependencyInjection;
using Multiroom.Server.Library;

namespace Multiroom.Server.Tests;

// Audiobook likes (phase 37): a book is liked by its node, an album the
// server classifies as a book is liked as one, and the album likes from
// before are asked once at start.
public sealed partial class ApiTests
{
    private async Task<(string Book, string Music)> StartWithBookAndMusicAsync()
    {
        var book = Directory.CreateDirectory(Path.Combine(_dir.Path, "music", "Book")).FullName;
        for (var i = 1; i <= 3; i++)
            LibraryTests.WriteTrack(Path.Combine(book, $"{i}.wav"), $"Chapter {i}", "Narrator", "The Book", i, "Hörbuch");
        var record = Directory.CreateDirectory(Path.Combine(_dir.Path, "music", "Record")).FullName;
        for (var i = 1; i <= 2; i++)
            LibraryTests.WriteTrack(Path.Combine(record, $"{i}.wav"), $"Song {i}", "Band", "Record", i, "Pop");
        await StartAsync();
        var indexer = _host!.Services.GetRequiredService<LibraryIndexer>();
        await Until(() => Task.FromResult(indexer.Status.Passes >= 1));
        var albums = (await Ok(HttpMethod.Get, "providers/local/browse?node=albums"))["nodes"]!.AsArray();
        string Node(string title) => albums.Single(n => n!["title"]!.GetValue<string>() == title)!["nodeId"]!.GetValue<string>();
        return (Node("The Book"), Node("Record"));
    }

    private async Task<JsonNode> AlbumAsync(string title) =>
        (await Ok(HttpMethod.Get, $"search?q={Uri.EscapeDataString(title)}&types=albums"))["items"]!["albums"]!.AsArray()
            .Single(a => a!["title"]!.GetValue<string>() == title)!;

    [Fact]
    public async Task A_book_is_liked_by_its_node_or_its_album_and_a_music_album_stays_an_album()
    {
        var (book, music) = await StartWithBookAndMusicAsync();

        var liked = await Send(HttpMethod.Post, "likes", $$$"""{"node":{"provider":"local","id":"{{{book}}}"}}""");
        Assert.Equal(HttpStatusCode.Created, liked.Status);
        var audiobook = liked.Body!["item"]!;
        Assert.Equal("audiobook", audiobook["kind"]!.GetValue<string>());
        Assert.Equal(("local:" + book, "The Book", "Narrator", 300L), (audiobook["reference"]!.GetValue<string>(),
            audiobook["title"]!.GetValue<string>(), audiobook["author"]!.GetValue<string>(),
            audiobook["durationMs"]!.GetValue<long>()));
        Assert.Null(audiobook["parts"]);

        // The album liked as an album: the server says it is the same book.
        var asAlbum = await Ok(HttpMethod.Post, "likes", $$"""{"album":{{(await AlbumAsync("The Book")).ToJsonString()}}}""");
        Assert.Equal((liked.Body["id"]!.GetValue<long>(), true), (asAlbum["id"]!.GetValue<long>(), asAlbum["item"]!["kind"]!.GetValue<string>() == "audiobook"));
        var record = await Ok(HttpMethod.Post, "likes", $$"""{"album":{{(await AlbumAsync("Record")).ToJsonString()}}}""");
        Assert.Equal("Record", record["item"]!["title"]!.GetValue<string>());

        Assert.Equal(["The Book"], (await Ok(HttpMethod.Get, "likes?kind=audiobook")).AsArray()
            .Select(l => l!["item"]!["title"]!.GetValue<string>()));
        Assert.Equal(["Record"], (await Ok(HttpMethod.Get, "likes?kind=album")).AsArray()
            .Select(l => l!["item"]!["title"]!.GetValue<string>()));
        var status = await Ok(HttpMethod.Post, "items/status",
            $$"""{"items":[{"kind":"audiobook","providerId":"local","providerItemId":"{{book}}"}]}""");
        Assert.True(status[0]!["liked"]!.GetValue<bool>());

        await Refused(HttpMethod.Post, "likes", $$$"""{"node":{"provider":"local","id":"{{{music}}}"}}""",
            HttpStatusCode.BadRequest, "invalidArgument", $"{music} is no book (two or more audiobook tracks): like its tracks or its album");
        await using var mcp = await McpAsync(null);
        Assert.Equal("The Book", (await Call(mcp, "list_likes", new { kind = "audiobook" }))["items"]![0]!["item"]!
            ["title"]!.GetValue<string>());
    }

    [Fact]
    public async Task A_liked_book_resumes_at_its_saved_place_and_an_album_like_from_before_is_made_a_book_at_start()
    {
        var (book, _) = await StartWithBookAndMusicAsync();
        var reference = (await Ok(HttpMethod.Post, "likes", $$$"""{"node":{"provider":"local","id":"{{{book}}}"}}"""))
            ["item"]!["reference"]!.GetValue<string>();

        // Queued through its reference, with resumeBooks on, at its place.
        await Ok(HttpMethod.Patch, "settings/playback", """{"resumeBooks":true}""");
        var parts = (await Ok(HttpMethod.Post, "zones/living/queue/items",
            $$"""{"node":{"provider":"local","id":"{{book}}"},"fromStart":true}"""))["queue"]!["items"]![0]!["item"]!["parts"]!;
        var track = parts[1]!["track"]!;
        Assert.True(_host!.Services.GetRequiredService<BookPositionStore>().Save(new BookPosition(reference, "The Book",
            "Narrator", 1, $"{track["providerId"]!.GetValue<string>()}:{track["providerItemId"]!.GetValue<string>()}", 50,
            150, 300, false, DateTimeOffset.UtcNow)).IsSuccess);
        var node = LikeStore.BookRef(reference);
        var again = await Ok(HttpMethod.Post, "zones/living/queue/items",
            $$"""{"node":{"provider":"{{node.ProviderId}}","id":"{{node.ProviderItemId}}"},"replace":true}""");
        var group = again["queue"]!["items"]![0]!["item"]!;
        Assert.Equal((1, 50L, true), (group["saved"]!["part"]!.GetValue<int>(), group["saved"]!["offsetMs"]!.GetValue<long>(),
            group["resume"]!.GetValue<bool>()));

        // An album like from before phase 37, as the migration leaves it:
        // asked once at the next start, and a book from then on.
        await Send(HttpMethod.Delete, $"likes/{(await Ok(HttpMethod.Get, "likes"))[0]!["id"]!.GetValue<long>()}");
        var album = (await AlbumAsync("The Book")).ToJsonString();
        _http!.Dispose();
        await _host.StopAsync(Ct);
        _host.Dispose();
        using (var connection = new LibraryDatabase(Path.Combine(_dir.Path, "data", "library.db")).Open())
        using (var command = connection.CreateCommand())
        {
            command.CommandText = """
                INSERT INTO liked (id, kind, provider_id, provider_item_id, snapshot, added) VALUES (50, 'album', 'local', $item, $album, 1);
                INSERT INTO liked_album_check (like_id) VALUES (50);
                """;
            command.Parameters.AddWithValue("$item", book);
            command.Parameters.AddWithValue("$album", album);
            command.ExecuteNonQuery();
        }
        await StartAsync();
        await Until(async () => (await Ok(HttpMethod.Get, "likes?kind=audiobook")).AsArray().Count == 1);
        var turned = (await Ok(HttpMethod.Get, "likes"))[0]!;
        Assert.Equal((50L, "local:" + book), (turned["id"]!.GetValue<long>(), turned["item"]!["reference"]!.GetValue<string>()));
    }
}
