using Avalonia;
using Avalonia.Controls;

namespace Multiroom.App;

public partial class MainView : UserControl
{
    private readonly ShellViewModel _model;
    public MainView() : this(true) { }
    public MainView(bool loadPreferences) : this(new ShellViewModel(), loadPreferences) { }
    public MainView(ShellViewModel model, bool loadPreferences = true)
    {
        InitializeComponent();
        _model = model;
        DataContext = _model;
        if (loadPreferences) AttachedToVisualTree += async (_, _) => await _model.InitializeAsync();
        SizeChanged += (_, _) =>
        {
            var compact = Bounds.Width < 700;
            Navigation.IsVisible = !compact;
            Body.ColumnDefinitions[0].Width = new GridLength(compact ? 0 : 190);
            _model.SetWidth(Bounds.Width);
        };
        DetachedFromVisualTree += async (_, _) => await _model.DisconnectAsync();
    }
    private void PagePicked(object? sender, SelectionChangedEventArgs e)
    {
        if (e.AddedItems.Count > 0) HamburgerButton.Flyout?.Hide();
    }
    private void SheetScrimTapped(object? sender, Avalonia.Input.TappedEventArgs e) => _model.Home.CloseSheet();
}
