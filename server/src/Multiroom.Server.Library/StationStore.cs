using Microsoft.Data.Sqlite;
using Multiroom.Server.Abstractions;

namespace Multiroom.Server.Library;

// A radio station as the webradio provider keeps it. StreamUrl is the
// provider's alone: it never leaves this server (rule 7), because the core
// fetches the media endpoint's relay instead. Folder is the folder it lies
// in, null at the top.
public sealed record StoredStation(long Id, string Name, Uri StreamUrl, Uri? Homepage, string? Genre, Uri? Logo,
    DateTime Added, long? Folder = null);

// What adding or importing a station takes; Folder is where it goes, null the top.
public sealed record NewStation(string Name, Uri StreamUrl, Uri? Homepage = null, string? Genre = null,
    Uri? Logo = null, long? Folder = null);

// Radio stations and their folders, in the library's database (Migrations/0004,
// 0028). As the playlist store: one connection per call, one transaction per
// write, committed only on success.
public sealed class StationStore(LibraryDatabase database)
{
    private const string Columns = "SELECT s.id, s.name, s.stream_url, s.homepage, s.genre, s.logo, s.added, s.folder_id FROM station s";

    private static readonly FolderTree FolderTable = new("station_folder", "station");

    // By name.
    public Result<IReadOnlyList<StoredStation>, LibraryError> Stations(int offset, int limit) =>
        Read(c => List(Rows(c, null, Columns + " ORDER BY s.name COLLATE NOCASE, s.id LIMIT $limit OFFSET $offset",
            ReadStation, ("$limit", limit), ("$offset", offset))));

    // Every station, or those of one genre (without case). The provider pages
    // them: a household's stations fit in memory.
    public Result<IReadOnlyList<StoredStation>, LibraryError> All(string? genre = null) =>
        Read(c => List(genre is null
            ? Rows(c, null, Columns, ReadStation)
            : Rows(c, null, Columns + " WHERE s.genre = $genre COLLATE NOCASE", ReadStation, ("$genre", genre))));

    // The genres stations have, each with how many, as first written.
    public Result<IReadOnlyList<(string Genre, int Stations)>, LibraryError> Genres() =>
        Read(c => List(Rows(c, null, "SELECT MIN(genre), COUNT(*) FROM station WHERE genre IS NOT NULL GROUP BY genre COLLATE NOCASE",
            r => (r.GetString(0), r.GetInt32(1)))));

    public Result<StoredStation, LibraryError> Station(long id) =>
        Read<StoredStation>(c => ById(c, null, id) is { } found ? found : NoStation(id));

    // Every word a prefix, all words required, over name and genre; by relevance.
    public Result<IReadOnlyList<StoredStation>, LibraryError> Search(string text, int offset, int limit)
    {
        if (LibraryCatalog.MatchExpression(text) is not { } match)
            return List(new List<StoredStation>());
        return Read(c => List(Rows(c, null, Columns + """

            JOIN station_search ON station_search.rowid = s.id
            WHERE station_search MATCH $match ORDER BY station_search.rank LIMIT $limit OFFSET $offset
            """, ReadStation, ("$match", match), ("$limit", limit), ("$offset", offset))));
    }

    // A stream URL already here answers its station unchanged, where it lies.
    public Result<StoredStation, LibraryError> Add(NewStation station)
    {
        var added = Import([station]);
        return added.IsSuccess ? added.Value[0] : added.Failure;
    }

    // All or none, in the order given, each answered as Add answers it.
    public Result<IReadOnlyList<StoredStation>, LibraryError> Import(IReadOnlyList<NewStation> stations)
    {
        if (stations.Count == 0)
            return Result.Fail(LibraryError.InvalidArgument, "no stations to add");
        foreach (var station in stations)
        {
            if (Invalid(station) is { } invalid)
                return Result.Fail(LibraryError.InvalidArgument, invalid);
        }

        return Write<IReadOnlyList<StoredStation>>((c, t) =>
        {
            var added = new List<StoredStation>(stations.Count);
            foreach (var station in stations)
            {
                if (station.Folder is { } folder && !FolderTable.Exists(c, t, folder))
                    return FolderTable.NotFound(folder);
                var existing = Rows(c, t, Columns + " WHERE s.stream_url = $url", ReadStation,
                    ("$url", station.StreamUrl.AbsoluteUri));
                if (existing.Count > 0)
                {
                    added.Add(existing[0]);
                    continue;
                }
                var name = station.Name.Trim();
                var genre = string.IsNullOrWhiteSpace(station.Genre) ? null : station.Genre.Trim();
                var id = Rows(c, t, """
                    INSERT INTO station (name, stream_url, homepage, genre, logo, added, folder_id)
                    VALUES ($name, $url, $homepage, $genre, $logo, $added, $folder)
                    RETURNING id
                    """, r => r.GetInt64(0), ("$name", name), ("$url", station.StreamUrl.AbsoluteUri),
                    ("$homepage", station.Homepage?.AbsoluteUri), ("$genre", genre),
                    ("$logo", station.Logo?.AbsoluteUri), ("$added", DateTime.UtcNow.Ticks), ("$folder", station.Folder))[0];
                Execute(c, t, "INSERT INTO station_search (rowid, name, genre) VALUES ($id, $name, $genre)",
                    ("$id", id), ("$name", name), ("$genre", genre));
                added.Add(ById(c, t, id)!);
            }
            return added;
        });
    }

    public Result<bool, LibraryError> Remove(long id) =>
        Write<bool>((c, t) =>
        {
            if (Execute(c, t, "DELETE FROM station WHERE id = $id", ("$id", id)) == 0)
                return NoStation(id);
            Execute(c, t, "DELETE FROM station_search WHERE rowid = $id", ("$id", id));
            return true;
        });

    // Into the folder, or to the top when null.
    public Result<StoredStation, LibraryError> MoveTo(long id, long? folder) =>
        Write<StoredStation>((c, t) =>
        {
            if (folder is { } f && !FolderTable.Exists(c, t, f))
                return FolderTable.NotFound(f);
            return Execute(c, t, "UPDATE station SET folder_id = $folder WHERE id = $id", ("$folder", folder), ("$id", id)) == 0
                ? NoStation(id)
                : ById(c, t, id)!;
        });

    // --- folders ------------------------------------------------------------

    // By name, each with the folder it lies in.
    public Result<IReadOnlyList<CollectionFolder>, LibraryError> Folders() => Read(c => List(FolderTable.All(c)));

    // In the parent, or at the top when null. Two folders in one place never
    // share a name (in any case); stations may.
    public Result<CollectionFolder, LibraryError> CreateFolder(string name, long? parent = null) =>
        Write((c, t) => FolderTable.Create(c, t, name, parent));

    public Result<CollectionFolder, LibraryError> RenameFolder(long folder, string name) =>
        Write((c, t) => FolderTable.Rename(c, t, folder, name));

    // Into the parent, or to the top when null; never into itself or a folder within it.
    public Result<CollectionFolder, LibraryError> MoveFolder(long folder, long? parent) =>
        Write((c, t) => FolderTable.Move(c, t, folder, parent));

    // With every folder within it and every station in any of them.
    public Result<bool, LibraryError> DeleteFolder(long folder) =>
        Write<bool>((c, t) =>
        {
            if (!FolderTable.Exists(c, t, folder))
                return FolderTable.NotFound(folder);
            const string Doomed = " (SELECT id FROM station WHERE folder_id IN (SELECT id FROM tree))";
            Execute(c, t, FolderTable.Within + " DELETE FROM station_search WHERE rowid IN" + Doomed, ("$id", folder));
            Execute(c, t, FolderTable.Within + " DELETE FROM station WHERE id IN" + Doomed, ("$id", folder));
            FolderTable.Delete(c, t, folder);
            return true;
        });

    private static string? Invalid(NewStation station)
    {
        if (string.IsNullOrWhiteSpace(station.Name))
            return $"the station at {station.StreamUrl} needs a name";
        if (!IsWeb(station.StreamUrl))
            return $"{station.Name}: the stream URL must be an absolute http or https URL";
        if (station.Homepage is { } homepage && !IsWeb(homepage))
            return $"{station.Name}: the homepage must be an absolute http or https URL";
        if (station.Logo is { } logo && !IsWeb(logo))
            return $"{station.Name}: the logo must be an absolute http or https URL";
        return null;
    }

    private static bool IsWeb(Uri uri) =>
        uri.IsAbsoluteUri && (uri.Scheme == Uri.UriSchemeHttp || uri.Scheme == Uri.UriSchemeHttps);

    private static StoredStation? ById(SqliteConnection c, SqliteTransaction? t, long id) =>
        Rows(c, t, Columns + " WHERE s.id = $id", ReadStation, ("$id", id)).FirstOrDefault();

    private static StoredStation ReadStation(SqliteDataReader r) =>
        new(r.GetInt64(0), r.GetString(1), new Uri(r.GetString(2)), UriOrNull(r, 3), r.IsDBNull(4) ? null : r.GetString(4),
            UriOrNull(r, 5), new DateTime(r.GetInt64(6), DateTimeKind.Utc), r.IsDBNull(7) ? null : r.GetInt64(7));

    private static Uri? UriOrNull(SqliteDataReader r, int column) => r.IsDBNull(column) ? null : new Uri(r.GetString(column));

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
#pragma warning disable CA2100 // Constant SQL; every value is a parameter.
        command.CommandText = sql;
#pragma warning restore CA2100
        foreach (var (name, value) in parameters)
            command.Parameters.AddWithValue(name, value ?? DBNull.Value);
        return command;
    }

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

    private static Failure<LibraryError> NoStation(long id) => Result.Fail(LibraryError.NotFound, $"no station {id}");
}
