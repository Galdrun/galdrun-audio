using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Interactivity;
using Avalonia.Threading;

namespace Multiroom.App;

// Ticks the slider's position while it is shown and sends one seek per drag or key press.
internal static class SeekSlider
{
    public static void Attach(Slider slider, Func<PlaybackViewModel?> model)
    {
        var progress = new UiTimer(() => model()?.RefreshProgress());
        IPointer? pointer = null;
        // Once a second and only while the slider is shown: the time shown changes once a second, the bar moves a few
        // pixels, and each tick costs a layout pass, a frame and calls into Java on Android (measured: 4 ticks a
        // second cost a phone's UI thread about a tenth of a core).
        void Shown()
        {
            if (!slider.IsEffectivelyVisible) { progress.Stop(); return; }
            if (!progress.IsRunning) progress.Start(TimeSpan.FromSeconds(1));
            model()?.RefreshProgress();
        }
        VisibilityWatch.Attach(slider, Shown);
        slider.DetachedFromVisualTree += (_, _) => { progress.Stop(); pointer = null; model()?.CancelScrub(); };
        slider.AddHandler(InputElement.PointerPressedEvent, (_, e) =>
        {
            if (model() is { CanScrub: true } playback && e.GetCurrentPoint(slider).Properties.IsLeftButtonPressed)
            { pointer = e.Pointer; playback.BeginScrub(); }
        }, RoutingStrategies.Tunnel);
        slider.AddHandler(InputElement.PointerReleasedEvent, (_, e) =>
        {
            if (e.Pointer != pointer) return;
            pointer = null;
            var playback = model();
            // Let the native slider finish updating its value before sending one seek.
            Dispatcher.UIThread.Post(async () => { if (playback is not null) await playback.CommitScrubCommand.ExecuteAsync(null); });
        }, RoutingStrategies.Tunnel);
        slider.PointerCaptureLost += (_, e) =>
        {
            if (e.Pointer == pointer) { pointer = null; model()?.CancelScrub(); }
        };
        slider.AddHandler(InputElement.KeyDownEvent, (_, e) =>
        { if (IsSeekKey(e.Key)) model()?.BeginScrub(); }, RoutingStrategies.Tunnel);
        slider.AddHandler(InputElement.KeyUpEvent, async (_, e) =>
        { if (IsSeekKey(e.Key) && model() is { } playback) await playback.CommitScrubCommand.ExecuteAsync(null); }, RoutingStrategies.Bubble, handledEventsToo: true);
    }

    private static bool IsSeekKey(Key key) => key is Key.Left or Key.Right or Key.Up or Key.Down or Key.PageUp or Key.PageDown or Key.Home or Key.End;
}
