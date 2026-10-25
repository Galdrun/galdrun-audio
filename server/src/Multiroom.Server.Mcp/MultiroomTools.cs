using System.ComponentModel;
using System.Text.Json;
using ModelContextProtocol;
using ModelContextProtocol.Server;
using Multiroom.Server.Abstractions;
using Multiroom.Server.Library;
using Multiroom.Server.Playback;
using Multiroom.Server.Playback.Core;
using Multiroom.Server.Providers;
using Multiroom.Server.Providers.AddIns;
using Multiroom.Server.Providers.WebRadio;

namespace Multiroom.Server.Mcp;

// One tool of the catalog and its tier.
public sealed record MultiroomTool(string Name, McpTier Tier, McpServerTool Tool);

// A list as a tool answers it: one page, where it starts, how long the whole
// list is, and whether anything was left out (Note says how to get it).
// Preferred, where a tool answers it, names the ids on the page of tracks the
// household liked or made a favourite (one mark either way).
public sealed record ToolList<T>(IReadOnlyList<T> Items, int Offset, int Total, bool Cut, string? Note = null,
    IReadOnlyList<long>? Preferred = null);

// A queue as a tool answers it: its items cut to one page, and with
// get_queue the ids of the items on it the household liked or made a favourite.
// A group's parts are left out (a book may have hundreds): Groups says how
// many each has and how long it is, and get_queue_item_parts pages them.
public sealed record QueuePage(QueueState Queue, int Offset, int Total, bool Cut, IReadOnlyList<long>? Preferred = null,
    IReadOnlyList<GroupSummary>? Groups = null);

// A group on a queue page: its item, its part count and its whole length,
// and where a listener stopped in it when it was queued (phase 33).
public sealed record GroupSummary(long Item, int Parts, long? DurationMs, long? SavedPositionMs = null);

// What an addition to a queue did: the queue after it, around the current
// item, and what a playlist, favourite or node left out.
public sealed record QueueAdded(QueuePage Queue, int? Queued = null, IReadOnlyList<PlaylistRefusal>? RefusedEntries = null,
    IReadOnlyList<RefusedTrack>? RefusedTracks = null, bool Truncated = false);

// A playable browse node by reference, as browse answered it.
public sealed record NodeRef(string Provider, string Id);

public enum QueueAddPlacement
{
    End,
    Next,
}

// The Galdrun MCP server's tool catalog. Every
// tool is a thin adapter: it checks its arguments, calls the service the REST
// API calls, and answers the same records, so an agent and an app see the
// same thing. A failure is an MCP tool error carrying the module's code and
// message ("notFound: no zone x"). Nothing here configures the house or holds
// a secret: folders, credentials, logins, AI settings, the resampler, room
// devices and input bindings stay on the REST API. A room's equalizer is
// sound, as its volume is, and is here.
public sealed class MultiroomTools
{
    public const int DefaultLimit = 20;
    public const int MaxLimit = UnifiedSearch.MaxLimit;

    // The items of a queue an answer shows around the current one.
    public const int QueueWindow = 20;

    private const string ZoneParameter = "The zone: its id, its name, or the name of any room in it.";

    // Phase 36: the household unless a user is named (list_users).
    private const string UserParameter = "Whose it is, a name from list_users; the household's when empty.";

    private const string UserScopeParameter = "Empty: the household's own. A name from list_users: theirs with the " +
        "household's. *: everyone's, each with its user.";

    private readonly List<MultiroomTool> _tools = [];
    private readonly JsonSerializerOptions _json;
    private readonly House _house;

    public MultiroomTools(UnifiedSearch search, ProviderRegistry providers, House house, IQueueEngine queues,
        PlaylistPlayer player, PlaylistStore playlists, LikeStore likes, AddInRegistry addIns, SongResolver resolver,
        PlayHistoryStore history, BookPositionStore bookPositions, ZoneKeeper zones, UserStore users, AudiobookLikes books, IProposals proposals,
        EqualizerProfiles equalizerProfiles, StationStore stations, JsonSerializerOptions json)
    {
        _json = json;
        _house = house;

        // --- read ----------------------------------------------------------------

        Add(McpTier.Read, "search", "Searches every music provider (the local library, Tidal, radio, ...) at once. " +
            "Answers tracks, albums, artists, stations and playlists, each naming its provider; a provider that is slow or failing is named in failures.",
            async ([Description("What to look for: a title, an artist, an album.")] string query,
                [Description("What kinds to answer; all when empty.")] SearchKinds[]? types = null,
                [Description("Results to skip per provider.")] int offset = 0,
                [Description("At most this many per provider, 1 to 100.")] int? limit = null,
                [Description("Only items of this type: music, audiobook or radio; every type when empty.")] MediaType? mediaType = null,
                CancellationToken cancellationToken = default) =>
            {
                if (string.IsNullOrWhiteSpace(query))
                    throw Refused("invalidArgument", "query is empty");
                var kinds = types is { Length: > 0 } ? types.Aggregate(SearchKinds.None, (a, k) => a | k) : SearchKinds.All;
                var found = await search.SearchAsync(query.Trim(), kinds == SearchKinds.None ? SearchKinds.All : kinds,
                    Offset(offset), Limit(limit), cancellationToken).ConfigureAwait(false);
                return mediaType is { } only ? UnifiedSearch.OfType(found, only) : found;
            });

        Add(McpTier.Read, "list_providers", "Lists the music providers with their state, capabilities and browse root nodes.",
            async (CancellationToken cancellationToken = default) =>
                await search.ProvidersAsync(withRoot: true, cancellationToken).ConfigureAwait(false));

        Add(McpTier.Read, "browse", "Browses one provider's tree: its root without a node, else the node's page. " +
            "A node that says playable can be queued whole with add_to_queue's node.",
            async ([Description("The provider id, from list_providers.")] string provider,
                [Description("The node id, from a previous browse or list_providers' root; empty for the root.")] string? node = null,
                [Description("A sort the node offers.")] string? sort = null,
                [Description("A text filter, where the node takes one.")] string? filter = null,
                [Description("A letter to start at, where the node offers letters.")] string? letter = null,
                int offset = 0, int? limit = null, CancellationToken cancellationToken = default) =>
            {
                var browser = Ok(providers.Get<IBrowseProvider>(provider));
                return Ok(await browser.BrowseAsync(new BrowseRequest(Text(node), Offset(offset), Limit(limit), Text(sort),
                    Text(filter), Text(letter)), cancellationToken).ConfigureAwait(false));
            });

        Add(McpTier.Read, "list_zones", "Lists the zones (groups of rooms that play together): id, name, members, volume, " +
            "transport, now playing and the queue around the current item.",
            () =>
            {
                var zones = house.Zones();
                var cut = false;
                var shown = zones.Select(z =>
                {
                    if (z.Queue is not { } queue)
                        return z;
                    var page = AroundCurrent(queue);
                    cut |= page.Cut;
                    return z with { Queue = page.Queue };
                }).ToList();
                return new ToolList<ZoneView>(shown, 0, shown.Count, cut,
                    cut ? $"each queue shows at most {QueueWindow} items from the current one; get_queue pages a whole queue" : null);
            });

        Add(McpTier.Read, "get_queue", "One zone's queue, one page of its items at a time. " +
            "preferred lists the ids of the items on the page the household liked or made a favourite.",
            ([Description(ZoneParameter)] string zone, int offset = 0, int? limit = null) =>
            {
                var page = Paged(Ok(queues.GetQueue(zone)), Offset(offset), Limit(limit));
                var preferred = Ok(PreferredTracks.Of(likes, playlists, page.Queue.Items.Select(i => i.Track).OfType<Track>()));
                return page with
                {
                    Preferred = [.. page.Queue.Items.Where(i => i.Track is { } t && preferred.Has(t)).Select(i => i.Id.Value)],
                };
            });

        Add(McpTier.Read, "get_queue_item_parts", "The parts (chapters, files) of one group item of a zone's queue, " +
            "an audiobook queued as one item, one page at a time.",
            ([Description(ZoneParameter)] string zone, [Description("The group's item id, from get_queue.")] long item,
                int offset = 0, int? limit = null) =>
            {
                var queue = Ok(queues.GetQueue(zone));
                var found = queue.Items.FirstOrDefault(i => i.Id.Value == item) ??
                    throw Refused("notFound", $"no item {item} in the queue of zone {zone}");
                return found.Group is { } book
                    ? Page(book.Parts, Offset(offset), Limit(limit))
                    : throw Refused("invalidArgument", $"item {item} is no group: it has no parts");
            });

        Add(McpTier.Read, "list_rooms", "Lists the rooms (players) with what this server asked for them and what the core reports.",
            async (CancellationToken cancellationToken = default) => await house.RoomsAsync(cancellationToken).ConfigureAwait(false));

        Add(McpTier.Read, "list_playlists", "Lists the household's stored playlists.",
            () => Ok(playlists.Playlists()));

        Add(McpTier.Read, "get_playlist_entries", "One stored playlist's tracks, one page at a time. " +
            "preferred lists the ids of the entries on the page the household liked or made a favourite.",
            ([Description("The playlist id, from list_playlists.")] long playlist, int offset = 0, int? limit = null) =>
            {
                var page = Page(Ok(playlists.Entries(playlist)), Offset(offset), Limit(limit));
                var preferred = Ok(PreferredTracks.Of(likes, playlists, page.Items.Select(e => e.Track).OfType<Track>()));
                return page with
                {
                    Preferred = [.. page.Items.Where(e => e.Track is { } t && preferred.Has(t)).Select(e => e.Id)],
                };
            });

        Add(McpTier.Read, "list_users", "The household's users: names a like, a favourite or a play may carry. " +
            "Without a user, the household's own marks are meant.",
            () => Ok(users.Users()));

        Add(McpTier.Read, "list_favorites", "The household's favourite tracks and stations, one page at a time.",
            ([Description(UserScopeParameter)] string? user = null, int offset = 0, int? limit = null) =>
                Page(Ok(playlists.Favorites(user)), Offset(offset), Limit(limit)));

        Add(McpTier.Read, "list_likes", "What the household liked (tracks, albums, artists, stations, audiobooks), one page at a time.",
            ([Description("Only this kind; all when empty.")] LikeKind? kind = null,
                [Description(UserScopeParameter)] string? user = null, int offset = 0, int? limit = null) =>
                Page(Ok(likes.Likes(kind, user)), Offset(offset), Limit(limit)));

        Add(McpTier.Read, "recommend", "Tracks like the given ones, from the library's metadata (shared artist, genre, era, playlists), " +
            "ranked with a reason each.",
            async ([Description("The seed tracks, as search or a queue answered them.")] Track[] tracks,
                [Description("At most this many, 1 to 100.")] int? limit = null, CancellationToken cancellationToken = default) =>
            {
                if (tracks.Length is 0 or > MaxLimit)
                    throw Refused("invalidArgument", $"tracks: 1 to {MaxLimit}");
                return await addIns.RecommendAsync(tracks, Limit(limit), cancellationToken).ConfigureAwait(false);
            });

        Add(McpTier.Read, "find_songs", "Finds songs named by title and artist among the providers that play tracks, " +
            "the library first. Answers, for each song in its order, the track found and how well it matched, or why none was. " +
            "Choose the songs yourself, by name, from your own knowledge; queue only the tracks this found.",
            async ([Description("The songs, 1 to 50: title and primary artist, album and year where known.")] SongSuggestion[] songs,
                [Description("Tracks not to answer, as search or a queue answered them: the seed, what is already queued.")] Track[]? exclude = null,
                CancellationToken cancellationToken = default) =>
                Ok(await resolver.ResolveAsync(songs, exclude ?? [], cancellationToken).ConfigureAwait(false)));

        Add(McpTier.Read, "get_history", "What the house played, newest first, one page at a time: each track with its zone and when it became audible. " +
            "Stations are not listed.",
            ([Description("Only this zone's plays; the whole house's when empty.")] string? zone = null,
                [Description(UserScopeParameter)] string? user = null, int offset = 0, int? limit = null) =>
            {
                var page = Ok(history.Page(Text(zone), Offset(offset), Limit(limit), user));
                return new ToolList<PlayedTrack>(page.Items, offset, page.Total, offset + page.Items.Count < page.Total);
            });

        Add(McpTier.Read, "get_proposals", "What the house (or a user) might like next, proposed by the AI from what it liked: " +
            "tracks, albums, artists and audiobooks the house can play, each with a reason, as the last run left them, " +
            "and whether background runs are on and an AI is available.",
            ([Description(UserParameter)] string? user = null) => Ok(proposals.View(user)));

        Add(McpTier.Control, "refresh_proposals", "Asks the AI for new proposals now, in the background: answers the run, " +
            "which is the running one when one runs already. Read the result later with get_proposals.",
            ([Description(UserParameter)] string? user = null) => Ok(proposals.Refresh(user)));

        Add(McpTier.Read, "list_book_positions", "Where a listener stopped in each audiobook, newest first, one page at a time: " +
            "the book's reference, its part and offset, its position and length in milliseconds, and whether it was finished. " +
            "The reference is the book's browse node as provider:id (\"local:book:3\" is provider local, id book:3): " +
            "queue it with add_to_queue's or replace_queue's node and play. Where the house resumes books, its group says " +
            "resume and it starts where it was left; otherwise seek the item to the savedPositionMs its entry in groups answers.",
            (int offset = 0, int? limit = null) =>
            {
                var page = Ok(bookPositions.Page(Offset(offset), Limit(limit)));
                return new ToolList<BookPosition>(page.Items, offset, page.Total, offset + page.Items.Count < page.Total);
            });

        Add(McpTier.Read, "get_autofill", "A zone's AutoFill: whether it adds songs by itself when the queue runs low, its settings, " +
            "whether a fill is running, why it waits (repeatAll, shuffle, nothingAdded) and what the last fill added.",
            ([Description(ZoneParameter)] string zone) => Ok(house.AutoFill(zone)));

        Add(McpTier.Read, "list_equalizer_profiles", "The house's equalizer profiles: the built-in ones, then the household's own, " +
            "each a preamp and bands in dB. A room's own equalizer is its desired.equalizer in list_rooms.",
            () => equalizerProfiles.All);

        // --- control -------------------------------------------------------------

        Add(McpTier.Control, "play", "Plays a zone: the given queue item, else resumes or starts at the current item. " +
            "A book queued to resume starts where it was left, unless fromStart.",
            async ([Description(ZoneParameter)] string zone, [Description("A queue item id, from get_queue.")] long? item = null,
                [Description("Play the item from its beginning, not from a book's saved place.")] bool fromStart = false,
                CancellationToken cancellationToken = default) =>
                AroundCurrent(Ok(await queues.PlayAsync(zone, item is { } id ? new QueueItemId(id) : null, cancellationToken,
                    fromStart).ConfigureAwait(false))));
        AddTransport("pause", "Pauses a zone.", queues.PauseAsync);
        AddTransport("stop", "Stops a zone.", queues.StopAsync);
        AddTransport("next", "Skips a zone to the next item.", queues.NextAsync);
        AddTransport("previous", "Goes back to the previous item in a zone.", queues.PreviousAsync);

        Add(McpTier.Control, "seek", "Seeks within a queue item.",
            async ([Description(ZoneParameter)] string zone, [Description("The queue item id.")] long item,
                [Description("Where in the item, in milliseconds.")] long positionMs, CancellationToken cancellationToken = default) =>
            {
                if (positionMs < 0)
                    throw Refused("invalidArgument", $"positionMs {positionMs} is negative");
                return AroundCurrent(Ok(await queues.SeekAsync(zone, new QueueItemId(item),
                    TimeSpan.FromMilliseconds(positionMs), cancellationToken).ConfigureAwait(false)));
            });

        Add(McpTier.Control, "add_to_queue", "Adds to a zone's queue, at the end or next: exactly one of tracks, a station, " +
            "a stored playlist, a favourite or a playable browse node.",
            async ([Description(ZoneParameter)] string zone, Track[]? tracks = null, RadioStation? station = null,
                [Description("A stored playlist id.")] long? playlist = null,
                [Description("A favourite id.")] long? favorite = null,
                [Description("A playable browse node.")] NodeRef? node = null,
                [Description("end (the default) or next: right after the current item.")] QueueAddPlacement placement = QueueAddPlacement.End,
                [Description("For tracks or a station at the end: insert at this index instead.")] int? index = null,
                [Description("A book starts at its beginning, not where a listener stopped in it.")] bool fromStart = false,
                CancellationToken cancellationToken = default) =>
                await AddAsync(queues, player, zone, tracks, station, playlist, favorite, node,
                    placement == QueueAddPlacement.Next ? QueuePlacement.Next : QueuePlacement.End, index, fromStart,
                    cancellationToken).ConfigureAwait(false));

        Add(McpTier.Control, "move_queue_item", "Moves a queue item to another index.",
            async ([Description(ZoneParameter)] string zone, long item, int index, CancellationToken cancellationToken = default) =>
                AroundCurrent(Ok(await queues.MoveAsync(zone, new QueueItemId(item), index, cancellationToken)
                    .ConfigureAwait(false))));

        // A queue is what plays now, not something kept: taking items out, emptying
        // it or replacing it is control, as adding to it is. What is stored
        // (playlists, zones) stays destructive.
        Add(McpTier.Control, "remove_queue_item", "Removes an item from a zone's queue.",
            async ([Description(ZoneParameter)] string zone, long item, CancellationToken cancellationToken = default) =>
                AroundCurrent(Ok(await queues.RemoveAsync(zone, new QueueItemId(item), cancellationToken)
                    .ConfigureAwait(false))));

        Add(McpTier.Control, "clear_queue", "Empties a zone's queue, stopped; its repeat and shuffle stay.",
            async ([Description(ZoneParameter)] string zone, CancellationToken cancellationToken = default) =>
            {
                Ok(queues.GetQueue(zone));
                return AroundCurrent(Ok(await queues.ReplaceEntriesAsync(zone, [], cancellationToken).ConfigureAwait(false)));
            });

        Add(McpTier.Control, "replace_queue", "Replaces a zone's whole queue, stopped: exactly one of tracks, a station, " +
            "a stored playlist, a favourite or a playable browse node.",
            async ([Description(ZoneParameter)] string zone, Track[]? tracks = null, RadioStation? station = null, long? playlist = null, long? favorite = null, NodeRef? node = null,
                [Description("A book starts at its beginning, not where a listener stopped in it.")] bool fromStart = false,
                CancellationToken cancellationToken = default) =>
                await AddAsync(queues, player, zone, tracks, station, playlist, favorite, node, QueuePlacement.Replace, null,
                    fromStart, cancellationToken).ConfigureAwait(false));

        Add(McpTier.Control, "set_queue_mode", "Sets a zone's repeat (off, one, all), shuffle, or both.",
            async ([Description(ZoneParameter)] string zone, RepeatMode? repeat = null, bool? shuffle = null, CancellationToken cancellationToken = default) =>
            {
                if (repeat is null && shuffle is null)
                    throw Refused("invalidArgument", "repeat, shuffle, or both");
                var state = queues.GetQueue(zone);
                if (repeat is { } r)
                    state = await queues.SetRepeatAsync(zone, r, cancellationToken).ConfigureAwait(false);
                if (state.IsSuccess && shuffle is { } s)
                    state = await queues.SetShuffleAsync(zone, s, cancellationToken).ConfigureAwait(false);
                return AroundCurrent(Ok(state));
            });

        Add(McpTier.Control, "set_autofill", "Switches a zone's AutoFill on or off and sets it: when the playing queue has threshold or fewer " +
            "items after the current one, count tracks are added at its end, chosen by source (ai: songs a model names for request; " +
            "recommendations: by the library's metadata), never a song among the house's last avoidRecent plays. " +
            "fallback fills from recommendations when the AI fails. Settings left out stay as they are.",
            ([Description(ZoneParameter)] string zone, bool? enabled = null,
                [Description("Items left after the current one that start a fill, 0 to 20.")] int? threshold = null,
                [Description("Tracks one fill adds, 1 to 50.")] int? count = null,
                [Description("ai or recommendations.")] AutoFillSource? source = null,
                [Description("What the AI is asked for, in words: \"songs that continue this queue\", \"calmer songs\".")] string? request = null,
                [Description("Fill from recommendations when the AI fails.")] bool? fallback = null,
                [Description("How many of the house's last plays are not chosen again, 0 to 10000.")] int? avoidRecent = null) =>
                Ok(house.SetAutoFill(zone, new AutoFillPatch(enabled, threshold, count, source, request, fallback, avoidRecent))));


        Add(McpTier.Control, "set_zone_volume", "Sets a zone's own volume (0.0 to 1.0) over its rooms', its mute, or both.",
            async ([Description(ZoneParameter)] string zone, double? volume = null, bool? muted = null, CancellationToken cancellationToken = default) =>
                Ok(await house.SetZoneLevelAsync(zone, volume, muted, cancellationToken).ConfigureAwait(false)));

        Add(McpTier.Control, "set_room_volume", "Sets one room's own volume (0.0 to 1.0), its mute, or both.",
            async ([Description("The room's client id, from list_rooms.")] string room, double? volume = null, bool? muted = null,
                CancellationToken cancellationToken = default) =>
                Ok(await house.SetRoomLevelAsync(room, volume, muted, cancellationToken).ConfigureAwait(false)));

        Add(McpTier.Control, "set_room_equalizer", "Sets one room's equalizer, exactly one of: a profile by name " +
            "(list_equalizer_profiles; copied into the room and switched on), enabled alone (switches the room's own " +
            "equalizer on or off, its bands kept), or the whole equalizer. For a small change (more bass, less treble) read " +
            "the room's from list_rooms, change its bands' gains and send it whole. Keep the preamp at or below minus the " +
            "highest boost, so that it does not clip.",
            async ([Description("The room's client id, from list_rooms.")] string room,
                [Description("A profile's name.")] string? profile = null, bool? enabled = null,
                [Description("The whole equalizer: enabled, preampDb and its bands.")] CoreEqualizer? equalizer = null,
                CancellationToken cancellationToken = default) =>
            {
                if ((profile is null ? 0 : 1) + (enabled is null ? 0 : 1) + (equalizer is null ? 0 : 1) != 1)
                    throw Refused("invalidArgument", "exactly one of profile, enabled or equalizer");
                if (profile is not null)
                    return Ok(await house.ApplyEqualizerProfileAsync(room, profile, cancellationToken).ConfigureAwait(false));
                if (equalizer is null)
                {
                    var current = Ok(await house.RoomAsync(room, cancellationToken).ConfigureAwait(false)).Desired.Equalizer ??
                        throw Refused("invalidArgument", $"room {room} has no equalizer yet: send a profile or the whole equalizer");
                    equalizer = current with { Enabled = enabled!.Value };
                }
                return Ok(await house.SetEqualizerAsync(room, equalizer, cancellationToken).ConfigureAwait(false));
            });

        Add(McpTier.Control, "set_zone_rooms", "Sets which rooms play in a zone: the full list, never a change to it. " +
            "The zone must exist: every room has one of its own, and a zone left with no room is deleted.",
            async ([Description(ZoneParameter)] string zone, [Description("Every room's client id that is to play in the zone.")] string[] rooms,
                CancellationToken cancellationToken = default) =>
                AroundCurrent(Ok(await zones.SetClientsAsync(zone, rooms, cancellationToken).ConfigureAwait(false))));

        // --- modify --------------------------------------------------------------

        Add(McpTier.Modify, "create_playlist", "Creates an empty stored playlist.",
            (string name) => Ok(playlists.CreatePlaylist(name)));

        Add(McpTier.Modify, "save_queue_as_playlist", "Saves a zone's queue (its tracks and books, in order; stations " +
            "are left out) as a stored playlist, in a folder or at the top level. A name in use there is refused unless overwrite, " +
            "which replaces that playlist's entries.",
            ([Description(ZoneParameter)] string zone, string name, bool overwrite = false, long? folder = null) =>
                Ok(playlists.SaveAs(name, Ok(queues.GetQueue(zone)).Items, overwrite, folder)));

        Add(McpTier.Modify, "add_to_playlist", "Adds tracks, or a playable browse node, to a stored playlist, at the end " +
            "or at an index. A node of audiobook tracks is one entry (the book), its parts kept in order.",
            async (long playlist, Track[]? tracks = null, NodeRef? node = null, int? index = null,
                CancellationToken cancellationToken = default) =>
            {
                if ((tracks is null) == (node is null))
                    throw Refused("invalidArgument", "exactly one of tracks or node");
                if (tracks is not null)
                {
                    return tracks.Length == 0
                        ? throw Refused("invalidArgument", "tracks is empty")
                        : Ok(playlists.AddEntries(playlist, tracks, index));
                }
                var expanded = Ok(await player.ExpandAsync(node!.Provider, node.Id, player.Settings.GroupBooks,
                    cancellationToken).ConfigureAwait(false));
                return Ok(expanded.Group is { } book
                    ? playlists.AddGroup(playlist, book, index)
                    : playlists.AddEntries(playlist, expanded.Tracks, index));
            });

        Add(McpTier.Modify, "add_favorite", "Makes a track, a station or a book (a browse node of audiobook tracks) " +
            "a favourite: exactly one of the three.",
            async (Track? track = null, RadioStation? station = null, NodeRef? node = null,
                [Description(UserParameter)] string? user = null, CancellationToken cancellationToken = default) =>
            {
                switch (track, station, node)
                {
                    case ({ } t, null, null):
                        return Ok(playlists.AddFavorite(t, user));
                    case (null, { } s, null):
                        return Ok(playlists.AddFavorite(s, user));
                    case (null, null, { } n):
                        var expanded = Ok(await player.ExpandAsync(n.Provider, n.Id, group: true, cancellationToken)
                            .ConfigureAwait(false));
                        return expanded.Group is { } book
                            ? Ok(playlists.AddFavorite(book, user))
                            : throw Refused("invalidArgument", $"{n.Id} is no book (two or more audiobook tracks)");
                    default:
                        throw Refused("invalidArgument", "exactly one of track, station or node");
                }
            });

        Add(McpTier.Modify, "like", "Likes a track, an album, an artist, a station or a book (a browse node of " +
            "audiobook tracks): exactly one. An album that is a book is liked as the book.",
            async (Track? track = null, Album? album = null, Artist? artist = null, RadioStation? station = null,
                NodeRef? node = null, [Description(UserParameter)] string? user = null,
                CancellationToken cancellationToken = default) =>
                Ok((track, album, artist, station, node) switch
                {
                    ({ } t, null, null, null, null) => likes.Add(t, user),
                    (null, { } a, null, null, null) =>
                        await books.LikeAlbumAsync(a, user, cancellationToken).ConfigureAwait(false),
                    (null, null, { } r, null, null) => likes.Add(r, user),
                    (null, null, null, { } s, null) => likes.Add(s, user),
                    (null, null, null, null, { } n) =>
                        await books.LikeBookAsync(n.Provider, n.Id, user, cancellationToken).ConfigureAwait(false),
                    _ => throw Refused("invalidArgument", "exactly one of track, album, artist, station or node"),
                }).Like);

        Add(McpTier.Modify, "save_equalizer_profile", "Saves an equalizer as a profile of the household's, by name: " +
            "a new one, or a household profile of that name replaced. Built-in profiles cannot be replaced.",
            (string name, double preampDb, CoreEqBand[] bands) => Ok(equalizerProfiles.Save(name, preampDb, bands)));

        Add(McpTier.Modify, "add_station", "Adds a web radio station by its stream URL (an http or https audio stream); " +
            "a URL already added answers that station. Answers the station, " +
            "ready for add_to_queue. Search first: a station the providers already have needs no adding.",
            ([Description("The station's name.")] string name, [Description("The stream's URL.")] Uri streamUrl,
                Uri? homepage = null, string? genre = null, [Description("The station's logo image URL.")] Uri? logo = null) =>
            {
                var added = Ok(stations.Add(new NewStation(name, streamUrl, homepage, genre, logo)));
                return WebRadioProvider.ToStation(added);
            });

        // Undoing a like or a favourite is the same tier as making one: the
        // agent may do both (the owner's decision, 2026-10-01).
        Add(McpTier.Modify, "unlike", "Removes a like.",
            ([Description("The like id, from list_likes.")] long like) => new { like, removed = Ok(likes.Remove(like)) });

        Add(McpTier.Modify, "remove_favorite", "Removes a favourite.",
            ([Description("The favourite id, from list_favorites.")] long favorite) =>
                new { favorite, removed = Ok(playlists.RemoveFavorite(favorite)) });

        // --- destructive ---------------------------------------------------------

        Add(McpTier.Destructive, "delete_playlist", "Deletes a stored playlist.",
            (long playlist) => new { playlist, deleted = Ok(playlists.DeletePlaylist(playlist)) });

        Add(McpTier.Destructive, "remove_playlist_entry", "Removes one entry from a stored playlist.",
            (long playlist, [Description("The entry id, from get_playlist_entries.")] long entry) =>
                Ok(playlists.RemoveEntry(playlist, entry)));


        Add(McpTier.Destructive, "delete_zone", "Deletes a zone and its queue; its rooms stop playing, each in a zone of its own.",
            async ([Description(ZoneParameter)] string zone, CancellationToken cancellationToken = default) =>
                new { zone, deleted = Ok(await zones.DeleteAsync(zone, cancellationToken).ConfigureAwait(false)) });

        void AddTransport(string name, string description,
            Func<string, CancellationToken, Task<Result<QueueState, QueueError>>> operation) =>
            Add(McpTier.Control, name, description, async ([Description(ZoneParameter)] string zone, CancellationToken cancellationToken = default) =>
                AroundCurrent(Ok(await operation(zone, cancellationToken).ConfigureAwait(false))));
    }

    public IReadOnlyList<MultiroomTool> Tools => _tools;

    public McpTier? TierOf(string name) => _tools.FirstOrDefault(t => t.Name == name)?.Tier;

    // A tool's zone argument as a person names it (House.ResolveZone).
    public string ResolveZone(string zone) => _house.ResolveZone(zone);

    private void Add(McpTier tier, string name, string description, Delegate handler) =>
        _tools.Add(new MultiroomTool(name, tier, McpServerTool.Create(handler, new McpServerToolCreateOptions
        {
            Name = name,
            Description = description,
            SerializerOptions = _json,
            ReadOnly = tier == McpTier.Read,
            Destructive = tier == McpTier.Destructive,
        })));

    private static async Task<QueueAdded> AddAsync(IQueueEngine queues, PlaylistPlayer player, string zone, Track[]? tracks,
        RadioStation? station, long? playlist, long? favorite, NodeRef? node, QueuePlacement placement, int? index,
        bool fromStart, CancellationToken cancellationToken)
    {
        var kinds = (tracks is null ? 0 : 1) + (station is null ? 0 : 1) + (playlist is null ? 0 : 1) +
            (favorite is null ? 0 : 1) + (node is null ? 0 : 1);
        if (kinds != 1)
            throw Refused("invalidArgument", "exactly one of tracks, station, playlist, favorite or node");
        if (index is not null && (placement != QueuePlacement.End || tracks is null && station is null))
            throw Refused("invalidArgument", "index places tracks or a station at the end only");

        if (playlist is { } p)
        {
            var added = Ok(await player.EnqueuePlaylistAsync(zone, p, placement, cancellationToken, fromStart).ConfigureAwait(false));
            return new QueueAdded(AroundCurrent(added.Queue), RefusedEntries: added.Refused);
        }
        if (favorite is { } f)
            return new QueueAdded(AroundCurrent(Ok(await player.EnqueueFavoriteAsync(zone, f, placement, cancellationToken, fromStart)
                .ConfigureAwait(false))));
        if (node is { } n)
        {
            var added = Ok(await player.EnqueueNodeAsync(zone, n.Provider, n.Id, placement, cancellationToken, fromStart: fromStart)
                .ConfigureAwait(false));
            return new QueueAdded(AroundCurrent(added.Queue), added.Queued, RefusedTracks: added.Refused,
                Truncated: added.Truncated);
        }
        if (station is { } s)
        {
            return new QueueAdded(AroundCurrent(Ok(placement switch
            {
                QueuePlacement.Next => await queues.AddNextAsync(zone, s, cancellationToken).ConfigureAwait(false),
                QueuePlacement.Replace => await queues.ReplaceAsync(zone, s, cancellationToken).ConfigureAwait(false),
                _ => await queues.AddAsync(zone, s, index, cancellationToken).ConfigureAwait(false),
            })));
        }
        if (tracks!.Length == 0)
            throw Refused("invalidArgument", "tracks is empty");
        return new QueueAdded(AroundCurrent(Ok(placement switch
        {
            QueuePlacement.Next => await queues.AddNextAsync(zone, tracks, cancellationToken).ConfigureAwait(false),
            QueuePlacement.Replace => await queues.ReplaceAsync(zone, tracks, cancellationToken).ConfigureAwait(false),
            _ => await queues.AddAsync(zone, tracks, index, cancellationToken).ConfigureAwait(false),
        })), tracks.Length);
    }

    // The queue from its current item on, at most QueueWindow items.
    private static QueuePage AroundCurrent(QueueState queue)
    {
        var at = queue.Current is { } current ? queue.Items.ToList().FindIndex(i => i.Id == current) : 0;
        return Paged(queue, Math.Max(at, 0), QueueWindow);
    }

    private static QueuePage Paged(QueueState queue, int offset, int limit)
    {
        var items = queue.Items.Skip(offset).Take(limit).ToList();
        List<GroupSummary> groups = [.. items.Where(i => i.Group is not null).Select(i => new GroupSummary(i.Id.Value,
            i.Group!.Parts.Count, i.Group.Duration is { } length ? (long)length.TotalMilliseconds : null,
            i.Group.Saved?.PositionMs))];
        items = [.. items.Select(i => i.Group is { } book ? i with { Group = book with { Parts = [] } } : i)];
        return new QueuePage(queue with { Items = items }, offset, queue.Items.Count, items.Count < queue.Items.Count,
            Groups: groups.Count == 0 ? null : groups);
    }

    private static ToolList<T> Page<T>(IReadOnlyList<T> all, int offset, int limit)
    {
        var items = all.Skip(offset).Take(limit).ToList();
        return new ToolList<T>(items, offset, all.Count, items.Count < all.Count);
    }

    private static int Offset(int offset) =>
        offset >= 0 ? offset : throw Refused("invalidArgument", $"offset: 0 or more, was {offset}");

    private static int Limit(int? limit) => limit switch
    {
        null => DefaultLimit,
        >= 1 and <= MaxLimit => limit.Value,
        _ => throw Refused("invalidArgument", $"limit: 1 to {MaxLimit}, was {limit}"),
    };

    private static string? Text(string? text) => string.IsNullOrWhiteSpace(text) ? null : text;

    // A module's failure as the tool's error: its outward code and message.
    private static T Ok<T, TCode>(Result<T, TCode> result)
        where TCode : struct, Enum
    {
        if (result.IsSuccess)
            return result.Value;
        var error = ApiError.From(result.Failure);
        throw Refused(error.Error, error.Message);
    }

    private static McpException Refused(string code, string message) => new($"{code}: {message}");
}
