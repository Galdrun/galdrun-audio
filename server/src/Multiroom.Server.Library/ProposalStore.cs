using System.Text.Json;
using System.Text.Json.Serialization;
using Microsoft.Data.Sqlite;
using Multiroom.Server.Abstractions;

namespace Multiroom.Server.Library;

public enum ProposalKind
{
    Track,
    Album,
    Artist,
    Audiobook,
}

public enum ProposalTrigger
{
    Manual,
    Background,
}

public enum ProposalRunState
{
    Running,
    Ok,
    Partial,
    Failed,
}

// Proposals' own failures (phase 38).
public enum ProposalError
{
    // No AI provider and model to ask.
    NotConfigured,

    // An unknown user, or a request the proposals cannot take.
    InvalidArgument,

    // No such proposal.
    NotFound,

    // The database could not be read or written.
    Unavailable,
}

// What a run cost, as the AI registry counts it.
public sealed record ProposalUsage(long InputTokens = 0, long OutputTokens = 0, long ReasoningTokens = 0,
    long WebSearches = 0);

// One asking of the AI for a scope (User, "" the household). NotFound is how
// many named items the house could not play.
public sealed record ProposalRun(
    long Id,
    string User,
    ProposalTrigger Trigger,
    DateTimeOffset Started,
    DateTimeOffset? Finished,
    ProposalRunState State,
    string? Error,
    ProposalUsage? Usage,
    int NotFound,
    [property: JsonIgnore] string Fingerprint);

// One item proposed. Item is the snapshot in its usual shape: a Track, an
// Album, an Artist or an Audiobook, answered with its kind (phase 40).
public sealed record ProposalItem(long Id, MediaItem Item, string? Reason) : IMediaEntry
{
    [JsonIgnore]
    public ProposalKind Kind { get; init; }

    [JsonIgnore]
    public string ProviderId { get; init; } = "";

    [JsonIgnore]
    public string ProviderItemId { get; init; } = "";

    [JsonIgnore]
    public string Key { get; init; } = "";
}

// A proposal as a run stores it, before it has an id.
public sealed record NewProposal(ProposalKind Kind, string ProviderId, string ProviderItemId, string Key, MediaItem Item,
    string? Reason);

// A scope's last finished good run and its items by kind, and its newest run
// whatever its state; either may be absent.
public sealed record ProposalLists(ProposalRun? Latest, ProposalRun? Good, IReadOnlyList<ProposalItem> Tracks,
    IReadOnlyList<ProposalItem> Albums, IReadOnlyList<ProposalItem> Artists, IReadOnlyList<ProposalItem> Audiobooks);

// What a scope must not be proposed: the items of its last runs and what it
// dismissed, by (kind, provider, item) and by key.
public sealed record ProposalAvoid(IReadOnlySet<(ProposalKind, string, string)> Items, IReadOnlySet<string> Keys,
    IReadOnlyList<Track> Tracks);

// The proposals as every caller reads and asks them (phase 38): the REST API
// and the MCP tools alike. The runner in Web implements it, as AutoFill's
// chooser is implemented there.
public interface IProposals
{
    // `user` as a like's write takes it: "" or absent the household.
    Result<ProposalsView, ProposalError> View(string? user);

    // A scope already running answers that run; there is never a second.
    Result<ProposalRun, ProposalError> Refresh(string? user);

    // In the scope of the run that proposed it.
    Result<bool, ProposalError> Dismiss(long proposal);
}

// What a view of the proposals needs to explain itself: whether background
// runs are on and when the next may come, whether an AI can be asked (and
// why not), the newest run, and the newest good run's lists.
public sealed record ProposalsView(
    ProposalBackground Background,
    bool AiAvailable,
    string? Reason,
    ProposalRun? Run,
    IReadOnlyList<ProposalItem> Tracks,
    IReadOnlyList<ProposalItem> Albums,
    IReadOnlyList<ProposalItem> Artists,
    IReadOnlyList<ProposalItem> Audiobooks);

public sealed record ProposalBackground(bool Enabled, int IntervalHours, DateTimeOffset? NextRunAt);

// Proposals (phase 38) in the library's database (Migrations/0019). A run
// adds to its scope's lists: they are every run's proposals, newest first,
// up to `keep` of each kind, the oldest removed past it. A run is kept while
// it holds a proposal, and the newest KeptRuns runs and the newest good one
// always, so a failed run never hides the lists. Nothing here asks an AI or
// resolves: the runner does.
public sealed class ProposalStore(LibraryDatabase database)
{
    public const int KeptRuns = 2;
    public const int MaxDismissed = 1000;

    private static readonly JsonSerializerOptions Json = new(JsonSerializerDefaults.Web);

    private const string RunColumns = """
        SELECT id, user, triggered_by, started, finished, state, error, usage, not_found, fingerprint FROM proposal_run
        """;

    // At start: a run that was running when the server stopped never ends.
    public Result<int, ProposalError> FailInterrupted(DateTimeOffset now) => Run<int>(c => Execute(c, null, """
        UPDATE proposal_run SET state = 'failed', finished = $now, error = 'interrupted by a restart of the server'
        WHERE state = 'running'
        """, ("$now", now.UtcTicks)));

    public Result<ProposalRun, ProposalError> Start(string user, ProposalTrigger trigger, string fingerprint,
        DateTimeOffset now) => Run<ProposalRun>(c =>
        {
            var id = Rows(c, null, """
                INSERT INTO proposal_run (user, triggered_by, started, state, fingerprint)
                VALUES ($user, $trigger, $now, 'running', $fingerprint) RETURNING id
                """, r => r.GetInt64(0), ("$user", user), ("$trigger", Name(trigger)), ("$now", now.UtcTicks),
                ("$fingerprint", fingerprint))[0];
            return RunById(c, null, id)!;
        });

    // The run ends with what it found, ranked in the AI's order within each
    // kind; then each kind holds the scope's newest `keep` proposals, and the
    // runs left holding none go (but the newest KeptRuns and the newest good one).
    public Result<ProposalRun, ProposalError> Finish(long run, ProposalRunState state, string? error, ProposalUsage usage,
        int notFound, IReadOnlyList<NewProposal> proposals, int keep, DateTimeOffset now) => Run<ProposalRun>(c =>
        {
            using var transaction = c.BeginTransaction();
            Execute(c, transaction, """
                UPDATE proposal_run SET state = $state, error = $error, usage = $usage, not_found = $notFound, finished = $now
                WHERE id = $id
                """, ("$id", run), ("$state", Name(state)), ("$error", error), ("$usage", JsonSerializer.Serialize(usage, Json)),
                ("$notFound", notFound), ("$now", now.UtcTicks));
            foreach (var (proposal, rank) in proposals.Select((p, i) => (p, i)))
            {
                Execute(c, transaction, """
                    INSERT INTO proposal (run_id, kind, provider_id, provider_item_id, item_key, snapshot, reason, rank)
                    VALUES ($run, $kind, $provider, $item, $key, $snapshot, $reason, $rank)
                    """, ("$run", run), ("$kind", Name(proposal.Kind)), ("$provider", proposal.ProviderId),
                    ("$item", proposal.ProviderItemId), ("$key", proposal.Key),
                    ("$snapshot", JsonSerializer.Serialize(proposal.Item, proposal.Item.GetType(), Json)),
                    ("$reason", proposal.Reason), ("$rank", rank));
            }
            var user = RunById(c, transaction, run)!.User;
            foreach (var kind in Enum.GetValues<ProposalKind>())
            {
                Execute(c, transaction, """
                    DELETE FROM proposal WHERE id IN (
                        SELECT p.id FROM proposal p JOIN proposal_run r ON r.id = p.run_id
                        WHERE r.user = $user AND p.kind = $kind ORDER BY p.run_id DESC, p.rank LIMIT -1 OFFSET $keep)
                    """, ("$user", user), ("$kind", Name(kind)), ("$keep", Math.Max(0, keep)));
            }
            Execute(c, transaction, $"""
                DELETE FROM proposal_run WHERE user = $user AND state <> 'running'
                  AND NOT EXISTS (SELECT 1 FROM proposal WHERE run_id = proposal_run.id)
                  AND id NOT IN (SELECT id FROM proposal_run WHERE user = $user ORDER BY id DESC LIMIT {KeptRuns})
                  AND id <> COALESCE((SELECT MAX(id) FROM proposal_run WHERE user = $user AND state IN ('ok', 'partial')), 0)
                """, ("$user", user));
            transaction.Commit();
            return RunById(c, null, run)!;
        });

    public Result<ProposalRun?, ProposalError> Latest(string user) =>
        Run<ProposalRun?>(c => Rows(c, null, RunColumns + " WHERE user = $user ORDER BY id DESC LIMIT 1", ReadRun,
            ("$user", user)).FirstOrDefault());

    public Result<ProposalRun?, ProposalError> GetRun(long run) => Run<ProposalRun?>(c => RunById(c, null, run));

    // Runs started since `since`, every scope counted.
    public Result<int, ProposalError> RunsSince(DateTimeOffset since) => Run<int>(c =>
        Rows(c, null, "SELECT COUNT(*) FROM proposal_run WHERE started >= $since", r => r.GetInt32(0),
            ("$since", since.UtcTicks))[0]);

    // The scope's lists: every run's items, newest run first and in the AI's
    // order within a run, each item once (its newest), less what was dismissed.
    public Result<ProposalLists, ProposalError> Lists(string user) => Run<ProposalLists>(c =>
    {
        var latest = Rows(c, null, RunColumns + " WHERE user = $user ORDER BY id DESC LIMIT 1", ReadRun, ("$user", user))
            .FirstOrDefault();
        var good = Rows(c, null, RunColumns + " WHERE user = $user AND state IN ('ok', 'partial') ORDER BY id DESC LIMIT 1",
            ReadRun, ("$user", user)).FirstOrDefault();
        var items = Rows(c, null, """
            SELECT p.id, p.kind, p.provider_id, p.provider_item_id, p.item_key, p.snapshot, p.reason
            FROM proposal p JOIN proposal_run r ON r.id = p.run_id
            WHERE r.user = $user ORDER BY p.run_id DESC, p.kind, p.rank
            """, ReadItem, ("$user", user)).DistinctBy(i => (i.Kind, i.Key)).ToList();
        List<ProposalItem> Of(ProposalKind kind) => [.. items.Where(i => i.Kind == kind)];
        return new ProposalLists(latest, good, Of(ProposalKind.Track), Of(ProposalKind.Album), Of(ProposalKind.Artist),
            Of(ProposalKind.Audiobook));
    });

    // Every item the scope's lists hold and what it dismissed; as tracks to
    // name to the AI, only its last KeptRuns runs' (the prompt stays short).
    public Result<ProposalAvoid, ProposalError> Avoid(string user) => Run<ProposalAvoid>(c =>
    {
        var proposed = Rows(c, null, """
            SELECT p.id, p.kind, p.provider_id, p.provider_item_id, p.item_key, p.snapshot, p.reason
            FROM proposal p JOIN proposal_run r ON r.id = p.run_id WHERE r.user = $user
            """, ReadItem, ("$user", user));
        var recent = Rows(c, null, $"""
            SELECT id, kind, provider_id, provider_item_id, item_key, snapshot, reason FROM proposal
            WHERE run_id IN (SELECT id FROM proposal_run WHERE user = $user ORDER BY id DESC LIMIT {KeptRuns})
            """, ReadItem, ("$user", user));
        var dismissed = Rows(c, null, "SELECT kind, provider_id, provider_item_id, item_key FROM proposal_dismissed WHERE user = $user",
            r => (Kind: KindOf(r.GetString(0)), Provider: r.GetString(1), Item: r.GetString(2), Key: r.GetString(3)),
            ("$user", user));
        return new ProposalAvoid(
            proposed.Select(p => (p.Kind, p.ProviderId, p.ProviderItemId)).Concat(dismissed.Select(d => (d.Kind, d.Provider, d.Item)))
                .ToHashSet(),
            proposed.Select(p => p.Key).Concat(dismissed.Select(d => d.Key)).ToHashSet(StringComparer.Ordinal),
            [.. recent.Select(p => p.Item).OfType<Track>()]);
    });

    // A proposal leaves its list and is avoided from then on, in the scope
    // of the run that proposed it; answered with that scope.
    public Result<(ProposalItem Item, string User), ProposalError> Dismiss(long proposal, DateTimeOffset now) =>
        Run<(ProposalItem, string)>(c =>
        {
            using var transaction = c.BeginTransaction();
            var found = Rows(c, transaction, """
                SELECT p.id, p.kind, p.provider_id, p.provider_item_id, p.item_key, p.snapshot, p.reason, r.user
                FROM proposal p JOIN proposal_run r ON r.id = p.run_id WHERE p.id = $id
                """, r => (Item: ReadItem(r), User: r.GetString(7)), ("$id", proposal)).FirstOrDefault();
            if (found.Item is null)
                return Result.Fail(ProposalError.NotFound, $"no proposal {proposal}");
            var user = found.User;
            Execute(c, transaction, """
                INSERT INTO proposal_dismissed (user, kind, provider_id, provider_item_id, item_key, dismissed)
                VALUES ($user, $kind, $provider, $item, $key, $now)
                """, ("$user", user), ("$kind", Name(found.Item.Kind)), ("$provider", found.Item.ProviderId),
                ("$item", found.Item.ProviderItemId), ("$key", found.Item.Key), ("$now", now.UtcTicks));
            Execute(c, transaction, "DELETE FROM proposal WHERE id = $id", ("$id", proposal));
            Execute(c, transaction, $"""
                DELETE FROM proposal_dismissed WHERE user = $user
                  AND id NOT IN (SELECT id FROM proposal_dismissed WHERE user = $user ORDER BY id DESC LIMIT {MaxDismissed})
                """, ("$user", user));
            transaction.Commit();
            return found;
        });

    private static ProposalRun? RunById(SqliteConnection c, SqliteTransaction? t, long id) =>
        Rows(c, t, RunColumns + " WHERE id = $id", ReadRun, ("$id", id)).FirstOrDefault();

    private static ProposalRun ReadRun(SqliteDataReader r) => new(
        r.GetInt64(0), r.GetString(1), r.GetString(2) == "manual" ? ProposalTrigger.Manual : ProposalTrigger.Background,
        new DateTimeOffset(r.GetInt64(3), TimeSpan.Zero), r.IsDBNull(4) ? null : new DateTimeOffset(r.GetInt64(4), TimeSpan.Zero),
        Enum.Parse<ProposalRunState>(r.GetString(5), ignoreCase: true), r.IsDBNull(6) ? null : r.GetString(6),
        r.IsDBNull(7) ? null : JsonSerializer.Deserialize<ProposalUsage>(r.GetString(7), Json), r.GetInt32(8), r.GetString(9));

    private static ProposalItem ReadItem(SqliteDataReader r)
    {
        var kind = KindOf(r.GetString(1));
        var json = r.GetString(5);
        MediaItem item = kind switch
        {
            ProposalKind.Track => JsonSerializer.Deserialize<Track>(json, Json)!,
            ProposalKind.Album => JsonSerializer.Deserialize<Album>(json, Json)!,
            ProposalKind.Artist => JsonSerializer.Deserialize<Artist>(json, Json)!,
            _ => JsonSerializer.Deserialize<Audiobook>(json, Json)!,
        };
        return new ProposalItem(r.GetInt64(0), item, r.IsDBNull(6) ? null : r.GetString(6))
        {
            Kind = kind,
            ProviderId = r.GetString(2),
            ProviderItemId = r.GetString(3),
            Key = r.GetString(4),
        };
    }

    private static ProposalKind KindOf(string name) => Enum.Parse<ProposalKind>(name, ignoreCase: true);

    private static string Name<T>(T value) where T : struct, Enum =>
        JsonNamingPolicy.CamelCase.ConvertName(value.ToString());

    private static int Execute(SqliteConnection c, SqliteTransaction? t, string sql, params (string Name, object? Value)[] parameters)
    {
        using var command = Command(c, t, sql, parameters);
        return command.ExecuteNonQuery();
    }

    private static List<T> Rows<T>(SqliteConnection c, SqliteTransaction? t, string sql, Func<SqliteDataReader, T> map,
        params (string Name, object? Value)[] parameters)
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
#pragma warning disable CA2100 // Only this class's own statements and constants; every value is a parameter.
        command.CommandText = sql;
#pragma warning restore CA2100
        foreach (var (name, value) in parameters)
            command.Parameters.AddWithValue(name, value ?? DBNull.Value);
        return command;
    }

    private Result<T, ProposalError> Run<T>(Func<SqliteConnection, Result<T, ProposalError>> run)
    {
        try
        {
            using var connection = database.Open();
            return run(connection);
        }
        catch (Exception e) when (e is SqliteException or JsonException)
        {
            return Result.Fail(ProposalError.Unavailable, e.Message);
        }
    }
}
