using System.Text.Json.Nodes;
using Multiroom.Server.Abstractions;

namespace Multiroom.Server.Tests;

// Audiobook chapters inside one file (phase 32), against FakeCore: a file is
// staged once for all its chapters, next and previous within it are a
// zone.seek, and the part follows the clock across a chapter's start.
public sealed partial class QueueEngineTests
{
    // A, the book, B: the book is `files` files of 180 s, each with three
    // chapters of 60 s.
    private async Task<(QueueState Queue, QueueItemId Book)> ChapterBookQueueAsync(int files = 1)
    {
        var music = await StartAsync("A", "B");
        var parts = new List<QueuePart>();
        for (var f = 1; f <= files; f++)
        {
            var track = _fake.AddTrack(_dir.Write($"book{f}.m4b", $"book file {f}"), files == 1 ? "The Book" : $"File {f}") with
            {
                Duration = TimeSpan.FromSeconds(180),
                MediaType = MediaType.Audiobook,
            };
            parts.AddRange(Enumerable.Range(0, 3).Select(c =>
                new QueuePart(track, Chapter: new Chapter($"Chapter {f}.{c + 1}", c * 60_000L, (c + 1) * 60_000L))));
        }
        var book = new QueueGroup("The Book", "Narrator", null, MediaType.Audiobook, "fake:book", parts);
        var queue = Ok(await _engine.AddEntriesAsync(Zone,
            [new QueueEntry(music[0]), new QueueEntry(Group: book), new QueueEntry(music[1])], null, Ct));
        Ok(await _engine.SetClientsAsync(Zone, ["roomA"], Ct));
        // zone.seek answered for whatever source is audible.
        _core.Answer("zone.seek", p => new JsonObject
        {
            ["zone"] = Zone,
            ["sourceId"] = Queue().NowPlaying?.SourceId,
            ["positionNs"] = p["positionNs"]!.DeepClone(),
            ["cutTimelineNs"] = 5000,
            ["timelineNs"] = 5150,
        });
        return (queue, queue.Items[1].Id);
    }

    // The source is heard from its start offset, the transport playing.
    private async Task HeardAsync(string source, ulong startOffsetNs = 0)
    {
        await EmitAsync(
            """{"event":"zone.transport","zone":"living","state":"playing","timelineNs":1000}""",
            $$"""{"event":"zone.sourceStarted","zone":"living","sourceId":"{{source}}","timelineNs":1000,"serverTimeNs":5000,"startOffsetNs":{{startOffsetNs}}}""");
        await Until(() => Queue().NowPlaying?.SourceId == source);
    }

    [Fact]
    public async Task A_book_in_one_file_is_staged_once_and_its_chapters_are_stepped_by_seeks()
    {
        var (queue, book) = await ChapterBookQueueAsync();
        Assert.Equal(TimeSpan.FromMinutes(3), queue.Items[1].Group!.Duration);

        Ok(await _engine.PlayAsync(Zone, book, Ct));
        var file = Staged()[^1];
        Assert.Equal(("The Book", "fake:t2"), (file.Title, file.TrackId));
        Assert.DoesNotContain("startOffsetNs", _core.All.Last(r => r.Command == "source.stage").Params,
            StringComparison.Ordinal);

        // Staged once: what the core is asked for next is B, not chapter 2.
        await EmitAsync($$"""{"event":"zone.sourceNeeded","zone":"living","afterSourceId":"{{file.SourceId}}"}""");
        await Until(() => Staged().Count == 2);
        Assert.Equal("B", Staged()[1].Title);
        await HeardAsync(file.SourceId);
        Assert.Equal(new QueuePartState(0, 3, "Chapter 1.1", 0, 60_000), Queue().Part);

        // Next and previous within the file: one zone.seek each.
        var before = _core.All.Count;
        var next = Ok(await _engine.NextAsync(Zone, Ct));
        Assert.Equal([("zone.seek", """{"zone":"living","positionNs":60000000000}""")], Sent(before));
        Assert.Equal((book, 1, 60_000L), (next.Current, next.Part!.Index, next.PositionMs));
        before = _core.All.Count;
        Ok(await _engine.NextAsync(Zone, Ct));
        Assert.Equal([("zone.seek", """{"zone":"living","positionNs":120000000000}""")], Sent(before));
        before = _core.All.Count;
        var back = Ok(await _engine.PreviousAsync(Zone, Ct));
        Assert.Equal([("zone.seek", """{"zone":"living","positionNs":60000000000}""")], Sent(before));
        Assert.Equal(("Chapter 1.2", 0L), (back.Part!.Title, back.Part.PositionMs));
        Assert.Equal(1, back.NowPlaying!.Part);

        // Past the last chapter, the next item.
        Ok(await _engine.NextAsync(Zone, Ct));
        before = _core.All.Count;
        var after = Ok(await _engine.NextAsync(Zone, Ct));
        Assert.Equal(("B", queue.Items[2].Id), (Staged()[^1].Title, after.Current));
        Assert.Contains(Sent(before), s => s.Command == "zone.stop");
    }

    [Fact]
    public async Task A_chapter_starts_on_the_clock_and_a_seek_anywhere_in_the_file_is_a_zone_seek()
    {
        var (_, book) = await ChapterBookQueueAsync();
        Ok(await _engine.PlayAsync(Zone, book, Ct));
        var parts = new List<int?>();
        _engine.QueueChanged += (_, state) =>
        {
            lock (parts)
                parts.Add(state.Part?.Index);
        };
        await HeardAsync(Staged()[^1].SourceId);

        // No event from the core: the engine's own clock crosses the start.
        _time.Advance(TimeSpan.FromSeconds(61));
        await Until(() => Queue().Part?.Index == 1);
        lock (parts)
            Assert.Contains(1, parts);
        var queue = Queue();
        Assert.Equal((61_000L, 1_000L, 1), (queue.PositionMs, queue.Part!.PositionMs, queue.NowPlaying!.Part));

        var before = _core.All.Count;
        var sought = Ok(await _engine.SeekAsync(Zone, book, TimeSpan.FromSeconds(150), Ct));
        Assert.Equal(["zone.seek"], Sent(before).Select(s => s.Command));
        Assert.Equal(("zone.seek", """{"zone":"living","positionNs":150000000000}"""), Sent(before)[0]);
        Assert.Equal((2, 150_000L, 30_000L), (sought.Part!.Index, sought.PositionMs, sought.Part.PositionMs));
    }

    [Fact]
    public async Task A_book_of_two_files_with_chapters_is_one_group_and_next_crosses_files_by_a_restage()
    {
        var (queue, book) = await ChapterBookQueueAsync(files: 2);
        Assert.Equal(6, queue.Items[1].Group!.Parts.Count);
        Assert.Equal(TimeSpan.FromMinutes(6), queue.Items[1].Group!.Duration);

        Ok(await _engine.PlayAsync(Zone, book, Ct));
        var first = Staged()[^1];
        Assert.Equal(("The Book — File 1", "fake:t2"), (first.Title, first.TrackId));
        await EmitAsync($$"""{"event":"zone.sourceNeeded","zone":"living","afterSourceId":"{{first.SourceId}}"}""");
        await Until(() => Staged().Count == 2);
        Assert.Equal(("The Book — File 2", "fake:t3"), (Staged()[1].Title, Staged()[1].TrackId));
        await HeardAsync(first.SourceId);

        var before = _core.All.Count;
        Ok(await _engine.NextAsync(Zone, Ct));
        Ok(await _engine.NextAsync(Zone, Ct));
        Assert.Equal(["zone.seek", "zone.seek"], Sent(before).Select(s => s.Command));
        Assert.Equal(2, Queue().Part!.Index);

        // From the last chapter of the first file: its second file, from its start.
        before = _core.All.Count;
        var crossed = Ok(await _engine.NextAsync(Zone, Ct));
        var second = Staged()[^1];
        Assert.Equal(
            [
                ("zone.stop", """{"zone":"living"}"""),
                ("source.clearStaged", """{"zone":"living"}"""),
                ("source.stage", $$"""{"zone":"living","sourceId":"{{second.SourceId}}","mediaUri":"{{second.MediaUri}}","trackId":"fake:t3","title":"The Book — File 2"}"""),
                ("zone.play", """{"zone":"living"}"""),
            ],
            Sent(before));
        Assert.Equal((book, 3, 180_000L), (crossed.Current, crossed.Part!.Index, crossed.PositionMs));
    }
}
