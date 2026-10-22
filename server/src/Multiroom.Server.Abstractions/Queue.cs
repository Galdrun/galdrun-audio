using System.Globalization;
using System.Text.Json.Serialization;

namespace Multiroom.Server.Abstractions;

// The queue interfaces: one queue per zone, and
// the transport that plays it through the core. Everything here is data a
// caller can hold; the engine behind IQueueEngine is Multiroom.Server.Playback's.

// One entry of one queue, minted by the engine and never reused. It is not a
// track id: the same track queued twice is two items.
public readonly record struct QueueItemId(long Value)
{
    public override string ToString() => Value.ToString(CultureInfo.InvariantCulture);
}

// A track, a station or a group, exactly one of the three, by
// provider-scoped identity plus metadata. No resolved stream is kept: one is
// resolved each time the item (or a part of it) is staged. Failure says why
// it did not play the last time it was tried; the queue then plays on past
// it, and skips it until it is played by name. A group fails when all its
// parts have.
public sealed record QueueItem(QueueItemId Id, Track? Track, RadioStation? Station, string? Failure = null,
    QueueGroup? Group = null) : IMediaEntry
{
    public string Title => Track?.Title ?? Group?.Title ?? Station!.Name;

    [JsonIgnore]
    public MediaItem Item => (MediaItem?)Track ?? (MediaItem?)Group ?? Station!;
}

// One item with parts (phase 31): an audiobook of many files is one thing to
// the listener. Its parts play in order, gapless, each its own staged
// source; the item's timeline is theirs end to end. Source is the node it
// was expanded from ("local:book:3"), for display and for a playlist to keep.
// Saved is where a listener stopped in it when it was queued (phase 33),
// answered whether or not it is used; Resume says play from the item starts
// there, the first time.
public sealed record QueueGroup(
    string Title,
    string? Artist,
    ArtworkRef? Artwork,
    MediaType Type,
    string? Source,
    IReadOnlyList<QueuePart> Parts,
    BookMark? Saved = null,
    bool Resume = false) : MediaItem
{
    // A book is its title and author, as an album is.
    public override string? ArtworkId => Artworks.AlbumId(Title, Artist) ?? Artworks.RefId(Artwork);

    // The parts' lengths added up; null when any is unknown, or when the
    // parts were left out of an answer.
    public TimeSpan? Duration => Parts.Count > 0 && Parts.All(p => p.Length is not null)
        ? TimeSpan.FromTicks(Parts.Sum(p => p.Length!.Value.Ticks))
        : null;
}

// One part of a group, and why it last failed to play: a failed part is
// passed, as a failed item is. A part is a file, or a chapter of one (phase
// 32): the chapters of one file are neighbours in the group, and the file is
// staged once for all of them.
public sealed record QueuePart(Track Track, string? Failure = null, Chapter? Chapter = null)
{
    [JsonIgnore]
    public string Title => Chapter?.Title ?? Track.Title;

    // The chapter's length, else the track's; null when unknown.
    [JsonIgnore]
    public TimeSpan? Length => Chapter is { } c
        ? c.EndMs is { } end ? TimeSpan.FromMilliseconds(end - c.StartMs) : null
        : Track.Duration;
}

// A place in a book (phase 33): a part, the offset in it, and the position
// in the whole book where every part before it has a known length.
public sealed record BookMark(int Part, long OffsetMs, long? PositionMs);

// What a caller queues: a track, a station or a group, exactly one.
public sealed record QueueEntry(Track? Track = null, RadioStation? Station = null, QueueGroup? Group = null);

public enum RepeatMode
{
    Off,
    One,
    All,
}

// The core's zone.transport, mirrored.
public enum QueueTransport
{
    Stopped,
    Starting,
    Playing,
    Paused,
}

// The audible instant of the current item: the timeline position it was heard
// from, and the server clock at that instant, from the core's
// zone.sourceStarted. After a resync it comes from `status`, which has no
// server time, and after a seek from zone.seek's answer. StartOffsetNs is
// where in the item (in a group: in the part) that position is; Part is the
// group's part audible, null for a track or a station.
public sealed record NowPlaying(
    QueueItemId Item,
    string SourceId,
    ulong? TimelineNs,
    ulong? ServerTimeNs,
    ulong StartOffsetNs,
    int? Part = null);

// One zone's queue as a caller sees it. Current is the queue position: the
// item audible, or the one playback starts from. PositionMs is where in
// Current the zone is, best effort, when the state was read or published; a
// caller runs it on by its own clock while Transport is Playing. Null when
// not known. DurationMs is Current's length (a group's whole length); in a
// group, PositionMs is in the whole item and Part says which part plays.
public sealed record QueueState(
    string Zone,
    IReadOnlyList<QueueItem> Items,
    QueueItemId? Current,
    RepeatMode Repeat,
    bool Shuffle,
    QueueTransport Transport,
    NowPlaying? NowPlaying,
    long? PositionMs = null,
    long? DurationMs = null,
    QueuePartState? Part = null);

// The part of the current group: "chapter 7 of 20", where in it and how long.
public sealed record QueuePartState(int Index, int Count, string Title, long? PositionMs, long? DurationMs);

public enum QueueError
{
    // No such zone, item, or next or previous item.
    NotFound,

    InvalidArgument,

    // Not now: no connection to the core, nothing playing to pause, a source
    // that cannot seek (radio never can).
    Unavailable,

    // The provider could not resolve the item, or the core refused it.
    Failed,
}

public interface IQueueEngine
{
    IReadOnlyList<QueueState> Queues { get; }

    Result<QueueState, QueueError> GetQueue(string zone);

    // At index, or at the end when null. A zone's queue exists from its first
    // addition; its core zone from its first play.
    Task<Result<QueueState, QueueError>> AddAsync(string zone, IReadOnlyList<Track> tracks, int? index,
        CancellationToken cancellationToken);

    Task<Result<QueueState, QueueError>> AddAsync(string zone, RadioStation station, int? index,
        CancellationToken cancellationToken);

    // Tracks, stations and groups in their order (phase 31), as AddAsync,
    // AddNextAsync and ReplaceAsync place them.
    Task<Result<QueueState, QueueError>> AddEntriesAsync(string zone, IReadOnlyList<QueueEntry> entries, int? index,
        CancellationToken cancellationToken);

    Task<Result<QueueState, QueueError>> AddEntriesNextAsync(string zone, IReadOnlyList<QueueEntry> entries,
        CancellationToken cancellationToken);

    Task<Result<QueueState, QueueError>> ReplaceEntriesAsync(string zone, IReadOnlyList<QueueEntry> entries,
        CancellationToken cancellationToken);

    // Right after the current item, or first when nothing is current: what
    // plays next, unless shuffle picks another.
    Task<Result<QueueState, QueueError>> AddNextAsync(string zone, IReadOnlyList<Track> tracks,
        CancellationToken cancellationToken);

    Task<Result<QueueState, QueueError>> AddNextAsync(string zone, RadioStation station,
        CancellationToken cancellationToken);

    // The whole queue, stopped, becomes these: "play now" is this and then
    // play. Repeat and shuffle stay as they are.
    Task<Result<QueueState, QueueError>> ReplaceAsync(string zone, IReadOnlyList<Track> tracks,
        CancellationToken cancellationToken);

    Task<Result<QueueState, QueueError>> ReplaceAsync(string zone, RadioStation station,
        CancellationToken cancellationToken);

    Task<Result<QueueState, QueueError>> RemoveAsync(string zone, QueueItemId item,
        CancellationToken cancellationToken);

    Task<Result<QueueState, QueueError>> MoveAsync(string zone, QueueItemId item, int index,
        CancellationToken cancellationToken);

    Task<Result<QueueState, QueueError>> SetRepeatAsync(string zone, RepeatMode repeat,
        CancellationToken cancellationToken);

    Task<Result<QueueState, QueueError>> SetShuffleAsync(string zone, bool shuffle,
        CancellationToken cancellationToken);

    // An item plays that item from its start (a group's first part, or its
    // saved place when it was queued to resume there, unless fromStart).
    // Null resumes a paused zone, and otherwise plays the current item from
    // the playhead (its part and offset), or the first.
    Task<Result<QueueState, QueueError>> PlayAsync(string zone, QueueItemId? item,
        CancellationToken cancellationToken, bool fromStart = false);

    Task<Result<QueueState, QueueError>> PauseAsync(string zone, CancellationToken cancellationToken);

    Task<Result<QueueState, QueueError>> StopAsync(string zone, CancellationToken cancellationToken);

    // In a group: the next part, or past its last the next item; previous
    // likewise backwards, into a group's last part.
    Task<Result<QueueState, QueueError>> NextAsync(string zone, CancellationToken cancellationToken);

    Task<Result<QueueState, QueueError>> PreviousAsync(string zone, CancellationToken cancellationToken);

    // A position within an item (a group's whole timeline): the audible one
    // (part) is sought in place, another one starts there.
    Task<Result<QueueState, QueueError>> SeekAsync(string zone, QueueItemId item, TimeSpan offset,
        CancellationToken cancellationToken);

    // The zone's full membership, never a delta.
    Task<Result<QueueState, QueueError>> SetClientsAsync(string zone, IReadOnlyList<string> clients,
        CancellationToken cancellationToken);

    // One room's volume, 0.0 to 1.0, and mute; at least one of the two.
    Task<Result<bool, QueueError>> SetVolumeAsync(string client, double? value, bool? muted,
        CancellationToken cancellationToken);

    // Stops the zone, deletes it in the core and forgets its queue.
    Task<Result<bool, QueueError>> DeleteAsync(string zone, CancellationToken cancellationToken);
}
