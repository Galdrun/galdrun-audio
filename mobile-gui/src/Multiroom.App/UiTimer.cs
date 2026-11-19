using Avalonia.Threading;

namespace Multiroom.App;

// Runs an action on the UI thread at an interval, for a repeating timer that must stay cheap on Android. A thread-pool
// timer posts each tick, and a tick still waiting is not posted again. Avalonia's DispatcherTimer re-arms an Android
// Handler through several calls into Java on every tick, which cost the UI thread far more than the work it ran
// (profiled: about 30 ms a tick on a phone).
public sealed class UiTimer : IDisposable
{
    private readonly Action _tick;
    private readonly Timer _timer;
    private int _queued;

    public UiTimer(Action tick)
    {
        _tick = tick;
        _timer = new Timer(_ => Post(), null, Timeout.Infinite, Timeout.Infinite);
    }

    public bool IsRunning { get; private set; }

    public void Start(TimeSpan interval)
    {
        IsRunning = true;
        _timer.Change(interval, interval);
    }

    public void Stop()
    {
        IsRunning = false;
        _timer.Change(Timeout.Infinite, Timeout.Infinite);
    }

    public void Dispose() => _timer.Dispose();

    private void Post()
    {
        if (Interlocked.Exchange(ref _queued, 1) == 0)
            Dispatcher.UIThread.Post(() => { _queued = 0; if (IsRunning) _tick(); });
    }
}
