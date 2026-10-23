using Microsoft.Data.Sqlite;
using Multiroom.Server.Abstractions;

namespace Multiroom.Server.Library;

// The folders of a store's items (playlists 0026, stations 0028): one table
// of id, name, parent_id and created. A folder lies in at most one other,
// null at the top; no two in one place share a name, in any case (checked
// here: a unique index cannot treat null parents as equal). Each call runs on
// the store's connection and, when it writes, in its transaction; removing
// what lies in a folder is the store's, by Within.
internal sealed class FolderTree(string table, string noun)
{
    private readonly string _columns = $"SELECT id, name, parent_id FROM {table}";

    // A WITH clause naming the folder $id and every folder within it `tree`.
    public string Within { get; } = $"""
        WITH RECURSIVE tree(id) AS (
            SELECT $id UNION ALL SELECT f.id FROM {table} f JOIN tree ON f.parent_id = tree.id)
        """;

    // By name, each with the folder it lies in.
    public List<CollectionFolder> All(SqliteConnection c) =>
        Rows(c, null, _columns + " ORDER BY name COLLATE NOCASE, id", Read);

    // In the parent, or at the top when null.
    public Result<CollectionFolder, LibraryError> Create(SqliteConnection c, SqliteTransaction t, string name, long? parent)
    {
        if (Trimmed(name) is not { } trimmed)
            return NoName();
        if (parent is { } p && !Exists(c, t, p))
            return NotFound(p);
        if (NameTaken(c, t, trimmed, parent, null))
            return NameTaken(trimmed);
        var id = Rows(c, t, $"INSERT INTO {table} (name, parent_id, created) VALUES ($name, $parent, $created) RETURNING id",
            r => r.GetInt64(0), ("$name", trimmed), ("$parent", parent), ("$created", DateTime.UtcNow.Ticks))[0];
        return ById(c, t, id);
    }

    public Result<CollectionFolder, LibraryError> Rename(SqliteConnection c, SqliteTransaction t, long folder, string name)
    {
        if (Trimmed(name) is not { } trimmed)
            return NoName();
        if (ById(c, t, folder) is not { IsSuccess: true } found)
            return NotFound(folder);
        if (NameTaken(c, t, trimmed, found.Value.Parent, folder))
            return NameTaken(trimmed);
        Execute(c, t, $"UPDATE {table} SET name = $name WHERE id = $id", ("$name", trimmed), ("$id", folder));
        return ById(c, t, folder);
    }

    // Into the parent, or to the top when null; never into itself or a folder within it.
    public Result<CollectionFolder, LibraryError> Move(SqliteConnection c, SqliteTransaction t, long folder, long? parent)
    {
        if (ById(c, t, folder) is not { IsSuccess: true } found)
            return NotFound(folder);
        if (parent is { } p)
        {
            if (!Exists(c, t, p))
                return NotFound(p);
            for (long? at = p; at is { } a; at = ById(c, t, a).Value.Parent)
            {
                if (a == folder)
                    return Result.Fail(LibraryError.InvalidArgument, $"folder {folder} cannot go into itself or a folder within it");
            }
        }
        if (NameTaken(c, t, found.Value.Name, parent, folder))
            return NameTaken(found.Value.Name);
        Execute(c, t, $"UPDATE {table} SET parent_id = $parent WHERE id = $id", ("$parent", parent), ("$id", folder));
        return ById(c, t, folder);
    }

    // The folder and every folder within it; what lies in them is the store's.
    public void Delete(SqliteConnection c, SqliteTransaction t, long folder) =>
        Execute(c, t, Within + $" DELETE FROM {table} WHERE id IN (SELECT id FROM tree)", ("$id", folder));

    public bool Exists(SqliteConnection c, SqliteTransaction? t, long folder) =>
        Rows(c, t, $"SELECT COUNT(*) FROM {table} WHERE id = $id", r => r.GetInt32(0), ("$id", folder))[0] > 0;

    public Failure<LibraryError> NotFound(long folder) => Result.Fail(LibraryError.NotFound, $"no {noun} folder {folder}");

    private bool NameTaken(SqliteConnection c, SqliteTransaction t, string name, long? parent, long? except) =>
        Rows(c, t, $"SELECT COUNT(*) FROM {table} WHERE name = $name COLLATE NOCASE AND parent_id IS $parent AND id IS NOT $except",
            r => r.GetInt32(0), ("$name", name), ("$parent", parent), ("$except", except))[0] > 0;

    private static Failure<LibraryError> NameTaken(string name) =>
        Result.Fail(LibraryError.AlreadyExists, $"a folder named {name} is already there");

    private static Failure<LibraryError> NoName() => Result.Fail(LibraryError.InvalidArgument, "a folder needs a name");

    private Result<CollectionFolder, LibraryError> ById(SqliteConnection c, SqliteTransaction? t, long folder) =>
        Rows(c, t, _columns + " WHERE id = $id", Read, ("$id", folder)) is [var found] ? found : NotFound(folder);

    private static CollectionFolder Read(SqliteDataReader r) =>
        new(r.GetInt64(0), r.GetString(1), r.IsDBNull(2) ? null : r.GetInt64(2));

    private static string? Trimmed(string name) => string.IsNullOrWhiteSpace(name) ? null : name.Trim();

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
#pragma warning disable CA2100 // Constant SQL and the store's own table name; every value is a parameter.
        command.CommandText = sql;
#pragma warning restore CA2100
        foreach (var (name, value) in parameters)
            command.Parameters.AddWithValue(name, value ?? DBNull.Value);
        return command;
    }
}
