using System.Net;
using System.Text.Json.Nodes;
using Microsoft.Extensions.DependencyInjection;

namespace Multiroom.Server.Tests;

// Phases 30 and 31 on the composed host: media types on the answers and as a
// search filter, a folder's declared type, a book queued as one group (or as
// single items when asked), kept in a playlist as one entry, and paged by its
// parts over MCP.
public sealed partial class ApiTests
{
    [Fact]
    public async Task Audiobooks_are_typed_filtered_queued_as_one_group_and_kept_in_a_playlist_as_one_entry()
    {
        var book = Directory.CreateDirectory(Path.Combine(_dir.Path, "music", "Book")).FullName;
        for (var i = 1; i <= 3; i++)
            LibraryTests.WriteTrack(Path.Combine(book, $"{i}.wav"), $"Chapter {i}", "Narrator", "The Book", i, "Hörbuch");
        LibraryTests.WriteTrack(Path.Combine(_dir.Path, "music", "song.wav"), "Chapter Song", "Band", "Record", 1, "Pop");
        await StartAsync();
        var indexer = _host!.Services.GetRequiredService<Library.LibraryIndexer>();
        await Until(() => Task.FromResult(indexer.Status.Passes >= 1));

        // Every track says what it is and why; a station is radio.
        var all = await Ok(HttpMethod.Get, "search?q=chapter&types=tracks");
        Assert.Equal(4, all["items"]!["tracks"]!.AsArray().Count);
        var books = await Ok(HttpMethod.Get, "search?q=chapter&mediaType=audiobook");
        Assert.Equal(["Chapter 1", "Chapter 2", "Chapter 3"], books["items"]!["tracks"]!.AsArray()
            .Select(t => t!["title"]!.GetValue<string>()).Order(StringComparer.Ordinal));
        Assert.All(books["items"]!["tracks"]!.AsArray(), t => Assert.Equal(("audiobook", "genre"),
            (t!["mediaType"]!.GetValue<string>(), t["mediaTypeSource"]!.GetValue<string>())));
        Assert.Equal(["Chapter Song"], (await Ok(HttpMethod.Get, "search?q=chapter&mediaType=music"))["items"]!["tracks"]!
            .AsArray().Select(t => t!["title"]!.GetValue<string>()));
        await Refused(HttpMethod.Get, "search?q=chapter&mediaType=podcast", HttpStatusCode.BadRequest, "invalidRequest",
            "mediaType: music, audiobook or radio, was podcast");
        await Refused(HttpMethod.Get, "providers/local/search?q=chapter&mediaType=music", HttpStatusCode.BadRequest,
            "invalidRequest", "unknown query parameter mediaType");
        var station = _fake.AddStation("Jazz FM", [1, 2, 3]);
        var favourite = await Send(HttpMethod.Post, "favorites", $$"""{"station":{{Json(station)}}}""");
        Assert.Equal("radio", favourite.Body!["item"]!["mediaType"]!.GetValue<string>());

        // A node of audiobook tracks is one group; asked, its parts go in one by one.
        var album = (await Ok(HttpMethod.Get, "providers/local/browse?node=albums")).AsObject()["nodes"]!.AsArray()
            .Single(n => n!["title"]!.GetValue<string>() == "The Book")!["nodeId"]!.GetValue<string>();
        var queued = await Ok(HttpMethod.Post, "zones/living/queue/items", $$$"""{"node":{"provider":"local","id":"{{{album}}}"}}""");
        Assert.Equal((3, true), (queued["queued"]!.GetValue<int>(), queued["grouped"]!.GetValue<bool>()));
        var group = Assert.Single(queued["queue"]!["items"]!.AsArray())!;
        Assert.Equal(("The Book", "audiobook", "local:" + album, 3), (group["title"]!.GetValue<string>(),
            group["item"]!["type"]!.GetValue<string>(), group["item"]!["source"]!.GetValue<string>(),
            group["item"]!["parts"]!.AsArray().Count));
        var single = await Ok(HttpMethod.Post, "zones/living/queue/items",
            $$"""{"node":{"provider":"local","id":"{{album}}"},"group":false,"replace":true}""");
        Assert.Equal(["Chapter 1", "Chapter 2", "Chapter 3"], Titles(single["queue"]!));

        // Over MCP a group's parts are paged, never answered whole with the queue.
        await Ok(HttpMethod.Post, "zones/living/queue/items",
            $$"""{"node":{"provider":"local","id":"{{album}}"},"replace":true}""");
        await using (var mcp = await McpAsync(null))
        {
            var page = await Call(mcp, "get_queue", new { zone = "living" });
            var item = page["queue"]!["items"]![0]!;
            Assert.Empty(item["item"]!["parts"]!.AsArray());
            Assert.Equal(3, page["groups"]![0]!["parts"]!.GetValue<int>());
            var parts = await Call(mcp, "get_queue_item_parts",
                new { zone = "living", item = item["id"]!.GetValue<long>(), limit = 2 });
            Assert.Equal((3, true), (parts["total"]!.GetValue<int>(), parts["cut"]!.GetValue<bool>()));
            Assert.Equal("Chapter 1", parts["items"]![0]!["track"]!["title"]!.GetValue<string>());
            var found = await Call(mcp, "search", new { query = "chapter", mediaType = "music" });
            Assert.Equal("Chapter Song", Assert.Single(found["items"]!["tracks"]!.AsArray())!["title"]!.GetValue<string>());
        }

        // A book is a bookFavourite; an album of music is not one as a node.
        var bookFavourite = await Send(HttpMethod.Post, "favorites", $$$"""{"node":{"provider":"local","id":"{{{album}}}"}}""");
        Assert.Equal(HttpStatusCode.Created, bookFavourite.Status);
        Assert.Equal(("The Book", 3), (bookFavourite.Body!["item"]!["title"]!.GetValue<string>(),
            bookFavourite.Body["item"]!["parts"]!.AsArray().Count));
        var record = (await Ok(HttpMethod.Get, "providers/local/browse?node=albums")).AsObject()["nodes"]!.AsArray()
            .Single(n => n!["title"]!.GetValue<string>() == "Record")!["nodeId"]!.GetValue<string>();
        await Refused(HttpMethod.Post, "favorites", $$$"""{"node":{"provider":"local","id":"{{{record}}}"}}""",
            HttpStatusCode.BadRequest, "invalidArgument");
        var fromFavourite = await Ok(HttpMethod.Post, "zones/living/queue/items",
            $$"""{"favorite":{{bookFavourite.Body["id"]!.GetValue<long>()}},"replace":true}""");
        Assert.Equal(["The Book"], Titles(fromFavourite));

        // A playlist keeps the book as one entry.
        var playlist = (await Ok(HttpMethod.Post, "playlists", """{"name":"Evening"}"""))["id"]!.GetValue<long>();
        var entries = await Ok(HttpMethod.Post, $"playlists/{playlist}/entries", $$$"""{"node":{"provider":"local","id":"{{{album}}}"}}""");
        var entry = Assert.Single(entries.AsArray())!;
        Assert.Equal(("The Book", 3), (entry["title"]!.GetValue<string>(), entry["item"]!["parts"]!.AsArray().Count));
        await Refused(HttpMethod.Post, $"playlists/{playlist}/entries", "{}", HttpStatusCode.BadRequest, "invalidRequest",
            "exactly one of tracks or node");
        var replayed = await Ok(HttpMethod.Post, "zones/living/queue/items", $$"""{"playlist":{{playlist}},"replace":true}""");
        Assert.Equal(["The Book"], Titles(replayed["queue"]!));

        // The house groups books unless switched off; a request still says otherwise.
        Assert.True((await Ok(HttpMethod.Get, "settings/playback"))["groupBooks"]!.GetValue<bool>());
        using (var events = await EventsAsync(null))
        {
            Assert.False((await Ok(HttpMethod.Patch, "settings/playback", """{"groupBooks":false}"""))["groupBooks"]!.GetValue<bool>());
            Assert.Contains("false", (await events.NextAsync(e => e.Type == "settings.playbackChanged")).Data,
                StringComparison.Ordinal);
        }
        var ungrouped = await Ok(HttpMethod.Post, "zones/living/queue/items",
            $$$"""{"node":{"provider":"local","id":"{{{album}}}"},"replace":true}""");
        Assert.Equal(["Chapter 1", "Chapter 2", "Chapter 3"], Titles(ungrouped["queue"]!));
        var asked = await Ok(HttpMethod.Post, "zones/living/queue/items",
            $$$"""{"node":{"provider":"local","id":"{{{album}}}"},"group":true,"replace":true}""");
        Assert.Equal(["The Book"], Titles(asked["queue"]!));
        await Refused(HttpMethod.Patch, "settings/playback", "{}", HttpStatusCode.BadRequest, "invalidRequest",
            "a playback settings patch needs groupBooks, resumeBooks, findRadioLikes, or several");
        await Refused(HttpMethod.Patch, "settings/playback", """{"groupbooks":true}""", HttpStatusCode.BadRequest,
            "invalidRequest");

        // A folder declares what it holds; radio is a station's, not a folder's.
        var other = Directory.CreateDirectory(Path.Combine(_dir.Path, "spoken")).FullName;
        var folder = await Send(HttpMethod.Post, "library/folders",
            $$"""{"type":"local","path":{{JsonValue.Create(other).ToJsonString()}},"mediaType":"audiobook"}""");
        Assert.Equal(HttpStatusCode.Created, folder.Status);
        Assert.Equal("audiobook", folder.Body!["mediaType"]!.GetValue<string>());
        var id = folder.Body["id"]!.GetValue<string>();
        Assert.Null((await Ok(HttpMethod.Patch, $"library/folders/{id}", """{"mediaType":""}"""))["mediaType"]);
        Assert.Equal("music", (await Ok(HttpMethod.Patch, $"library/folders/{id}", """{"mediaType":"music"}"""))["mediaType"]!
            .GetValue<string>());
        await Refused(HttpMethod.Patch, $"library/folders/{id}", """{"mediaType":"radio"}""", HttpStatusCode.BadRequest,
            "invalidArgument", "a folder is declared music or audiobook, not radio");
        await Refused(HttpMethod.Patch, $"library/folders/{id}", """{"mediaType":"podcast"}""", HttpStatusCode.BadRequest,
            "invalidArgument");
    }
}
