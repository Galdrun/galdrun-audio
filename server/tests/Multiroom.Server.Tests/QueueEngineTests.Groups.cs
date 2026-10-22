using System.Text.Json.Nodes;
using Microsoft.Extensions.Logging.Abstractions;
using Multiroom.Server.Abstractions;
using Multiroom.Server.Playback;
using Multiroom.Server.Playback.Core;

namespace Multiroom.Server.Tests;

// Audiobooks as one item with parts (phase 31): a five-part book of the fake
// provider between two music tracks, against FakeCore. Every part is its own
// staged source; the item's timeline is the parts' end to end.
public sealed partial class QueueEngineTests
{
    private static readonly TimeSpan PartLength = TimeSpan.FromSeconds(60);

    // A, the book, B: the book's parts are t2 to t6, 60 s each unless a
    // length is left out.
    private async Task<(QueueState Queue, QueueItemId Book)> BookQueueAsync(int? unknownPart = null)
    {
        var music = await StartAsync("A", "B");
        QueuePart[] parts = [.. Enumerable.Range(1, 5).Select(i => new QueuePart(
            _fake.AddTrack(_dir.Write($"ch{i}.flac", $"chapter {i}"), $"Chapter {i}") with
            {
                Duration = i - 1 == unknownPart ? null : PartLength,
                MediaType = MediaType.Audiobook,
            }))];
        var book = new QueueGroup("The Book", "Narrator", null, MediaType.Audiobook, "fake:book", parts);
        var queue = Ok(await _engine.AddEntriesAsync(Zone,
            [new QueueEntry(music[0]), new QueueEntry(Group: book), new QueueEntry(music[1])], null, Ct));
        Ok(await _engine.SetClientsAsync(Zone, ["roomA"], Ct));
        return (queue, queue.Items[1].Id);
    }

    // The last source staged is heard: the core started it.
    private async Task HeardAsync(ulong timelineNs = 1000)
    {
        var source = Staged()[^1].SourceId;
        await EmitAsync(
            $$"""{"event":"zone.transport","zone":"living","state":"playing","timelineNs":{{timelineNs}}}""",
            $$"""{"event":"zone.sourceStarted","zone":"living","sourceId":"{{source}}","timelineNs":{{timelineNs}},"serverTimeNs":5000,"startOffsetNs":0}""");
        await Until(() => Queue().NowPlaying?.SourceId == source);
    }

    [Fact]
    public async Task A_book_is_one_item_whose_parts_stage_in_order_and_next_and_previous_step_them()
    {
        var (queue, book) = await BookQueueAsync();
        Assert.Equal(["A", "The Book", "B"], queue.Items.Select(i => i.Title));
        Assert.Equal(TimeSpan.FromMinutes(5), queue.Items[1].Group!.Duration);

        Ok(await _engine.PlayAsync(Zone, book, Ct));
        var first = Staged()[^1];
        Assert.Equal(("The Book — Chapter 1", "fake:t2"), (first.Title, first.TrackId));

        // Gapless: the next part is staged behind on the core's question.
        await EmitAsync($$"""{"event":"zone.sourceNeeded","zone":"living","afterSourceId":"{{first.SourceId}}"}""");
        await Until(() => Staged().Count == 2);
        Assert.Equal(("The Book — Chapter 2", "fake:t3"), (Staged()[1].Title, Staged()[1].TrackId));
        await EmitAsync(
            $$"""{"event":"zone.transport","zone":"living","state":"playing","timelineNs":1000}""",
            $$"""{"event":"zone.sourceStarted","zone":"living","sourceId":"{{first.SourceId}}","timelineNs":1000,"serverTimeNs":5000,"startOffsetNs":0}""");
        await Until(() => Queue().NowPlaying is not null);
        Assert.Equal((book, 0, 5), (Queue().Current, Queue().Part!.Index, Queue().Part!.Count));

        // The core opens part 2 at the boundary: a change of Part, not of Current.
        await EmitAsync($$"""{"event":"zone.sourceStarted","zone":"living","sourceId":"{{Staged()[1].SourceId}}","timelineNs":61000,"serverTimeNs":6000,"startOffsetNs":0}""");
        await Until(() => Queue().Part?.Index == 1);
        Assert.Equal((book, 1, "Chapter 2"), (Queue().Current, Queue().NowPlaying!.Part, Queue().Part!.Title));

        Ok(await _engine.NextAsync(Zone, Ct));
        Assert.Equal("The Book — Chapter 3", Staged()[^1].Title);
        Ok(await _engine.NextAsync(Zone, Ct));
        Ok(await _engine.NextAsync(Zone, Ct));
        Assert.Equal(("The Book — Chapter 5", 4), (Staged()[^1].Title, Queue().Part!.Index));

        // Past the last part, the next item; back from it, the book's last part.
        var next = Ok(await _engine.NextAsync(Zone, Ct));
        Assert.Equal(("B", queue.Items[2].Id), (Staged()[^1].Title, next.Current));
        Assert.Null(next.Part);
        Ok(await _engine.PreviousAsync(Zone, Ct));
        Assert.Equal(("The Book — Chapter 5", book), (Staged()[^1].Title, Queue().Current));

        // Played by name, a book starts at its first part; previous from there leaves it.
        Ok(await _engine.PlayAsync(Zone, book, Ct));
        Assert.Equal("The Book — Chapter 1", Staged()[^1].Title);
        Ok(await _engine.PreviousAsync(Zone, Ct));
        Assert.Equal(("A", queue.Items[0].Id), (Staged()[^1].Title, Queue().Current));
    }

    [Fact]
    public async Task The_timeline_is_the_whole_book_and_a_seek_lands_in_the_part_it_falls_in()
    {
        var (_, book) = await BookQueueAsync();
        Ok(await _engine.PlayAsync(Zone, book, Ct));
        Ok(await _engine.NextAsync(Zone, Ct));
        Ok(await _engine.NextAsync(Zone, Ct));
        await HeardAsync();
        _time.Advance(TimeSpan.FromSeconds(10));

        var queue = Queue();
        Assert.Equal((130_000L, 300_000L), (queue.PositionMs, queue.DurationMs));
        Assert.Equal(new QueuePartState(2, 5, "Chapter 3", 10_000, 60_000), queue.Part);

        // Within the part playing: zone.seek with the position in the part.
        var audible = Staged()[^1].SourceId;
        _core.Answer("zone.seek", p => new JsonObject
        {
            ["zone"] = Zone,
            ["sourceId"] = audible,
            ["positionNs"] = p["positionNs"]!.DeepClone(),
            ["cutTimelineNs"] = 5000,
            ["timelineNs"] = 5150,
        });
        var before = _core.All.Count;
        var sought = Ok(await _engine.SeekAsync(Zone, book, TimeSpan.FromSeconds(150), Ct));
        Assert.Equal([("zone.seek", """{"zone":"living","positionNs":30000000000}""")], Sent(before));
        Assert.Equal((150_000L, 30_000L), (sought.PositionMs, sought.Part!.PositionMs));

        // Into part 4: stop, stage it at the offset in it, play.
        before = _core.All.Count;
        var into = Ok(await _engine.SeekAsync(Zone, book, TimeSpan.FromSeconds(200), Ct));
        var part4 = Staged()[^1];
        Assert.Equal(
            [
                ("zone.stop", """{"zone":"living"}"""),
                ("source.stage", $$"""{"zone":"living","sourceId":"{{part4.SourceId}}","mediaUri":"{{part4.MediaUri}}","trackId":"fake:t5","title":"The Book — Chapter 4","startOffsetNs":20000000000}"""),
                ("zone.play", """{"zone":"living"}"""),
            ],
            Sent(before));
        Assert.Equal((book, 3, 200_000L), (into.Current, into.Part!.Index, into.PositionMs));
    }

    [Fact]
    public async Task A_part_of_unknown_length_refuses_a_seek_across_it_by_name()
    {
        var (_, book) = await BookQueueAsync(unknownPart: 2);
        Assert.Null(Queue().Items[1].Group!.Duration);
        Ok(await _engine.PlayAsync(Zone, book, Ct));
        await HeardAsync();

        var refused = await _engine.SeekAsync(Zone, book, TimeSpan.FromSeconds(150), Ct);
        Assert.Equal(QueueError.Unavailable, refused.Failure.Code);
        Assert.Contains("a part's length is unknown", refused.Failure.Detail, StringComparison.Ordinal);

        // A part whose start is known still is sought into.
        Ok(await _engine.SeekAsync(Zone, book, TimeSpan.FromSeconds(70), Ct));
        Assert.Equal("The Book — Chapter 2", Staged()[^1].Title);
        Assert.Null(Queue().DurationMs);
    }

    [Fact]
    public async Task Repeat_one_replays_the_whole_book_and_shuffle_never_splits_it()
    {
        var (_, book) = await BookQueueAsync();
        Ok(await _engine.SetRepeatAsync(Zone, RepeatMode.One, Ct));
        Ok(await _engine.PlayAsync(Zone, book, Ct));
        for (var i = 0; i < 4; i++)
            Ok(await _engine.NextAsync(Zone, Ct));
        var last = Staged()[^1];
        Assert.Equal("The Book — Chapter 5", last.Title);
        await EmitAsync($$"""{"event":"zone.sourceNeeded","zone":"living","afterSourceId":"{{last.SourceId}}"}""");
        await Until(() => Staged()[^1].SourceId != last.SourceId);
        Assert.Equal("The Book — Chapter 1", Staged()[^1].Title);

        Ok(await _engine.SetRepeatAsync(Zone, RepeatMode.Off, Ct));
        Ok(await _engine.SetShuffleAsync(Zone, true, Ct));
        Ok(await _engine.PlayAsync(Zone, book, Ct));
        var first = Staged()[^1];
        await EmitAsync($$"""{"event":"zone.sourceNeeded","zone":"living","afterSourceId":"{{first.SourceId}}"}""");
        await Until(() => Staged()[^1].SourceId != first.SourceId);
        Assert.Equal("The Book — Chapter 2", Staged()[^1].Title);
        for (var i = 0; i < 5; i++)
            Ok(await _engine.NextAsync(Zone, Ct));
        Assert.True(Staged()[^1].Title is "A" or "B", Staged()[^1].Title);
    }

    [Fact]
    public async Task A_failed_part_is_marked_and_passed_and_a_book_whose_parts_all_failed_is_marked()
    {
        var (_, book) = await BookQueueAsync();
        Ok(await _engine.PlayAsync(Zone, book, Ct));
        var first = Staged()[^1];
        await EmitAsync($$"""{"event":"zone.sourceNeeded","zone":"living","afterSourceId":"{{first.SourceId}}"}""");
        await Until(() => Staged().Count == 2);

        await EmitAsync($$"""{"event":"zone.sourceFailed","zone":"living","sourceId":"{{Staged()[1].SourceId}}","reason":"HTTP 404"}""");
        await Until(() => Staged().Count == 3);
        Assert.Equal("The Book — Chapter 3", Staged()[2].Title);
        var group = Queue().Items[1];
        Assert.Equal([null, "HTTP 404", null, null, null], group.Group!.Parts.Select(p => p.Failure));
        Assert.Null(group.Failure);

        var all = new QueueGroup("Gone", null, null, MediaType.Audiobook, null,
            [new QueuePart(new Track("fake", "nope-1", "x")), new QueuePart(new Track("fake", "nope-2", "y"))]);
        var added = Ok(await _engine.AddEntriesAsync(Zone, [new QueueEntry(Group: all)], null, Ct));
        Assert.Equal(QueueError.Failed, (await _engine.PlayAsync(Zone, added.Items[^1].Id, Ct)).Failure.Code);
        Assert.NotNull(Queue().Items[^1].Group!.Parts[0].Failure);
        Assert.Equal(QueueError.InvalidArgument,
            (await _engine.AddEntriesAsync(Zone, [new QueueEntry(Group: all with { Parts = [] })], null, Ct)).Failure.Code);
        Assert.Equal(QueueError.InvalidArgument, (await _engine.AddEntriesAsync(Zone, [new QueueEntry()], null, Ct)).Failure.Code);
    }

    [Fact]
    public async Task A_restart_of_this_server_resumes_a_book_in_its_part_at_its_offset_and_reads_a_format_3_file()
    {
        var (_, book) = await BookQueueAsync();
        Ok(await _engine.PlayAsync(Zone, book, Ct));
        Ok(await _engine.NextAsync(Zone, Ct));
        Ok(await _engine.NextAsync(Zone, Ct));
        await HeardAsync();
        _time.Advance(TimeSpan.FromSeconds(42));
        var path = Path.Combine(_dir.Path, "playback.json");
        var store = new PlaybackStore(new PlaybackStoreOptions(path, false), _engine, _link, NullLogger<PlaybackStore>.Instance);
        await store.StartAsync(Ct);
        await store.StopAsync(Ct);
        await _link.StopAsync(Ct);
        Assert.Contains("\"version\":10", await File.ReadAllTextAsync(path, Ct), StringComparison.Ordinal);

        _core.StatusBody = FakeCore.EmptyStatus;
        var synchronised = new Synchronised();
        _restartedLink = new CoreLink(new CoreLinkOptions("127.0.0.1", _core.Port), [synchronised],
            NullLogger<CoreLink>.Instance);
        var engine = new QueueEngine(_restartedLink, _providers, _media, NullLogger<QueueEngine>.Instance, _time);
        var restored = new PlaybackStore(new PlaybackStoreOptions(path, false), engine, _restartedLink,
            NullLogger<PlaybackStore>.Instance);
        await restored.StartAsync(Ct);
        var queue = engine.GetQueue(Zone).Value;
        Assert.Equal((book, 2, 162_000L), (queue.Current, queue.Part!.Index, queue.PositionMs));
        Assert.Equal(5, queue.Items[1].Group!.Parts.Count);
        Assert.Equal(MediaType.Audiobook, queue.Items[1].Group!.Parts[0].Track.MediaType);

        await _restartedLink.StartAsync(Ct);
        await synchronised.Next.Task.WaitAsync(Patience, Ct);
        Ok(await engine.PlayAsync(Zone, null, Ct));
        var stage = JsonNode.Parse(_core.On(2).Single(r => r.Command == "source.stage").Params)!;
        Assert.Equal(("The Book — Chapter 3", 42_000_000_000UL),
            (stage["title"]!.GetValue<string>(), stage["startOffsetNs"]!.GetValue<ulong>()));
        await restored.StopAsync(Ct);

        // A format 3 file (no groups, no part) still loads.
        await File.WriteAllTextAsync(path, """
            {"version":3,"zones":[],"rooms":{},"queues":[{"zone":"attic","items":[{"id":{"value":7},
              "track":{"providerId":"fake","providerItemId":"t0","title":"A"},"station":null}],
              "current":{"value":7},"repeat":"off","shuffle":false,"positionNs":5000000000,"playing":false}]}
            """, Ct);
        var older = new QueueEngine(_restartedLink, _providers, _media, NullLogger<QueueEngine>.Instance, _time);
        var reader = new PlaybackStore(new PlaybackStoreOptions(path, false), older, _restartedLink,
            NullLogger<PlaybackStore>.Instance);
        await reader.StartAsync(Ct);
        var attic = older.GetQueue("attic").Value;
        Assert.Equal((new QueueItemId(7), 5_000L, null), (attic.Current, attic.PositionMs, attic.Part));
        await reader.StopAsync(Ct);
    }

    [Fact]
    public async Task The_playback_settings_are_kept_in_playback_json()
    {
        await StartAsync("A");
        var path = Path.Combine(_dir.Path, "playback.json");
        var settings = new PlaybackSettings();
        var store = new PlaybackStore(new PlaybackStoreOptions(path, false), _engine, _link, NullLogger<PlaybackStore>.Instance,
            settings: settings);
        await store.StartAsync(Ct);
        settings.Set(groupBooks: false, resumeBooks: true);
        await store.StopAsync(Ct);
        Assert.Contains("\"version\":10", await File.ReadAllTextAsync(path, Ct), StringComparison.Ordinal);

        var restored = new PlaybackSettings();
        var again = new PlaybackStore(new PlaybackStoreOptions(path, false), _engine, _link, NullLogger<PlaybackStore>.Instance,
            settings: restored);
        await again.StartAsync(Ct);
        Assert.Equal((false, true), (restored.GroupBooks, restored.ResumeBooks));
        await again.StopAsync(Ct);
    }

    [Fact]
    public async Task AutoFill_adds_nothing_to_a_queue_that_ends_in_a_book_and_says_why()
    {
        var (_, book) = await BookQueueAsync();
        Ok(await _engine.RemoveAsync(Zone, Queue().Items[2].Id, Ct));
        var chooser = new Counting();
        var autoFill = new AutoFill(_engine, chooser, NullLogger<AutoFill>.Instance);
        await autoFill.StartAsync(Ct);
        Assert.True(autoFill.Set(Zone, new AutoFillPatch(Enabled: true, Threshold: 3)).IsSuccess);

        Ok(await _engine.PlayAsync(Zone, book, Ct));
        await HeardAsync();
        await Until(() => autoFill.View(Zone).Last is not null);

        // A and the book are the last items: A is music, so it seeds.
        Assert.Equal(1, chooser.Calls);
        Assert.Equal(["A"], chooser.Seen.Select(t => t.Title));

        Ok(await _engine.RemoveAsync(Zone, Queue().Items[0].Id, Ct));
        await Until(() => autoFill.View(Zone).Last?.Reason?.Contains("hold no music", StringComparison.Ordinal) == true);
        Assert.Equal(1, chooser.Calls);
        Assert.Equal(0, autoFill.View(Zone).Last!.Added);
        await autoFill.StopAsync(Ct);
    }

    // Offers a book's chapter and a song: only the song may go in.
    private sealed class Counting : IAutoFillChooser
    {
        public int Calls { get; private set; }

        public IReadOnlyList<Track> Seen { get; private set; } = [];

        public Task<AutoFillChoice> ChooseAsync(string zone, IReadOnlyList<Track> queue, AutoFillSettings settings,
            CancellationToken cancellationToken)
        {
            Calls++;
            Seen = queue;
            return Task.FromResult(new AutoFillChoice([], settings.Source, Reason: "none"));
        }
    }
}
