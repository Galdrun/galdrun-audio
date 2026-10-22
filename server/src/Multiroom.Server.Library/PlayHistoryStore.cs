using System.Text.Json;
using Microsoft.Data.Sqlite;
using Multiroom.Server.Abstractions;

namespace Multiroom.Server.Library;

// One track as it was played: in which zone and when.
// User is who was listening in the zone (phase 36), "" for the household.
public sealed record PlayedTrack(long Id, string Zone, Track Track, DateTimeOffset Played, string User = "");

// The house's play history, in the library's database (Migrations/0012),
// house-wide with the zone on each row (the owner's decision, 2026-10-01). A
// row is a reference plus the track as JSON; nothing here resolves. Only the
// newest MaxRows rows are kept (rule 6): each write drops what fell out.
public sealed class PlayHistoryStore(LibraryDatabase database)
{
    public const int MaxRows = 10_000;

    private const string Columns = "SELECT id, zone, snapshot, played, user FROM play_history";

    private static readonly JsonSerializerOptions Snapshot = new(JsonSerializerDefaults.Web);

    // User is the zone's at the moment the track became audible, "" for the
    // household; a user removed since is recorded as the household.
    public Result<PlayedTrack, LibraryError> Record(string zone, Track track, DateTimeOffset played,
        string user = UserStore.Household)
    {
        if (track.ProviderId.Length == 0 || track.ProviderItemId.Length == 0)
            return Result.Fail(LibraryError.InvalidArgument, $"{track.Title} has no provider-scoped id");
        return Run<PlayedTrack>(c =>
        {
            using var transaction = c.BeginTransaction();
            using (var insert = Command(c, """
                INSERT INTO play_history (zone, provider_id, provider_item_id, snapshot, played, user)
                VALUES ($zone, $provider, $item, $snapshot, $played,
                    COALESCE((SELECT name FROM household_user WHERE name = $user), ''))
                """, ("$zone", zone), ("$provider", track.ProviderId), ("$item", track.ProviderItemId),
                ("$snapshot", JsonSerializer.Serialize(track, Snapshot)), ("$played", played.UtcTicks), ("$user", user)))
            {
                insert.Transaction = transaction;
                insert.ExecuteNonQuery();
            }
            long id;
            string recorded;
            using (var last = Command(c, "SELECT id, user FROM play_history WHERE id = last_insert_rowid()"))
            {
                last.Transaction = transaction;
                using var reader = last.ExecuteReader();
                reader.Read();
                (id, recorded) = (reader.GetInt64(0), reader.GetString(1));
            }
            using (var prune = Command(c, "DELETE FROM play_history WHERE id <= $below", ("$below", id - MaxRows)))
            {
                prune.Transaction = transaction;
                prune.ExecuteNonQuery();
            }
            transaction.Commit();
            return new PlayedTrack(id, zone, track, new DateTimeOffset(played.UtcTicks, TimeSpan.Zero), recorded);
        });
    }

    // The newest `limit` plays, newest first, house-wide.
    public Result<IReadOnlyList<PlayedTrack>, LibraryError> Recent(int limit) =>
        Run<IReadOnlyList<PlayedTrack>>(c => Rows(c, Columns + " ORDER BY id DESC LIMIT $limit", ("$limit", limit)));

    // One page, newest first, of one zone's plays or the house's, in a
    // user's scope (UserStore.Scope: the household's, a user's with the
    // household's, or everyone's), and how many there are. A play is one
    // event, so nothing is merged.
    public Result<(IReadOnlyList<PlayedTrack> Items, int Total), LibraryError> Page(string? zone, int offset, int limit,
        string? user = UserStore.Household) =>
        Run<(IReadOnlyList<PlayedTrack>, int)>(c =>
        {
            var scope = UserStore.Canonical(c, null, user, everyone: true);
            if (!scope.IsSuccess)
                return scope.Failure;
            var where = " WHERE " + UserStore.Scope(scope.Value) + (zone is null ? "" : " AND zone = $zone");
            var items = Rows(c, Columns + where + " ORDER BY id DESC LIMIT $limit OFFSET $offset",
                ("$zone", zone), ("$user", scope.Value), ("$limit", limit), ("$offset", offset));
            using var count = Command(c, "SELECT COUNT(*) FROM play_history" + where, ("$zone", zone),
                ("$user", scope.Value));
            return (items, (int)(long)count.ExecuteScalar()!);
        });

    private static List<PlayedTrack> Rows(SqliteConnection c, string sql, params (string Name, object? Value)[] parameters)
    {
        using var command = Command(c, sql, parameters);
        using var reader = command.ExecuteReader();
        var rows = new List<PlayedTrack>();
        while (reader.Read())
        {
            rows.Add(new PlayedTrack(reader.GetInt64(0), reader.GetString(1),
                JsonSerializer.Deserialize<Track>(reader.GetString(2), Snapshot)!,
                new DateTimeOffset(reader.GetInt64(3), TimeSpan.Zero), reader.GetString(4)));
        }
        return rows;
    }

    private static SqliteCommand Command(SqliteConnection c, string sql, params (string Name, object? Value)[] parameters)
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
