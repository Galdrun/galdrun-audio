using Multiroom.Server.Abstractions;
using Multiroom.Server.Library;
using Multiroom.Server.Providers;

namespace Multiroom.Server.Playback;

// Where what is queued goes: at the end (or before the item at an index),
// right after the current item, or in place of the whole queue ("play now"
// is Replace and then play).
public enum QueuePlacement
{
    End,
    Next,
    Replace,
}

// A playlist entry that was not queued, and why.
public sealed record PlaylistRefusal(PlaylistEntry Entry, string Reason);

// A playlist queued: the queue afterwards, and the entries left out.
public sealed record PlaylistEnqueued(QueueState Queue, IReadOnlyList<PlaylistRefusal> Refused);

// A browse node queued by reference: the queue afterwards, how many tracks
// went in, those the provider left out and why, whether MaxNodeTracks cut
// the rest, and whether they went in as one group (phase 31).
public sealed record NodeEnqueued(QueueState Queue, int Queued, IReadOnlyList<RefusedTrack> Refused, bool Truncated,
    bool Grouped = false);

// A node expanded for a queue or a playlist: one group, or its tracks.
public sealed record ExpandedNode(QueueGroup? Group, IReadOnlyList<Track> Tracks, IReadOnlyList<RefusedTrack> Refused,
    bool Truncated);

// Playlists, favourites and browse nodes into a zone's queue. An entry whose
// provider cannot stream it now (not registered, disabled, without the
// capability) is refused by name, never dropped silently: a favourite fails,
// and a playlist queues the rest and names what it left out. A provider that
// is up but has lost the item is found at stage time, where the queue marks
// the item failed and plays on past it. A node (an album, a playlist, a
// folder, an artist's top tracks) is expanded by its provider, at most
// MaxNodeTracks tracks. A node whose tracks are all audiobook (two or more,
// phase 30's verdict) goes in as one group (phase 31), unless the caller
// asks for single items; anything else goes in track by track. A book goes in
// with where a listener stopped in it (phase 33), and starts there when the
// house resumes books and the caller did not ask for its start: the zone's
// listener's place, else the household's.
public sealed class PlaylistPlayer(PlaylistStore store, IQueueEngine queues, ProviderRegistry providers,
    PlaybackSettings? settings = null, BookPositionStore? positions = null, ZoneUsers? users = null)
{
    // Whether books go in as one item when the caller does not say.
    public PlaybackSettings Settings { get; } = settings ?? new PlaybackSettings();

    // One node queues at most this many tracks: a folder of a whole NAS
    // would otherwise be one queue of a hundred thousand.
    public const int MaxNodeTracks = 500;

    public async Task<Result<PlaylistEnqueued, QueueError>> EnqueuePlaylistAsync(string zone, long playlist,
        QueuePlacement placement, CancellationToken cancellationToken, bool fromStart = false, int? index = null)
    {
        var entries = store.Entries(playlist);
        if (!entries.IsSuccess)
            return Map(entries.Failure);
        if (entries.Value.Count == 0)
            return Result.Fail(QueueError.NotFound, $"playlist {playlist} is empty");

        // A group's part that cannot play is refused by name, and the group
        // goes in without it; a group with no part left is refused whole.
        var refused = new List<PlaylistRefusal>();
        var queue = new List<QueueEntry>();
        foreach (var entry in entries.Value)
        {
            if (entry.Group is { } group)
            {
                var parts = new List<QueuePart>();
                foreach (var part in group.Parts)
                {
                    if (Unplayable(part.Track, null) is { } why)
                        refused.Add(new PlaylistRefusal(entry, $"part {part.Track.Title}: {why}"));
                    else
                        parts.Add(part);
                }
                if (parts.Count > 0)
                    queue.AddRange(Book(group with { Parts = parts }, fromStart, zone));
            }
            else if (Unplayable(entry.Track, null) is { } reason)
            {
                refused.Add(new PlaylistRefusal(entry, reason));
            }
            else
            {
                queue.Add(new QueueEntry(entry.Track));
            }
        }
        if (queue.Count == 0)
        {
            return Result.Fail(QueueError.Unavailable, $"no entry of playlist {playlist} can play: " +
                string.Join("; ", refused.Select(r => $"{r.Entry.Title} ({r.Reason})")));
        }

        var queued = await PlaceAsync(zone, queue, placement, index, cancellationToken).ConfigureAwait(false);
        return queued.IsSuccess ? new PlaylistEnqueued(queued.Value, refused) : queued.Failure;
    }

    public async Task<Result<QueueState, QueueError>> EnqueueFavoriteAsync(string zone, long favorite,
        QueuePlacement placement, CancellationToken cancellationToken, bool fromStart = false, int? index = null)
    {
        var found = store.GetFavorite(favorite);
        if (!found.IsSuccess)
            return Map(found.Failure);
        // A book goes in whole, as one group, or not at all: a favourite
        // has no list of refusals to name a part in.
        if (found.Value.Group is { } book)
        {
            var parts = book.Parts.Select(p => (p.Track.Title, Reason: Unplayable(p.Track, null)))
                .Where(p => p.Reason is not null).ToList();
            if (parts.Count > 0)
            {
                return Result.Fail(QueueError.Unavailable, $"favourite {favorite} ({book.Title}): " +
                    string.Join("; ", parts.Take(5).Select(p => $"part {p.Title}: {p.Reason}")));
            }
            return await PlaceAsync(zone, Book(book, fromStart, zone), placement, index, cancellationToken).ConfigureAwait(false);
        }
        var (track, station) = (found.Value.Track, found.Value.Station);
        if (Unplayable(track, station) is { } reason)
            return Result.Fail(QueueError.Unavailable, $"favourite {favorite} ({found.Value.Title}): {reason}");

        return track is not null
            ? await PlaceAsync(zone, [track], placement, index, cancellationToken).ConfigureAwait(false)
            : await PlaceAsync(zone, station!, placement, index, cancellationToken).ConfigureAwait(false);
    }

    // The node as its provider expands it. Nothing is queued, and nothing
    // replaced, when it expands to no track that can play. `group` false
    // queues a book's parts as single items; null is the house's setting.
    public async Task<Result<NodeEnqueued, QueueError>> EnqueueNodeAsync(string zone, string provider, string nodeId,
        QueuePlacement placement, CancellationToken cancellationToken, bool? group = null, bool fromStart = false,
        int? index = null)
    {
        var expanded = await ExpandAsync(provider, nodeId, group ?? Settings.GroupBooks, cancellationToken)
            .ConfigureAwait(false);
        if (!expanded.IsSuccess)
            return expanded.Failure;
        var (book, tracks, refused, truncated) = expanded.Value;
        var queued = book is not null
            ? await PlaceAsync(zone, [new QueueEntry(Group: Marked(book, fromStart, zone))], placement, index, cancellationToken).ConfigureAwait(false)
            : await PlaceAsync(zone, tracks, placement, index, cancellationToken).ConfigureAwait(false);
        return queued.IsSuccess
            ? new NodeEnqueued(queued.Value, tracks.Count, refused, truncated, book is not null)
            : queued.Failure;
    }

    // The node's tracks, as one group when they are all audiobook and
    // `group` allows it: titled by the node's own header (an album's, a
    // book's, a folder's name), else by the first track's album.
    public async Task<Result<ExpandedNode, QueueError>> ExpandAsync(string provider, string nodeId, bool group,
        CancellationToken cancellationToken)
    {
        var expander = providers.Get<IContainerExpander>(provider);
        if (!expander.IsSuccess)
            return Map(expander.Failure);
        if (providers.Get<IStreamResolver>(provider) is { IsSuccess: false } resolver)
            return Result.Fail(QueueError.Unavailable, $"{provider} cannot stream what {nodeId} holds: {resolver.Failure}");

        var expanded = await expander.Value.ExpandAsync(nodeId, MaxNodeTracks, cancellationToken).ConfigureAwait(false);
        if (!expanded.IsSuccess)
            return Map(expanded.Failure);
        var (tracks, refused, truncated, chapterMarks) = expanded.Value;
        if (tracks.Count == 0)
        {
            return Result.Fail(refused.Count == 0 ? QueueError.NotFound : QueueError.Unavailable,
                refused.Count == 0
                    ? $"{nodeId} holds nothing to play"
                    : $"nothing of {nodeId} can play: " + string.Join("; ", refused.Take(5).Select(r => $"{r.Track.Title} ({r.Reason})")));
        }

        // A file with chapters is a part per chapter (phase 32); one file
        // with two chapters is a book.
        QueuePart[] parts = [.. tracks.SelectMany(t =>
            chapterMarks?.GetValueOrDefault(t.ProviderItemId) is { Count: >= 2 } chapters
                ? chapters.Select(c => new QueuePart(t, Chapter: c))
                : [new QueuePart(t)])];
        if (!group || parts.Length < 2 || tracks.Any(t => t.MediaType != MediaType.Audiobook))
            return new ExpandedNode(null, tracks, refused, truncated);
        BrowseNode? header = null;
        if (providers.Get<IBrowseProvider>(provider) is { IsSuccess: true } browser &&
            await browser.Value.BrowseAsync(new BrowseRequest(nodeId, 0, 1), cancellationToken).ConfigureAwait(false)
                is { IsSuccess: true } page)
        {
            header = page.Value.Node;
        }
        var first = tracks[0];
        return new ExpandedNode(new QueueGroup(header?.Title ?? first.Album ?? first.Title, first.Artist,
            header?.Artwork ?? first.Artwork, MediaType.Audiobook, $"{provider}:{nodeId}", parts),
            tracks, refused, truncated);
    }

    // A stored book as one item, or file by file when the house does not
    // group books: a file's chapters are one track.
    private List<QueueEntry> Book(QueueGroup book, bool fromStart, string zone) => Settings.GroupBooks
        ? [new QueueEntry(Group: Marked(book, fromStart, zone))]
        : [.. book.Parts.Where((p, i) => p.Chapter is null || i == 0 || book.Parts[i - 1].Track != p.Track)
            .Select(p => new QueueEntry(p.Track))];

    // The book with its saved place, while that is still a part of it (the
    // part's track the one saved); a finished book has none.
    private QueueGroup Marked(QueueGroup book, bool fromStart, string zone)
    {
        if (positions is null || positions.Get(BookPositions.ReferenceOf(book), users?.Of(zone) ?? UserStore.Household) is not { IsSuccess: true, Value: { } saved } ||
            saved.Finished || saved.Part >= book.Parts.Count ||
            saved.PartReference != BookPositions.ReferenceOf(book.Parts[saved.Part].Track))
        {
            return book;
        }
        return book with
        {
            Saved = new BookMark(saved.Part, saved.OffsetMs, saved.PositionMs),
            Resume = Settings.ResumeBooks && !fromStart,
        };
    }

    // `index` places only at the end's turn: Next and Replace say where themselves.
    private Task<Result<QueueState, QueueError>> PlaceAsync(string zone, IReadOnlyList<QueueEntry> entries,
        QueuePlacement placement, int? index, CancellationToken cancellationToken) => placement switch
    {
        QueuePlacement.Next => queues.AddEntriesNextAsync(zone, entries, cancellationToken),
        QueuePlacement.Replace => queues.ReplaceEntriesAsync(zone, entries, cancellationToken),
        _ => queues.AddEntriesAsync(zone, entries, index, cancellationToken),
    };

    private Task<Result<QueueState, QueueError>> PlaceAsync(string zone, IReadOnlyList<Track> tracks,
        QueuePlacement placement, int? index, CancellationToken cancellationToken) => placement switch
    {
        QueuePlacement.Next => queues.AddNextAsync(zone, tracks, cancellationToken),
        QueuePlacement.Replace => queues.ReplaceAsync(zone, tracks, cancellationToken),
        _ => queues.AddAsync(zone, tracks, index, cancellationToken),
    };

    private Task<Result<QueueState, QueueError>> PlaceAsync(string zone, RadioStation station,
        QueuePlacement placement, int? index, CancellationToken cancellationToken) => placement switch
    {
        QueuePlacement.Next => queues.AddNextAsync(zone, station, cancellationToken),
        QueuePlacement.Replace => queues.ReplaceAsync(zone, station, cancellationToken),
        _ => queues.AddAsync(zone, station, index, cancellationToken),
    };

    // Null when the provider can stream it now.
    private string? Unplayable(Track? track, RadioStation? station)
    {
        if (track is not null)
        {
            var resolver = providers.Get<IStreamResolver>(track.ProviderId);
            return resolver.IsSuccess ? null : resolver.Failure.ToString();
        }
        var live = providers.Get<ILiveStreamResolver>(station!.ProviderId);
        return live.IsSuccess ? null : live.Failure.ToString();
    }

    private static Failure<QueueError> Map(Failure<LibraryError> failure) => Result.Fail(failure.Code switch
    {
        LibraryError.NotFound => QueueError.NotFound,
        LibraryError.InvalidArgument => QueueError.InvalidArgument,
        _ => QueueError.Failed,
    }, failure.Detail);

    private static Failure<QueueError> Map(Failure<ProviderError> failure) => Result.Fail(failure.Code switch
    {
        ProviderError.NotFound => QueueError.NotFound,
        ProviderError.InvalidArgument => QueueError.InvalidArgument,
        ProviderError.Unavailable => QueueError.Unavailable,
        _ => QueueError.Failed,
    }, failure.Detail);
}
