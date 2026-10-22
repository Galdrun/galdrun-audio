using Avalonia.Controls;
using Avalonia.Interactivity;

namespace Multiroom.App;

public partial class BrowserView : UserControl
{
    public BrowserView()
    {
        InitializeComponent();
        SizeChanged += (_, _) => { if (DataContext is BrowserViewModel model) model.SetWidth(Bounds.Width); };
    }
    private void SelectCard(object? sender, RoutedEventArgs e)
    {
        if (sender is Button { Tag: MediaRow row } && DataContext is BrowserViewModel model)
            model.SelectedItem = row;
    }
}
