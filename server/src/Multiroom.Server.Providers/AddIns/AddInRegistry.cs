using System.Diagnostics;
using System.Text.Json;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;
using Multiroom.Server.Abstractions;

namespace Multiroom.Server.Providers.AddIns;

// One add-in to be started: its id, how to make it, and how long one call
// of it may take.
public sealed record AddInRegistration(string Id, Func<IServiceProvider, IAddIn> Create, TimeSpan Timeout);

public static class AddInServiceCollectionExtensions
{
    public static readonly TimeSpan DefaultTimeout = TimeSpan.FromSeconds(5);

    // One line per add-in, in the host.
    public static IServiceCollection AddAddIn(this IServiceCollection services, string id,
        Func<IServiceProvider, IAddIn> create, TimeSpan? timeout = null) =>
        services.AddSingleton(new AddInRegistration(id, create, timeout ?? DefaultTimeout));
}

public enum AddInState
{
    Ready,

    // Failed to start, so the registry holds no instance.
    Disabled,
}

// What an add-in has cost since the server started: calls, the answers that
// were failures, the calls abandoned at the timeout, those refused because
// MaxInFlight calls had not answered, the calls not answered yet (abandoned
// ones included), and the time callers waited, in total.
public sealed record AddInStatus(
    string Id,
    AddInState State,
    string Detail,
    string? DisplayName,
    IReadOnlyList<string> Kinds,
    long TimeoutMs,
    long Calls,
    long Failures,
    long Timeouts,
    long Busy,
    long InFlight,
    long WaitedMs);

// One track of a merged "more like this": its score summed over the add-ins
// that named it, and what each one said.
public sealed record RecommendedTrack(Track Track, double Score, IReadOnlyList<RecommendationSource> Sources);

// Score is the add-in's own, scaled so its best answer is 1.
public sealed record RecommendationSource(string AddIn, double Score, string Reason);

public sealed record RecommendationCount(string AddIn, int Tracks);

public sealed record AddInFailure(string AddIn, string Error, string Message);

public sealed record RecommendationAnswer(
    IReadOnlyList<RecommendedTrack> Items,
    IReadOnlyList<RecommendationCount> Sources,
    IReadOnlyList<AddInFailure> Failures);

// Every registered add-in, started once in registration order, like the
// providers: one that fails to start (its factory threw, it reports another
// id, it is neither kind) is disabled, logged and asked for nothing; an id
// registered twice refuses the server.
//
// Every call goes through here, bounded (the contract in AddIns.cs): by the
// add-in's timeout, and by MaxInFlight calls not answered yet, abandoned ones
// included, past which the add-in is not asked. An exception is a failure.
public sealed partial class AddInRegistry(
    IEnumerable<AddInRegistration> registrations,
    IServiceProvider services,
    ILogger<AddInRegistry> log) : IHostedService, IAsyncDisposable
{
    public const int MaxInFlight = 4;
    public const int MaxSeed = 100;

    private IReadOnlyList<Entry> _entries = [];

    public Task StartAsync(CancellationToken cancellationToken)
    {
        var all = registrations.ToList();
        var twice = all.GroupBy(r => r.Id, StringComparer.Ordinal).FirstOrDefault(g => g.Count() > 1);
        if (twice is not null)
            throw new InvalidOperationException($"add-in id {twice.Key} is registered twice");
        _entries = [.. all.Select(Start)];
        return Task.CompletedTask;
    }

    public Task StopAsync(CancellationToken cancellationToken) => Task.CompletedTask;

    public IReadOnlyList<AddInStatus> Status => [.. _entries.Select(e => new AddInStatus(e.Id,
        e.AddIn is null ? AddInState.Disabled : AddInState.Ready, e.Reason, e.AddIn?.DisplayName, KindsOf(e.AddIn),
        (long)e.Timeout.TotalMilliseconds, Interlocked.Read(ref e.Calls), Interlocked.Read(ref e.Failures),
        Interlocked.Read(ref e.Timeouts), Interlocked.Read(ref e.Busy), Interlocked.Read(ref e.InFlight),
        Interlocked.Read(ref e.WaitedMs)))];

    // The enrichment add-ins that started, in registration order.
    public IReadOnlyList<IEnrichmentPlugin> Enrichers => [.. _entries.Select(e => e.AddIn).OfType<IEnrichmentPlugin>()];

    public Task<Result<JsonElement, AddInError>> EnrichAsync(IEnrichmentPlugin enricher, Track track,
        CancellationToken cancellationToken) =>
        CallAsync(_entries.First(e => ReferenceEquals(e.AddIn, enricher)),
            ct => enricher.EnrichAsync(track, ct), cancellationToken);

    // Every recommendation add-in, in parallel, with at most MaxSeed seed
    // tracks (the first ones). Each one's scores are scaled
    // so its best is 1, then summed per track; the seed's own tracks are left
    // out, and a track one add-in names twice counts once, at its best. An add-in that fails, is busy or does not answer in time is named
    // in Failures and costs nothing more: the others' answers are merged.
    public async Task<RecommendationAnswer> RecommendAsync(IReadOnlyList<Track> seed, int limit,
        CancellationToken cancellationToken)
    {
        // Music only, both ways (phase 30): an audiobook is no seed and no answer.
        seed = [.. seed.Where(MediaTypeClassifier.IsMusic).Take(MaxSeed)];
        var asked = _entries.Where(e => e.AddIn is IRecommendationPlugin).Select(async e =>
            (e.Id, Answer: await CallAsync(e, ct => ((IRecommendationPlugin)e.AddIn!).RecommendAsync(seed, limit, ct),
                cancellationToken).ConfigureAwait(false)));
        var answers = await Task.WhenAll(asked).ConfigureAwait(false);

        var inSeed = seed.Select(t => (t.ProviderId, t.ProviderItemId)).ToHashSet();
        var merged = new Dictionary<(string, string), (Track Track, List<RecommendationSource> Sources)>();
        var counts = new List<RecommendationCount>();
        var failures = new List<AddInFailure>();
        foreach (var (id, answer) in answers)
        {
            if (!answer.IsSuccess)
            {
                failures.Add(new AddInFailure(id, ApiError.From(answer.Failure).Error, answer.Failure.Detail));
                continue;
            }
            var usable = answer.Value.Where(r => double.IsFinite(r.Score) && r.Score > 0 && MediaTypeClassifier.IsMusic(r.Track) &&
                    !inSeed.Contains((r.Track.ProviderId, r.Track.ProviderItemId)))
                .OrderByDescending(r => r.Score)
                .DistinctBy(r => (r.Track.ProviderId, r.Track.ProviderItemId))
                .ToList();
            counts.Add(new RecommendationCount(id, usable.Count));
            var best = usable.Count == 0 ? 1 : usable.Max(r => r.Score);
            foreach (var r in usable)
            {
                var key = (r.Track.ProviderId, r.Track.ProviderItemId);
                if (!merged.TryGetValue(key, out var entry))
                    merged[key] = entry = (r.Track, []);
                entry.Sources.Add(new RecommendationSource(id, r.Score / best, r.Reason));
            }
        }

        return new RecommendationAnswer([.. merged.Values
            .Select(m => new RecommendedTrack(m.Track, m.Sources.Sum(s => s.Score), m.Sources))
            .OrderByDescending(r => r.Score)
            .ThenBy(r => r.Track.Title, StringComparer.OrdinalIgnoreCase)
            .ThenBy(r => r.Track.ProviderId, StringComparer.Ordinal)
            .ThenBy(r => r.Track.ProviderItemId, StringComparer.Ordinal)
            .Take(limit)], counts, failures);
    }

    public ValueTask DisposeAsync()
    {
        foreach (var entry in _entries)
        {
            if (entry.AddIn is IDisposable disposable)
                disposable.Dispose();
        }
        _entries = [];
        return ValueTask.CompletedTask;
    }

    private static async Task<Result<T, AddInError>> CallAsync<T>(Entry entry,
        Func<CancellationToken, Task<Result<T, AddInError>>> call, CancellationToken cancellationToken)
    {
        Interlocked.Increment(ref entry.Calls);
        if (Interlocked.Increment(ref entry.InFlight) > MaxInFlight)
        {
            Interlocked.Decrement(ref entry.InFlight);
            Interlocked.Increment(ref entry.Busy);
            return Result.Fail(AddInError.Unavailable, $"{entry.Id} is busy: {MaxInFlight} calls have not answered");
        }

        var clock = Stopwatch.StartNew();
        var bounded = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        bounded.CancelAfter(entry.Timeout);
        Task<Result<T, AddInError>> task;
        try
        {
            task = call(bounded.Token);
        }
#pragma warning disable CA1031 // An add-in's fault is that add-in's missing answer, never the caller's.
        catch (Exception e)
#pragma warning restore CA1031
        {
            task = Task.FromException<Result<T, AddInError>>(e);
        }
        // Counted out when it ends, answered or abandoned; the token lives as
        // long as the call does.
        _ = task.ContinueWith(_ =>
        {
            Interlocked.Decrement(ref entry.InFlight);
            bounded.Dispose();
        }, CancellationToken.None, TaskContinuationOptions.ExecuteSynchronously, TaskScheduler.Default);

        try
        {
            var result = await task.WaitAsync(entry.Timeout, cancellationToken).ConfigureAwait(false);
            if (!result.IsSuccess)
                Interlocked.Increment(ref entry.Failures);
            return result;
        }
        catch (Exception e) when (e is TimeoutException ||
            (e is OperationCanceledException && !cancellationToken.IsCancellationRequested))
        {
            Interlocked.Increment(ref entry.Timeouts);
            return Result.Fail(AddInError.Timeout,
                $"no answer within {entry.Timeout.TotalMilliseconds:0} ms");
        }
#pragma warning disable CA1031 // An add-in's fault is that add-in's missing answer, never the caller's.
        catch (Exception e) when (!cancellationToken.IsCancellationRequested)
#pragma warning restore CA1031
        {
            Interlocked.Increment(ref entry.Failures);
            return Result.Fail(AddInError.Failed, e.Message);
        }
        finally
        {
            Interlocked.Add(ref entry.WaitedMs, clock.ElapsedMilliseconds);
        }
    }

    private Entry Start(AddInRegistration registration)
    {
        IAddIn addIn;
        try
        {
            addIn = registration.Create(services);
        }
#pragma warning disable CA1031 // An add-in's fault is that add-in's outage, never the server's.
        catch (Exception e)
#pragma warning restore CA1031
        {
            return Disable(registration, e.Message);
        }

        var refusal = addIn.Id != registration.Id
            ? $"registered as {registration.Id} but reports {addIn.Id}"
            : addIn is not (IRecommendationPlugin or IEnrichmentPlugin)
                ? "neither a recommendation nor an enrichment add-in"
                : null;
        if (refusal is not null)
        {
            (addIn as IDisposable)?.Dispose();
            return Disable(registration, refusal);
        }

        var kinds = string.Join(", ", KindsOf(addIn));
        LogStarted(log, addIn.Id, addIn.DisplayName, kinds, registration.Timeout.TotalMilliseconds);
        return new Entry(registration.Id, addIn, "", registration.Timeout);
    }

    private Entry Disable(AddInRegistration registration, string reason)
    {
        LogDisabled(log, registration.Id, reason);
        return new Entry(registration.Id, null, reason, registration.Timeout);
    }

    private static List<string> KindsOf(IAddIn? addIn) =>
    [
        .. addIn is IRecommendationPlugin ? ["recommendation"] : Array.Empty<string>(),
        .. addIn is IEnrichmentPlugin ? ["enrichment"] : Array.Empty<string>(),
    ];

    [LoggerMessage(Level = LogLevel.Information,
        Message = "add-in {Id} ({DisplayName}) started: {Kinds}, {TimeoutMs:0} ms per call")]
    private static partial void LogStarted(ILogger log, string id, string displayName, string kinds, double timeoutMs);

    [LoggerMessage(Level = LogLevel.Warning, Message = "add-in {Id} failed to start and is disabled: {Reason}")]
    private static partial void LogDisabled(ILogger log, string id, string reason);

    // AddIn is null when disabled; Reason says why. The counters are
    // Interlocked.
    private sealed class Entry(string id, IAddIn? addIn, string reason, TimeSpan timeout)
    {
        public long Calls;
        public long Failures;
        public long Timeouts;
        public long Busy;
        public long InFlight;
        public long WaitedMs;

        public string Id { get; } = id;

        public IAddIn? AddIn { get; } = addIn;

        public string Reason { get; } = reason;

        public TimeSpan Timeout { get; } = timeout;
    }
}
