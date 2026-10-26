using System.Text.Json.Nodes;
using Microsoft.Extensions.Logging.Abstractions;
using Multiroom.Server.Abstractions;
using Multiroom.Server.Library;
using Multiroom.Server.Playback;

namespace Multiroom.Server.Tests;

// Where a listener stopped in a book (phase 33), against FakeCore: always
// saved, applied when the house resumes books, and a finished book starts
// again from its beginning.
public sealed partial class QueueEngineTests
{
    // The library's database with the positions and the favourites, the
    // recorder started on the engine, and a player that queues a favourite
    // book with its saved place.
    private async Task<(BookPositionStore Positions, PlaybackSettings Settings, PlaylistPlayer Player, BookPositions Recorder,
        PlaylistStore Store)> PositionsAsync()
    {
        var database = new LibraryDatabase(Path.Combine(_dir.Path, "library.db"));
        Assert.True(database.Migrate().IsSuccess);
        var positions = new BookPositionStore(database);
        var settings = new PlaybackSettings();
        var store = new PlaylistStore(database);
        var player = new PlaylistPlayer(store, _engine, _providers, settings, positions);
        var recorder = new BookPositions(_engine, positions, NullLogger<BookPositions>.Instance, _time);
        await recorder.StartAsync(Ct);
        return (positions, settings, player, recorder, store);
    }

    private static BookPosition? Saved(BookPositionStore positions) => positions.Get("fake:book").Value;

    // Queued from its favourite, as any path queues a book: the new item.
    private static async Task<QueueItem> QueueBookAsync(PlaylistPlayer player, long favourite, bool fromStart = false)
    {
        var queue = Ok(await player.EnqueueFavoriteAsync(Zone, favourite, QueuePlacement.End, Ct, fromStart));
        return queue.Items[^1];
    }

    [Fact]
    public void A_listener_has_a_place_of_their_own_and_reads_the_households_where_they_have_none()
    {
        var database = new LibraryDatabase(Path.Combine(_dir.Path, "library.db"));
        Assert.True(database.Migrate().IsSuccess);
        var positions = new BookPositionStore(database);
        Assert.True(new UserStore(database).Add("Alice").IsSuccess);
        var at = DateTimeOffset.UnixEpoch;
        BookPosition Place(string reference, long offset, string user) =>
            new(reference, "The Book", null, 0, null, offset, offset, null, false, at = at.AddMinutes(1), user);

        Assert.True(positions.Save(Place("fake:book", 10, "")).IsSuccess);
        Assert.Equal(10, positions.Get("fake:book", "Alice").Value!.OffsetMs);
        Assert.True(positions.Save(Place("fake:book", 20, "Alice")).IsSuccess);
        Assert.True(positions.Save(Place("fake:other", 30, "")).IsSuccess);
        Assert.Equal((10L, 20L), (positions.Get("fake:book").Value!.OffsetMs, positions.Get("fake:book", "Alice").Value!.OffsetMs));
        Assert.Equal([("fake:other", ""), ("fake:book", "Alice")],
            positions.Page(0, 10, "Alice").Value.Items.Select(p => (p.Reference, p.User)));
        Assert.Equal(3, positions.Page(0, 10, UserStore.Everyone).Value.Total);

        // A user unknown is saved as the household.
        Assert.True(positions.Save(Place("fake:third", 40, "Nobody")).IsSuccess);
        Assert.Equal("", positions.Get("fake:third").Value!.User);

        Assert.True(positions.Delete("fake:book", "Alice").IsSuccess);
        Assert.Equal(10, positions.Get("fake:book", "Alice").Value!.OffsetMs);
    }

    [Fact]
    public async Task A_book_s_place_is_always_saved_and_resumed_only_when_the_house_says_so()
    {
        var (positions, settings, player, recorder, store) = await PositionsAsync();
        var (queue, book) = await BookQueueAsync();
        var favourite = store.AddFavorite(queue.Items[1].Group!).Value.Id;

        // Paused in part 3 at 0:50: saved, the setting off.
        Ok(await _engine.PlayAsync(Zone, book, Ct));
        Ok(await _engine.NextAsync(Zone, Ct));
        Ok(await _engine.NextAsync(Zone, Ct));
        await HeardAsync();
        _time.Advance(TimeSpan.FromSeconds(50));
        Ok(await _engine.PauseAsync(Zone, Ct));
        await Until(() => Saved(positions) is { Part: 2, OffsetMs: 50_000 });
        Assert.Equal((170_000L, 300_000L, false, "fake:t4"), (Saved(positions)!.PositionMs, Saved(positions)!.DurationMs,
            Saved(positions)!.Finished, Saved(positions)!.PartReference));

        // Removed: left there, and kept.
        Ok(await _engine.RemoveAsync(Zone, book, Ct));
        Assert.Equal((2, 50_000L), (Saved(positions)!.Part, Saved(positions)!.OffsetMs));

        // Queued again with the setting off: answered, not used.
        var off = await QueueBookAsync(player, favourite);
        Assert.Equal((new BookMark(2, 50_000, 170_000), false), (off.Group!.Saved, off.Group.Resume));
        Ok(await _engine.PlayAsync(Zone, off.Id, Ct));
        Assert.Equal("The Book — Chapter 1", Staged()[^1].Title);

        // With it on: part 3 at 0:50.
        settings.Set(groupBooks: null, resumeBooks: true);
        var on = await QueueBookAsync(player, favourite);
        Assert.True(on.Group!.Resume);
        Ok(await _engine.PlayAsync(Zone, on.Id, Ct));
        var stage = JsonNode.Parse(_core.All.Last(r => r.Command == "source.stage").Params)!;
        Assert.Equal(("The Book — Chapter 3", 50_000_000_000UL),
            (stage["title"]!.GetValue<string>(), stage["startOffsetNs"]!.GetValue<ulong>()));

        // fromStart begins at part 1, queued so or played so, and the place
        // is overwritten as it plays.
        var fresh = await QueueBookAsync(player, favourite, fromStart: true);
        Assert.False(fresh.Group!.Resume);
        var resumable = await QueueBookAsync(player, favourite);
        Ok(await _engine.PlayAsync(Zone, resumable.Id, Ct, fromStart: true));
        Assert.Equal("The Book — Chapter 1", Staged()[^1].Title);
        await HeardAsync();
        _time.Advance(TimeSpan.FromSeconds(5));
        Ok(await _engine.PauseAsync(Zone, Ct));
        await Until(() => Saved(positions) is { Part: 0, OffsetMs: 5_000 });
        await recorder.StopAsync(Ct);
    }

    [Fact]
    public async Task A_book_played_to_its_end_is_finished_and_starts_again_from_its_beginning()
    {
        var (positions, settings, player, recorder, store) = await PositionsAsync();
        settings.Set(groupBooks: null, resumeBooks: true);
        var (queue, book) = await BookQueueAsync();
        var favourite = store.AddFavorite(queue.Items[1].Group!).Value.Id;

        Ok(await _engine.PlayAsync(Zone, book, Ct));
        for (var i = 0; i < 4; i++)
            Ok(await _engine.NextAsync(Zone, Ct));
        await HeardAsync();
        _time.Advance(TimeSpan.FromSeconds(45));
        Ok(await _engine.NextAsync(Zone, Ct));
        await Until(() => Saved(positions) is { Finished: true });
        Assert.Equal((0, 0L, null), (Saved(positions)!.Part, Saved(positions)!.OffsetMs, Saved(positions)!.PositionMs));

        var again = await QueueBookAsync(player, favourite);
        Assert.Equal((null, false), (again.Group!.Saved, again.Group.Resume));

        // Kept in the database: another store over it reads the same.
        Assert.True(new BookPositionStore(new LibraryDatabase(Path.Combine(_dir.Path, "library.db"))).Get("fake:book").Value!.Finished);
        await recorder.StopAsync(Ct);
    }

    [Fact]
    public void The_newest_thousand_books_are_kept()
    {
        var database = new LibraryDatabase(Path.Combine(_dir.Path, "library.db"));
        Assert.True(database.Migrate().IsSuccess);
        var positions = new BookPositionStore(database);
        var at = new DateTimeOffset(2026, 10, 1, 0, 0, 0, TimeSpan.Zero);
        for (var i = 0; i <= BookPositionStore.MaxRows; i++)
        {
            Assert.True(positions.Save(new BookPosition($"fake:book{i}", $"Book {i}", null, 0, null, i, null, null, false,
                at.AddSeconds(i))).IsSuccess);
        }
        var page = positions.Page(0, 2).Value;
        Assert.Equal((BookPositionStore.MaxRows, "fake:book1000"), (page.Total, page.Items[0].Reference));
        Assert.Null(positions.Get("fake:book0").Value);
        Assert.NotNull(positions.Get("fake:book1").Value);
        Assert.True(positions.Delete("fake:book1").Value);
        Assert.Equal(LibraryError.NotFound, positions.Delete("fake:book1").Failure.Code);
    }
}
