using System.Collections.Concurrent;
using System.Threading.Channels;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;
using Multiroom.Server.Abstractions;

namespace Multiroom.Server.Playback;

public enum AutoFillSource
{
    // Phase 24's suggestion loop: a model names songs, the resolver finds them.
    Ai,

    // Phase 12's recommendation merge: for a house without an AI provider.
    Recommendations,
}

// One zone's AutoFill: when a playing queue has
// Threshold or fewer items after the current one, Count tracks are added at
// its end, chosen by Source with Request. Fallback fills from
// recommendations when the AI fails. AvoidRecent: tracks among the house's
// last this many plays are not chosen (0: none avoided).
public sealed record AutoFillSettings(
    bool Enabled = false,
    int Threshold = 3,
    int Count = 5,
    AutoFillSource Source = AutoFillSource.Ai,
    string Request = AutoFillSettings.DefaultRequest,
    bool Fallback = false,
    int AvoidRecent = AutoFillSettings.DefaultAvoidRecent)
{
    public const string DefaultRequest = "songs that continue this queue";
    public const int DefaultAvoidRecent = 100;
    public const int MaxThreshold = 20;
    public const int MaxCount = 50;
    public const int MaxAvoidRecent = 10_000;
    public const int MaxRequestLength = 500;
}

// Null keeps a setting.
public sealed record AutoFillPatch(
    bool? Enabled = null,
    int? Threshold = null,
    int? Count = null,
    AutoFillSource? Source = null,
    string? Request = null,
    bool? Fallback = null,
    int? AvoidRecent = null);

// What one fill did: how many tracks went in, from which source, whether the
// fallback filled, and why nothing (or the fallback) was used.
public sealed record AutoFillResult(string Zone, DateTimeOffset At, int Added, AutoFillSource Source, bool Fallback = false,
    string? Reason = null);

// A zone's AutoFill as a caller sees it: its settings, whether a fill is
// running, why it waits (repeatAll: a repeat-all queue never fills;
// shuffle: neither does a shuffled one; nothingAdded: the last fill added nothing, and the next waits until the
// queue is changed by other hands), and the last fill.
public sealed record AutoFillView(
    bool Enabled,
    int Threshold,
    int Count,
    AutoFillSource Source,
    string Request,
    bool Fallback,
    int AvoidRecent,
    bool Filling,
    string? Waiting,
    AutoFillResult? Last);

// What a fill adds, in order, and from where: the host's, which wires phase
// 24's suggestions and phase 12's recommendations. It never throws for a
// source that failed: it answers no tracks and the reason.
public sealed record AutoFillChoice(IReadOnlyList<Track> Tracks, AutoFillSource Source, bool Fallback = false,
    string? Reason = null);

public interface IAutoFillChooser
{
    // `queue` is the zone's tracks in order; none of them is to be chosen.
    Task<AutoFillChoice> ChooseAsync(string zone, IReadOnlyList<Track> queue, AutoFillSettings settings,
        CancellationToken cancellationToken);
}

// The trigger (phase 26): normal server logic on QueueEngine.QueueChanged, no
// MCP. The chooser only chooses; the tracks go in through the queue engine
// here. At most one fill per zone at a time; a fill that added nothing is not
// repeated until the queue's items change; a repeat-all or shuffled queue
// never fills.
// "Left" counts the items after the current one in queue order.
//
// QueueChanged is raised under the zone's gate, so the handler only notes
// the zone; the check and the fill run on this service's own task.
public sealed partial class AutoFill : BackgroundService
{
    // The items at the queue's end a fill seeds from: none of them music, and
    // the zone is not filled (phase 30).
    public const int SeedItems = 10;

    private readonly QueueEngine _queues;
    private readonly IAutoFillChooser _chooser;
    private readonly ILogger _log;
    private readonly TimeProvider _time;
    private readonly ConcurrentDictionary<string, ZoneFill> _zones = new(StringComparer.Ordinal);
    private readonly ConcurrentDictionary<string, bool> _pending = new(StringComparer.Ordinal);
    private readonly Channel<bool> _wake =
        Channel.CreateBounded<bool>(new BoundedChannelOptions(1) { FullMode = BoundedChannelFullMode.DropWrite });
    private readonly ConcurrentDictionary<Task, bool> _fills = new();

    public AutoFill(QueueEngine queues, IAutoFillChooser chooser, ILogger<AutoFill> log, TimeProvider? time = null)
    {
        _queues = queues;
        _chooser = chooser;
        _log = log;
        _time = time ?? TimeProvider.System;
        queues.QueueChanged += (_, state) => Note(state.Zone);
        queues.QueueDeleted += (sender, zone) =>
        {
            if (_zones.TryRemove(zone, out _))
                Changed?.Invoke(this, zone);
        };
    }

    // A zone's settings changed (by the API, a tool or a deleted zone); the
    // argument is the zone. playback.json and the event stream listen.
    public event EventHandler<string>? Changed;

    // A fill ended, whatever it added.
    public event EventHandler<AutoFillResult>? Filled;

    // A fill started or ended (its view's filling); the argument is the zone.
    // Not a change of settings, so playback.json does not listen.
    public event EventHandler<string>? FillingChanged;

    public AutoFillView View(string zone)
    {
        var fill = _zones.GetValueOrDefault(zone);
        var settings = fill?.Settings ?? new AutoFillSettings();
        string? waiting = null;
        if (settings.Enabled && _queues.GetQueue(zone) is { IsSuccess: true } queue)
        {
            if (queue.Value.Repeat == RepeatMode.All)
                waiting = "repeatAll";
            else if (queue.Value.Shuffle)
                waiting = "shuffle";
            else if (fill?.BlockedAt is { } blocked && blocked.SequenceEqual(queue.Value.Items.Select(i => i.Id)))
                waiting = "nothingAdded";
        }
        return new AutoFillView(settings.Enabled, settings.Threshold, settings.Count, settings.Source, settings.Request,
            settings.Fallback, settings.AvoidRecent, fill?.Filling == true, waiting, fill?.Last);
    }

    // The zone is the caller's to check (House). A change clears the wait
    // after a fill that added nothing and looks at the queue again.
    public Result<AutoFillView, QueueError> Set(string zone, AutoFillPatch patch)
    {
        var current = _zones.GetValueOrDefault(zone)?.Settings ?? new AutoFillSettings();
        if (patch.Threshold is < 0 or > AutoFillSettings.MaxThreshold)
            return Result.Fail(QueueError.InvalidArgument, $"autoFill.threshold: 0 to {AutoFillSettings.MaxThreshold}, was {patch.Threshold}");
        if (patch.Count is < 1 or > AutoFillSettings.MaxCount)
            return Result.Fail(QueueError.InvalidArgument, $"autoFill.count: 1 to {AutoFillSettings.MaxCount}, was {patch.Count}");
        if (patch.AvoidRecent is < 0 or > AutoFillSettings.MaxAvoidRecent)
            return Result.Fail(QueueError.InvalidArgument, $"autoFill.avoidRecent: 0 to {AutoFillSettings.MaxAvoidRecent}, was {patch.AvoidRecent}");
        if (patch.Request is { } text && (string.IsNullOrWhiteSpace(text) || text.Trim().Length > AutoFillSettings.MaxRequestLength))
            return Result.Fail(QueueError.InvalidArgument, $"autoFill.request: 1 to {AutoFillSettings.MaxRequestLength} characters");
        var settings = new AutoFillSettings(patch.Enabled ?? current.Enabled, patch.Threshold ?? current.Threshold,
            patch.Count ?? current.Count, patch.Source ?? current.Source, patch.Request?.Trim() ?? current.Request,
            patch.Fallback ?? current.Fallback, patch.AvoidRecent ?? current.AvoidRecent);
        var fill = _zones.GetOrAdd(zone, _ => new ZoneFill());
        lock (fill)
        {
            fill.Settings = settings;
            fill.BlockedAt = null;
        }
        Changed?.Invoke(this, zone);
        Note(zone);
        return View(zone);
    }

    // Every zone's settings, as playback.json keeps them.
    public IReadOnlyDictionary<string, AutoFillSettings> Save() =>
        _zones.Where(z => z.Value.Settings is not null)
            .ToDictionary(z => z.Key, z => z.Value.Settings!, StringComparer.Ordinal);

    // At startup, before anything plays.
    public void Restore(IReadOnlyDictionary<string, AutoFillSettings> zones)
    {
        foreach (var (zone, settings) in zones)
            _zones[zone] = new ZoneFill { Settings = settings };
    }

    public override async Task StopAsync(CancellationToken cancellationToken)
    {
        await base.StopAsync(cancellationToken).ConfigureAwait(false);
        await Task.WhenAll(_fills.Keys).ConfigureAwait(false);
    }

    protected override async Task ExecuteAsync(CancellationToken stoppingToken)
    {
        try
        {
            await foreach (var wake in _wake.Reader.ReadAllAsync(stoppingToken).ConfigureAwait(false))
            {
                foreach (var zone in _pending.Keys)
                {
                    _pending.TryRemove(zone, out _);
                    Check(zone, stoppingToken);
                }
            }
        }
        catch (OperationCanceledException) when (stoppingToken.IsCancellationRequested)
        {
            // Stopping.
        }
    }

    private void Note(string zone)
    {
        if (!_zones.ContainsKey(zone))
            return;
        _pending[zone] = true;
        _wake.Writer.TryWrite(true);
    }

    // Starts a fill when the zone wants one now.
    private void Check(string zone, CancellationToken stoppingToken)
    {
        if (!_zones.TryGetValue(zone, out var fill) || _queues.GetQueue(zone) is not { IsSuccess: true } read)
            return;
        var queue = read.Value;
        AutoFillSettings settings;
        lock (fill)
        {
            if (fill.Settings is not { Enabled: true } enabled || fill.Filling)
                return;
            settings = enabled;
            if (queue.Transport is not (QueueTransport.Playing or QueueTransport.Starting) || queue.Repeat == RepeatMode.All || queue.Shuffle)
                return;
            if (fill.BlockedAt is { } blocked)
            {
                if (blocked.SequenceEqual(queue.Items.Select(i => i.Id)))
                    return;
                fill.BlockedAt = null;
            }
            var at = queue.Current is { } current ? queue.Items.ToList().FindIndex(i => i.Id == current) : -1;
            if (at < 0 || queue.Items.Count - at - 1 > settings.Threshold)
                return;
            fill.Filling = true;
        }
        FillingChanged?.Invoke(this, zone);
        var task = FillAsync(zone, fill, queue, settings, stoppingToken);
        _fills[task] = true;
        _ = task.ContinueWith(t => _fills.TryRemove(t, out _), TaskScheduler.Default);
    }

    private async Task FillAsync(string zone, ZoneFill fill, QueueState queue, AutoFillSettings settings,
        CancellationToken stoppingToken)
    {
        AutoFillResult result;
        try
        {
            // Music only (phase 30): a queue that ends in a book is not
            // filled, and nothing but music is seeded from or added.
            if (!queue.Items.TakeLast(SeedItems).Any(i => i.Track is { } t && MediaTypeClassifier.IsMusic(t)))
            {
                result = new AutoFillResult(zone, _time.GetUtcNow(), 0, settings.Source,
                    Reason: $"the queue's last {SeedItems} items hold no music");
            }
            else
            {
                LogFilling(_log, zone, queue.Items.Count, settings.Count, settings.Source);
                var choice = await _chooser.ChooseAsync(zone,
                    [.. queue.Items.Select(i => i.Track).OfType<Track>().Where(MediaTypeClassifier.IsMusic)],
                    settings, stoppingToken).ConfigureAwait(false);
                var added = 0;
                var reason = choice.Reason;
                List<Track> music = [.. choice.Tracks.Where(MediaTypeClassifier.IsMusic)];
                if (music.Count > 0)
                {
                    var add = await _queues.AddAsync(zone, music, null, stoppingToken).ConfigureAwait(false);
                    if (add.IsSuccess)
                        added = music.Count;
                    else
                        reason = $"{add.Failure.Code}: {add.Failure.Detail}";
                }
                result = new AutoFillResult(zone, _time.GetUtcNow(), added, choice.Source, choice.Fallback, reason);
            }
        }
        catch (OperationCanceledException) when (stoppingToken.IsCancellationRequested)
        {
            return;
        }
#pragma warning disable CA1031 // The chooser's edge: a fill that throws adds nothing and says why; the trigger lives on.
        catch (Exception e)
#pragma warning restore CA1031
        {
            result = new AutoFillResult(zone, _time.GetUtcNow(), 0, settings.Source, Reason: $"failed: {e.GetType().Name}");
        }
        finally
        {
            lock (fill)
                fill.Filling = false;
            FillingChanged?.Invoke(this, zone);
        }

        lock (fill)
        {
            fill.Last = result;
            // Nothing went in: not again until the items change by other hands.
            if (result.Added == 0 && _queues.GetQueue(zone) is { IsSuccess: true } after)
                fill.BlockedAt = [.. after.Value.Items.Select(i => i.Id)];
        }
        LogFilled(_log, zone, result.Added, result.Source, result.Fallback, result.Reason ?? "-");
        Filled?.Invoke(this, result);
        // Still at or under the threshold after a small fill: look again.
        Note(zone);
    }

    [LoggerMessage(Level = LogLevel.Debug, Message = "zone {Zone}: AutoFill asks {Count} tracks from {Source} ({Items} queued)")]
    private static partial void LogFilling(ILogger log, string zone, int items, int count, AutoFillSource source);

    [LoggerMessage(Level = LogLevel.Information,
        Message = "zone {Zone}: AutoFill added {Added} from {Source} (fallback {Fallback}; {Reason})")]
    private static partial void LogFilled(ILogger log, string zone, int added, AutoFillSource source, bool fallback,
        string reason);

    // One zone's settings and state; locked on itself.
    private sealed class ZoneFill
    {
        public AutoFillSettings? Settings { get; set; }

        public bool Filling { get; set; }

        public IReadOnlyList<QueueItemId>? BlockedAt { get; set; }

        public AutoFillResult? Last { get; set; }
    }
}
