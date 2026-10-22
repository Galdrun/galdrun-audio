using Microsoft.Data.Sqlite;
using Multiroom.Server.Abstractions;

namespace Multiroom.Server.Library;

// Where a listener stopped in one book (phase 33). Reference is the book's
// group source ("local:book:3"); Part and OffsetMs the place, PartReference
// the part's own track ("local:track:12"), PositionMs the place in the whole
// book where every earlier part's length is known. A finished book has no
// place: it starts again from its beginning.
public sealed record BookPosition(
    string Reference,
    string Title,
    string? Artist,
    int Part,
    string? PartReference,
    long OffsetMs,
    long? PositionMs,
    long? DurationMs,
    bool Finished,
    DateTimeOffset Updated);

// The books' positions, in the library's database (Migrations/0017), one row
// per book, house-wide, so every app and room sees the same place. Written by
// BookPositions on its own task, read by PlaylistPlayer when a book is
// queued and by the API. Only the newest MaxRows books are kept (rule 6).
public sealed class BookPositionStore(LibraryDatabase database)
{
    public const int MaxRows = 1000;

    private const string Columns = """
        SELECT reference, title, artist, part, part_reference, offset_ms, position_ms, duration_ms, finished, updated
        FROM book_position
        """;

    // A book's position replaces the one it had; the oldest beyond MaxRows go.
    public Result<BookPosition, LibraryError> Save(BookPosition position)
    {
        if (position.Reference.Length == 0)
            return Result.Fail(LibraryError.InvalidArgument, $"{position.Title} has no reference");
        return Run<BookPosition>(c =>
        {
            using var transaction = c.BeginTransaction();
            Execute(c, transaction, """
                INSERT INTO book_position (reference, title, artist, part, part_reference, offset_ms, position_ms,
                    duration_ms, finished, updated)
                VALUES ($reference, $title, $artist, $part, $partReference, $offset, $position, $duration, $finished,
                    $updated)
                ON CONFLICT (reference) DO UPDATE SET title = excluded.title, artist = excluded.artist,
                    part = excluded.part, part_reference = excluded.part_reference, offset_ms = excluded.offset_ms,
                    position_ms = excluded.position_ms, duration_ms = excluded.duration_ms,
                    finished = excluded.finished, updated = excluded.updated
                """, ("$reference", position.Reference), ("$title", position.Title), ("$artist", position.Artist),
                ("$part", position.Part), ("$partReference", position.PartReference), ("$offset", position.OffsetMs),
                ("$position", position.PositionMs), ("$duration", position.DurationMs),
                ("$finished", position.Finished), ("$updated", position.Updated.UtcTicks));
            Execute(c, transaction, """
                DELETE FROM book_position WHERE reference NOT IN (
                    SELECT reference FROM book_position ORDER BY updated DESC, rowid DESC LIMIT $max)
                """, ("$max", MaxRows));
            transaction.Commit();
            return position with { Updated = new DateTimeOffset(position.Updated.UtcTicks, TimeSpan.Zero) };
        });
    }

    // Null when the book has none.
    public Result<BookPosition?, LibraryError> Get(string reference) =>
        Run<BookPosition?>(c => Rows(c, Columns + " WHERE reference = $reference", ("$reference", reference))
            .FirstOrDefault());

    // One page, newest first, and how many there are.
    public Result<(IReadOnlyList<BookPosition> Items, int Total), LibraryError> Page(int offset, int limit) =>
        Run<(IReadOnlyList<BookPosition>, int)>(c =>
        {
            var items = Rows(c, Columns + " ORDER BY updated DESC, rowid DESC LIMIT $limit OFFSET $offset",
                ("$limit", limit), ("$offset", offset));
            using var count = Command(c, "SELECT COUNT(*) FROM book_position");
            return (items, (int)(long)count.ExecuteScalar()!);
        });

    // "Start over": the book's position is forgotten.
    public Result<bool, LibraryError> Delete(string reference) =>
        Run<bool>(c =>
        {
            using var command = Command(c, "DELETE FROM book_position WHERE reference = $reference",
                ("$reference", reference));
            return command.ExecuteNonQuery() > 0
                ? true
                : Result.Fail(LibraryError.NotFound, $"no saved position for {reference}");
        });

    private static List<BookPosition> Rows(SqliteConnection c, string sql,
        params (string Name, object? Value)[] parameters)
    {
        using var command = Command(c, sql, parameters);
        using var reader = command.ExecuteReader();
        var rows = new List<BookPosition>();
        while (reader.Read())
        {
            long? Long(int column) => reader.IsDBNull(column) ? null : reader.GetInt64(column);
            rows.Add(new BookPosition(reader.GetString(0), reader.GetString(1),
                reader.IsDBNull(2) ? null : reader.GetString(2), reader.GetInt32(3),
                reader.IsDBNull(4) ? null : reader.GetString(4), reader.GetInt64(5), Long(6), Long(7),
                reader.GetBoolean(8), new DateTimeOffset(reader.GetInt64(9), TimeSpan.Zero)));
        }
        return rows;
    }

    private static void Execute(SqliteConnection c, SqliteTransaction t, string sql,
        params (string Name, object? Value)[] parameters)
    {
        using var command = Command(c, sql, parameters);
        command.Transaction = t;
        command.ExecuteNonQuery();
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
        catch (SqliteException e)
        {
            return Result.Fail(LibraryError.UnusableDatabase, e.Message);
        }
    }
}
