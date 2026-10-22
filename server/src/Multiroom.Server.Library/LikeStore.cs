using System.Text.Json;
using Microsoft.Data.Sqlite;
using Multiroom.Server.Abstractions;

namespace Multiroom.Server.Library;

// What is liked: one kind of item and its provider-scoped identity.
public sealed record ItemRef(LikeKind Kind, string ProviderId, string ProviderItemId);

public enum LikeKind
{
    Track,
    Album,
    Artist,
    Station,

    // Phase 37: a book, by the node it is queued by.
    Audiobook,
}

// Likes, in the library's database (Migrations/0011, 0018): a reference plus
// the liked record as JSON, each item at most once per user. One connection
// per call. Nothing here resolves: a like is listed from its snapshot whether
// its provider still has the item or not.
//
// A user (phase 36) is "" for the household or a name UserStore knows; a
// write naming anyone else is refused by name. A read's user is a scope
// (UserStore.Scope): the household's likes, a user's with the household's,
// or everyone's.
public sealed class LikeStore(LibraryDatabase database)
{
    public const int MaxLookup = 100;

    private const string Columns = "SELECT id, kind, snapshot, added, user, found FROM liked";

    private static readonly JsonSerializerOptions Snapshot = new(JsonSerializerDefaults.Web);

    // A like added (not one that was there already) or removed, by whoever:
    // the REST API and the MCP tools alike.
    public event EventHandler<LikedItem>? Added;

    public event EventHandler<LikedItem>? Removed;

    // A like whose radio song was found as a provider track (SetFound).
    public event EventHandler<LikedItem>? Changed;

    // In the order they were liked; only `kind` when given.
    public Result<IReadOnlyList<LikedItem>, LibraryError> Likes(LikeKind? kind, string? user = UserStore.Household) =>
        Run<IReadOnlyList<LikedItem>>(c =>
        {
            var scope = UserStore.Canonical(c, null, user, everyone: true);
            if (!scope.IsSuccess)
                return scope.Failure;
            var where = " WHERE " + UserStore.Scope(scope.Value) + (kind is null ? "" : " AND kind = $kind");
            var rows = Rows(c, Columns + where + " ORDER BY id", ReadLike, ("$user", scope.Value),
                ("$kind", kind is { } k ? KindName(k) : null));
            return UserStore.Merged(rows, scope.Value, RefOf, l => l.User);
        });

    // An item already liked by that user answers its like unchanged, with
    // Added false.
    public Result<(LikedItem Like, bool Added), LibraryError> Add(Track track, string? user = UserStore.Household) =>
        Add(LikeKind.Track, track.ProviderId, track.ProviderItemId, track.Title, track, user);

    public Result<(LikedItem Like, bool Added), LibraryError> Add(Album album, string? user = UserStore.Household) =>
        Add(LikeKind.Album, album.ProviderId, album.ProviderItemId, album.Title, album, user);

    public Result<(LikedItem Like, bool Added), LibraryError> Add(Artist artist, string? user = UserStore.Household) =>
        Add(LikeKind.Artist, artist.ProviderId, artist.ProviderItemId, artist.Name, artist, user);

    public Result<(LikedItem Like, bool Added), LibraryError> Add(RadioStation station, string? user = UserStore.Household) =>
        Add(LikeKind.Station, station.ProviderId, station.ProviderItemId, station.Name, station, user);

    // Phase 37: a book by its reference, the group's Source.
    public Result<(LikedItem Like, bool Added), LibraryError> Add(Audiobook book, string? user = UserStore.Household)
    {
        var item = BookRef(book.Reference);
        return item.ProviderId.Length == 0 || item.ProviderItemId.Length == 0
            ? Result.Fail(LibraryError.InvalidArgument, $"{book.Title} names no node it came from")
            : Add(LikeKind.Audiobook, item.ProviderId, item.ProviderItemId, book.Title, book, user);
    }

    // A book's reference as an item: `provider:node`, the node itself may
    // hold colons (`local:folder:C:\Books\Novel`).
    public static ItemRef BookRef(string reference) => reference.Split(':', 2) is [var provider, var node]
        ? new ItemRef(LikeKind.Audiobook, provider, node)
        : new ItemRef(LikeKind.Audiobook, "", "");

    // Phase 37: the album likes from before it, not yet asked once whether
    // their album is a book (Migrations/0018).
    public Result<IReadOnlyList<LikedItem>, LibraryError> UncheckedAlbums() =>
        Run<IReadOnlyList<LikedItem>>(c => Rows(c,
            Columns + " WHERE id IN (SELECT like_id FROM liked_album_check) ORDER BY id", ReadLike));

    // The answer for one of them: a book makes it an audiobook like (or, when
    // its user liked that book already, removes it), null leaves it an album;
    // either way it is not asked again. A like turned into a book is
    // announced as Added, with its id kept.
    public Result<bool, LibraryError> Checked(long like, Audiobook? book)
    {
        var item = book is null ? null : BookRef(book.Reference);
        var result = Run<LikedItem?>(c =>
        {
            var turned = false;
            using (var transaction = c.BeginTransaction())
            {
                if (book is not null && item!.ProviderId.Length > 0)
                {
                    turned = Execute(c, transaction, """
                        UPDATE OR IGNORE liked SET kind = 'audiobook', provider_id = $provider, provider_item_id = $item,
                            snapshot = $snapshot
                        WHERE id = $id AND kind = 'album'
                        """, ("$id", like), ("$provider", item.ProviderId), ("$item", item.ProviderItemId),
                        ("$snapshot", JsonSerializer.Serialize(book, Snapshot))) > 0;
                    if (!turned)
                        Execute(c, transaction, "DELETE FROM liked WHERE id = $id AND kind = 'album'", ("$id", like));
                }
                Execute(c, transaction, "DELETE FROM liked_album_check WHERE like_id = $id", ("$id", like));
                transaction.Commit();
            }
            return turned ? Rows(c, Columns + " WHERE id = $id", ReadLike, ("$id", like)).FirstOrDefault() : null;
        });
        if (!result.IsSuccess)
            return result.Failure;
        if (result.Value is { } changed)
            Added?.Invoke(this, changed);
        return result.Value is not null;
    }

    // Songs liked as a radio played them, whoever's, not found as a provider
    // track yet (Migrations/0027).
    public Result<IReadOnlyList<LikedItem>, LibraryError> UnfoundRadioSongs() =>
        Run<IReadOnlyList<LikedItem>>(c => Rows(c,
            Columns + " WHERE kind = 'track' AND provider_id = $provider AND found IS NULL ORDER BY id", ReadLike,
            ("$provider", RadioTrackMetadata.ProviderId)));

    // What a radio song's like was found as; announced as Changed. False when
    // the like is gone or is no radio song.
    public Result<bool, LibraryError> SetFound(long like, FoundTrack found)
    {
        var changed = Run<LikedItem?>(c =>
            Execute(c, "UPDATE liked SET found = $found WHERE id = $id AND kind = 'track' AND provider_id = $provider",
                ("$id", like), ("$provider", RadioTrackMetadata.ProviderId),
                ("$found", JsonSerializer.Serialize(found, Snapshot))) > 0
                ? Rows(c, Columns + " WHERE id = $id", ReadLike, ("$id", like)).FirstOrDefault()
                : null);
        if (!changed.IsSuccess)
            return changed.Failure;
        if (changed.Value is { } item)
            Changed?.Invoke(this, item);
        return changed.Value is not null;
    }

    public Result<bool, LibraryError> Remove(long like)
    {
        var removed = Run<LikedItem>(c => Rows(c, Columns + " WHERE id = $id", ReadLike, ("$id", like)) is [var found] &&
            Execute(c, "DELETE FROM liked WHERE id = $id", ("$id", like)) > 0
                ? found
                : Result.Fail(LibraryError.NotFound, $"no like {like}"));
        if (!removed.IsSuccess)
            return removed.Failure;
        Removed?.Invoke(this, removed.Value);
        return true;
    }

    // The like id of each item that is liked in the user's scope (the
    // user's own before the household's); the others are left out.
    public Result<Dictionary<ItemRef, long>, LibraryError> Liked(IReadOnlyList<ItemRef> items,
        string? user = UserStore.Household) =>
        Run<Dictionary<ItemRef, long>>(c =>
        {
            var scope = UserStore.Canonical(c, null, user, everyone: true);
            if (!scope.IsSuccess)
                return scope.Failure;
            var liked = new Dictionary<ItemRef, long>();
            foreach (var item in items.Distinct())
            {
                if (Rows(c, $"""
                        SELECT id FROM liked WHERE kind = $kind AND provider_id = $provider AND provider_item_id = $item
                          AND {UserStore.Scope(scope.Value)}
                        ORDER BY user = '', id LIMIT 1
                        """, r => (long?)r.GetInt64(0), ("$kind", KindName(item.Kind)), ("$provider", item.ProviderId),
                        ("$item", item.ProviderItemId), ("$user", scope.Value)) is [{ } id])
                {
                    liked[item] = id;
                }
            }
            return liked;
        });

    internal static ItemRef RefOf(LikedItem like) => like switch
    {
        { Track: { } t } => new ItemRef(LikeKind.Track, t.ProviderId, t.ProviderItemId),
        { Album: { } a } => new ItemRef(LikeKind.Album, a.ProviderId, a.ProviderItemId),
        { Artist: { } a } => new ItemRef(LikeKind.Artist, a.ProviderId, a.ProviderItemId),
        { Audiobook: { } b } => BookRef(b.Reference),
        _ => new ItemRef(LikeKind.Station, like.Station!.ProviderId, like.Station.ProviderItemId),
    };

    private Result<(LikedItem Like, bool Added), LibraryError> Add<T>(LikeKind kind, string providerId, string itemId,
        string title, T item, string? user)
    {
        if (providerId.Length == 0 || itemId.Length == 0)
            return Result.Fail(LibraryError.InvalidArgument, $"{title} has no provider-scoped id");
        var result = Run<(LikedItem, bool)>(c =>
        {
            var owner = UserStore.Canonical(c, null, user);
            if (!owner.IsSuccess)
                return owner.Failure;
            var added = Execute(c, """
                INSERT INTO liked (kind, provider_id, provider_item_id, snapshot, added, user)
                VALUES ($kind, $provider, $item, $snapshot, $added, $user)
                ON CONFLICT (kind, provider_id, provider_item_id, user) DO NOTHING
                """, ("$kind", KindName(kind)), ("$provider", providerId), ("$item", itemId),
                ("$snapshot", JsonSerializer.Serialize(item, Snapshot)), ("$added", DateTime.UtcNow.Ticks),
                ("$user", owner.Value)) > 0;
            var like = Rows(c, Columns + " WHERE kind = $kind AND provider_id = $provider AND provider_item_id = $item AND user = $user",
                ReadLike, ("$kind", KindName(kind)), ("$provider", providerId), ("$item", itemId), ("$user", owner.Value))[0];
            return (like, added);
        });
        if (result.IsSuccess && result.Value.Item2)
            Added?.Invoke(this, result.Value.Item1);
        return result;
    }

    internal static string KindName(LikeKind kind) => kind switch
    {
        LikeKind.Track => "track",
        LikeKind.Album => "album",
        LikeKind.Artist => "artist",
        LikeKind.Audiobook => "audiobook",
        _ => "station",
    };

    private static LikedItem ReadLike(SqliteDataReader r)
    {
        var id = r.GetInt64(0);
        var json = r.GetString(2);
        var added = new DateTimeOffset(r.GetInt64(3), TimeSpan.Zero);
        var user = r.GetString(4);
        return r.GetString(1) switch
        {
            "track" => new LikedItem(id, JsonSerializer.Deserialize<Track>(json, Snapshot), null, null, null, added, user,
                Found: r.IsDBNull(5) ? null : JsonSerializer.Deserialize<FoundTrack>(r.GetString(5), Snapshot)),
            "album" => new LikedItem(id, null, JsonSerializer.Deserialize<Album>(json, Snapshot), null, null, added, user),
            "artist" => new LikedItem(id, null, null, JsonSerializer.Deserialize<Artist>(json, Snapshot), null, added, user),
            "audiobook" => new LikedItem(id, null, null, null, null, added, user,
                JsonSerializer.Deserialize<Audiobook>(json, Snapshot)),
            _ => new LikedItem(id, null, null, null, JsonSerializer.Deserialize<RadioStation>(json, Snapshot), added, user),
        };
    }

    private static int Execute(SqliteConnection c, string sql, params (string Name, object? Value)[] parameters)
    {
        using var command = Command(c, sql, parameters);
        return command.ExecuteNonQuery();
    }

    private static int Execute(SqliteConnection c, SqliteTransaction t, string sql,
        params (string Name, object? Value)[] parameters)
    {
        using var command = Command(c, sql, parameters);
        command.Transaction = t;
        return command.ExecuteNonQuery();
    }

    private static List<T> Rows<T>(SqliteConnection c, string sql, Func<SqliteDataReader, T> map,
        params (string Name, object? Value)[] parameters)
    {
        using var command = Command(c, sql, parameters);
        using var reader = command.ExecuteReader();
        var rows = new List<T>();
        while (reader.Read())
            rows.Add(map(reader));
        return rows;
    }

    private static SqliteCommand Command(SqliteConnection c, string sql, (string Name, object? Value)[] parameters)
    {
        var command = c.CreateCommand();
#pragma warning disable CA2100 // Only this class's own statements; every value is a parameter.
        command.CommandText = sql;
#pragma warning restore CA2100
        foreach (var (name, value) in parameters)
            command.Parameters.AddWithValue(name, value ?? DBNull.Value);
        return command;
    }

    private Result<T, LibraryError> Run<T>(Func<SqliteConnection, Result<T, LibraryError>> run)
    {
        try
        {
            using var connection = database.Open();
            return run(connection);
        }
        catch (Exception e) when (e is SqliteException or JsonException)
        {
            return Result.Fail(LibraryError.UnusableDatabase, e.Message);
        }
    }
}
