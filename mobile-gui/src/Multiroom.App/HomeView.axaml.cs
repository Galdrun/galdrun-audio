using Avalonia.Controls;
using Avalonia.Interactivity;

namespace Multiroom.App;

public partial class HomeView : UserControl
{
    public HomeView() => InitializeComponent();

    private async void PlayFavourite(object? sender, RoutedEventArgs e)
    {
        if (DataContext is ShellViewModel shell && sender is Button { Tag: HomeItem item })
            await shell.Home.PlayCommand.ExecuteAsync(item);
    }

    private async void QueueFavourite(object? sender, RoutedEventArgs e)
    {
        if (DataContext is ShellViewModel shell && sender is MenuItem { Tag: HomeItem item })
            await shell.Home.QueueCommand.ExecuteAsync(item);
    }
}
