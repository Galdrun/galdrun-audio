using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Markup.Xaml;

namespace Multiroom.App;

public partial class App : Application
{
    public override void Initialize() => AvaloniaXamlLoader.Load(this);
    public override void OnFrameworkInitializationCompleted()
    {
        if (ApplicationLifetime is IClassicDesktopStyleApplicationLifetime desktop)
            desktop.MainWindow = new Window { Title = Branding.Name, Width = 1280, Height = 850, MinWidth = 360,
                MinHeight = 600, Content = new MainView() };
        else if (ApplicationLifetime is IActivityApplicationLifetime activity)
            activity.MainViewFactory = () => new MainView();
        else if (ApplicationLifetime is ISingleViewApplicationLifetime single)
            single.MainView = new MainView();
        base.OnFrameworkInitializationCompleted();
    }
}
