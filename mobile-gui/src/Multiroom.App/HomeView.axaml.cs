using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Interactivity;
using Avalonia.Threading;

namespace Multiroom.App;

public partial class HomeView : UserControl
{
    // The narrowest a cover gets in the "See all" grid, and the space beside it:
    // three columns fit on a 390 wide phone.
    private const double Cover = 100, Gap = 14;
    public static readonly StyledProperty<int> GridColumnsProperty = AvaloniaProperty.Register<HomeView, int>(nameof(GridColumns), 1);
    public int GridColumns { get => GetValue(GridColumnsProperty); set => SetValue(GridColumnsProperty, value); }

    // A long press opens the sheet, and its release would click the cover too.
    private bool _held;

    public HomeView()
    {
        InitializeComponent();
        ShelfGrid.SizeChanged += (_, e) => GridColumns = Math.Max(1, (int)((e.NewSize.Width + Gap) / (Cover + Gap)));
        AddHandler(PointerPressedEvent, (_, _) => _held = false, RoutingStrategies.Tunnel, handledEventsToo: true);
    }

    private HomeViewModel? Home => (DataContext as ShellViewModel)?.Home;

    private void OpenItem(object? sender, RoutedEventArgs e)
    {
        if (_held) { _held = false; return; }
        if (Home is { } home && sender is Button { Tag: HomeItem item })
        {
            if (item.Items.Node is not null) home.OpenCommand.Execute(item);
            else home.OpenSheetCommand.Execute(item);
        }
    }

    // The shade is drawn first and the shelf opened after that frame, so it shows while the covers lay out.
    private void SeeAll(object? sender, RoutedEventArgs e)
    {
        if (Home is not { } home || sender is not Button { Tag: HomeShelf shelf }) return;
        ShelfLoading.IsVisible = true;
        void Open()
        {
            home.SeeAllCommand.Execute(shelf);
            Dispatcher.UIThread.Post(() => ShelfLoading.IsVisible = false, DispatcherPriority.Background);
        }
        if (TopLevel.GetTopLevel(this) is { } top) top.RequestAnimationFrame(_ => Dispatcher.UIThread.Post(Open, DispatcherPriority.Background));
        else Open();
    }

    private void RequestActions(object? sender, ContextRequestedEventArgs e)
    {
        e.Handled = true; _held = true;
        if (sender is Button { Tag: HomeItem item }) Home?.OpenSheetCommand.Execute(item);
    }
}
