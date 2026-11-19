using System.Diagnostics;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Media;
using Avalonia.Threading;

namespace Multiroom.App;

// The app's CPU use once a second, while the setting "Show performance" shows it: the whole app in % of all cores,
// the UI thread and Avalonia's render thread in % of one core with the clock of the core each last ran on (on Linux
// and Android, from /proc and /sys; elsewhere the whole app only). A thread's % is time, not work: with little to
// do the system lowers the clock, and the same work takes longer. It samples on a thread-pool thread and changes
// its text once a second, one small frame; nothing runs while it is hidden or the app is in the background. GPU
// time is not readable by an app (docs/android-rendering.md), and frames cannot be counted without asking for
// frames, so neither is shown.
public sealed class PerformanceView : TextBlock
{
    private const double KernelTicksPerSecond = 100; // USER_HZ, the unit of /proc/<pid>/task/<tid>/stat on Linux and Android.
    private const string TaskFolder = "/proc/self/task";
    // A thread's user and system time in kernel ticks (-1: unavailable) and the core it last ran on.
    private readonly record struct ThreadTime(long Ticks, int Core);
    private readonly record struct Sample(long Time, TimeSpan App, ThreadTime Ui, ThreadTime Render);
    private readonly Timer _timer;
    private bool _foreground = true;
    private bool _running;
    private Sample? _last;
    private int _renderThread; // Avalonia's "Render Thread"; 0: not looked for since the start, -1: none.

    public PerformanceView()
    {
        IsHitTestVisible = false;
        FontFeatures = FontFeatureCollection.Parse("tnum"); // Digits of one width.
        _timer = new Timer(_ => Tick(), null, Timeout.Infinite, Timeout.Infinite);
        VisibilityWatch.Attach(this, Update);
        if (Application.Current?.TryGetFeature<IActivatableLifetime>() is { } lifetime)
        {
            lifetime.Activated += (_, e) => { if (e.Kind == ActivationKind.Background) { _foreground = true; Update(); } };
            lifetime.Deactivated += (_, e) => { if (e.Kind == ActivationKind.Background) { _foreground = false; Update(); } };
        }
    }

    // Whether it samples (tests).
    public bool IsMeasuring => _running;

    private void Update()
    {
        var run = _foreground && IsEffectivelyVisible && TopLevel.GetTopLevel(this) is not null;
        if (run == _running) return;
        _running = run;
        _last = null; _renderThread = 0;
        if (run) { Text = "CPU …"; _timer.Change(TimeSpan.Zero, TimeSpan.FromSeconds(1)); }
        else _timer.Change(Timeout.Infinite, Timeout.Infinite);
    }

    private void Tick()
    {
        var now = Take();
        if (_last is not { } last) { _last = now; return; }
        _last = now;
        var seconds = Stopwatch.GetElapsedTime(last.Time, now.Time).TotalSeconds;
        var cores = Environment.ProcessorCount;
        var text = $"CPU app {Percent((now.App - last.App).TotalSeconds, seconds * cores)} of {cores} cores";
        text += ThreadText("UI", last.Ui, now.Ui, seconds) + ThreadText("render", last.Render, now.Render, seconds);
        Dispatcher.UIThread.Post(() => { if (_running) Text = text; });
    }

    // Three places, padded with figure spaces (as wide as a digit), so the line keeps its width from 0 to 100%.
    private static string Percent(double busy, double seconds) => $"{100 * busy / seconds:0}".PadLeft(3, '\u2007') + "%";

    private static string ThreadText(string name, ThreadTime last, ThreadTime now, double seconds)
    {
        if (now.Ticks < 0 || last.Ticks < 0) return "";
        var text = $" · {name} {Percent((now.Ticks - last.Ticks) / KernelTicksPerSecond, seconds)}";
        return CoreClock(now.Core) is { } gigahertz ? text + $" @ {gigahertz:0.0} GHz" : text;
    }

    // The core's clock now, in GHz; null where unavailable.
    private static double? CoreClock(int core)
    {
        if (core < 0) return null;
        try { return long.Parse(File.ReadAllText($"/sys/devices/system/cpu/cpu{core}/cpufreq/scaling_cur_freq")) / 1e6; }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or FormatException) { return null; }
    }

    private Sample Take()
    {
        if (_renderThread == 0) _renderThread = FindRenderThread();
        var render = _renderThread > 0 ? ReadThread(_renderThread) : new(-1, -1);
        // On Linux and Android the main thread, which is Avalonia's UI thread there, has the process id.
        return new(Stopwatch.GetTimestamp(), Environment.CpuUsage.TotalTime, ReadThread(Environment.ProcessId), render);
    }

    private static ThreadTime ReadThread(int thread)
    {
        try
        {
            var stat = File.ReadAllText($"{TaskFolder}/{thread}/stat");
            // The fields after the thread name, which is in parentheses and may contain spaces: utime and stime are
            // the 14th and 15th field, the core it last ran on the 39th (12th, 13th and 37th after the name).
            var fields = stat[(stat.LastIndexOf(')') + 2)..].Split(' ');
            return new(long.Parse(fields[11]) + long.Parse(fields[12]), int.Parse(fields[36]));
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or FormatException or IndexOutOfRangeException)
        { return new(-1, -1); }
    }

    private static int FindRenderThread()
    {
        if (!Directory.Exists(TaskFolder)) return -1;
        try
        {
            foreach (var folder in Directory.EnumerateDirectories(TaskFolder))
                if (File.ReadAllText(Path.Combine(folder, "comm")).TrimEnd() == "Render Thread" && int.TryParse(Path.GetFileName(folder), out var id))
                    return id;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
        return -1;
    }
}
