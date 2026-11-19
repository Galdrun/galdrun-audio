using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Interactivity;
using Avalonia.Threading;

namespace Multiroom.App;

// One row of the "See all" grid.
public sealed record CoverRow(IReadOnlyList<HomeItem> Items);

public partial class HomeView : UserControl
{
    // The shown covers cut into rows of GridColumns, so the grid can virtualize by row.
    public static readonly Avalonia.Data.Converters.FuncMultiValueConverter<object?, System.Collections.IList?> Rows = new(values =>
    {
        var list = values.ToArray();
        if (list is not [IReadOnlyList<HomeItem> items, int columns] || columns < 1) return null;
        return items.Chunk(columns).Select(row => new CoverRow(row)).ToArray();
    });

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
        _cover = (Avalonia.Controls.Templates.IDataTemplate)Resources["Cover"]!;
        ShelfItems.CreateRow = CreateCoverRow;
        ShelfGrid.SizeChanged += (_, e) => GridColumns = Math.Max(1, (int)((e.NewSize.Width + Gap) / (Cover + Gap)));
        AddHandler(PointerPressedEvent, (_, _) => _held = false, RoutingStrategies.Tunnel, handledEventsToo: true);
    }

    private HomeViewModel? Home => (DataContext as ShellViewModel)?.Home;

    // A row of the "See all" grid keeps its covers: given another row (scrolling, a tab), each cover only gets
    // another item (its ContentControl reuses the built template), so nothing is rebuilt or restyled.
    private readonly Avalonia.Controls.Templates.IDataTemplate _cover;
    private Control CreateCoverRow()
    {
        var row = new Avalonia.Controls.Primitives.UniformGrid { Rows = 1 };
        row.DataContextChanged += (_, _) => FillRow(row);
        return row;
    }
    private void FillRow(Avalonia.Controls.Primitives.UniformGrid row)
    {
        var items = (row.DataContext as CoverRow)?.Items ?? [];
        var columns = GridColumns;
        row.Columns = columns;
        while (row.Children.Count > columns) row.Children.RemoveAt(row.Children.Count - 1);
        while (row.Children.Count < columns)
            row.Children.Add(new Viewbox { Stretch = Avalonia.Media.Stretch.Uniform, Margin = new Thickness(0, 0, Gap, 18),
                VerticalAlignment = Avalonia.Layout.VerticalAlignment.Top, Child = new ContentControl { ContentTemplate = _cover } });
        for (var i = 0; i < columns; i++)
        {
            var cell = (Viewbox)row.Children[i];
            var item = i < items.Count ? items[i] : null;
            // The last row's empty cells keep their cover (hidden) for the next full row.
            if (item is not null) ((ContentControl)cell.Child!).Content = item;
            cell.IsVisible = item is not null;
        }
    }

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
            Dispatcher.UIThread.Post(() => ShelfLoading.IsVisible = false, DispatcherPriority.Input);
        }
        if (TopLevel.GetTopLevel(this) is { } top) top.RequestAnimationFrame(_ => Dispatcher.UIThread.Post(Open, DispatcherPriority.Input));
        else Open();
    }

    // A long press on the Tracks or Albums tab opens the sheet for all of them.
    private void RequestTabActions(object? sender, ContextRequestedEventArgs e)
    {
        if (sender is not Button { DataContext: ShelfTab tab } || Home is not { OpenShelf: { } shelf } home || shelf.WholeKind(tab.Kind) is not { } whole) return;
        e.Handled = true;
        home.OpenSheetCommand.Execute(whole);
    }

    private void RequestActions(object? sender, ContextRequestedEventArgs e)
    {
        e.Handled = true; _held = true;
        if (sender is Button { Tag: HomeItem item }) Home?.OpenSheetCommand.Execute(item);
    }
}
