using System.Collections.Concurrent;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;
using Multiroom.Server.Abstractions;
using Multiroom.Server.Library;
using Multiroom.Server.Playback.Core;

namespace Multiroom.Server.Playback;

// Runs the timed plays (TimedPlayStore): when the server's local time enters
// an enabled block of an enabled timed play, its playlist starts in those of
// its rooms that play nothing, grouped into one zone; when the time leaves
// every block, that zone stops. Blocks that touch (22:00-24:00 and the next
// day's 00:00-02:00) are one stretch: nothing stops or starts between them.
//
// - A start is tried once per stretch. When every room plays something, or a
//   phone plays on it over Bluetooth, it is skipped, and so is the stop.
// - The stop happens only while the zone's queue still holds an item the
//   start queued; a queue replaced by hand is left playing.
// - Only the zone's volume is set, never a room's. Fades ramp the zone volume
//   on their own task; a volume set by someone else meanwhile ends the fade.
//   After a fade out the zone gets its volume back, stopped, once the stop
//   is heard (StopHeard).
// - The run is kept in the store, so a restart neither starts it again nor
//   forgets to stop it. A stretch the server (or the core) missed the start
//   of is started late; one that ended meanwhile is not.
// - A sleep timer (SleepTimers) overrides them: a zone whose timer is due
//   fades out (the playback setting sleepFadeSeconds) and stops, ending a run in it; while a zone's
//   timer runs, its rooms count as busy and a block's end leaves it playing.
//   The timer is checked on the same tick, so it ends up to 5 s late.
public sealed partial class TimedPlays(
    TimedPlayStore store,
    House house,
    ZoneKeeper zones,
    IQueueEngine queues,
    PlaylistPlayer player,
    CoreLink link,
    SleepTimers sleepTimers,
    ILogger<TimedPlays> log,
    TimeProvider? time = null) : BackgroundService
{
    private static readonly TimeSpan TickInterval = TimeSpan.FromSeconds(5);
    private static readonly TimeSpan FadeStep = TimeSpan.FromMilliseconds(250);
    private static readonly TimeSpan StartPatience = TimeSpan.FromSeconds(60);
    // A stop is heard after the audio the rooms still hold: the zone volume
    // comes back only after that, or a faded out zone ends loud.
    private static readonly TimeSpan StopHeard = TimeSpan.FromSeconds(2);

    private readonly TimeProvider _time = time ?? TimeProvider.System;
    private readonly SemaphoreSlim _gate = new(1, 1);
    private readonly ConcurrentDictionary<string, (CancellationTokenSource Cancel, Task Task)> _fades =
        new(StringComparer.Ordinal);
    private readonly CancellationTokenSource _stopping = new();

    // The timed plays inside a stretch at the last tick; null before the first.
    private HashSet<long>? _inside;

    // The block a timed play is inside at `now`, or null: none, or the timed
    // play is off or has no playlist or room.
    public static TimedPlayBlock? ActiveBlock(TimedPlayDefinition definition, DateTime now)
    {
        if (!definition.Enabled || definition.Playlist is null || definition.Rooms.Count == 0)
            return null;
        var minute = (now.Hour * 60) + now.Minute;
        return definition.Blocks.FirstOrDefault(b =>
            b.Enabled && b.Day == now.DayOfWeek && b.Start <= minute && minute < b.End);
    }

    // One tick at `now` (local time), for a test that does not run the loop.
    // Nothing is decided while no core is connected.
    public async Task TickAsync(DateTime now, CancellationToken cancellationToken)
    {
        await _gate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            if (!link.IsConnected)
                return;
            var all = store.All();
            if (!all.IsSuccess)
            {
                LogFailed(log, 0, all.Failure.ToString());
                return;
            }
            var due = sleepTimers.TakeDue();
            foreach (var zone in due)
                await SleepAsync(zone, all.Value).ConfigureAwait(false);
            if (due.Count > 0)
                all = store.All();
            if (!all.IsSuccess)
                return;
            _inside ??= [.. all.Value.Where(p => p.Run is not null).Select(p => p.Id)];
            _inside.IntersectWith(all.Value.Select(p => p.Id));
            foreach (var play in all.Value)
            {
                if (ActiveBlock(play.Definition, now) is not { } block)
                {
                    _inside.Remove(play.Id);
                    if (play.Run is { } run)
                        await StopAsync(play, run).ConfigureAwait(false);
                }
                else if (_inside.Add(play.Id) && play.Run is null)
                {
                    await StartAsync(play, block, cancellationToken).ConfigureAwait(false);
                }
            }
        }
        finally
        {
            _gate.Release();
        }
    }

    // Deletes a timed play, stopping what it started.
    public async Task<Result<bool, LibraryError>> DeleteAsync(long id, CancellationToken cancellationToken)
    {
        await _gate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            var found = store.Get(id);
            if (!found.IsSuccess)
                return found.Failure;
            if (found.Value.Run is { } run)
                await StopAsync(found.Value, run).ConfigureAwait(false);
            return store.Delete(id);
        }
        finally
        {
            _gate.Release();
        }
    }

    protected override async Task ExecuteAsync(CancellationToken stoppingToken)
    {
        using var timer = new PeriodicTimer(TickInterval, _time);
        try
        {
            do
            {
                try
                {
                    await TickAsync(_time.GetLocalNow().DateTime, stoppingToken).ConfigureAwait(false);
                }
                catch (Exception e) when (e is not OperationCanceledException)
                {
                    LogFailed(log, 0, e.Message);
                }
            }
            while (await timer.WaitForNextTickAsync(stoppingToken).ConfigureAwait(false));
        }
        catch (OperationCanceledException) when (stoppingToken.IsCancellationRequested)
        {
            // Stopping.
        }
    }

    public override async Task StopAsync(CancellationToken cancellationToken)
    {
        await _stopping.CancelAsync().ConfigureAwait(false);
        await base.StopAsync(cancellationToken).ConfigureAwait(false);
        await Task.WhenAll(_fades.Values.Select(f => f.Task)).WaitAsync(cancellationToken).ConfigureAwait(false);
    }

    public override void Dispose()
    {
        _gate.Dispose();
        _stopping.Dispose();
        base.Dispose();
    }

    private async Task StartAsync(TimedPlay play, TimedPlayBlock block, CancellationToken cancellationToken)
    {
        var definition = play.Definition;
        var rooms = definition.Rooms.Where(r => link.Desired.ZoneOf(r) is not null).ToList();
        var idle = rooms.Where(r => !Busy(r)).ToList();
        if (idle.Count == 0)
        {
            LogSkipped(log, definition.Name);
            return;
        }
        var zone = link.Desired.ZoneOf(idle[0])!;
        await CancelFadeAsync(zone).ConfigureAwait(false);

        if (!link.Desired.MembersOf(zone).ToHashSet(StringComparer.Ordinal).SetEquals(idle))
        {
            var moved = await zones.SetClientsAsync(zone, idle, cancellationToken).ConfigureAwait(false);
            if (!moved.IsSuccess)
            {
                LogFailed(log, play.Id, moved.Failure.ToString());
                return;
            }
        }

        var volume = block.OverrideVolume ? block.Volume : definition.Volume;
        var fadeIn = definition.FadeInSeconds > 0;
        await house.SetZoneLevelAsync(zone, fadeIn ? 0.0 : volume, false, cancellationToken).ConfigureAwait(false);

        var queued = await player.EnqueuePlaylistAsync(zone, definition.Playlist!.Value, QueuePlacement.Replace,
            cancellationToken, fromStart: !block.Resume).ConfigureAwait(false);
        if (!queued.IsSuccess)
        {
            await house.SetZoneLevelAsync(zone, volume, null, cancellationToken).ConfigureAwait(false);
            LogFailed(log, play.Id, queued.Failure.ToString());
            return;
        }
        await queues.SetRepeatAsync(zone, definition.Repeat, cancellationToken).ConfigureAwait(false);
        await queues.SetShuffleAsync(zone, definition.Shuffle, cancellationToken).ConfigureAwait(false);
        house.SetAutoFill(zone, new AutoFillPatch(Enabled: definition.AutoFill));

        var items = queued.Value.Queue.Items.Select(i => i.Id).ToList();
        var played = await PlayAsync(zone, items, block.Resume ? play.Resume : null, definition.Shuffle,
            cancellationToken).ConfigureAwait(false);
        if (played is not null)
        {
            await house.SetZoneLevelAsync(zone, volume, null, cancellationToken).ConfigureAwait(false);
            LogFailed(log, play.Id, played.Value.ToString());
            return;
        }
        store.SetRun(play.Id, new TimedPlayRun(zone, [.. items.Select(i => i.Value)], volume));
        LogStarted(log, definition.Name, zone, idle.Count);
        if (fadeIn)
            Fade(zone, ct => FadeInAsync(zone, volume, definition.FadeInSeconds, ct));
    }

    // Where the last run stopped, else the start (with shuffle a random item).
    private async Task<Failure<QueueError>?> PlayAsync(string zone, List<QueueItemId> items, TimedPlayResume? resume,
        bool shuffle, CancellationToken cancellationToken)
    {
        if (resume is { } at && at.Index >= 0 && at.Index < items.Count)
        {
            var item = items[at.Index];
            if (at.PositionMs > 0 && (await queues.SeekAsync(zone, item, TimeSpan.FromMilliseconds(at.PositionMs),
                    cancellationToken).ConfigureAwait(false)).IsSuccess)
            {
                return null;
            }
            var resumed = await queues.PlayAsync(zone, item, cancellationToken).ConfigureAwait(false);
            return resumed.IsSuccess ? null : resumed.Failure;
        }
        var first = shuffle ? items[Random.Shared.Next(items.Count)] : items[0];
        var started = await queues.PlayAsync(zone, first, cancellationToken, fromStart: true).ConfigureAwait(false);
        return started.IsSuccess ? null : started.Failure;
    }

    // Playing or starting, or a phone plays on the room over Bluetooth, or its
    // zone's sleep timer runs. A queue played to its end stays "playing" with
    // no current item: idle.
    private bool Busy(string room)
    {
        if (link.Model.Current.Rooms.GetValueOrDefault(room)?.LocalSource is not null)
            return true;
        if (link.Desired.ZoneOf(room) is not { } zone)
            return false;
        if (sleepTimers.IsActive(zone))
            return true;
        var queue = queues.GetQueue(zone);
        return queue.IsSuccess && queue.Value.Current is not null &&
            queue.Value.Transport is QueueTransport.Playing or QueueTransport.Starting;
    }

    // The run is over at once; the fade out, where the last run stopped and
    // the stop itself follow on their own task.
    // A zone whose sleep timer runs is left to it.
    private async Task StopAsync(TimedPlay play, TimedPlayRun run)
    {
        store.SetRun(play.Id, null);
        if (sleepTimers.IsActive(run.Zone))
        {
            LogLeftToSleep(log, play.Definition.Name, run.Zone);
            return;
        }
        await CancelFadeAsync(run.Zone).ConfigureAwait(false);
        var queue = queues.GetQueue(run.Zone);
        if (!queue.IsSuccess || !queue.Value.Items.Any(i => run.Items.Contains(i.Id.Value)))
        {
            LogLeft(log, play.Definition.Name, run.Zone);
            return;
        }
        Fade(run.Zone, ct => FadeOutAsync(run.Zone, play.Definition.FadeOutSeconds, run.Volume, (play, run), ct));
    }

    // A due sleep timer: the zone fades out and stops, whatever plays; a timed
    // play's run in it ends there, its place kept for a resume.
    private async Task SleepAsync(string zone, IReadOnlyList<TimedPlay> plays)
    {
        (TimedPlay, TimedPlayRun)? ended = null;
        foreach (var play in plays.Where(p => p.Run?.Zone == zone))
        {
            store.SetRun(play.Id, null);
            ended ??= (play, play.Run!);
        }
        await CancelFadeAsync(zone).ConfigureAwait(false);
        if (queues.GetQueue(zone) is not { IsSuccess: true })
            return;
        LogSleeping(log, zone);
        Fade(zone, ct => FadeOutAsync(zone, player.Settings.SleepFadeSeconds, 1.0, ended, ct));
    }

    private async Task FadeInAsync(string zone, double volume, int seconds, CancellationToken cancellationToken)
    {
        // The ramp starts when the zone is heard (its source started in the
        // rooms), not while it buffers or the rooms still wait to start: the
        // queue's transport says "stopped" or "playing" before that.
        var waited = TimeSpan.Zero;
        while (waited < StartPatience && queues.GetQueue(zone) is { IsSuccess: true } q && q.Value.NowPlaying is null)
        {
            await Task.Delay(FadeStep, _time, cancellationToken).ConfigureAwait(false);
            waited += FadeStep;
        }
        await RampAsync(zone, 0.0, volume, seconds, cancellationToken).ConfigureAwait(false);
    }

    // Fades the zone out over `seconds` while it plays, then stops it and gives
    // it its volume back (`fallback` when it has none). A timed play's run
    // keeps where it stopped, for a resume.
    private async Task FadeOutAsync(string zone, int seconds, double fallback, (TimedPlay Play, TimedPlayRun Run)? ended,
        CancellationToken cancellationToken)
    {
        var level = link.Desired.MembersOf(zone).Count > 0 ? link.Desired.ZoneLevel(zone).Volume : fallback;
        var faded = false;
        try
        {
            if (seconds > 0 && queues.GetQueue(zone) is { IsSuccess: true } q && q.Value.Transport == QueueTransport.Playing)
            {
                faded = true;
                await RampAsync(zone, level, 0.0, seconds, cancellationToken).ConfigureAwait(false);
            }
        }
        finally
        {
            // Stopped and its volume back even when the server stops meanwhile.
            var queue = queues.GetQueue(zone);
            if (queue.IsSuccess)
            {
                if (ended is var (play, run))
                {
                    var index = queue.Value.Current is { } current ? run.Items.ToList().IndexOf(current.Value) : -1;
                    store.SetResume(play.Id, index < 0 ? null : new TimedPlayResume(index, queue.Value.PositionMs ?? 0));
                }
                if (queue.Value.Transport != QueueTransport.Stopped)
                    await queues.StopAsync(zone, CancellationToken.None).ConfigureAwait(false);
            }
            if (faded)
                await Task.Delay(StopHeard, _time, CancellationToken.None).ConfigureAwait(false);
            if (link.Desired.Zones.Any(z => z.Zone == zone))
                await house.SetZoneLevelAsync(zone, level, null, CancellationToken.None).ConfigureAwait(false);
            if (ended is var (stopped, _))
                LogStopped(log, stopped.Definition.Name, zone);
        }
    }

    // The zone volume from `from` to `to` in steps; it ends early when the
    // volume is not the one this ramp set last.
    private async Task RampAsync(string zone, double from, double to, int seconds, CancellationToken cancellationToken)
    {
        var steps = Math.Max(1, (int)(TimeSpan.FromSeconds(seconds) / FadeStep));
        var last = from;
        for (var i = 1; i <= steps; i++)
        {
            await Task.Delay(FadeStep, _time, cancellationToken).ConfigureAwait(false);
            if (link.Desired.MembersOf(zone).Count == 0 ||
                Math.Abs(link.Desired.ZoneLevel(zone).Volume - last) > 0.001)
                return;
            last = from + ((to - from) * i / steps);
            await house.SetZoneLevelAsync(zone, last, null, cancellationToken).ConfigureAwait(false);
        }
    }

    // One fade per zone: a new one ends the one before.
    private void Fade(string zone, Func<CancellationToken, Task> fade)
    {
        var cancel = CancellationTokenSource.CreateLinkedTokenSource(_stopping.Token);
        var registered = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        Task? task = null;
        task = Task.Run(async () =>
        {
            await registered.Task.ConfigureAwait(false);
            try
            {
                await fade(cancel.Token).ConfigureAwait(false);
            }
            catch (OperationCanceledException) when (cancel.IsCancellationRequested)
            {
                // Ended by a newer fade or by the server stopping.
            }
#pragma warning disable CA1031 // A fade's failure is logged; it must not end the server.
            catch (Exception e)
#pragma warning restore CA1031
            {
                LogFailed(log, 0, $"zone {zone}: {e.Message}");
            }
            finally
            {
                _fades.TryRemove(KeyValuePair.Create(zone, (cancel, task!)));
                cancel.Dispose();
            }
        }, CancellationToken.None);
        _fades[zone] = (cancel, task);
        registered.SetResult();
    }

    private async Task CancelFadeAsync(string zone)
    {
        if (!_fades.TryGetValue(zone, out var fade))
            return;
        await fade.Cancel.CancelAsync().ConfigureAwait(false);
        await fade.Task.ConfigureAwait(false);
    }

    [LoggerMessage(Level = LogLevel.Information, Message = "timed play {Name}: started in zone {Zone} ({Rooms} rooms)")]
    private static partial void LogStarted(ILogger log, string name, string zone, int rooms);

    [LoggerMessage(Level = LogLevel.Information, Message = "timed play {Name}: every room plays already; not started")]
    private static partial void LogSkipped(ILogger log, string name);

    [LoggerMessage(Level = LogLevel.Information, Message = "timed play {Name}: zone {Zone} stopped")]
    private static partial void LogStopped(ILogger log, string name, string zone);

    [LoggerMessage(Level = LogLevel.Information,
        Message = "timed play {Name}: zone {Zone} plays something else now; left playing")]
    private static partial void LogLeft(ILogger log, string name, string zone);

    [LoggerMessage(Level = LogLevel.Information,
        Message = "timed play {Name}: zone {Zone} has a sleep timer running; left to it")]
    private static partial void LogLeftToSleep(ILogger log, string name, string zone);

    [LoggerMessage(Level = LogLevel.Information, Message = "sleep timer: zone {Zone} fades out and stops")]
    private static partial void LogSleeping(ILogger log, string zone);

    [LoggerMessage(Level = LogLevel.Warning, Message = "timed play {Id}: {Failure}")]
    private static partial void LogFailed(ILogger log, long id, string failure);
}
