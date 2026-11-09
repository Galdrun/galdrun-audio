using System.Collections.Concurrent;

namespace Multiroom.Server.Playback;

// A zone's sleep timer as the API answers it: off, or when it ends and the
// whole seconds left.
public sealed record SleepTimerView(bool Active, DateTimeOffset? EndsAt = null, int RemainingSeconds = 0);

// The zones' sleep timers, in memory: a restart ends them. TimedPlays fades
// out and stops a zone whose timer is due, and leaves a zone with a running
// timer to it: no timed play starts in its rooms or stops it at a block's end.
public sealed class SleepTimers(TimeProvider? time = null)
{
    public const int MaxMinutes = 720;

    private readonly TimeProvider _time = time ?? TimeProvider.System;
    private readonly ConcurrentDictionary<string, DateTimeOffset> _ends = new(StringComparer.Ordinal);

    // A zone's timer was set, cleared or ran out.
    public event EventHandler<string>? Changed;

    public SleepTimerView View(string zone)
    {
        if (!_ends.TryGetValue(zone, out var end))
            return new SleepTimerView(false);
        var left = end - _time.GetUtcNow();
        return new SleepTimerView(true, end, Math.Max(0, (int)Math.Ceiling(left.TotalSeconds)));
    }

    public bool IsActive(string zone) => _ends.ContainsKey(zone);

    // From now on, `minutes` (1 to MaxMinutes; the caller checks); 0 clears it.
    public void Set(string zone, int minutes)
    {
        if (minutes <= 0)
        {
            Clear(zone);
            return;
        }
        _ends[zone] = _time.GetUtcNow().AddMinutes(minutes);
        Changed?.Invoke(this, zone);
    }

    public void Clear(string zone)
    {
        if (_ends.TryRemove(zone, out _))
            Changed?.Invoke(this, zone);
    }

    // The zones whose timer has run out, each cleared.
    public IReadOnlyList<string> TakeDue()
    {
        var now = _time.GetUtcNow();
        var due = _ends.Where(e => e.Value <= now).Select(e => e.Key).ToList();
        foreach (var zone in due)
            Clear(zone);
        return due;
    }
}
