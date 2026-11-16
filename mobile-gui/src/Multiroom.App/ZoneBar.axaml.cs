using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Interactivity;

namespace Multiroom.App;

public partial class ZoneBar : UserControl
{
    public ZoneBar()
    {
        InitializeComponent();
        ZoneSwipe.Attach(MediaButton, direction =>
        { if (DataContext is ShellViewModel model) model.Playback.ChangeZoneCommand.Execute(direction); });
        ZoneSwipe.Attach(ZoneButton, direction =>
        { if (DataContext is ShellViewModel model) model.Playback.ChangeZoneCommand.Execute(direction); });
        ZoneSwipe.Attach(NowPlayingZoneSelector, direction =>
        { if (DataContext is ShellViewModel model) model.Playback.ChangeZoneCommand.Execute(direction); });
        SeekSlider.Attach(ZonePositionSlider, () => (DataContext as ShellViewModel)?.Playback);
        SizeChanged += (_, _) =>
        {
            var narrow = Bounds.Width < 520;
            MediaTitle.FontSize = narrow ? 16 : 18;
        };
        AddHandler(PointerPressedEvent, (_, _) => _held = false, RoutingStrategies.Tunnel, handledEventsToo: true);
    }

    // A long press switches the sleep timer off, and its release would tap it too.
    private bool _held;
    private PlaybackViewModel? Playback => (DataContext as ShellViewModel)?.Playback;
    private void SleepTapped(object? sender, RoutedEventArgs e)
    {
        if (_held) { _held = false; return; }
        Playback?.SleepCommand.Execute(null);
    }
    private void SleepHeld(object? sender, ContextRequestedEventArgs e)
    {
        e.Handled = true; _held = true;
        Playback?.SleepOffCommand.Execute(null);
    }
}
