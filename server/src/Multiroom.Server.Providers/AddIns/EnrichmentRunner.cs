using System.Diagnostics;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;
using Multiroom.Server.Abstractions;
using Multiroom.Server.Library;

namespace Multiroom.Server.Providers.AddIns;

// Asks each enrichment add-in about the library's tracks it has not been
// asked about (EnrichmentStore.Pending), BatchSize at a time and one call at
// a time, on a task of its own: never on a search or playback path.
//
// A track is recorded with what the add-in answered, or with its failure
// when it refused the track (not found, invalid, failed); that failure is not
// asked again until the track's file changes. An add-in that does not answer
// in time, is busy or is unavailable rests for RetryInterval, and the others
// go on. When nothing is left to ask, it waits for the indexer to record
// something, or for RetryInterval.
public sealed partial class EnrichmentRunner(
    AddInRegistry addIns,
    EnrichmentStore store,
    LibraryIndexer indexer,
    ILogger<EnrichmentRunner> log) : BackgroundService
{
    public const int BatchSize = 50;
    public static readonly TimeSpan RetryInterval = TimeSpan.FromSeconds(30);
    private static readonly TimeSpan Tick = TimeSpan.FromSeconds(1);

    // A stop ends it wherever it waits: what was recorded stays, the rest is
    // asked at the next start. The host takes a cancellation that escapes as
    // the service's failure.
    protected override async Task ExecuteAsync(CancellationToken stoppingToken)
    {
        try
        {
            await RunAsync(stoppingToken).ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (stoppingToken.IsCancellationRequested)
        {
        }
    }

    private async Task RunAsync(CancellationToken stoppingToken)
    {
        var enrichers = addIns.Enrichers;
        if (enrichers.Count == 0)
            return;

        var resting = new Dictionary<IEnrichmentPlugin, long>();
        while (!stoppingToken.IsCancellationRequested)
        {
            var library = (indexer.Status.Indexed, indexer.Status.Passes);
            var recorded = false;
            foreach (var enricher in enrichers)
            {
                if (resting.TryGetValue(enricher, out var since) &&
                    Stopwatch.GetElapsedTime(since) < RetryInterval)
                    continue;
                resting.Remove(enricher);
                var (asked, rests) = await BatchAsync(enricher, stoppingToken).ConfigureAwait(false);
                recorded |= asked;
                if (rests)
                    resting[enricher] = Stopwatch.GetTimestamp();
            }
            if (recorded)
                continue;

            var idle = Stopwatch.StartNew();
            while (idle.Elapsed < RetryInterval && (indexer.Status.Indexed, indexer.Status.Passes) == library)
                await Task.Delay(Tick, stoppingToken).ConfigureAwait(false);
        }
    }

    // Whether anything was recorded, and whether the add-in rests now.
    private async Task<(bool Recorded, bool Rests)> BatchAsync(IEnrichmentPlugin enricher,
        CancellationToken cancellationToken)
    {
        var pending = store.Pending(enricher.Id, BatchSize);
        if (!pending.IsSuccess)
        {
            LogStoreFailed(log, enricher.Id, pending.Failure.ToString());
            return (false, true);
        }

        var recorded = false;
        foreach (var track in pending.Value)
        {
            var answer = await addIns.EnrichAsync(enricher, track, cancellationToken).ConfigureAwait(false);
            if (!answer.IsSuccess && answer.Failure.Code is AddInError.Timeout or AddInError.Unavailable)
            {
                LogResting(log, enricher.Id, track.ProviderItemId, answer.Failure, RetryInterval.TotalSeconds);
                return (recorded, true);
            }
            var stored = answer.IsSuccess
                ? store.Record(enricher.Id, track.ProviderItemId, answer.Value, null)
                : store.Record(enricher.Id, track.ProviderItemId, null, answer.Failure.ToString());
            if (!stored.IsSuccess)
            {
                LogStoreFailed(log, enricher.Id, stored.Failure.ToString());
                return (recorded, true);
            }
            recorded = true;
        }
        return (recorded, false);
    }

    [LoggerMessage(Level = LogLevel.Information,
        Message = "add-in {Id} did not enrich {Track} ({Reason}); it rests for {Seconds:0} s")]
    private static partial void LogResting(ILogger log, string id, string track, Failure<AddInError> reason,
        double seconds);

    [LoggerMessage(Level = LogLevel.Error, Message = "enrichment of add-in {Id} could not use the library: {Reason}")]
    private static partial void LogStoreFailed(ILogger log, string id, string reason);
}
