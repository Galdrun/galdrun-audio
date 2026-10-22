using Avalonia.Controls;

namespace Multiroom.App;

public partial class ZoneBar : UserControl
{
    public ZoneBar()
    {
        InitializeComponent();
        ZoneSwipe.Attach(MediaButton, direction =>
        { if (DataContext is ShellViewModel model) model.Playback.ChangeZoneCommand.Execute(direction); });
        ZoneSwipe.Attach(NowPlayingZoneSelector, direction =>
        { if (DataContext is ShellViewModel model) model.Playback.ChangeZoneCommand.Execute(direction); });
        SizeChanged += (_, _) =>
        {
            var narrow = Bounds.Width < 520;
            MediaTitle.FontSize = narrow ? 16 : 18;
        };
    }
}
