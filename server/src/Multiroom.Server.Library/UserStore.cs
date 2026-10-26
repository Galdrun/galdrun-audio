using Microsoft.Data.Sqlite;
using Multiroom.Server.Abstractions;

namespace Multiroom.Server.Library;

public sealed record HouseholdUser(string Name, DateTimeOffset Added);

// The household's users (phase 36), in the library's database
// (Migrations/0018): a label on a like, a favourite or a play, not an account.
// The empty user is the household. A name is unique without regard to case
// and is stored, and answered, as it was added.
//
// A user's rows are read with the household's: Scope and Merged are the one
// rule the stores share. A user removed leaves their rows to the household,
// or takes them along.
public sealed class UserStore(LibraryDatabase database)
{
    public const string Household = "";

    // A read of every row, each with its user: an admin view.
    public const string Everyone = "*";

    public const int MaxName = 40;

    // A user removed, by their name; their rows are the household's now.
    public event EventHandler<string>? Removed;

    // In the order they were added.
    public Result<IReadOnlyList<HouseholdUser>, LibraryError> Users() => Run<IReadOnlyList<HouseholdUser>>(c =>
    {
        using var command = Command(c, null, "SELECT name, added FROM household_user ORDER BY id");
        using var reader = command.ExecuteReader();
        var users = new List<HouseholdUser>();
        while (reader.Read())
            users.Add(new HouseholdUser(reader.GetString(0), new DateTimeOffset(reader.GetInt64(1), TimeSpan.Zero)));
        return users;
    });

    public Result<HouseholdUser, LibraryError> Add(string name)
    {
        var trimmed = name.Trim();
        if (trimmed.Length is 0 or > MaxName)
            return Result.Fail(LibraryError.InvalidArgument, $"a user's name is 1 to {MaxName} characters");
        if (trimmed == Everyone)
            return Result.Fail(LibraryError.InvalidArgument, $"{Everyone} names every user, not one");
        return Run<HouseholdUser>(c =>
        {
            var added = DateTime.UtcNow.Ticks;
            using var insert = Command(c, null, """
                INSERT INTO household_user (name, added) VALUES ($name, $added) ON CONFLICT (name) DO NOTHING
                """, ("$name", trimmed), ("$added", added));
            return insert.ExecuteNonQuery() == 0
                ? Result.Fail(LibraryError.AlreadyExists, $"a user {trimmed} exists")
                : new HouseholdUser(trimmed, new DateTimeOffset(added, TimeSpan.Zero));
        });
    }

    // Their likes and favourites become the household's, except one the
    // household has already, which is left once; their plays and book
    // positions become the household's, except a book the household has a
    // place in; their proposals and dismissals go. With deleteTheirs, their
    // likes, favourites, plays and book positions go too.
    public Result<bool, LibraryError> Remove(string name, bool deleteTheirs = false)
    {
        var removed = Run<string>(c =>
        {
            using var transaction = c.BeginTransaction();
            var user = Canonical(c, transaction, name);
            if (!user.IsSuccess || user.Value.Length == 0)
                return Result.Fail(LibraryError.NotFound, $"no user {name}");
            foreach (var table in (string[])["liked", "favorite"])
            {
                Execute(c, transaction, $"""
                    DELETE FROM {table} WHERE user = $user AND ($all OR EXISTS (SELECT 1 FROM {table} h
                        WHERE h.user = '' AND h.kind = {table}.kind AND h.provider_id = {table}.provider_id
                          AND h.provider_item_id = {table}.provider_item_id))
                    """, ("$user", user.Value), ("$all", deleteTheirs));
                Execute(c, transaction, $"UPDATE {table} SET user = '' WHERE user = $user", ("$user", user.Value));
            }
            if (deleteTheirs)
                Execute(c, transaction, "DELETE FROM play_history WHERE user = $user", ("$user", user.Value));
            Execute(c, transaction, "UPDATE play_history SET user = '' WHERE user = $user", ("$user", user.Value));
            Execute(c, transaction, """
                DELETE FROM book_position WHERE user = $user AND ($all OR EXISTS (SELECT 1 FROM book_position h
                    WHERE h.user = '' AND h.reference = book_position.reference))
                """, ("$user", user.Value), ("$all", deleteTheirs));
            Execute(c, transaction, "UPDATE book_position SET user = '' WHERE user = $user", ("$user", user.Value));
            // Their proposals (phase 38) were theirs alone.
            Execute(c, transaction, "DELETE FROM proposal_run WHERE user = $user", ("$user", user.Value));
            Execute(c, transaction, "DELETE FROM proposal_dismissed WHERE user = $user", ("$user", user.Value));
            Execute(c, transaction, "DELETE FROM household_user WHERE name = $user", ("$user", user.Value));
            transaction.Commit();
            return user.Value;
        });
        if (!removed.IsSuccess)
            return removed.Failure;
        Removed?.Invoke(this, removed.Value);
        return true;
    }

    // A user as a request names them: empty or absent is the household, a
    // name is answered as it was added, and anything else is refused by name
    // so that a typo never invents a person. Everyone only where it may be.
    public Result<string, LibraryError> Resolve(string? user, bool everyone = false) =>
        Run(c => Canonical(c, null, user, everyone));

    internal static Result<string, LibraryError> Canonical(SqliteConnection c, SqliteTransaction? t, string? user,
        bool everyone = false)
    {
        var text = user?.Trim() ?? "";
        if (text.Length == 0)
            return Household;
        if (text == Everyone)
            return everyone ? Everyone : Result.Fail(LibraryError.InvalidArgument, $"{Everyone} names no one user here");
        using var command = Command(c, t, "SELECT name FROM household_user WHERE name = $name", ("$name", text));
        return command.ExecuteScalar() is string name
            ? name
            : Result.Fail(LibraryError.InvalidArgument, $"no user {text}");
    }

    // The rows a scope reads, as a condition on a table's `user` with $user
    // as its parameter: the household's alone, a user's with the household's
    // (only: theirs alone), or everyone's.
    internal static string Scope(string user, bool only = false) => user switch
    {
        Everyone => "1 = 1",
        Household => "user = ''",
        _ => only ? "user = $user" : "user IN ('', $user)",
    };

    // An item marked by the household and by the user is answered once, as
    // the user's; the order of the rows is kept.
    internal static List<T> Merged<T, TKey>(List<T> rows, string user, Func<T, TKey> key, Func<T, string> userOf)
        where TKey : notnull
    {
        if (user is Everyone or Household)
            return rows;
        var own = rows.Where(r => userOf(r).Length > 0).Select(key).ToHashSet();
        return [.. rows.Where(r => userOf(r).Length > 0 || !own.Contains(key(r)))];
    }

    private static int Execute(SqliteConnection c, SqliteTransaction t, string sql, params (string Name, object? Value)[] parameters)
    {
        using var command = Command(c, t, sql, parameters);
        return command.ExecuteNonQuery();
    }

    private static SqliteCommand Command(SqliteConnection c, SqliteTransaction? t, string sql,
        params (string Name, object? Value)[] parameters)
    {
        var command = c.CreateCommand();
        command.Transaction = t;
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
