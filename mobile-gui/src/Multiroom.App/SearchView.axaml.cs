using Avalonia.Controls;
using Avalonia.Controls.Templates;
using Avalonia.Controls.Primitives;
using Avalonia.Input;
using Avalonia.Interactivity;

namespace Multiroom.App;

public partial class SearchView : UserControl
{
    // A long press opens the sheet, and its release would click the row too.
    private bool _held;

    public SearchView()
    {
        InitializeComponent();
        var template = (IDataTemplate)Resources["ResultRow"]!;
        ResultList.CreateRow = () =>
        {
            var row = new ContentControl { ContentTemplate = template, HorizontalContentAlignment = Avalonia.Layout.HorizontalAlignment.Stretch };
            row.DataContextChanged += (_, _) => row.Content = row.DataContext;
            return row;
        };
        ResultScroll.ScrollChanged += (_, _) =>
        {
            if (Model is { HasMore: true, IsSearching: false } model
                && ResultScroll.Offset.Y + ResultScroll.Viewport.Height >= ResultScroll.Extent.Height - 4 * ResultList.RowHeight)
                _ = model.MoreCommand.ExecuteAsync(null);
        };
        AddHandler(PointerPressedEvent, (_, _) => _held = false, RoutingStrategies.Tunnel, handledEventsToo: true);
        // A tap into the box shows the kind's recent searches, also when it already has the focus.
        SearchBox.GotFocus += (_, _) => Model?.ShowRecent();
        SearchBox.AddHandler(PointerReleasedEvent, (_, _) => Model?.ShowRecent(), RoutingStrategies.Tunnel, handledEventsToo: true);
        SearchBox.LostFocus += (_, _) => { if (Model is { } model) model.IsRecentOpen = false; };
    }

    // The sources are the server's (they change with it): the menu is made when it opens, the picked one checked.
    // (A flyout filled only in its Opening event stays shut: Avalonia does not open an empty menu.)
    private void ChooseSource(object? sender, RoutedEventArgs e)
    {
        if (Model is not { } model) return;
        var menu = new MenuFlyout { Placement = PlacementMode.BottomEdgeAlignedLeft };
        foreach (var scope in model.Scopes)
            menu.Items.Add(SourceItem(scope, scope == model.Scope, model));
        menu.ShowAt(SourceTab);
    }

    // A source of the menu, large enough to tap: its icon, its name, a check on the picked one.
    private static MenuItem SourceItem(SearchScope scope, bool picked, SearchViewModel model)
    {
        Avalonia.Controls.Shapes.Path Line(string data) => new()
        {
            Data = Avalonia.Media.StreamGeometry.Parse(data), Stroke = Avalonia.Media.Brushes.White, StrokeThickness = 1.7,
            StrokeJoin = Avalonia.Media.PenLineJoin.Round, StrokeLineCap = Avalonia.Media.PenLineCap.Round, Width = 24, Height = 24,
            Stretch = Avalonia.Media.Stretch.None, VerticalAlignment = Avalonia.Layout.VerticalAlignment.Center,
        };
        var row = new Grid { ColumnDefinitions = new("Auto,*,Auto"), MinWidth = 220 };
        var icon = Line(scope.Icon);
        if (scope.IsFilled) { icon.Fill = Avalonia.Media.Brushes.White; icon.StrokeThickness = 0; }
        row.Children.Add(icon);
        var label = new TextBlock { Text = scope.Label, FontSize = 16, Margin = new Avalonia.Thickness(16, 0),
            VerticalAlignment = Avalonia.Layout.VerticalAlignment.Center, FontWeight = picked ? Avalonia.Media.FontWeight.SemiBold : Avalonia.Media.FontWeight.Normal };
        Grid.SetColumn(label, 1); row.Children.Add(label);
        if (picked) { var check = Line("M 5,12.5 L 10,17.5 L 19,7"); Grid.SetColumn(check, 2); row.Children.Add(check); }
        var item = new MenuItem { Header = row, Command = model.ChooseScopeCommand, CommandParameter = scope, MinHeight = 52,
            Padding = new Avalonia.Thickness(16, 0) };
        Avalonia.Automation.AutomationProperties.SetName(item, scope.Label);
        return item;
    }

    private SearchViewModel? Model => DataContext as SearchViewModel;

    private void OpenRow(object? sender, RoutedEventArgs e)
    {
        if (_held) { _held = false; return; }
        if (sender is Button { Tag: SearchRow row }) Model?.OpenCommand.Execute(row);
    }

    private void RequestActions(object? sender, ContextRequestedEventArgs e)
    {
        e.Handled = true; _held = true;
        if (sender is Button { Tag: SearchRow row }) Model?.RequestActions(row);
    }
}
