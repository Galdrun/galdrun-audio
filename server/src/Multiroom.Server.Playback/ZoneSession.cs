using Microsoft.Extensions.Logging;
using Multiroom.Server.Abstractions;
using Multiroom.Server.Library;
using Multiroom.Server.Playback.Core;
using Multiroom.Server.Providers;

namespace Multiroom.Server.Playback;

// One zone's queue, and what the core holds of it: the sources it has opened
// and the one source staged ahead of them. Every operation and every core
// event runs under one gate, one at a time, so an event that was on the wire
// while an operation ran is handled after it -- which is why an event naming
// a source this session no longer holds is ignored rather than trusted.
//
// The staging loop: the core asks with zone.sourceNeeded after it opens a
// source with nothing behind it; the session resolves the next item, publishes
// it and stages it. One source is staged ahead, never more. A queue edit that
// changes what comes next clears the staged source and stages the new next.
//
// Skipping is stop, clear, stage and play: the core has no zone.skip yet.
// StartAtAsync is the one place that shape lives.
//
// A group (phase 31) is one item with parts: each part is its own staged
// source, so a position is a Spot (the item and its part) plus an offset in
// that part. Automatic advance and a caller's next and previous step parts
// within a group before they leave it; repeat one repeats the whole group,
// and shuffle picks among items, never parts.
//
// A part may be a chapter of a file (phase 32). The chapters of one file are
// a run of neighbouring parts staged once, as one source; next and previous
// within it are a zone.seek, and the chapter audible is worked out from the
// playhead, which is kept on the file's own timeline. A timer on
// QueueEngine.Time publishes the change of part when a chapter starts.
#pragma warning disable CA1001 // The gate is a SemaphoreSlim whose AvailableWaitHandle is never read, so it holds no handle to release; and a deleted session may still have callers waiting on it. The chapter timer is disposed when the session is deleted.
internal sealed partial class ZoneSession(string zone, QueueEngine engine)
#pragma warning restore CA1001
{
    private readonly SemaphoreSlim _gate = new(1, 1);
    private readonly List<QueueItem> _items = [];

    // Sources of this queue the core has opened, oldest first. The first is
    // audible; the last is the one staging continues after.
    private readonly List<Staging> _opened = [];

    // Shuffle: the items played, in order, so previous can walk back. Those
    // from _cycleStart on are the ones played in this cycle, which the next
    // pick avoids.
    private readonly List<QueueItemId> _history = [];
    private int _cycleStart;
    private (QueueItemId From, QueueItemId Pick)? _shufflePick;

    private Staging? _staged;
    private QueueItemId? _current;
    // The part of _current (a group's), 0 for a track or a station.
    private int _currentPart;
    private NowPlaying? _nowPlaying;
    private QueueTransport _transport;
    private RepeatMode _repeat;
    private bool _shuffle;
    private bool _zoneCreated;
    private bool _deleted;
    private volatile QueueState _state = new(zone, [], null, RepeatMode.Off, false, QueueTransport.Stopped, null);

    // Where the zone is in which item, best effort: from sourceStarted, the
    // transport events, a seek's answer and `status`, and run on by the clock
    // while the zone plays. A core that restarted is resumed from it, and a
    // stopped queue plays on from it.
    private volatile Playhead? _playhead;

    // Restored with auto-resume: played once the core is first in step.
    private bool _resumeOnSync;

    // Fires when the playhead reaches the next chapter's start in its file.
    private ITimer? _chapterTimer;

    // An item and its part: 0 for a track or a station.
    private readonly record struct Spot(QueueItemId Item, int Part);

    // Retune: the station's second try after its source failed, until it is
    // heard. It gets no third.
    private sealed record Staging(string SourceId, QueueItemId Item, int Part, Uri MediaUri,
        bool Retune = false)
    {
        public Spot Spot => new(Item, Part);
    }

    // PositionNs into Part of Item at the time provider's timestamp At,
    // running on from there while Running. For a chapter it is on its file's
    // timeline, and Part is any chapter of that file: Located says which.
    private sealed record Playhead(QueueItemId Item, int Part, ulong PositionNs, long At, bool Running);

    public string Zone => zone;

    public QueueState State => WithPosition(_state);

    // What is kept across a restart of this server. Read without the gate:
    // the two fields it reads are each published whole.
    public SavedQueueState Saved
    {
        get
        {
            var state = _state;
            var playhead = _playhead;
            var held = playhead is not null && playhead.Item == state.Current;
            var (part, offset) = held ? Located(state.Items.FirstOrDefault(i => i.Id == playhead!.Item), playhead!) : default;
            return new SavedQueueState(zone, state.Items, state.Current, state.Repeat, state.Shuffle,
                held ? offset : null,
                state.Transport is QueueTransport.Starting or QueueTransport.Playing,
                held ? part : state.Part?.Index ?? 0);
        }
    }

    // Before the session is shared: at startup, before CoreLink connects.
    public void Restore(SavedQueueState saved, bool resume)
    {
        _items.AddRange(saved.Items);
        _current = saved.Current is { } c && IndexOf(c) >= 0 ? c : null;
        _currentPart = _current is { } at && saved.Part >= 0 && saved.Part < PartCount(ItemOf(at)!) ? saved.Part : 0;
        _repeat = saved.Repeat;
        _shuffle = saved.Shuffle;
        if (_shuffle && _current is { } current)
            _history.Add(current);
        _playhead = _current is { } item && saved.PositionNs is { } position
            ? new Playhead(item, _currentPart, FileStartNs(ItemOf(item)!, _currentPart) + position,
                engine.Time.GetTimestamp(), Running: false)
            : null;
        _resumeOnSync = resume && saved.Playing && _current is not null;
        _state = Snapshot();
    }

    // One operation under the gate. Past the wait it is not cancelled: a
    // command half-sent would leave this session and the core disagreeing.
    public async Task<Result<QueueState, QueueError>> RunAsync(Func<Task<Failure<QueueError>?>> operation,
        CancellationToken cancellationToken)
    {
        await _gate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            if (_deleted)
                return Result.Fail(QueueError.NotFound, $"zone {zone} has been deleted");
            Follow();
            var failure = await operation().ConfigureAwait(false);
            Follow();
            var previous = _state;
            _state = Snapshot();
            if (!_deleted && !Same(previous, _state))
                engine.OnQueueChanged(WithPosition(_state));
            ArmChapterTimer();
            return failure is { } f ? f : WithPosition(_state);
        }
        finally
        {
            _gate.Release();
        }
    }

    // --- the queue ----------------------------------------------------------

    public async Task<Failure<QueueError>?> AddAsync(IEnumerable<QueueEntry> entries, int? index)
    {
        if (index is { } at && (at < 0 || at > _items.Count))
            return Result.Fail(QueueError.InvalidArgument, $"index {at} is outside 0..{_items.Count}");
        _items.InsertRange(index ?? _items.Count, entries.Select(ItemOf));
        await RestageAsync().ConfigureAwait(false);
        return null;
    }

    public Task<Failure<QueueError>?> AddNextAsync(IEnumerable<QueueEntry> entries) =>
        AddAsync(entries, _current is { } current ? IndexOf(current) + 1 : 0);

    // Everything goes, stopped, and these take its place: "play now" is this
    // and then play. Repeat and shuffle are the queue's and stay.
    public async Task<Failure<QueueError>?> ReplaceAsync(IEnumerable<QueueEntry> entries)
    {
        var stopped = await StopCoreAsync().ConfigureAwait(false);
        if (stopped is not null)
            return stopped;
        // Stopped, as the core answered: a play right after ("play now") must
        // not wait for its zone.transport to say so.
        _transport = QueueTransport.Stopped;
        _playhead = null;
        SetCurrent(null);
        _history.Clear();
        _cycleStart = 0;
        _shufflePick = null;
        _items.Clear();
        _items.AddRange(entries.Select(ItemOf));
        return null;
    }

    public async Task<Failure<QueueError>?> RemoveAsync(QueueItemId item)
    {
        var index = IndexOf(item);
        if (index < 0)
            return NoItem(item);

        // The core has opened it: it is audible, or about to be. Playback moves
        // on to what would have come after it.
        if (_opened.Any(s => s.Item == item))
        {
            var target = UserNextItem(item);
            _items.RemoveAt(index);
            _history.Remove(item);
            if (target is { } next && next != item)
                return await StartAtAsync(new Spot(next, FirstPart(ItemOf(next)!)), 0).ConfigureAwait(false);
            SetCurrent(null);
            return await StopCoreAsync().ConfigureAwait(false);
        }

        _items.RemoveAt(index);
        _history.Remove(item);
        if (_current == item)
            SetCurrent(index < _items.Count ? _items[index].Id : null);
        await RestageAsync().ConfigureAwait(false);
        return null;
    }

    public async Task<Failure<QueueError>?> MoveAsync(QueueItemId item, int index)
    {
        var from = IndexOf(item);
        if (from < 0)
            return NoItem(item);
        if (index < 0 || index >= _items.Count)
            return Result.Fail(QueueError.InvalidArgument, $"index {index} is outside 0..{_items.Count - 1}");
        var moved = _items[from];
        _items.RemoveAt(from);
        _items.Insert(index, moved);
        await RestageAsync().ConfigureAwait(false);
        return null;
    }

    public async Task<Failure<QueueError>?> SetRepeatAsync(RepeatMode repeat)
    {
        _repeat = repeat;
        await RestageAsync().ConfigureAwait(false);
        return null;
    }

    public async Task<Failure<QueueError>?> SetShuffleAsync(bool shuffle)
    {
        if (shuffle == _shuffle)
            return null;
        _shuffle = shuffle;
        _history.Clear();
        _cycleStart = 0;
        _shufflePick = null;
        if (shuffle && _current is { } current)
            _history.Add(current);
        await RestageAsync().ConfigureAwait(false);
        return null;
    }

    // --- transport ----------------------------------------------------------

    public async Task<Failure<QueueError>?> PlayAsync(QueueItemId? item, bool fromStart = false)
    {
        if (item is { } named)
        {
            if (ItemOf(named) is not { } chosen)
                return NoItem(named);
            var (from, at) = !fromStart && SavedPlace(chosen) is { } saved
                ? saved
                : (new Spot(named, FirstPart(chosen)), 0UL);
            return await StartAtAsync(from, at).ConfigureAwait(false);
        }

        if (_transport == QueueTransport.Paused)
        {
            var resumed = await SendAsync(new ZonePlay(zone)).ConfigureAwait(false);
            if (!resumed.IsSuccess)
                return resumed.Failure;
            _transport = Mirror(resumed.Value.Transport);
            Anchor(null, running: true);
            return null;
        }
        if (_transport is QueueTransport.Starting or QueueTransport.Playing)
            return null;

        // From where it stopped, when it did not stop by a caller's stop.
        var start = _current ?? _items.FirstOrDefault()?.Id;
        if (start is not { } first)
            return Result.Fail(QueueError.NotFound, $"the queue of zone {zone} is empty");
        var (spot, offset) = ResumeAt(first);
        return await StartAtAsync(spot, offset).ConfigureAwait(false);
    }

    public async Task<Failure<QueueError>?> PauseAsync()
    {
        var paused = await SendAsync(new ZonePause(zone)).ConfigureAwait(false);
        if (!paused.IsSuccess)
            return paused.Failure;
        _transport = QueueTransport.Paused;
        Anchor(paused.Value.TimelineNs, running: false);
        return null;
    }

    // A caller's stop: the next play starts the current item from its start.
    public async Task<Failure<QueueError>?> StopAsync()
    {
        var failure = await StopCoreAsync().ConfigureAwait(false);
        if (failure is null)
            _playhead = null;
        return failure;
    }

    public async Task<Failure<QueueError>?> NextAsync()
    {
        if (_current is not { } current)
            return Result.Fail(QueueError.NotFound, $"nothing is current in zone {zone}");
        if (UserNext(new Spot(current, _currentPart)) is not { } next)
            return Result.Fail(QueueError.NotFound, $"nothing comes after item {current} in zone {zone}");
        return InAudibleFile(next)
            ? await SeekInPlaceAsync(next, 0).ConfigureAwait(false)
            : await StartAtAsync(next, 0).ConfigureAwait(false);
    }

    public async Task<Failure<QueueError>?> PreviousAsync()
    {
        if (_current is not { } current)
            return Result.Fail(QueueError.NotFound, $"nothing is current in zone {zone}");
        if (UserPrevious(new Spot(current, _currentPart)) is not { } previous)
            return Result.Fail(QueueError.NotFound, $"nothing comes before item {current} in zone {zone}");
        return InAudibleFile(previous)
            ? await SeekInPlaceAsync(previous, 0).ConfigureAwait(false)
            : await StartAtAsync(previous, 0).ConfigureAwait(false);
    }

    // The item (part) audible, or another chapter of the file audible:
    // zone.seek, in place, which the core does without a stop. Any other
    // item or part: it starts at the offset. In a group the offset is on the
    // whole timeline, mapped to a part; a group with a part of unknown length
    // can be sought only within the part playing.
    public async Task<Failure<QueueError>?> SeekAsync(QueueItemId item, TimeSpan offset)
    {
        if (offset < TimeSpan.Zero)
            return Result.Fail(QueueError.InvalidArgument, $"offset {offset} is negative");
        var index = IndexOf(item);
        if (index < 0)
            return NoItem(item);
        var target = _items[index];
        if (target.Station is not null)
            return Result.Fail(QueueError.Unavailable, $"{target.Title} is radio, which is never seekable");

        var positionNs = checked((ulong)offset.Ticks * 100);
        var part = 0;
        if (target.Group is { } book)
        {
            var audible = _nowPlaying?.Item == item ? _nowPlaying.Part : null;
            if (PartAt(book, positionNs, audible) is not { } mapped)
            {
                return Result.Fail(QueueError.Unavailable,
                    $"{target.Title}: a part's length is unknown, so a seek lands only within the part playing");
            }
            (part, positionNs) = mapped;
        }
        var spot = new Spot(item, part);
        var heard = _nowPlaying?.Item == item && (_nowPlaying.Part ?? 0) == part &&
            _transport is QueueTransport.Playing or QueueTransport.Paused;
        return heard || InAudibleFile(spot)
            ? await SeekInPlaceAsync(spot, positionNs).ConfigureAwait(false)
            : await StartAtAsync(spot, positionNs).ConfigureAwait(false);
    }

    // zone.seek within the source audible, to a part (a chapter of its file)
    // and the offset in it.
    private async Task<Failure<QueueError>?> SeekInPlaceAsync(Spot spot, ulong offsetNs)
    {
        var fileNs = FileStartNs(ItemOf(spot.Item)!, spot.Part) + offsetNs;
        var sought = await SendAsync(new ZoneSeek(zone, fileNs)).ConfigureAwait(false);
        if (!sought.IsSuccess)
            return sought.Failure;
        _nowPlaying = _nowPlaying! with
        {
            SourceId = sought.Value.SourceId,
            TimelineNs = sought.Value.TimelineNs,
            ServerTimeNs = null,
            StartOffsetNs = fileNs,
            Part = _nowPlaying.Part is null ? null : spot.Part,
        };
        _playhead = new Playhead(spot.Item, spot.Part, fileNs, engine.Time.GetTimestamp(),
            _transport == QueueTransport.Playing);
        SetCurrent(spot.Item, spot.Part);
        return null;
    }

    // Another chapter of the file audible (phase 32), while it plays or is
    // paused: reached by a seek, not a restage.
    private bool InAudibleFile(Spot spot) =>
        _nowPlaying is { Part: { } heard } now && now.Item == spot.Item && heard != spot.Part &&
        _transport is QueueTransport.Playing or QueueTransport.Paused &&
        ItemOf(spot.Item)?.Group is { } book && book.Parts[spot.Part].Chapter is not null &&
        RunOf(book, heard) == RunOf(book, spot.Part);

    public async Task<Failure<QueueError>?> SetClientsAsync(IReadOnlyList<string> clients)
    {
        if (await EnsureZoneAsync().ConfigureAwait(false) is { } failure)
            return failure;
        var set = await SendAsync(new ZoneSetClients(zone, clients)).ConfigureAwait(false);
        return set.IsSuccess ? null : set.Failure;
    }

    public async Task<Failure<QueueError>?> DeleteAsync()
    {
        if (_zoneCreated || engine.Link.Model.Current.Zones.ContainsKey(zone))
        {
            // A zone the core no longer has is as deleted as it gets.
            var deleted = await SendAsync(new ZoneDelete(zone)).ConfigureAwait(false);
            if (!deleted.IsSuccess && deleted.Failure.Code != QueueError.NotFound)
                return deleted.Failure;
        }
        // Deleting the zone ends every source of it in the core.
        Forget();
        _deleted = true;
        return null;
    }

    // --- core events --------------------------------------------------------

    public async Task<Failure<QueueError>?> OnEventAsync(CoreEvent coreEvent)
    {
        switch (coreEvent)
        {
            case SourceNeeded e:
                if (_staged?.SourceId == e.AfterSourceId)
                    Opened();
                if (_opened.Count > 0 && _opened[^1].SourceId == e.AfterSourceId)
                    await StageNextAsync().ConfigureAwait(false);
                break;

            case SourceStarted e:
                if (_staged?.SourceId == e.SourceId)
                    Opened();
                var started = _opened.FindIndex(s => s.SourceId == e.SourceId);
                if (started >= 0)
                {
                    Started(started, new NowPlaying(_opened[started].Item, e.SourceId, e.TimelineNs, e.ServerTimeNs,
                        e.StartOffsetNs, PartOf(_opened[started])));
                }
                break;

            case SourceFailed e:
                await FailedAsync(e.SourceId, e.Reason).ConfigureAwait(false);
                break;

            case ZoneEnded e:
                if (_staged is null && _opened.Count > 0 && _opened[^1].SourceId == e.AfterSourceId)
                    await EndedAsync().ConfigureAwait(false);
                break;

            case ZoneTransport e:
                _transport = Mirror(e.State);
                if (e.State != TransportState.Starting)
                    Anchor(e.TimelineNs, running: e.State == TransportState.Playing);
                if (e.State == TransportState.Stopped)
                    _nowPlaying = null;
                break;
        }
        return null;
    }

    // After the events that could have said otherwise were lost, or after a
    // reconnect: the model is the fact, read from one `status`.
    public async Task<Failure<QueueError>?> ReconcileAsync(CoreZone? core)
    {
        var wasPlaying = _transport is QueueTransport.Starting or QueueTransport.Playing;
        if (core is not null)
            _transport = Mirror(core.Transport);

        // Restored playing, with auto-resume on: the first sync starts it.
        if (_resumeOnSync)
        {
            _resumeOnSync = false;
            if (_opened.Count == 0 && _staged is null && _current is { } restored)
                return await ResumeAsync(restored).ConfigureAwait(false);
        }
        if (_opened.Count == 0 && _staged is null)
            return null;

        // Gone, stopped or failed: the core holds nothing of this queue now,
        // and the position is kept. Idle behind sources it had opened is a
        // core that restarted: its zone was created again by the reconnect,
        // with its rooms, and a queue that was playing plays on from there.
        if (core is null || core.State is ZoneState.Idle or ZoneState.Failed)
        {
            LogLost(engine.Log, zone, core?.State.ToString() ?? "gone");
            Anchor(null, running: false);
            Forget();
            if (wasPlaying && core is { State: ZoneState.Idle } && _current is { } current)
                return await ResumeAsync(current).ConfigureAwait(false);
            return null;
        }

        // A staged source the core no longer lists failed, or was opened: the
        // core drops one no other way than those and source.clearStaged, which
        // is this session's own.
        if (_staged is { } staged && core.Staged.All(s => s.SourceId != staged.SourceId))
        {
            if (core.FailedSources.FirstOrDefault(f => f.SourceId == staged.SourceId) is { } failed)
            {
                engine.Media.Retire(staged.MediaUri);
                MarkFailed(staged.Spot, failed.Reason);
                _staged = null;
            }
            else
            {
                Opened();
            }
        }

        if (core.Source is { Ended: false } source && _nowPlaying?.SourceId != source.SourceId &&
            _opened.FindIndex(s => s.SourceId == source.SourceId) is var audible and >= 0)
        {
            Started(audible, new NowPlaying(_opened[audible].Item, source.SourceId, source.TimelineStartNs, null,
                source.StartOffsetNs ?? 0, PartOf(_opened[audible])));
        }

        // The playhead as `status` has it: the audible source's offset plus
        // how far the timeline has run since it started.
        if (core.Source is { Ended: false, TimelineStartNs: { } from } audibleSource && core.TimelineNs is { } at &&
            at >= from && _opened.Count > 0 && _opened[0].SourceId == audibleSource.SourceId)
        {
            _playhead = new Playhead(_opened[0].Item, _opened[0].Part, (audibleSource.StartOffsetNs ?? 0) + (at - from),
                engine.Time.GetTimestamp(), core.Transport == TransportState.Playing);
        }

        if (core.State == ZoneState.Ended)
            await EndedAsync().ConfigureAwait(false);
        else
            await StageNextAsync().ConfigureAwait(false);
        return null;
    }

    // --- staging ------------------------------------------------------------

    // Stop, clear, stage at the offset, play: the only way into a new item
    // or part, and for now the only way to skip.
    private async Task<Failure<QueueError>?> StartAtAsync(Spot spot, ulong offsetNs)
    {
        if (await EnsureZoneAsync().ConfigureAwait(false) is { } unmade)
            return unmade;
        if (await StopCoreAsync().ConfigureAwait(false) is { } unstopped)
            return unstopped;

        var item = spot.Item;
        ClearFailure(spot);
        var fileNs = FileStartNs(ItemOf(item)!, spot.Part) + offsetNs;
        var staged = await StageAsync(ItemOf(item)!, spot.Part, fileNs).ConfigureAwait(false);
        if (!staged.IsSuccess)
            return staged.Failure;

        // Synchronous in the core: it answers once the source is open, which
        // takes GETs the media endpoint serves on its own threads.
        var played = await SendAsync(new ZonePlay(zone)).ConfigureAwait(false);
        if (!played.IsSuccess)
        {
            engine.Media.Retire(staged.Value.MediaUri);
            if (played.Failure.Code == QueueError.Failed)
                MarkFailed(spot, played.Failure.Detail);
            return played.Failure;
        }

        _opened.Add(staged.Value);
        SetCurrent(item, spot.Part);
        _nowPlaying = null;
        _transport = Mirror(played.Value.Transport);
        // Not audible yet: it runs from sourceStarted.
        _playhead = new Playhead(item, spot.Part, fileNs, engine.Time.GetTimestamp(), Running: false);
        // A saved place is used once: from here the playhead says where it is.
        if (ItemOf(item)?.Group is { Resume: true } book)
            _items[IndexOf(item)] = _items[IndexOf(item)] with { Group = book with { Resume = false } };
        return null;
    }

    // The item again, from the playhead where it is that item's (its part
    // and offset). Radio is never seekable, and starts where the station is.
    private async Task<Failure<QueueError>?> ResumeAsync(QueueItemId item)
    {
        var (spot, offset) = ResumeAt(item);
        var title = ItemOf(item)!.Title;
        var failure = await StartAtAsync(spot, offset).ConfigureAwait(false);
        if (failure is { } f)
        {
            var reason = f.ToString();
            LogResumeFailed(engine.Log, zone, reason);
        }
        else
        {
            var position = TimeSpan.FromTicks((long)(offset / 100));
            LogResumed(engine.Log, zone, title, position);
        }
        return failure;
    }

    // From the playhead where it is that item's (its part and offset), else
    // from a book's saved place (phase 33), else from the start.
    private (Spot Spot, ulong OffsetNs) ResumeAt(QueueItemId item)
    {
        var found = ItemOf(item)!;
        if (_playhead is { } playhead && playhead.Item == item && found.Station is null)
        {
            var (part, offset) = Located(found, playhead);
            return (new Spot(item, part), offset);
        }
        return SavedPlace(found) ?? (new Spot(item, FirstPart(found)), 0);
    }

    // A book queued to resume at its saved place, while that is a part of it.
    private static (Spot Spot, ulong OffsetNs)? SavedPlace(QueueItem item) =>
        item.Group is { Resume: true, Saved: { } mark } book && mark.Part >= 0 && mark.Part < book.Parts.Count &&
        mark.OffsetMs >= 0
            ? (new Spot(item.Id, mark.Part), (ulong)mark.OffsetMs * 1_000_000)
            : null;

    // The state with the playhead's position in its current item (in a group:
    // on the whole timeline, and in the part, the chapter its position falls
    // in), now. Kept out of _state, so a position that ran on is no change.
    // Read without the gate, so only from the state's own items.
    private QueueState WithPosition(QueueState state)
    {
        if (_playhead is not { } playhead || playhead.Item != state.Current)
            return state;
        var item = state.Items.FirstOrDefault(i => i.Id == playhead.Item);
        var (part, inPart) = Located(item, playhead);
        if (state.Part is null || item?.Group is not { } book || part >= book.Parts.Count)
            return state with { PositionMs = (long)(inPart / 1_000_000) };
        var before = PartStartNs(book, part);
        return state with
        {
            PositionMs = before is { } start ? (long)((start + inPart) / 1_000_000) : null,
            Part = PartState(book, part) with { PositionMs = (long)(inPart / 1_000_000) },
        };
    }

    // The queue as it stands, without the running position.
    private QueueState Snapshot()
    {
        var current = _current is { } id ? ItemOf(id) : null;
        var duration = current?.Group?.Duration ?? current?.Track?.Duration;
        QueuePartState? part = current?.Group is { } book && _currentPart < book.Parts.Count
            ? PartState(book, _currentPart)
            : null;
        return new QueueState(zone, [.. _items], _current, _repeat, _shuffle, _transport, _nowPlaying, null,
            Ms(duration), part);
    }

    private static QueuePartState PartState(QueueGroup book, int part) =>
        new(part, book.Parts.Count, book.Parts[part].Title, null, Ms(book.Parts[part].Length));

    // The chapter audible, from the playhead (phase 32): a chapter's start is
    // crossed with no event from the core, so the part follows the clock.
    private void Follow()
    {
        if (_playhead is not { } playhead || playhead.Item != _current ||
            ItemOf(playhead.Item) is not { Group: { } book } item || playhead.Part >= book.Parts.Count ||
            book.Parts[playhead.Part].Chapter is null)
        {
            return;
        }
        var (part, _) = Located(item, playhead);
        if (part == _currentPart)
            return;
        _currentPart = part;
        if (_nowPlaying is { } now && now.Item == playhead.Item)
            _nowPlaying = now with { Part = part };
    }

    // Set after every operation: a run on to the next chapter's start in the
    // file playing, which publishes the change of part.
    private void ArmChapterTimer()
    {
        _chapterTimer?.Dispose();
        _chapterTimer = null;
        if (_deleted || _playhead is not { Running: true } playhead || playhead.Item != _current ||
            ItemOf(playhead.Item)?.Group is not { } book || playhead.Part >= book.Parts.Count ||
            book.Parts[playhead.Part].Chapter is null)
        {
            return;
        }
        var position = Position(playhead);
        var next = ChapterAt(book, playhead.Part, position) + 1;
        if (next > RunOf(book, playhead.Part).Last)
            return;
        // Rounded up, so the playhead is past the start when it fires.
        var due = TimeSpan.FromTicks((long)((FileStartNs(book.Parts[next]) - position + 99) / 100));
        _chapterTimer = engine.Time.CreateTimer(
            _ => _ = RunAsync(() => Task.FromResult<Failure<QueueError>?>(null), CancellationToken.None),
            null, due, Timeout.InfiniteTimeSpan);
    }

    private static long? Ms(TimeSpan? duration) => duration is { } d ? (long)d.TotalMilliseconds : null;

    private ulong Position(Playhead playhead) => playhead.Running
        ? playhead.PositionNs + ((ulong)engine.Time.GetElapsedTime(playhead.At).Ticks * 100)
        : playhead.PositionNs;

    // The transport moved at timelineNs (null: now, unknown). Where the
    // audible source's start on the timeline is known, the position is
    // measured from it; otherwise the clock's estimate is kept.
    private void Anchor(ulong? timelineNs, bool running)
    {
        if (_playhead is not { } playhead)
            return;
        var position = _nowPlaying is { TimelineNs: { } heard } now && now.Item == playhead.Item &&
                       timelineNs is { } at && at >= heard
            ? now.StartOffsetNs + (at - heard)
            : Position(playhead);
        _playhead = new Playhead(playhead.Item, playhead.Part, position, engine.Time.GetTimestamp(), running);
    }

    // Stopped first and cleared second: a clear behind a source still playing
    // would make the core ask for another with zone.sourceNeeded.
    private async Task<Failure<QueueError>?> StopCoreAsync()
    {
        var core = engine.Link.Model.Current.Zones.GetValueOrDefault(zone);
        if (core is not null && (core.Transport != TransportState.Stopped || core.State is ZoneState.Playing or ZoneState.Ended))
        {
            var stopped = await SendAsync(new ZoneStop(zone)).ConfigureAwait(false);
            if (!stopped.IsSuccess)
                return stopped.Failure;
        }
        if (_staged is not null || core?.Staged.Count > 0)
        {
            var cleared = await SendAsync(new SourceClearStaged(zone)).ConfigureAwait(false);
            if (!cleared.IsSuccess)
                return cleared.Failure;
        }
        Forget();
        return null;
    }

    // What comes next changed: the staged source is taken back and the new
    // next one staged. Nothing to do while the core has opened nothing of this
    // queue: play stages then.
    private async Task RestageAsync()
    {
        if (_opened.Count == 0 || NextOf(_opened[^1].Spot) == _staged?.Spot)
            return;

        if (_staged is { } staged)
        {
            var cleared = await SendAsync(new SourceClearStaged(zone)).ConfigureAwait(false);
            if (!cleared.IsSuccess)
            {
                // The resync after the reconnect puts it right.
                LogRestageFailed(engine.Log, zone, cleared.Failure.ToString());
                return;
            }
            if (cleared.Value.Cleared == 0)
            {
                // The core opened it before the clear arrived.
                Opened();
            }
            else
            {
                engine.Media.Retire(staged.MediaUri);
                _staged = null;
            }
        }
        await StageNextAsync().ConfigureAwait(false);
    }

    // Stages what comes after the last opened source, unless something is
    // staged already. An item that will not resolve is marked and passed.
    private async Task StageNextAsync()
    {
        if (_opened.Count == 0 || _staged is not null)
            return;
        var after = _opened[^1].Spot;
        // A station never ends. Staged behind one, the next item would be
        // opened by the core at the station's first hiccup, instead of the
        // station re-tuning. It is staged once the station fails for good,
        // and a caller's next starts it anyway.
        if (ItemOf(after.Item) is { Station: not null, Failure: null })
            return;
        var places = _items.Sum(PartCount);
        for (var tries = 0; tries <= places; tries++)
        {
            if (NextOf(after) is not { } next)
                return;
            var item = ItemOf(next.Item)!;
            var staged = await StageAsync(item, next.Part, FileStartNs(item, next.Part)).ConfigureAwait(false);
            if (staged.IsSuccess)
            {
                _staged = staged.Value;
                return;
            }
            // The core refused or is away: not the item's fault, and not
            // worth trying the next one for.
            if (!Failed(next))
            {
                LogRestageFailed(engine.Log, zone, staged.Failure.ToString());
                return;
            }
        }
    }

    // Resolved at stage time, as the SPI demands: the first bytes are ready
    // when the core is told the URL. A group's part is staged under the
    // part's own id, titled "<book> — <part>" so a room names both; a file
    // with chapters is staged once for all of them, titled by the file (the
    // book alone, when the file is the book). offsetNs is in the file.
    private async Task<Result<Staging, QueueError>> StageAsync(QueueItem item, int part, ulong offsetNs,
        bool retune = false)
    {
        var track = item.Group?.Parts[part].Track ?? item.Track;
        var title = item.Group is not { } book
            ? item.Title
            : book.Parts[part].Chapter is null
                ? $"{book.Title} \u2014 {track!.Title}"
                : track!.Title == book.Title ? book.Title : $"{book.Title} \u2014 {track.Title}";
        var resolved = await ResolveAsync(track, item.Station).ConfigureAwait(false);
        var spot = new Spot(item.Id, part);
        if (!resolved.IsSuccess)
        {
            var reason = resolved.Failure.ToString();
            LogUnresolved(engine.Log, zone, title, reason);
            MarkFailed(spot, reason);
            return Result.Fail(QueueError.Failed, $"{title}: {reason}");
        }
        MarkFormat(spot, resolved.Value.Format);

        var uri = engine.Media.Publish(resolved.Value);
        var sourceId = engine.NextSourceId();
        var (providerId, providerItemId) = track is { } t
            ? (t.ProviderId, t.ProviderItemId)
            : (item.Station!.ProviderId, item.Station.ProviderItemId);
        var answer = await SendAsync(new SourceStage(zone, sourceId, uri.AbsoluteUri, $"{providerId}:{providerItemId}",
            title, offsetNs == 0 ? null : offsetNs)).ConfigureAwait(false);
        if (!answer.IsSuccess)
        {
            engine.Media.Retire(uri);
            return answer.Failure;
        }
        LogStaged(engine.Log, zone, sourceId, title);
        return new Staging(sourceId, item.Id, part, uri, retune);
    }

    private async Task<Result<ResolvedStream, ProviderError>> ResolveAsync(Track? track, RadioStation? station)
    {
        if (track is not null)
        {
            var resolver = engine.Providers.Get<IStreamResolver>(track.ProviderId);
            return resolver.IsSuccess
                ? await resolver.Value.ResolveAsync(track, CancellationToken.None).ConfigureAwait(false)
                : resolver.Failure;
        }
        var live = engine.Providers.Get<ILiveStreamResolver>(station!.ProviderId);
        return live.IsSuccess
            ? await live.Value.ResolveAsync(station, CancellationToken.None).ConfigureAwait(false)
            : live.Failure;
    }

    // An opened source that failed is kept in the list: staging goes on after
    // it, passing it by.
    private async Task FailedAsync(string sourceId, string reason)
    {
        var staged = _staged?.SourceId == sourceId ? _staged : null;
        var failed = staged ?? _opened.Find(s => s.SourceId == sourceId);
        if (failed is null)
            return;
        LogSourceFailed(engine.Log, zone, sourceId, reason);
        engine.Media.Retire(failed.MediaUri);
        if (staged is not null)
            _staged = null;
        var last = staged is not null || failed == _opened[^1];

        // A radio that hiccups re-tunes once, rather than ending the queue.
        // Nothing is staged behind a station, so the core has opened nothing
        // in its place; staged now, the new source plays on from where the
        // failed one stopped.
        if (last && !failed.Retune && ItemOf(failed.Item) is { Station: not null } station)
        {
            var retuned = await StageAsync(station, 0, 0, retune: true).ConfigureAwait(false);
            if (retuned.IsSuccess)
            {
                LogRetuned(engine.Log, zone, station.Title);
                _staged = retuned.Value;
            }
            else if (ItemOf(station.Id)?.Failure is null)
            {
                // The core refused or is away; the resync puts it right.
                LogRestageFailed(engine.Log, zone, retuned.Failure.ToString());
            }
            else
            {
                await StageNextAsync().ConfigureAwait(false);
            }
            return;
        }

        MarkFailed(failed.Spot, reason);
        if (last)
            await StageNextAsync().ConfigureAwait(false);
    }

    // The core ran out of staged sources after this queue's last one. Unless
    // something has come since, the queue has played out.
    private async Task EndedAsync()
    {
        await StageNextAsync().ConfigureAwait(false);
        if (_staged is not null)
            return;
        LogEnded(engine.Log, zone);
        Forget();
        SetCurrent(null);
        _playhead = null;
    }

    private async Task<Failure<QueueError>?> EnsureZoneAsync()
    {
        if (_zoneCreated)
            return null;
        var created = await SendAsync(new ZoneCreate(zone)).ConfigureAwait(false);
        if (!created.IsSuccess)
            return created.Failure;
        _zoneCreated = true;
        return null;
    }

    private void Opened()
    {
        _opened.Add(_staged!);
        _staged = null;
    }

    // The source at `index` is audible; the ones before it are done.
    private void Started(int index, NowPlaying nowPlaying)
    {
        foreach (var done in _opened.Take(index))
            engine.Media.Retire(done.MediaUri);
        _opened.RemoveRange(0, index);
        // Heard: a station that fails from here on re-tunes again.
        _opened[0] = _opened[0] with { Retune = false };
        SetCurrent(nowPlaying.Item, _opened[0].Part);
        _nowPlaying = nowPlaying;
        _playhead = new Playhead(nowPlaying.Item, _opened[0].Part, nowPlaying.StartOffsetNs, engine.Time.GetTimestamp(),
            _transport != QueueTransport.Paused);
        if (_shuffle && (_history.Count == 0 || _history[^1] != nowPlaying.Item))
            _history.Add(nowPlaying.Item);
    }

    // The core is done with every source of this queue.
    private void Forget()
    {
        foreach (var opened in _opened)
            engine.Media.Retire(opened.MediaUri);
        if (_staged is { } staged)
            engine.Media.Retire(staged.MediaUri);
        _opened.Clear();
        _staged = null;
        _nowPlaying = null;
    }

    // --- order --------------------------------------------------------------

    // What plays after `from` by itself: a group's next part (past the
    // chapters of the file `from` stages, which play from it), then the next
    // item from its first part. Repeat one is the same item again (a group
    // from its first part), under a new sourceId; failed items and parts are
    // passed.
    private Spot? NextOf(Spot at)
    {
        var item = ItemOf(at.Item);
        var last = item?.Group is { } book && at.Part < book.Parts.Count ? RunOf(book, at.Part).Last : at.Part;
        if (item is not null && NextPart(item, last) is { } part)
            return at with { Part = part };
        QueueItemId? next = _repeat == RepeatMode.One
            ? item is { Failure: null } ? at.Item : null
            : _shuffle ? ShuffleNext(at.Item, _repeat == RepeatMode.All) : Step(at.Item, 1, _repeat == RepeatMode.All);
        return next is { } id ? new Spot(id, FirstPart(ItemOf(id)!)) : null;
    }

    // What a caller's next moves to: a group's next part, else the next item;
    // repeat one is passed over as if it were repeat all.
    private Spot? UserNext(Spot at)
    {
        if (ItemOf(at.Item) is { } item && NextPart(item, at.Part) is { } part)
            return at with { Part = part };
        return UserNextItem(at.Item) is { } next ? new Spot(next, FirstPart(ItemOf(next)!)) : null;
    }

    private QueueItemId? UserNextItem(QueueItemId at) =>
        _shuffle ? ShuffleNext(at, _repeat != RepeatMode.Off) : Step(at, 1, _repeat != RepeatMode.Off);

    // A group's previous part, else the previous item at its last part.
    // Previous never restarts the current part.
    private Spot? UserPrevious(Spot at)
    {
        if (ItemOf(at.Item) is { } item && PreviousPart(item, at.Part) is { } part)
            return at with { Part = part };
        return UserPreviousItem(at.Item) is { } previous ? new Spot(previous, LastPart(ItemOf(previous)!)) : null;
    }

    private QueueItemId? UserPreviousItem(QueueItemId from)
    {
        if (!_shuffle)
            return Step(from, -1, _repeat != RepeatMode.Off);

        // Back along what was played; the item is added again when it starts.
        var at = _history.LastIndexOf(from);
        if (at <= 0 || IndexOf(_history[at - 1]) < 0)
            return null;
        var previous = _history[at - 1];
        _history.RemoveRange(at - 1, _history.Count - (at - 1));
        _cycleStart = Math.Min(_cycleStart, _history.Count);
        return previous;
    }

    private QueueItemId? Step(QueueItemId from, int direction, bool wrap)
    {
        var index = IndexOf(from);
        if (index < 0)
            return null;
        var count = _items.Count;
        for (var k = 1; k <= count; k++)
        {
            var j = index + (k * direction);
            if (!wrap && (j < 0 || j >= count))
                return null;
            j = ((j % count) + count) % count;
            if (_items[j].Failure is null)
                return _items[j].Id;
        }
        return null;
    }

    // A random item not played in this cycle. The pick is kept for `from`, so
    // an edit that does not touch it stages the same one.
    private QueueItemId? ShuffleNext(QueueItemId from, bool wrap)
    {
        var played = _history.Skip(_cycleStart).Append(from).ToHashSet();
        if (_shufflePick is { } memo && memo.From == from && !played.Contains(memo.Pick) &&
            ItemOf(memo.Pick) is { Failure: null })
        {
            return memo.Pick;
        }

        var candidates = _items.Where(i => i.Failure is null && !played.Contains(i.Id)).ToList();
        if (candidates.Count == 0)
        {
            if (!wrap)
                return null;
            // A new cycle: anything but the item it follows.
            _cycleStart = _history.Count;
            candidates = [.. _items.Where(i => i.Failure is null && i.Id != from)];
            if (candidates.Count == 0)
                return ItemOf(from) is { Failure: null } ? from : null;
        }
        var pick = candidates[Random.Shared.Next(candidates.Count)].Id;
        _shufflePick = (from, pick);
        return pick;
    }

    // --- helpers ------------------------------------------------------------

    // Record equality, except that the items are compared by value.
    private static bool Same(QueueState a, QueueState b) =>
        a with { Items = b.Items } == b && a.Items.SequenceEqual(b.Items);

    private int IndexOf(QueueItemId item) => _items.FindIndex(i => i.Id == item);

    private QueueItem? ItemOf(QueueItemId item) => _items.Find(i => i.Id == item);

    private QueueItem ItemOf(QueueEntry entry) =>
        new(engine.NextItemId(), entry.Track, entry.Station, Group: entry.Group);

    private void SetCurrent(QueueItemId? item, int part = 0)
    {
        _current = item;
        _currentPart = item is null ? 0 : part;
    }

    // The group's part a staging is, null for a track or a station.
    private int? PartOf(Staging staging) => ItemOf(staging.Item)?.Group is null ? null : staging.Part;

    private static int PartCount(QueueItem item) => item.Group?.Parts.Count ?? 1;

    // The first part not failed, else the first; 0 outside a group.
    private static int FirstPart(QueueItem item) =>
        item.Group is { } book ? Math.Max(0, IndexOfPart(book, p => p.Failure is null)) : 0;

    private static int LastPart(QueueItem item)
    {
        if (item.Group is not { } book)
            return 0;
        for (var i = book.Parts.Count - 1; i >= 0; i--)
        {
            if (book.Parts[i].Failure is null)
                return i;
        }
        return book.Parts.Count - 1;
    }

    // The next (previous) part not failed within the group; null past its end.
    private static int? NextPart(QueueItem item, int part)
    {
        if (item.Group is not { } book)
            return null;
        for (var i = part + 1; i < book.Parts.Count; i++)
        {
            if (book.Parts[i].Failure is null)
                return i;
        }
        return null;
    }

    private static int? PreviousPart(QueueItem item, int part)
    {
        if (item.Group is not { } book)
            return null;
        for (var i = part - 1; i >= 0; i--)
        {
            if (book.Parts[i].Failure is null)
                return i;
        }
        return null;
    }

    private static int IndexOfPart(QueueGroup book, Func<QueuePart, bool> match)
    {
        for (var i = 0; i < book.Parts.Count; i++)
        {
            if (match(book.Parts[i]))
                return i;
        }
        return -1;
    }

    // Where the part starts on the group's timeline: the earlier parts'
    // lengths added up; null when one of them is unknown.
    private static ulong? PartStartNs(QueueGroup book, int part)
    {
        ulong start = 0;
        for (var i = 0; i < part; i++)
        {
            if (book.Parts[i].Length is not { } length)
                return null;
            start += (ulong)length.Ticks * 100;
        }
        return start;
    }

    // Where a part starts in its file: a chapter's start, else 0.
    private static ulong FileStartNs(QueuePart part) => part.Chapter is { } c ? (ulong)c.StartMs * 1_000_000 : 0;

    private static ulong FileStartNs(QueueItem item, int part) =>
        item.Group is { } book && part < book.Parts.Count ? FileStartNs(book.Parts[part]) : 0;

    // The parts one staged source plays (phase 32): a chapter and its
    // neighbours of the same file; any other part alone.
    private static (int First, int Last) RunOf(QueueGroup book, int part)
    {
        if (book.Parts[part].Chapter is null)
            return (part, part);
        bool Same(int i) => book.Parts[i].Chapter is not null && book.Parts[i].Track == book.Parts[part].Track;
        var (first, last) = (part, part);
        while (first > 0 && Same(first - 1))
            first--;
        while (last + 1 < book.Parts.Count && Same(last + 1))
            last++;
        return (first, last);
    }

    // The chapter of the part's file that a position in the file falls in:
    // the last to start at or before it.
    private static int ChapterAt(QueueGroup book, int part, ulong fileNs)
    {
        var (first, last) = RunOf(book, part);
        var at = first;
        for (var i = first + 1; i <= last && FileStartNs(book.Parts[i]) <= fileNs; i++)
            at = i;
        return at;
    }

    // The playhead as a part and the offset in it: for a chapter, the one its
    // position in the file falls in.
    private (int Part, ulong OffsetNs) Located(QueueItem? item, Playhead playhead)
    {
        var position = Position(playhead);
        if (item?.Group is not { } book || playhead.Part >= book.Parts.Count || book.Parts[playhead.Part].Chapter is null)
            return (playhead.Part, position);
        var part = ChapterAt(book, playhead.Part, position);
        var start = FileStartNs(book.Parts[part]);
        return (part, position >= start ? position - start : 0);
    }

    // A position on the group's timeline as a part and the offset in it. A
    // part of unknown length ends the map, unless the position is in the
    // part audible (whose start is known); past the end is the last part.
    private static (int Part, ulong OffsetNs)? PartAt(QueueGroup book, ulong positionNs, int? audible)
    {
        ulong start = 0;
        for (var i = 0; i < book.Parts.Count; i++)
        {
            if (book.Parts[i].Length is not { } length)
                return i == audible && positionNs >= start ? (i, positionNs - start) : null;
            var end = start + ((ulong)length.Ticks * 100);
            if (positionNs < end || i == book.Parts.Count - 1)
                return (i, positionNs - start);
            start = end;
        }
        return null;
    }

    private bool Failed(Spot spot) => ItemOf(spot.Item) is { } item &&
        (item.Failure is not null || item.Group?.Parts[spot.Part].Failure is not null);

    // A group's part is marked (with the other chapters of its file, which
    // are the same source), and the group with it once every part has
    // failed; anything else is marked whole.
    private void MarkFailed(Spot spot, string reason)
    {
        var index = IndexOf(spot.Item);
        if (index < 0)
            return;
        if (_items[index].Group is not { } book)
        {
            _items[index] = _items[index] with { Failure = reason };
            return;
        }
        var parts = book.Parts.ToArray();
        var (first, last) = RunOf(book, spot.Part);
        for (var i = first; i <= last; i++)
            parts[i] = parts[i] with { Failure = reason };
        _items[index] = _items[index] with
        {
            Group = book with { Parts = parts },
            Failure = parts.All(p => p.Failure is not null) ? reason : null,
        };
    }

    // What the provider found the bytes to be, over what the track was listed
    // as: a track item only, since a group's parts are local files.
    private void MarkFormat(Spot spot, AudioFormat? format)
    {
        var index = IndexOf(spot.Item);
        if (format is null || index < 0 || _items[index].Track is not { } track || track.Format == format)
            return;
        _items[index] = _items[index] with { Track = track with { Format = format } };
    }

    // Played by name: the item, and the part it starts at, are tried again.
    private void ClearFailure(Spot spot)
    {
        var index = IndexOf(spot.Item);
        if (index < 0)
            return;
        var item = _items[index];
        if (item.Group is { } book && book.Parts[spot.Part].Failure is not null)
        {
            var parts = book.Parts.ToArray();
            var (first, last) = RunOf(book, spot.Part);
            for (var i = first; i <= last; i++)
                parts[i] = parts[i] with { Failure = null };
            item = item with { Group = book with { Parts = parts } };
        }
        _items[index] = item.Failure is null ? item : item with { Failure = null };
    }

    private Failure<QueueError> NoItem(QueueItemId item) =>
        Result.Fail(QueueError.NotFound, $"no item {item} in the queue of zone {zone}");

    private async Task<Result<T, QueueError>> SendAsync<T>(CoreCommand<T> command)
    {
        var answer = await engine.Link.SendAsync(command, CancellationToken.None).ConfigureAwait(false);
        if (answer.IsSuccess)
            return answer.Value;
        var code = answer.Failure.Code switch
        {
            CoreError.NotFound => QueueError.NotFound,
            CoreError.InvalidArgument => QueueError.InvalidArgument,
            CoreError.Unavailable or CoreError.NotConnected or CoreError.ConnectionLost => QueueError.Unavailable,
            _ => QueueError.Failed,
        };
        return Result.Fail(code, $"{command.Name}: {answer.Failure}");
    }

    private static QueueTransport Mirror(TransportState state) => state switch
    {
        TransportState.Stopped => QueueTransport.Stopped,
        TransportState.Starting => QueueTransport.Starting,
        TransportState.Playing => QueueTransport.Playing,
        TransportState.Paused => QueueTransport.Paused,
        _ => throw new ArgumentOutOfRangeException(nameof(state)),
    };

    [LoggerMessage(Level = LogLevel.Debug, Message = "zone {Zone}: staged {SourceId} ({Title})")]
    private static partial void LogStaged(ILogger log, string zone, string sourceId, string title);

    [LoggerMessage(Level = LogLevel.Warning, Message = "zone {Zone}: {Title} did not resolve: {Reason}")]
    private static partial void LogUnresolved(ILogger log, string zone, string title, string reason);

    [LoggerMessage(Level = LogLevel.Warning, Message = "zone {Zone}: source {SourceId} failed: {Reason}")]
    private static partial void LogSourceFailed(ILogger log, string zone, string sourceId, string reason);

    [LoggerMessage(Level = LogLevel.Information, Message = "zone {Zone}: {Title} failed, staged again to re-tune")]
    private static partial void LogRetuned(ILogger log, string zone, string title);

    [LoggerMessage(Level = LogLevel.Warning, Message = "zone {Zone}: could not stage what comes next: {Failure}")]
    private static partial void LogRestageFailed(ILogger log, string zone, string failure);

    [LoggerMessage(Level = LogLevel.Information, Message = "zone {Zone}: the queue has played out")]
    private static partial void LogEnded(ILogger log, string zone);

    [LoggerMessage(Level = LogLevel.Warning,
        Message = "zone {Zone}: the core holds none of its queue any more ({State}); stopped where it was")]
    private static partial void LogLost(ILogger log, string zone, string state);

    [LoggerMessage(Level = LogLevel.Information, Message = "zone {Zone}: resumed {Title} at {Position}")]
    private static partial void LogResumed(ILogger log, string zone, string title, TimeSpan position);

    [LoggerMessage(Level = LogLevel.Warning, Message = "zone {Zone}: could not resume: {Failure}")]
    private static partial void LogResumeFailed(ILogger log, string zone, string failure);
}
