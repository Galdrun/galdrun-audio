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
        var progress = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(250) };
        IPointer? pointer = null;
        progress.Tick += (_, _) => { if (slider.IsEffectivelyVisible) model()?.RefreshProgress(); };
        slider.AttachedToVisualTree += (_, _) => { model()?.RefreshProgress(); progress.Start(); };
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
