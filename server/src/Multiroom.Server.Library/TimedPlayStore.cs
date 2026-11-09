using System.Text.Json;
using System.Text.Json.Serialization;
using Microsoft.Data.Sqlite;
using Multiroom.Server.Abstractions;

namespace Multiroom.Server.Library;

// One block of a timed play's week: on Day from Start to End, in minutes of
// the server's local day (End 1440 is midnight). Resume (the default)
// continues where the last run of the timed play stopped, else the playlist
// starts over. Volume
// is the zone's while it plays, when OverrideVolume; the timed play's otherwise.
public sealed record TimedPlayBlock(
    DayOfWeek Day,
    int Start,
    int End,
    bool Enabled = true,
    bool Resume = true,
    bool OverrideVolume = false,
    double Volume = 0.3);

// What a timed play does: a playlist in some rooms, as one zone at a zone
// volume, with AutoFill, shuffle and repeat as given, faded in and out, in the
// blocks of its week (TimedPlays runs it).
public sealed record TimedPlayDefinition(
    string Name,
    bool Enabled = true,
    IReadOnlyList<string>? Rooms = null,
    long? Playlist = null,
    double Volume = 0.3,
    bool AutoFill = false,
    bool Shuffle = false,
    RepeatMode Repeat = RepeatMode.Off,
    int FadeInSeconds = 3,
    int FadeOutSeconds = 3,
    IReadOnlyList<TimedPlayBlock>? Blocks = null)
{
    public IReadOnlyList<string> Rooms { get; init; } = Rooms ?? [];

    public IReadOnlyList<TimedPlayBlock> Blocks { get; init; } = Blocks ?? [];
}

// A start the scheduler made: the zone, the queue items it put there (the
// queue is still the timed play's while it holds one of them), and the zone
// volume it played at.
public sealed record TimedPlayRun(string Zone, IReadOnlyList<long> Items, double Volume);

// Where the last run stopped: the index among its queued items, and the
// position in that item.
public sealed record TimedPlayResume(int Index, long PositionMs);

public sealed record TimedPlay(long Id, TimedPlayDefinition Definition, TimedPlayRun? Run, TimedPlayResume? Resume);

// The timed plays, in the library's database (Migrations/0032). Changed
// names the timed play after any write that changed it, its run and resume
// place included, whoever wrote: the REST API, a tool or the scheduler.
public sealed class TimedPlayStore(LibraryDatabase database)
{
    public event EventHandler<long>? Changed;

    public const int MaxNameLength = 64;
    public const int MaxFadeSeconds = 600;
    public const int MinutesPerDay = 24 * 60;

    private static readonly JsonSerializerOptions Options = new(JsonSerializerDefaults.Web)
    {
        Converters = { new JsonStringEnumConverter(JsonNamingPolicy.CamelCase) },
    };

    public Result<IReadOnlyList<TimedPlay>, LibraryError> All() =>
        Run<IReadOnlyList<TimedPlay>>(c =>
        {
            using var command = Command(c, "SELECT id, definition, run, resume FROM timed_play ORDER BY id");
            using var reader = command.ExecuteReader();
            var all = new List<TimedPlay>();
            while (reader.Read())
                all.Add(Read(reader));
            return all;
        });

    public Result<TimedPlay, LibraryError> Get(long id) =>
        Run<TimedPlay>(c =>
        {
            using var command = Command(c, "SELECT id, definition, run, resume FROM timed_play WHERE id = $id", ("$id", id));
            using var reader = command.ExecuteReader();
            return reader.Read() ? Read(reader) : Result.Fail(LibraryError.NotFound, $"no timed play {id}");
        });

    public Result<TimedPlay, LibraryError> Create(TimedPlayDefinition definition)
    {
        var valid = Validated(definition);
        if (!valid.IsSuccess)
            return valid.Failure;
        var created = Run<long>(c =>
        {
            using var command = Command(c, "INSERT INTO timed_play (definition) VALUES ($definition) RETURNING id",
                ("$definition", JsonSerializer.Serialize(valid.Value, Options)));
            return (long)command.ExecuteScalar()!;
        });
        if (!created.IsSuccess)
            return created.Failure;
        Changed?.Invoke(this, created.Value);
        return Get(created.Value);
    }

    // The definition replaced; its run and resume place are kept.
    public Result<TimedPlay, LibraryError> Update(long id, TimedPlayDefinition definition)
    {
        var valid = Validated(definition);
        if (!valid.IsSuccess)
            return valid.Failure;
        var set = Execute("UPDATE timed_play SET definition = $definition WHERE id = $id",
            ("$id", id), ("$definition", JsonSerializer.Serialize(valid.Value, Options)));
        if (!set.IsSuccess)
            return set.Failure;
        if (!set.Value)
            return Result.Fail(LibraryError.NotFound, $"no timed play {id}");
        Changed?.Invoke(this, id);
        return Get(id);
    }

    public Result<bool, LibraryError> Delete(long id)
    {
        var deleted = Execute("DELETE FROM timed_play WHERE id = $id", ("$id", id));
        if (deleted is { IsSuccess: true, Value: false })
            return Result.Fail(LibraryError.NotFound, $"no timed play {id}");
        if (deleted.IsSuccess)
            Changed?.Invoke(this, id);
        return deleted;
    }

    public Result<bool, LibraryError> SetRun(long id, TimedPlayRun? run) =>
        Notified(id, Execute("UPDATE timed_play SET run = $run WHERE id = $id",
            ("$id", id), ("$run", run is null ? null : JsonSerializer.Serialize(run, Options))));

    public Result<bool, LibraryError> SetResume(long id, TimedPlayResume? resume) =>
        Notified(id, Execute("UPDATE timed_play SET resume = $resume WHERE id = $id",
            ("$id", id), ("$resume", resume is null ? null : JsonSerializer.Serialize(resume, Options))));

    private Result<bool, LibraryError> Notified(long id, Result<bool, LibraryError> written)
    {
        if (written is { IsSuccess: true, Value: true })
            Changed?.Invoke(this, id);
        return written;
    }

    // Trimmed name and rooms; every value in range, and a day's enabled or
    // not blocks never overlapping.
    public static Result<TimedPlayDefinition, LibraryError> Validated(TimedPlayDefinition definition)
    {
        var name = definition.Name.Trim();
        if (name.Length is 0 or > MaxNameLength)
            return Invalid($"name: 1 to {MaxNameLength} characters");
        var rooms = definition.Rooms.Select(r => r.Trim()).ToList();
        if (rooms.Any(r => r.Length == 0) || rooms.Distinct(StringComparer.Ordinal).Count() != rooms.Count)
            return Invalid("rooms: distinct room ids, none empty");
        if (definition.Volume is < 0.0 or > 1.0)
            return Invalid("volume: 0.0 to 1.0");
        if (definition.FadeInSeconds is < 0 or > MaxFadeSeconds || definition.FadeOutSeconds is < 0 or > MaxFadeSeconds)
            return Invalid($"fadeInSeconds and fadeOutSeconds: 0 to {MaxFadeSeconds}");
        if (!Enum.IsDefined(definition.Repeat))
            return Invalid("repeat: off, one or all");
        foreach (var block in definition.Blocks)
        {
            if (!Enum.IsDefined(block.Day))
                return Invalid("a block's day: monday to sunday");
            if (block.Start < 0 || block.End > MinutesPerDay || block.Start >= block.End)
                return Invalid($"a block runs from start to a later end, minutes 0 to {MinutesPerDay}");
            if (block.Volume is < 0.0 or > 1.0)
                return Invalid("a block's volume: 0.0 to 1.0");
        }
        foreach (var day in definition.Blocks.GroupBy(b => b.Day))
        {
            var sorted = day.OrderBy(b => b.Start).ToList();
            for (var i = 1; i < sorted.Count; i++)
            {
                if (sorted[i].Start < sorted[i - 1].End)
                    return Invalid($"blocks on {day.Key} overlap");
            }
        }
        return definition with
        {
            Name = name,
            Rooms = rooms,
            Blocks = [.. definition.Blocks.OrderBy(b => (((int)b.Day + 6) % 7, b.Start))],
        };
    }

    private static Failure<LibraryError> Invalid(string detail) => Result.Fail(LibraryError.InvalidArgument, detail);

    private static TimedPlay Read(SqliteDataReader reader) => new(
        reader.GetInt64(0),
        JsonSerializer.Deserialize<TimedPlayDefinition>(reader.GetString(1), Options)!,
        reader.IsDBNull(2) ? null : JsonSerializer.Deserialize<TimedPlayRun>(reader.GetString(2), Options),
        reader.IsDBNull(3) ? null : JsonSerializer.Deserialize<TimedPlayResume>(reader.GetString(3), Options));

    private Result<bool, LibraryError> Execute(string sql, params (string Name, object? Value)[] parameters) =>
        Run<bool>(c =>
        {
            using var command = Command(c, sql, parameters);
            return command.ExecuteNonQuery() > 0;
        });

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
