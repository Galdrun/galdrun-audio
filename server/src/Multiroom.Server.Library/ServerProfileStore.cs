using Microsoft.Data.Sqlite;
using Multiroom.Server.Abstractions;

namespace Multiroom.Server.Library;

// The server's name and the images of the server and its rooms, in the
// library's database (Migrations/0010). An image's owner is ServerOwner or
// RoomOwner(client); a room need not be known to the core to have one.
public sealed class ServerProfileStore(LibraryDatabase database)
{
    public const string ServerOwner = "server";
    public const int MaxNameLength = 64;
    public const int MaxImageBytes = 5 * 1024 * 1024;

    public static string RoomOwner(string client) => $"room:{client}";

    public Result<string, LibraryError> Name()
    {
        try
        {
            using var connection = database.Open();
            using var command = connection.CreateCommand();
            command.CommandText = "SELECT name FROM server_profile WHERE id = 1";
            return (string)command.ExecuteScalar()!;
        }
        catch (SqliteException e)
        {
            return Result.Fail(LibraryError.UnusableDatabase, e.Message);
        }
    }

    // Trimmed; not empty, at most MaxNameLength.
    public Result<string, LibraryError> SetName(string name)
    {
        name = name.Trim();
        if (name.Length is 0 or > MaxNameLength)
            return Result.Fail(LibraryError.InvalidArgument, $"name: 1 to {MaxNameLength} characters");
        var set = Execute("UPDATE server_profile SET name = $name WHERE id = 1", ("$name", name));
        return set.IsSuccess ? name : set.Failure;
    }

    public Result<bool, LibraryError> HasImage(string owner)
    {
        try
        {
            using var connection = database.Open();
            using var command = connection.CreateCommand();
            command.CommandText = "SELECT 1 FROM profile_image WHERE owner = $owner";
            command.Parameters.AddWithValue("$owner", owner);
            return command.ExecuteScalar() is not null;
        }
        catch (SqliteException e)
        {
            return Result.Fail(LibraryError.UnusableDatabase, e.Message);
        }
    }

    public Result<Artwork, LibraryError> Image(string owner)
    {
        try
        {
            using var connection = database.Open();
            using var command = connection.CreateCommand();
            command.CommandText = "SELECT content_type, bytes FROM profile_image WHERE owner = $owner";
            command.Parameters.AddWithValue("$owner", owner);
            using var reader = command.ExecuteReader();
            return reader.Read()
                ? new Artwork(reader.GetString(0), (byte[])reader.GetValue(1))
                : Result.Fail(LibraryError.NotFound, $"{owner} has no image");
        }
        catch (SqliteException e)
        {
            return Result.Fail(LibraryError.UnusableDatabase, e.Message);
        }
    }

    // A JPEG, PNG, GIF or WebP of at most MaxImageBytes, its type read from
    // its bytes; answers that type.
    public Result<string, LibraryError> SetImage(string owner, byte[] bytes)
    {
        if (bytes.Length > MaxImageBytes)
            return Result.Fail(LibraryError.InvalidArgument, $"an image of at most {MaxImageBytes} bytes");
        if (FolderFiles.ImageType(bytes) is not { } type)
            return Result.Fail(LibraryError.InvalidArgument, "a JPEG, PNG, GIF or WebP image");
        var set = Execute("""
            INSERT INTO profile_image (owner, content_type, bytes, updated) VALUES ($owner, $type, $bytes, $updated)
            ON CONFLICT (owner) DO UPDATE SET content_type = excluded.content_type, bytes = excluded.bytes,
                updated = excluded.updated
            """, ("$owner", owner), ("$type", type), ("$bytes", bytes), ("$updated", DateTime.UtcNow.Ticks));
        return set.IsSuccess ? type : set.Failure;
    }

    // True when there was one.
    public Result<bool, LibraryError> RemoveImage(string owner) =>
        Execute("DELETE FROM profile_image WHERE owner = $owner", ("$owner", owner));

    private Result<bool, LibraryError> Execute(string sql, params (string Name, object Value)[] parameters)
    {
        try
        {
            using var connection = database.Open();
            using var command = connection.CreateCommand();
#pragma warning disable CA2100 // Only this class's own statements.
            command.CommandText = sql;
#pragma warning restore CA2100
            foreach (var (name, value) in parameters)
                command.Parameters.AddWithValue(name, value);
            return command.ExecuteNonQuery() > 0;
        }
        catch (SqliteException e)
        {
            return Result.Fail(LibraryError.UnusableDatabase, e.Message);
        }
    }
}
