using System.Text.Json;
using Microsoft.Data.Sqlite;
using Multiroom.Server.Abstractions;

namespace Multiroom.Server.Library;

// One add-in's rows: tracks it answered for, tracks it failed on, and shown
// tracks it has not been asked about yet.
public sealed record EnrichmentCounts(long Enriched, long Failed, long Pending);

// What the enrichment add-ins derived for the library's tracks (phase 12):
// one JSON value per track and add-in, or why there is none. A row goes with
// its track, and the indexer deletes it when the track's file changes, so the
// track is asked about again; a failure is not retried before that. Each call
// is on its own connection, like the other stores.
public sealed class EnrichmentStore(LibraryDatabase database, LibraryCatalog catalog)
{
    // Shown tracks the add-in has nothing recorded for: what it is asked
    // about next. The ones the house keeps (in a playlist, a favourite,
    // liked: LibraryCatalog.Kept) first, then oldest first.
    public Result<IReadOnlyList<Track>, LibraryError> Pending(string addIn, int limit) =>
        Read<IReadOnlyList<Track>>(c => Rows(c, LibraryCatalog.TrackColumns + """

            WHERE NOT EXISTS (SELECT 1 FROM enrichment e WHERE e.addin = $addin AND e.track_id = t.id)
            ORDER BY
            """ + LibraryCatalog.Kept + " DESC, t.id LIMIT $limit", catalog.TrackOf, ("$addin", addIn),
            ("$limit", limit), ("$provider", LibraryCatalog.LocalProvider)));

    // Keeps what the add-in answered, or why it did not (exactly one of the
    // two). False when the track is gone meanwhile.
    public Result<bool, LibraryError> Record(string addIn, string trackItemId, JsonElement? data, string? failure)
    {
        if (LibraryCatalog.Id(trackItemId, LibraryCatalog.TrackPrefix) is not { } id)
            return Result.Fail(LibraryError.NotFound, $"{trackItemId} is no library track");
        return Read<bool>(c => Rows(c, """
            INSERT INTO enrichment (track_id, addin, data, failure, enriched)
            SELECT id, $addin, $data, $failure, $now FROM track WHERE id = $id
            ON CONFLICT (addin, track_id) DO UPDATE SET data = excluded.data, failure = excluded.failure,
                enriched = excluded.enriched
            RETURNING 1
            """, _ => true, ("$id", id), ("$addin", addIn), ("$data", data?.GetRawText()), ("$failure", failure),
            ("$now", DateTime.UtcNow.Ticks)).Count > 0);
    }

    // What the add-in derived for the track. NotFound when it has not been
    // asked yet, or failed (the detail says why).
    public Result<JsonElement, LibraryError> Get(string addIn, string trackItemId)
    {
        if (LibraryCatalog.Id(trackItemId, LibraryCatalog.TrackPrefix) is not { } id)
            return Result.Fail(LibraryError.NotFound, $"{trackItemId} is no library track");
        return Read<JsonElement>(c =>
        {
            var rows = Rows(c, "SELECT data, failure FROM enrichment WHERE addin = $addin AND track_id = $id",
                r => (Data: r.IsDBNull(0) ? null : r.GetString(0), Failure: r.IsDBNull(1) ? null : r.GetString(1)),
                ("$addin", addIn), ("$id", id));
            if (rows.Count == 0)
                return Result.Fail(LibraryError.NotFound, $"{addIn} has nothing for {trackItemId} yet");
            if (rows[0].Data is not { } data)
                return Result.Fail(LibraryError.NotFound, $"{addIn} could not enrich {trackItemId}: {rows[0].Failure}");
            using var document = JsonDocument.Parse(data);
            return document.RootElement.Clone();
        });
    }

    // Forgets the add-in's failures, and its answers whose `field` ("cover")
    // is `value` ("notFound"), so those tracks are asked again. Answers how
    // many.
    public Result<int, LibraryError> Forget(string addIn, IReadOnlyList<string> fields, string value) =>
        Read<int>(c =>
        {
            var forgotten = Rows(c, "DELETE FROM enrichment WHERE addin = $addin AND failure IS NOT NULL RETURNING 1",
                _ => 1, ("$addin", addIn)).Count;
            foreach (var field in fields)
            {
                forgotten += Rows(c, """
                    DELETE FROM enrichment WHERE addin = $addin AND json_extract(data, $path) = $value RETURNING 1
                    """, _ => 1, ("$addin", addIn), ("$path", "$." + field), ("$value", value)).Count;
            }
            return forgotten;
        });

    public Result<EnrichmentCounts, LibraryError> Counts(string addIn) =>
        Read<EnrichmentCounts>(c => Rows(c, $"""
            SELECT (SELECT COUNT(*) FROM enrichment WHERE addin = $addin AND failure IS NULL),
                   (SELECT COUNT(*) FROM enrichment WHERE addin = $addin AND failure IS NOT NULL),
                   (SELECT COUNT(*) FROM track t {LibraryCatalog.Shown}
                    WHERE NOT EXISTS (SELECT 1 FROM enrichment e WHERE e.addin = $addin AND e.track_id = t.id))
            """, r => new EnrichmentCounts(r.GetInt64(0), r.GetInt64(1), r.GetInt64(2)), ("$addin", addIn))[0]);

    private static List<T> Rows<T>(SqliteConnection connection, string sql, Func<SqliteDataReader, T> map,
        params (string Name, object? Value)[] parameters)
    {
        using var command = connection.CreateCommand();
#pragma warning disable CA2100 // Constant SQL; every value is a parameter.
        command.CommandText = sql;
#pragma warning restore CA2100
        foreach (var (name, value) in parameters)
            command.Parameters.AddWithValue(name, value ?? DBNull.Value);
        using var reader = command.ExecuteReader();
        var rows = new List<T>();
        while (reader.Read())
            rows.Add(map(reader));
        return rows;
    }

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
}
