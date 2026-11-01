using Avalonia.Controls;

namespace Multiroom.App;

public partial class NowPlayingView : UserControl
{
    private PlaybackViewModel? Model => (DataContext as ShellViewModel)?.Playback;

    public NowPlayingView()
    {
        InitializeComponent();
        ZoneSwipe.Attach(CoverButton, direction => Model?.ChangeZoneCommand.Execute(direction));
        SeekSlider.Attach(PositionSlider, () => Model);
    }
}
