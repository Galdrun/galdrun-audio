using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Interactivity;

namespace Multiroom.App;

public partial class BrowserView : UserControl
{
    public BrowserView()
    {
        InitializeComponent();
        SizeChanged += (_, _) => { if (DataContext is BrowserViewModel model) model.SetWidth(Bounds.Width); };
        AddHandler(PointerPressedEvent, (_, _) => _held = false, RoutingStrategies.Tunnel, handledEventsToo: true);
    }
    // A long press opens the sheet, and its release would click the row too.
    private bool _held;
    private void SelectCard(object? sender, RoutedEventArgs e)
    {
        if (_held) { _held = false; return; }
        if (sender is Button { Tag: MediaRow row } && DataContext is BrowserViewModel model)
            model.SelectedItem = row;
    }
    private void OpenRow(object? sender, RoutedEventArgs e)
    {
        if (_held) { _held = false; return; }
        if (sender is Button { Tag: MediaRow row } && DataContext is BrowserViewModel model)
            model.OpenRowCommand.Execute(row);
    }
    private void RequestRowActions(object? sender, ContextRequestedEventArgs e)
    {
        if (sender is not Button { Tag: MediaRow row } || DataContext is not BrowserViewModel model || !model.RequestActions(row)) return;
        e.Handled = true; _held = true;
    }
}
