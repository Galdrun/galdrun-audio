using System.Net;
using Microsoft.Extensions.DependencyInjection;
using Multiroom.Server.Library;

namespace Multiroom.Server.Tests;

// Phases 32 and 33 on the composed host: a book that is one .m4b with
// chapters queued as one group of chapters, the house's resumeBooks, the
// saved positions listed and forgotten, and fromStart taken where books are
// queued and played.
public sealed partial class ApiTests
{
    [Fact]
    public async Task A_chaptered_book_is_queued_by_its_chapters_and_its_saved_place_is_listed_and_forgotten()
    {
        var folder = Directory.CreateDirectory(Path.Combine(_dir.Path, "music", "Novel")).FullName;
        Mp4Fixture.WriteWithChapters(Path.Combine(folder, "novel.m4b"), 180_000,
            [("Opening", 0), ("The Middle", 60_000), ("The End", 120_000)], Mp4Fixture.ChapterStyle.Chpl);
        await StartAsync();
        var indexer = _host!.Services.GetRequiredService<LibraryIndexer>();
        await Until(() => Task.FromResult(indexer.Status.Passes >= 1));

        // One file, three chapters: one group of three parts, each its chapter.
        var node = "folder:" + folder;
        var queued = await Ok(HttpMethod.Post, "zones/living/queue/items",
            $$"""{"node":{"provider":"local","id":{{Json(node)}}},"fromStart":true}""");
        Assert.True(queued["grouped"]!.GetValue<bool>());
        var parts = queued["queue"]!["items"]![0]!["item"]!["parts"]!.AsArray();
        Assert.Equal([("Opening", 0L, 60_000L), ("The Middle", 60_000L, 120_000L), ("The End", 120_000L, 180_000L)],
            parts.Select(p => (p!["chapter"]!["title"]!.GetValue<string>(), p["chapter"]!["startMs"]!.GetValue<long>(),
                p["chapter"]!["endMs"]!.GetValue<long>())));
        Assert.Equal("00:03:00", queued["queue"]!["items"]![0]!["item"]!["duration"]!.GetValue<string>());

        // The house resumes books only when switched on; the place is saved either way.
        var settings = await Ok(HttpMethod.Get, "settings/playback");
        Assert.False(settings["resumeBooks"]!.GetValue<bool>());
        Assert.True((await Ok(HttpMethod.Patch, "settings/playback", """{"resumeBooks":true}"""))["resumeBooks"]!.GetValue<bool>());

        // Positions: none, then one, listed and forgotten by reference.
        Assert.Equal(0, (await Ok(HttpMethod.Get, "books/positions"))["total"]!.GetValue<int>());
        var reference = "local:" + node;
        var track = parts[1]!["track"]!;
        var part = $"{track["providerId"]!.GetValue<string>()}:{track["providerItemId"]!.GetValue<string>()}";
        Assert.True(_host.Services.GetRequiredService<BookPositionStore>().Save(new BookPosition(reference, "novel", null, 1,
            part, 5_000, 65_000, 180_000, false, DateTimeOffset.UtcNow)).IsSuccess);
        var page = await Ok(HttpMethod.Get, "books/positions?limit=5");
        Assert.Equal((1, reference, 65_000L), (page["total"]!.GetValue<int>(), page["items"]![0]!["reference"]!.GetValue<string>(),
            page["items"]![0]!["positionMs"]!.GetValue<long>()));
        await Refused(HttpMethod.Get, "books/positions?zone=living", HttpStatusCode.BadRequest, "invalidRequest");

        // Queued again with the setting on: it says where it resumes.
        var again = await Ok(HttpMethod.Post, "zones/living/queue/items",
            $$"""{"node":{"provider":"local","id":{{Json(node)}}},"replace":true}""");
        var group = again["queue"]!["items"]![0]!["item"]!;
        Assert.Equal((1, 5_000L, 65_000L, true), (group["saved"]!["part"]!.GetValue<int>(),
            group["saved"]!["offsetMs"]!.GetValue<long>(), group["saved"]!["positionMs"]!.GetValue<long>(),
            group["resume"]!.GetValue<bool>()));
        await using (var mcp = await McpAsync(null))
        {
            var queue = await Call(mcp, "get_queue", new { zone = "living" });
            Assert.Equal(65_000, queue["groups"]![0]!["savedPositionMs"]!.GetValue<long>());
            var listed = await Call(mcp, "list_book_positions");
            Assert.Equal(reference, listed["items"]![0]!["reference"]!.GetValue<string>());
        }

        var deleted = await Send(HttpMethod.Delete, "books/positions/" + Uri.EscapeDataString(reference));
        Assert.Equal(HttpStatusCode.NoContent, deleted.Status);
        await Refused(HttpMethod.Delete, "books/positions/" + Uri.EscapeDataString(reference), HttpStatusCode.NotFound,
            "notFound", $"no saved position for {reference}");
        await Refused(HttpMethod.Post, "zones/living/play", """{"fromstart":true}""", HttpStatusCode.BadRequest,
            "invalidRequest");
    }
}
