using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Interactivity;
using Avalonia.Threading;

namespace Multiroom.App;

public partial class NowPlayingView : UserControl
{
    private readonly DispatcherTimer _progress = new() { Interval = TimeSpan.FromMilliseconds(250) };
    private IPointer? _sliderPointer;
    private PlaybackViewModel? Model => (DataContext as ShellViewModel)?.Playback;

    public NowPlayingView()
    {
        InitializeComponent();
        ZoneSwipe.Attach(CoverButton, direction => Model?.ChangeZoneCommand.Execute(direction));
        _progress.Tick += (_, _) => { if (IsEffectivelyVisible) Model?.RefreshProgress(); };
        AttachedToVisualTree += (_, _) => { Model?.RefreshProgress(); _progress.Start(); };
        DetachedFromVisualTree += (_, _) => { _progress.Stop(); _sliderPointer = null; Model?.CancelScrub(); };
        PositionSlider.AddHandler(PointerPressedEvent, (_, e) =>
        {
            if (Model is { CanScrub: true } model && e.GetCurrentPoint(PositionSlider).Properties.IsLeftButtonPressed)
            { _sliderPointer = e.Pointer; model.BeginScrub(); }
        }, RoutingStrategies.Tunnel);
        PositionSlider.AddHandler(PointerReleasedEvent, (_, e) =>
        {
            if (e.Pointer != _sliderPointer) return;
            _sliderPointer = null;
            var model = Model;
            // Let the native slider finish updating its value before sending one seek.
            Dispatcher.UIThread.Post(async () => { if (model is not null) await model.CommitScrubCommand.ExecuteAsync(null); });
        }, RoutingStrategies.Tunnel);
        PositionSlider.PointerCaptureLost += (_, e) =>
        {
            if (e.Pointer == _sliderPointer) { _sliderPointer = null; Model?.CancelScrub(); }
        };
        PositionSlider.AddHandler(KeyDownEvent, (_, e) =>
        { if (IsSeekKey(e.Key)) Model?.BeginScrub(); }, RoutingStrategies.Tunnel);
        PositionSlider.AddHandler(KeyUpEvent, async (_, e) =>
        { if (IsSeekKey(e.Key) && Model is { } model) await model.CommitScrubCommand.ExecuteAsync(null); }, RoutingStrategies.Bubble, handledEventsToo: true);
    }

    private static bool IsSeekKey(Key key) => key is Key.Left or Key.Right or Key.Up or Key.Down or Key.PageUp or Key.PageDown or Key.Home or Key.End;
}
