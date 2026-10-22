using System.Globalization;
using Microsoft.Data.Sqlite;
using Multiroom.Server.Abstractions;

namespace Multiroom.Server.Library;

public enum LibraryError
{
    // The database file cannot be opened, read or migrated.
    UnusableDatabase,

    // The database was migrated by a newer server than this one.
    NewerSchema,

    // A file's bytes did not probe as audio with tags.
    UnreadableFile,

    // No such playlist, entry or favourite.
    NotFound,

    // A name, a position or an entry the playlists cannot take.
    InvalidArgument,

    // A playlist of that name exists, and was not to be overwritten.
    AlreadyExists,

    // A library folder's server did not answer, or the connection broke.
    Unreachable,

    // A NAS refused the folder's login, or the login may not read there.
    AuthFailed,
}

// The library's SQLite file, in WAL mode: one writer (the indexer), any
// number of readers, each on a connection of its own.
//
// The schema is the numbered .sql files in Migrations/, embedded in this
// assembly. Migrate() applies each one above the version in schema_version,
// in order, each under its own transaction.
public sealed class LibraryDatabase(string path)
{
    private const string MigrationPrefix = "migrations/";

    public string Path { get; } = path;

    // Not pooled: a pooled connection keeps the file open after the host is
    // gone, and opening a local SQLite file is cheap.
    public SqliteConnection Open()
    {
        var connection = new SqliteConnection(new SqliteConnectionStringBuilder
        {
            DataSource = Path,
            Pooling = false,
        }.ToString());
        try
        {
            connection.Open();
            return connection;
        }
        catch
        {
            connection.Dispose();
            throw;
        }
    }

    // The schema version the database is at afterwards.
    public Result<int, LibraryError> Migrate()
    {
        try
        {
            using var connection = Open();
            Execute(connection, null, """
                PRAGMA journal_mode = WAL;
                CREATE TABLE IF NOT EXISTS schema_version (version INTEGER NOT NULL);
                """);
            using (var read = connection.CreateCommand())
            {
                read.CommandText = "SELECT IFNULL(MAX(version), 0) FROM schema_version";
                var current = Convert.ToInt32(read.ExecuteScalar(), CultureInfo.InvariantCulture);
                var migrations = Migrations();
                var latest = migrations.Count == 0 ? 0 : migrations[^1].Version;
                if (current > latest)
                {
                    return Result.Fail(LibraryError.NewerSchema,
                        $"{Path} is at schema {current}; this server knows {latest}");
                }

                // SQLite's procedure for rebuilding a table: foreign keys off
                // (only possible outside a transaction), then checked before
                // each commit. Dropping a referenced table with them on would
                // run its ON DELETE actions.
                Execute(connection, null, "PRAGMA foreign_keys = OFF");
                foreach (var (version, sql) in migrations.Where(m => m.Version > current))
                {
                    using var transaction = connection.BeginTransaction();
                    Execute(connection, transaction, sql);
                    using (var check = connection.CreateCommand())
                    {
                        check.Transaction = transaction;
                        check.CommandText = "PRAGMA foreign_key_check";
                        using var violations = check.ExecuteReader();
                        if (violations.Read())
                        {
                            return Result.Fail(LibraryError.UnusableDatabase,
                                $"{Path}: migration {version} breaks a foreign key in {violations.GetString(0)}");
                        }
                    }
                    Execute(connection, transaction,
                        $"INSERT INTO schema_version (version) VALUES ({version.ToString(CultureInfo.InvariantCulture)})");
                    transaction.Commit();
                    current = version;
                }
                return current;
            }
        }
        catch (SqliteException e)
        {
            return Result.Fail(LibraryError.UnusableDatabase, $"{Path}: {e.Message}");
        }
    }

    private static void Execute(SqliteConnection connection, SqliteTransaction? transaction, string sql)
    {
        using var command = connection.CreateCommand();
        command.Transaction = transaction;
#pragma warning disable CA2100 // Only this assembly's own migration scripts and a formatted integer.
        command.CommandText = sql;
#pragma warning restore CA2100
        command.ExecuteNonQuery();
    }

    // Migrations/0001_library.sql is version 1.
    private static List<(int Version, string Sql)> Migrations()
    {
        var assembly = typeof(LibraryDatabase).Assembly;
        var migrations = new List<(int, string)>();
        foreach (var name in assembly.GetManifestResourceNames())
        {
            if (!name.StartsWith(MigrationPrefix, StringComparison.Ordinal))
                continue;
            var file = name[MigrationPrefix.Length..];
            var version = int.Parse(file[..file.IndexOf('_', StringComparison.Ordinal)], CultureInfo.InvariantCulture);
            using var reader = new StreamReader(assembly.GetManifestResourceStream(name)!);
            migrations.Add((version, reader.ReadToEnd()));
        }
        return [.. migrations.OrderBy(m => m.Item1)];
    }
}
