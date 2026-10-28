using Avalonia;
using Avalonia.Controls;
using Avalonia.Interactivity;

namespace Multiroom.App;

public partial class HomeView : UserControl
{
    // The narrowest a cover gets in the "See all" grid, and the space beside it:
    // three columns fit on a 390 wide phone.
    private const double Cover = 100, Gap = 14;
    public static readonly StyledProperty<int> GridColumnsProperty = AvaloniaProperty.Register<HomeView, int>(nameof(GridColumns), 1);
    public int GridColumns { get => GetValue(GridColumnsProperty); set => SetValue(GridColumnsProperty, value); }

    public HomeView()
    {
        InitializeComponent();
        ShelfGrid.SizeChanged += (_, e) => GridColumns = Math.Max(1, (int)((e.NewSize.Width + Gap) / (Cover + Gap)));
    }

    private async void PlayItem(object? sender, RoutedEventArgs e)
    {
        if (DataContext is ShellViewModel shell && sender is Button { Tag: HomeItem item })
            await shell.Home.PlayCommand.ExecuteAsync(item);
    }

    private async void QueueItem(object? sender, RoutedEventArgs e)
    {
        if (DataContext is ShellViewModel shell && sender is MenuItem { Tag: HomeItem item })
            await shell.Home.QueueCommand.ExecuteAsync(item);
    }
}
