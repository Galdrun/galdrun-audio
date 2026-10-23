using Microsoft.Data.Sqlite;
using Multiroom.Server.Abstractions;

namespace Multiroom.Server.Library;

// Favourites and playlists, in the library's database beside the indexer's
// tables (Migrations/0003). Each call is one connection and, when it writes,
// one transaction, so positions stay 0 to n-1 whatever fails halfway. A write
// waits for the indexer's open batch to commit (SQLite has one writer at a
// time), within the connection's command timeout.
//
// Nothing here resolves: an entry is listed from its snapshot whether its
// provider still has it or not. Refusing to play it is the enqueuer's.
public sealed class PlaylistStore(LibraryDatabase database)
{
    private const string PlaylistColumns = """
        SELECT p.id, p.name, p.position, p.created,
               (SELECT COUNT(*) FROM playlist_item i WHERE i.playlist_id = p.id), p.folder_id
        FROM playlist p
        """;

    private const string EntryColumns = """
        SELECT id, provider_id, provider_item_id, title, artist, album, duration_ms,
               codec, sample_rate, bit_depth, channels, bitrate, media_type, kind
        FROM playlist_item WHERE playlist_id = $playlist
        """;

    private const string FavoritePartColumns = """
        SELECT favorite_id, provider_id, provider_item_id, title, artist, album, duration_ms,
               codec, sample_rate, bit_depth, channels, bitrate, media_type, chapter, start_ms, end_ms
        FROM favorite_part
        """;

    private const string FavoriteColumns = """
        SELECT id, kind, provider_id, provider_item_id, title, artist, album, duration_ms,
               codec, sample_rate, bit_depth, channels, bitrate, media_type, added, user
        FROM favorite
        """;

    private const string PartColumns = """
        SELECT item_id, provider_id, provider_item_id, title, artist, album, duration_ms,
               codec, sample_rate, bit_depth, channels, bitrate, media_type, chapter, start_ms, end_ms
        FROM playlist_item_part
        """;

    private const string TrackKind = "track";
    private const string GroupKind = "group";
    private const string StationKind = "station";

    // --- playlists ----------------------------------------------------------

    // In their order.
    public Result<IReadOnlyList<Playlist>, LibraryError> Playlists() =>
        Read(c => List(Rows(c, null, PlaylistColumns + " ORDER BY p.position", ReadPlaylist)));

    // Placed after the others, in the folder (null: at the top). Names need not be unique.
    public Result<Playlist, LibraryError> CreatePlaylist(string name, long? folder = null)
    {
        if (Trimmed(name) is not { } trimmed)
            return NoName();
        return Write((c, t) =>
        {
            if (folder is { } f && !FolderExists(c, t, f))
                return NoFolder(f);
            var id = Rows(c, t, """
                INSERT INTO playlist (name, position, created, folder_id)
                VALUES ($name, (SELECT COUNT(*) FROM playlist), $created, $folder)
                RETURNING id
                """, r => r.GetInt64(0), ("$name", trimmed), ("$created", DateTime.UtcNow.Ticks), ("$folder", folder))[0];
            return PlaylistById(c, t, id);
        });
    }

    // Into the folder, or to the top when null.
    public Result<Playlist, LibraryError> MovePlaylistTo(long playlist, long? folder) =>
        Write((c, t) =>
        {
            if (folder is { } f && !FolderExists(c, t, f))
                return NoFolder(f);
            return Execute(c, t, "UPDATE playlist SET folder_id = $folder WHERE id = $id", ("$folder", folder), ("$id", playlist)) == 0
                ? NoPlaylist(playlist)
                : PlaylistById(c, t, playlist);
        });

    public Result<Playlist, LibraryError> RenamePlaylist(long playlist, string name)
    {
        if (Trimmed(name) is not { } trimmed)
            return NoName();
        return Write((c, t) => Execute(c, t, "UPDATE playlist SET name = $name WHERE id = $id",
                ("$name", trimmed), ("$id", playlist)) == 0
            ? NoPlaylist(playlist)
            : PlaylistById(c, t, playlist));
    }

    public Result<Playlist, LibraryError> MovePlaylist(long playlist, int index) =>
        Write((c, t) =>
        {
            if (PlaylistById(c, t, playlist) is not { IsSuccess: true } found)
                return NoPlaylist(playlist);
            var count = Count(c, t, "SELECT COUNT(*) FROM playlist");
            if (index < 0 || index >= count)
                return OutOfRange(index, count - 1);
            MoveRow(c, t, "playlist", "1", playlist, found.Value.Position, index);
            return PlaylistById(c, t, playlist);
        });

    // Its entries with it.
    public Result<bool, LibraryError> DeletePlaylist(long playlist) =>
        Write<bool>((c, t) =>
        {
            if (PlaylistById(c, t, playlist) is not { IsSuccess: true } found)
                return NoPlaylist(playlist);
            Execute(c, t, "DELETE FROM playlist WHERE id = $id", ("$id", playlist));
            Shift(c, t, "playlist", "1", found.Value.Position + 1, int.MaxValue, -1);
            return true;
        });

    // --- folders ------------------------------------------------------------

    private static readonly FolderTree FolderTable = new("playlist_folder", "playlist");

    // By name, each with the folder it lies in.
    public Result<IReadOnlyList<CollectionFolder>, LibraryError> Folders() => Read(c => List(FolderTable.All(c)));

    // In the parent, or at the top when null. Two folders in one place never
    // share a name (in any case); playlists may.
    public Result<CollectionFolder, LibraryError> CreateFolder(string name, long? parent = null) =>
        Write((c, t) => FolderTable.Create(c, t, name, parent));

    public Result<CollectionFolder, LibraryError> RenameFolder(long folder, string name) =>
        Write((c, t) => FolderTable.Rename(c, t, folder, name));

    // Into the parent, or to the top when null; never into itself or a folder within it.
    public Result<CollectionFolder, LibraryError> MoveFolder(long folder, long? parent) =>
        Write((c, t) => FolderTable.Move(c, t, folder, parent));

    // With every folder within it and every playlist in any of them, their
    // entries too; the other playlists close up their positions.
    public Result<bool, LibraryError> DeleteFolder(long folder) =>
        Write<bool>((c, t) =>
        {
            if (!FolderExists(c, t, folder))
                return NoFolder(folder);
            var doomed = Rows(c, t, FolderTable.Within + " SELECT position FROM playlist WHERE folder_id IN (SELECT id FROM tree) ORDER BY position DESC",
                r => r.GetInt32(0), ("$id", folder));
            Execute(c, t, FolderTable.Within + " DELETE FROM playlist WHERE folder_id IN (SELECT id FROM tree)", ("$id", folder));
            // From the last up, so each shift sees the positions as they were.
            foreach (var position in doomed)
                Shift(c, t, "playlist", "1", position + 1, int.MaxValue, -1);
            FolderTable.Delete(c, t, folder);
            return true;
        });

    private static bool FolderExists(SqliteConnection c, SqliteTransaction? t, long folder) => FolderTable.Exists(c, t, folder);

    private static Failure<LibraryError> NoFolder(long folder) => FolderTable.NotFound(folder);

    // --- entries ------------------------------------------------------------

    // In playlist order, each from its snapshot.
    public Result<IReadOnlyList<PlaylistEntry>, LibraryError> Entries(long playlist) =>
        Read(c => EntriesOf(c, null, playlist));

    // At index, or at the end when null. The tracks are snapshotted as given.
    public Result<IReadOnlyList<PlaylistEntry>, LibraryError> AddEntries(long playlist, IReadOnlyList<Track> tracks,
        int? index)
    {
        if (tracks.Count == 0)
            return Result.Fail(LibraryError.InvalidArgument, "no tracks to add");
        if (tracks.FirstOrDefault(t => t.ProviderId.Length == 0 || t.ProviderItemId.Length == 0) is { } unnamed)
            return Result.Fail(LibraryError.InvalidArgument, $"{unnamed.Title} has no provider-scoped id");
        return Write((c, t) =>
        {
            if (!Exists(c, t, playlist))
                return NoPlaylist(playlist);
            var count = EntryCount(c, t, playlist);
            var at = index ?? count;
            if (at < 0 || at > count)
                return OutOfRange(at, count);
            Shift(c, t, "playlist_item", "playlist_id = $scope", at, int.MaxValue, tracks.Count, playlist);
            foreach (var (track, offset) in tracks.Select((track, offset) => (track, offset)))
                InsertTrack(c, t, playlist, at + offset, track);
            return EntriesOf(c, t, playlist);
        });
    }

    // A group as one entry (phase 31), at index or at the end: its node and
    // snapshot, and its parts as they are now, stored in their order.
    public Result<IReadOnlyList<PlaylistEntry>, LibraryError> AddGroup(long playlist, QueueGroup group, int? index)
    {
        if (group.Parts.Count == 0)
            return Result.Fail(LibraryError.InvalidArgument, $"{group.Title} has no parts");
        if (group.Parts.FirstOrDefault(p => p.Track.ProviderId.Length == 0 || p.Track.ProviderItemId.Length == 0) is { } unnamed)
            return Result.Fail(LibraryError.InvalidArgument, $"{unnamed.Track.Title} has no provider-scoped id");
        return Write((c, t) =>
        {
            if (!Exists(c, t, playlist))
                return NoPlaylist(playlist);
            var count = EntryCount(c, t, playlist);
            var at = index ?? count;
            if (at < 0 || at > count)
                return OutOfRange(at, count);
            Shift(c, t, "playlist_item", "playlist_id = $scope", at, int.MaxValue, 1, playlist);
            InsertGroup(c, t, playlist, at, group);
            return EntriesOf(c, t, playlist);
        });
    }

    // A queue saved as a playlist: its tracks and groups, in order, under the
    // name in the folder (null: at the top) -- a new playlist, or, with
    // overwrite, the first of that name (in any case) in that folder emptied
    // and filled again. Stations are not playlist entries
    // and are left out; a list with nothing else is refused.
    public Result<Playlist, LibraryError> SaveAs(string name, IReadOnlyList<QueueItem> items, bool overwrite, long? folder = null)
    {
        if (Trimmed(name) is not { } trimmed)
            return NoName();
        var kept = items.Where(i => i.Track is not null || i.Group is { Parts.Count: > 0 }).ToList();
        if (kept.Count == 0)
            return Result.Fail(LibraryError.InvalidArgument, "nothing to save: a playlist holds tracks and books, not stations");
        if (kept.SelectMany(i => i.Track is { } track ? [track] : i.Group!.Parts.Select(p => p.Track))
                .FirstOrDefault(t => t.ProviderId.Length == 0 || t.ProviderItemId.Length == 0) is { } unnamed)
            return Result.Fail(LibraryError.InvalidArgument, $"{unnamed.Title} has no provider-scoped id");
        return Write((c, t) =>
        {
            if (folder is { } f && !FolderExists(c, t, f))
                return NoFolder(f);
            var existing = Rows(c, t, "SELECT id FROM playlist WHERE name = $name COLLATE NOCASE AND folder_id IS $folder ORDER BY position",
                r => r.GetInt64(0), ("$name", trimmed), ("$folder", folder));
            long playlist;
            if (existing.Count == 0)
            {
                playlist = Rows(c, t, """
                    INSERT INTO playlist (name, position, created, folder_id)
                    VALUES ($name, (SELECT COUNT(*) FROM playlist), $created, $folder)
                    RETURNING id
                    """, r => r.GetInt64(0), ("$name", trimmed), ("$created", DateTime.UtcNow.Ticks), ("$folder", folder))[0];
            }
            else if (!overwrite)
            {
                return Result.Fail(LibraryError.AlreadyExists, $"a playlist named {trimmed} is already there");
            }
            else
            {
                playlist = existing[0];
                Execute(c, t, """
                    DELETE FROM playlist_item_part WHERE item_id IN (SELECT id FROM playlist_item WHERE playlist_id = $id)
                    """, ("$id", playlist));
                Execute(c, t, "DELETE FROM playlist_item WHERE playlist_id = $id", ("$id", playlist));
            }
            foreach (var (item, at) in kept.Select((item, at) => (item, at)))
            {
                if (item.Track is { } track)
                    InsertTrack(c, t, playlist, at, track);
                else
                    InsertGroup(c, t, playlist, at, item.Group!);
            }
            return PlaylistById(c, t, playlist);
        });
    }

    private static void InsertTrack(SqliteConnection c, SqliteTransaction t, long playlist, int position, Track track) =>
        Execute(c, t, """
            INSERT INTO playlist_item
                (playlist_id, position, provider_id, provider_item_id, title, artist, album, duration_ms,
                 codec, sample_rate, bit_depth, channels, bitrate, media_type)
            VALUES ($playlist, $position, $provider, $item, $title, $artist, $album, $duration,
                $codec, $sampleRate, $bitDepth, $channels, $bitrate, $mediaType)
            """, [("$playlist", playlist), ("$position", position), ("$provider", track.ProviderId),
            ("$item", track.ProviderItemId), ("$title", track.Title), ("$artist", track.Artist),
            ("$album", track.Album), ("$duration", (long?)track.Duration?.TotalMilliseconds),
            ("$mediaType", LibraryCatalog.NameOf(track.MediaType)),
            .. LibraryCatalog.FormatValues(track.Format)]);

    // Its node and snapshot as one entry, and its parts as they are now.
    private static void InsertGroup(SqliteConnection c, SqliteTransaction t, long playlist, int at, QueueGroup group)
    {
        var (provider, node) = group.Source?.Split(':', 2) is [var p, var n] ? (p, n) : ("", "");
        var item = Rows(c, t, """
            INSERT INTO playlist_item
                (playlist_id, position, provider_id, provider_item_id, title, artist, duration_ms, media_type, kind)
            VALUES ($playlist, $position, $provider, $item, $title, $artist, $duration, $mediaType, 'group')
            RETURNING id
            """, r => r.GetInt64(0), ("$playlist", playlist), ("$position", at), ("$provider", provider),
            ("$item", node), ("$title", group.Title), ("$artist", group.Artist),
            ("$duration", (long?)group.Duration?.TotalMilliseconds), ("$mediaType", LibraryCatalog.NameOf<MediaType>(group.Type)))[0];
        InsertParts(c, t, "playlist_item_part", "item_id", item, group.Parts);
    }

    public Result<IReadOnlyList<PlaylistEntry>, LibraryError> RemoveEntry(long playlist, long entry) =>
        Write((c, t) =>
        {
            if (!Exists(c, t, playlist))
                return NoPlaylist(playlist);
            if (EntryPosition(c, t, playlist, entry) is not { } position)
                return NoEntry(playlist, entry);
            Execute(c, t, "DELETE FROM playlist_item WHERE id = $id", ("$id", entry));
            Shift(c, t, "playlist_item", "playlist_id = $scope", position + 1, int.MaxValue, -1, playlist);
            return EntriesOf(c, t, playlist);
        });

    public Result<IReadOnlyList<PlaylistEntry>, LibraryError> MoveEntry(long playlist, long entry, int index) =>
        Write((c, t) =>
        {
            if (!Exists(c, t, playlist))
                return NoPlaylist(playlist);
            if (EntryPosition(c, t, playlist, entry) is not { } position)
                return NoEntry(playlist, entry);
            var count = EntryCount(c, t, playlist);
            if (index < 0 || index >= count)
                return OutOfRange(index, count - 1);
            MoveRow(c, t, "playlist_item", "playlist_id = $scope", entry, position, index, playlist);
            return EntriesOf(c, t, playlist);
        });

    // --- favourites ---------------------------------------------------------

    // A favourite added (not one that was there already) or removed, by
    // whoever: the REST API and the MCP tools alike (phase 36).
    public event EventHandler<Favorite>? FavoriteAdded;

    public event EventHandler<Favorite>? FavoriteRemoved;

    // In the order they were added, in a user's scope as likes are read
    // (UserStore.Scope): the household's, a user's with the household's, or
    // everyone's.
    public Result<IReadOnlyList<Favorite>, LibraryError> Favorites(string? user = UserStore.Household) =>
        Read<IReadOnlyList<Favorite>>(c =>
        {
            var scope = UserStore.Canonical(c, null, user, everyone: true);
            if (!scope.IsSuccess)
                return scope.Failure;
            var rows = Rows(c, null, FavoriteColumns + " WHERE " + UserStore.Scope(scope.Value) + " ORDER BY id",
                ReadFavorite, ("$user", scope.Value));
            return WithParts(c, null, UserStore.Merged(rows, scope.Value, KeyOf, f => f.User));
        });

    public Result<Favorite, LibraryError> GetFavorite(long favorite) =>
        Read<Favorite>(c => FavoriteBy(c, null, "id = $id", ("$id", favorite)) is { } found
            ? found
            : NoFavorite(favorite));

    // A track already that user's favourite answers the favourite it is,
    // unchanged.
    public Result<Favorite, LibraryError> AddFavorite(Track track, string? user = UserStore.Household) =>
        AddFavorite(TrackKind, track.ProviderId, track.ProviderItemId, track.Title, track.Artist, track.Album,
            (long?)track.Duration?.TotalMilliseconds, track.Format, track.MediaType, user);

    public Result<Favorite, LibraryError> AddFavorite(RadioStation station, string? user = UserStore.Household) =>
        AddFavorite(StationKind, station.ProviderId, station.ProviderItemId, station.Name, null, null, null, null,
            null, user);

    // A group (a book) by the node it was expanded from, with its parts as
    // they are now. A node already a favourite answers the favourite it is,
    // its parts unchanged.
    public Result<Favorite, LibraryError> AddFavorite(QueueGroup group, string? user = UserStore.Household)
    {
        if (group.Parts.Count == 0)
            return Result.Fail(LibraryError.InvalidArgument, $"{group.Title} has no parts");
        if (group.Source?.Split(':', 2) is not [var provider, var node] || provider.Length == 0 || node.Length == 0)
            return Result.Fail(LibraryError.InvalidArgument, $"{group.Title} names no node it came from");
        if (group.Parts.FirstOrDefault(p => p.Track.ProviderId.Length == 0 || p.Track.ProviderItemId.Length == 0) is { } unnamed)
            return Result.Fail(LibraryError.InvalidArgument, $"{unnamed.Track.Title} has no provider-scoped id");
        return Announced(Write<(Favorite, bool)>((c, t) =>
        {
            var owner = UserStore.Canonical(c, t, user);
            if (!owner.IsSuccess)
                return owner.Failure;
            var added = Rows(c, t, """
                INSERT INTO favorite (kind, provider_id, provider_item_id, title, artist, duration_ms, added, media_type, user)
                VALUES ('group', $provider, $item, $title, $artist, $duration, $added, $mediaType, $user)
                ON CONFLICT (kind, provider_id, provider_item_id, user) DO NOTHING
                RETURNING id
                """, r => r.GetInt64(0), ("$provider", provider), ("$item", node), ("$title", group.Title),
                ("$artist", group.Artist), ("$duration", (long?)group.Duration?.TotalMilliseconds),
                ("$added", DateTime.UtcNow.Ticks), ("$mediaType", LibraryCatalog.NameOf<MediaType>(group.Type)),
                ("$user", owner.Value));
            if (added is [var id])
                InsertParts(c, t, "favorite_part", "favorite_id", id, group.Parts);
            return (FavoriteBy(c, t, "kind = 'group' AND provider_id = $provider AND provider_item_id = $item AND user = $user",
                ("$provider", provider), ("$item", node), ("$user", owner.Value))!, added.Count > 0);
        }));
    }

    // The favourite id of each item that is a favourite in the user's scope
    // (the user's own before the household's); the others, albums and
    // artists (never favourites) included, are left out.
    public Result<Dictionary<ItemRef, long>, LibraryError> Favorited(IReadOnlyList<ItemRef> items,
        string? user = UserStore.Household) =>
        Read<Dictionary<ItemRef, long>>(c =>
        {
            var scope = UserStore.Canonical(c, null, user, everyone: true);
            if (!scope.IsSuccess)
                return scope.Failure;
            var found = new Dictionary<ItemRef, long>();
            foreach (var item in items.Distinct().Where(i => i.Kind is LikeKind.Track or LikeKind.Station))
            {
                if (Rows(c, null, $"""
                        SELECT id FROM favorite WHERE kind = $kind AND provider_id = $provider AND provider_item_id = $item
                          AND {UserStore.Scope(scope.Value)}
                        ORDER BY user = '', id LIMIT 1
                        """, r => (long?)r.GetInt64(0), ("$kind", LikeStore.KindName(item.Kind)), ("$provider", item.ProviderId),
                        ("$item", item.ProviderItemId), ("$user", scope.Value)) is [{ } id])
                {
                    found[item] = id;
                }
            }
            return found;
        });

    // The tracks among `items` that are in a playlist, as an entry or a
    // book's part, or a part of a favourite book; by anyone. Search answers
    // them first (KeptTracks).
    public Result<HashSet<(string ProviderId, string ProviderItemId)>, LibraryError> InPlaylists(
        IReadOnlyList<ItemRef> items) =>
        Read<HashSet<(string, string)>>(c =>
        {
            var found = new HashSet<(string, string)>();
            foreach (var item in items.Distinct().Where(i => i.Kind == LikeKind.Track))
            {
                if (Rows(c, null, """
                        SELECT 1 WHERE EXISTS (SELECT 1 FROM playlist_item WHERE provider_id = $provider AND provider_item_id = $item)
                            OR EXISTS (SELECT 1 FROM playlist_item_part WHERE provider_id = $provider AND provider_item_id = $item)
                            OR EXISTS (SELECT 1 FROM favorite_part WHERE provider_id = $provider AND provider_item_id = $item)
                        """, _ => true, ("$provider", item.ProviderId), ("$item", item.ProviderItemId)).Count > 0)
                {
                    found.Add((item.ProviderId, item.ProviderItemId));
                }
            }
            return found;
        });

    public Result<bool, LibraryError> RemoveFavorite(long favorite)
    {
        var removed = Write<Favorite>((c, t) => FavoriteBy(c, t, "id = $id", ("$id", favorite)) is { } found &&
            Execute(c, t, "DELETE FROM favorite WHERE id = $id", ("$id", favorite)) > 0
                ? found
                : NoFavorite(favorite));
        if (!removed.IsSuccess)
            return removed.Failure;
        FavoriteRemoved?.Invoke(this, removed.Value);
        return true;
    }

    private Result<Favorite, LibraryError> AddFavorite(string kind, string providerId, string itemId, string title,
        string? artist, string? album, long? durationMs, AudioFormat? format, MediaType? mediaType, string? user)
    {
        if (providerId.Length == 0 || itemId.Length == 0)
            return Result.Fail(LibraryError.InvalidArgument, $"{title} has no provider-scoped id");
        return Announced(Write<(Favorite, bool)>((c, t) =>
        {
            var owner = UserStore.Canonical(c, t, user);
            if (!owner.IsSuccess)
                return owner.Failure;
            var added = Execute(c, t, """
                INSERT INTO favorite (kind, provider_id, provider_item_id, title, artist, album, duration_ms, added,
                    codec, sample_rate, bit_depth, channels, bitrate, media_type, user)
                VALUES ($kind, $provider, $item, $title, $artist, $album, $duration, $added,
                    $codec, $sampleRate, $bitDepth, $channels, $bitrate, $mediaType, $user)
                ON CONFLICT (kind, provider_id, provider_item_id, user) DO NOTHING
                """, [("$kind", kind), ("$provider", providerId), ("$item", itemId), ("$title", title),
                ("$artist", artist), ("$album", album), ("$duration", durationMs), ("$added", DateTime.UtcNow.Ticks),
                ("$mediaType", LibraryCatalog.NameOf(mediaType)), ("$user", owner.Value),
                .. LibraryCatalog.FormatValues(format)]) > 0;
            return (FavoriteBy(c, t, "kind = $kind AND provider_id = $provider AND provider_item_id = $item AND user = $user",
                ("$kind", kind), ("$provider", providerId), ("$item", itemId), ("$user", owner.Value))!, added);
        }));
    }

    // After the commit: a favourite that was there already announces nothing.
    private Result<Favorite, LibraryError> Announced(Result<(Favorite Favorite, bool Added), LibraryError> written)
    {
        if (!written.IsSuccess)
            return written.Failure;
        if (written.Value.Added)
            FavoriteAdded?.Invoke(this, written.Value.Favorite);
        return written.Value.Favorite;
    }

    private static (string Kind, string ProviderId, string ProviderItemId) KeyOf(Favorite f) => f switch
    {
        { Track: { } t } => (TrackKind, t.ProviderId, t.ProviderItemId),
        { Station: { } s } => (StationKind, s.ProviderId, s.ProviderItemId),
        _ => (GroupKind, f.Group!.Source!, ""),
    };

    // --- rows ---------------------------------------------------------------

    private static Result<Playlist, LibraryError> PlaylistById(SqliteConnection c, SqliteTransaction? t, long id) =>
        Rows(c, t, PlaylistColumns + " WHERE p.id = $id", ReadPlaylist, ("$id", id)) is [var found]
            ? found
            : NoPlaylist(id);

    // A group's parts are read in one more query, for the whole playlist.
    private static Result<IReadOnlyList<PlaylistEntry>, LibraryError> EntriesOf(SqliteConnection c,
        SqliteTransaction? t, long playlist)
    {
        if (!Exists(c, t, playlist))
            return NoPlaylist(playlist);
        var rows = Rows(c, t, EntryColumns + " ORDER BY position", r => (Id: r.GetInt64(0), Track: ReadTrack(r, 1),
            Group: r.GetString(13) == GroupKind), ("$playlist", playlist));
        var parts = rows.Any(e => e.Group)
            ? Rows(c, t, PartColumns + " " + """
                  WHERE item_id IN (SELECT id FROM playlist_item WHERE playlist_id = $playlist AND kind = 'group')
                  ORDER BY item_id, position
                  """, r => (Entry: r.GetInt64(0), Part: ReadPart(r)), ("$playlist", playlist))
                .ToLookup(p => p.Entry, p => p.Part)
            : null;
        return List([.. rows.Select(e => e.Group
            ? new PlaylistEntry(e.Id, null, new QueueGroup(e.Track.Title, e.Track.Artist, null,
                e.Track.MediaType ?? MediaType.Audiobook,
                e.Track.ProviderId.Length == 0 ? null : $"{e.Track.ProviderId}:{e.Track.ProviderItemId}",
                [.. parts![e.Id]]))
            : new PlaylistEntry(e.Id, e.Track))]);
    }

    private static Favorite? FavoriteBy(SqliteConnection c, SqliteTransaction? t, string where,
        params (string, object?)[] parameters) =>
        WithParts(c, t, Rows(c, t, FavoriteColumns + " WHERE " + where, ReadFavorite, parameters)).FirstOrDefault();

    // The groups among the favourites with their parts, read in one query.
    private static List<Favorite> WithParts(SqliteConnection c, SqliteTransaction? t, List<Favorite> favorites)
    {
        var groups = favorites.Where(f => f.Group is not null).Select(f => f.Id).ToList();
        if (groups.Count == 0)
            return favorites;
        var ids = string.Join(", ", groups.Select((_, i) => $"$f{i.ToString(System.Globalization.CultureInfo.InvariantCulture)}"));
        var parts = Rows(c, t, FavoritePartColumns + $" WHERE favorite_id IN ({ids}) ORDER BY favorite_id, position",
                r => (Favorite: r.GetInt64(0), Part: ReadPart(r)),
                [.. groups.Select((id, i) => ($"$f{i.ToString(System.Globalization.CultureInfo.InvariantCulture)}", (object?)id))])
            .ToLookup(p => p.Favorite, p => p.Part);
        return [.. favorites.Select(f => f.Group is { } g ? f with { Group = g with { Parts = [.. parts[f.Id]] } } : f)];
    }

    // A group's parts in order, a reference plus a snapshot each, under their owner.
    private static void InsertParts(SqliteConnection c, SqliteTransaction t, string table, string owner, long ownerId,
        IReadOnlyList<QueuePart> parts)
    {
        foreach (var (part, chapter, position) in parts.Select((part, position) => (part.Track, part.Chapter, position)))
        {
            Execute(c, t, $"""
                INSERT INTO {table}
                    ({owner}, position, provider_id, provider_item_id, title, artist, album, duration_ms,
                     codec, sample_rate, bit_depth, channels, bitrate, media_type, chapter, start_ms, end_ms)
                VALUES ($owner, $position, $provider, $item, $title, $artist, $album, $duration,
                    $codec, $sampleRate, $bitDepth, $channels, $bitrate, $mediaType, $chapter, $start, $end)
                """, [("$owner", ownerId), ("$position", position), ("$provider", part.ProviderId),
                ("$item", part.ProviderItemId), ("$title", part.Title), ("$artist", part.Artist),
                ("$album", part.Album), ("$duration", (long?)part.Duration?.TotalMilliseconds),
                ("$mediaType", LibraryCatalog.NameOf(part.MediaType)), ("$chapter", chapter?.Title),
                ("$start", chapter?.StartMs), ("$end", chapter?.EndMs),
                .. LibraryCatalog.FormatValues(part.Format)]);
        }
    }

    // A part row: the owner, the track's snapshot from 1, and its chapter
    // (phase 32) from 13.
    private static QueuePart ReadPart(SqliteDataReader r) => new(ReadTrack(r, 1), Chapter: r.IsDBNull(13)
        ? null
        : new Chapter(r.GetString(13), r.GetInt64(14), r.IsDBNull(15) ? null : r.GetInt64(15)));

    private static Playlist ReadPlaylist(SqliteDataReader r) =>
        new(r.GetInt64(0), r.GetString(1), r.GetInt32(2), new DateTimeOffset(r.GetInt64(3), TimeSpan.Zero),
            r.GetInt32(4), r.IsDBNull(5) ? null : r.GetInt64(5));

    // provider_id, provider_item_id, title, artist, album, duration_ms, the
    // five format columns and media_type from `first`. The snapshot keeps no
    // artwork; a library track's is its own reference, which the local
    // provider answers with its album's cover (or its artist's picture).
    private static Track ReadTrack(SqliteDataReader r, int first)
    {
        var provider = r.GetString(first);
        var item = r.GetString(first + 1);
        return new(provider, item, r.GetString(first + 2), StringOrNull(r, first + 3),
            StringOrNull(r, first + 4), r.IsDBNull(first + 5) ? null : TimeSpan.FromMilliseconds(r.GetInt64(first + 5)),
            provider == LibraryCatalog.LocalProvider && item.StartsWith(LibraryCatalog.TrackPrefix, StringComparison.Ordinal)
                ? new ArtworkRef(provider, item)
                : null,
            LibraryCatalog.FormatOf(r, first + 6), LibraryCatalog.EnumOrNull<MediaType>(r, first + 11));
    }

    private static Favorite ReadFavorite(SqliteDataReader r)
    {
        var added = new DateTimeOffset(r.GetInt64(14), TimeSpan.Zero);
        var user = r.GetString(15);
        return r.GetString(1) switch
        {
            TrackKind => new Favorite(r.GetInt64(0), ReadTrack(r, 2), null, added, User: user),
            // Its parts are read by WithParts.
            GroupKind => new Favorite(r.GetInt64(0), null, null, added, new QueueGroup(r.GetString(4), StringOrNull(r, 5),
                null, LibraryCatalog.EnumOrNull<MediaType>(r, 13) ?? MediaType.Audiobook, $"{r.GetString(2)}:{r.GetString(3)}", []),
                user),
            _ => new Favorite(r.GetInt64(0), null, new RadioStation(r.GetString(2), r.GetString(3), r.GetString(4)), added,
                User: user),
        };
    }

    private static bool Exists(SqliteConnection c, SqliteTransaction? t, long playlist) =>
        Count(c, t, "SELECT COUNT(*) FROM playlist WHERE id = $id", ("$id", playlist)) > 0;

    private static int EntryCount(SqliteConnection c, SqliteTransaction t, long playlist) =>
        Count(c, t, "SELECT COUNT(*) FROM playlist_item WHERE playlist_id = $playlist", ("$playlist", playlist));

    private static int? EntryPosition(SqliteConnection c, SqliteTransaction t, long playlist, long entry) =>
        Rows(c, t, "SELECT position FROM playlist_item WHERE id = $id AND playlist_id = $playlist",
            r => (int?)r.GetInt32(0), ("$id", entry), ("$playlist", playlist)).FirstOrDefault();

    // The row `id` from position `from` to `to`, the rows between moving one
    // place towards where it was.
    private static void MoveRow(SqliteConnection c, SqliteTransaction t, string table, string scope, long id,
        int from, int to, long scopeId = 0)
    {
        if (from < to)
            Shift(c, t, table, scope, from + 1, to, -1, scopeId);
        else if (from > to)
            Shift(c, t, table, scope, to, from - 1, 1, scopeId);
        Execute(c, t, $"UPDATE {table} SET position = $to WHERE id = $id", ("$to", to), ("$id", id));
    }

    // Positions low..high of the rows in scope, moved by delta.
    private static void Shift(SqliteConnection c, SqliteTransaction t, string table, string scope, int low, int high,
        int delta, long scopeId = 0) =>
        Execute(c, t, $"UPDATE {table} SET position = position + $delta WHERE {scope} AND position BETWEEN $low AND $high",
            ("$delta", delta), ("$low", low), ("$high", high), ("$scope", scopeId));

    private static int Count(SqliteConnection c, SqliteTransaction? t, string sql,
        params (string, object?)[] parameters) =>
        Rows(c, t, sql, r => r.GetInt32(0), parameters)[0];

    private static int Execute(SqliteConnection c, SqliteTransaction? t, string sql,
        params (string, object?)[] parameters)
    {
        using var command = Command(c, t, sql, parameters);
        return command.ExecuteNonQuery();
    }

    private static List<T> Rows<T>(SqliteConnection c, SqliteTransaction? t, string sql,
        Func<SqliteDataReader, T> map, params (string, object?)[] parameters)
    {
        using var command = Command(c, t, sql, parameters);
        using var reader = command.ExecuteReader();
        var rows = new List<T>();
        while (reader.Read())
            rows.Add(map(reader));
        return rows;
    }

    private static SqliteCommand Command(SqliteConnection c, SqliteTransaction? t, string sql,
        (string Name, object? Value)[] parameters)
    {
        var command = c.CreateCommand();
        command.Transaction = t;
#pragma warning disable CA2100 // Constant SQL and this class's own table names; every value is a parameter.
        command.CommandText = sql;
#pragma warning restore CA2100
        foreach (var (name, value) in parameters)
            command.Parameters.AddWithValue(name, value ?? DBNull.Value);
        return command;
    }

    private static string? StringOrNull(SqliteDataReader r, int column) =>
        r.IsDBNull(column) ? null : r.GetString(column);

    private static string? Trimmed(string name) => string.IsNullOrWhiteSpace(name) ? null : name.Trim();

    private static Result<IReadOnlyList<T>, LibraryError> List<T>(List<T> rows) => rows;

    private Result<T, LibraryError> Read<T>(Func<SqliteConnection, Result<T, LibraryError>> read)
    {
        try
        {
            using var connection = database.Open();
            return read(connection);
        }
        catch (SqliteException e)
        {
            return Result.Fail(LibraryError.UnusableDatabase, e.Message);
        }
    }

    // Committed only when the write succeeds.
    private Result<T, LibraryError> Write<T>(Func<SqliteConnection, SqliteTransaction, Result<T, LibraryError>> write)
    {
        try
        {
            using var connection = database.Open();
            using var transaction = connection.BeginTransaction();
            var result = write(connection, transaction);
            if (result.IsSuccess)
                transaction.Commit();
            return result;
        }
        catch (SqliteException e)
        {
            return Result.Fail(LibraryError.UnusableDatabase, e.Message);
        }
    }

    private static Failure<LibraryError> NoName() =>
        Result.Fail(LibraryError.InvalidArgument, "a playlist needs a name");

    private static Failure<LibraryError> NoPlaylist(long playlist) =>
        Result.Fail(LibraryError.NotFound, $"no playlist {playlist}");

    private static Failure<LibraryError> NoEntry(long playlist, long entry) =>
        Result.Fail(LibraryError.NotFound, $"no entry {entry} in playlist {playlist}");

    private static Failure<LibraryError> NoFavorite(long favorite) =>
        Result.Fail(LibraryError.NotFound, $"no favourite {favorite}");

    private static Failure<LibraryError> OutOfRange(int index, int last) =>
        Result.Fail(LibraryError.InvalidArgument, $"index {index} is outside 0..{last}");
}
