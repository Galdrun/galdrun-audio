using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Interactivity;

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
        _model.PropertyChanged += (_, e) => { if (e.PropertyName == nameof(ShellViewModel.HasInfo) && !_model.HasInfo) InfoButton.Flyout?.Hide(); };
        AttachedToVisualTree += (_, _) => { if (TopLevel.GetTopLevel(this) is { } top) top.BackRequested += SystemBack; };
        DetachedFromVisualTree += (_, _) => { if (TopLevel.GetTopLevel(this) is { } top) top.BackRequested -= SystemBack; };
        // A right click (or the mouse's back button) is the desktop's system back, wherever it lands, unless it
        // opened something there (a Home cover's sheet). Lists and buttons mark pointer events handled, so look past that.
        var contextTaken = false;
        AddHandler(PointerPressedEvent, (_, _) => contextTaken = false, RoutingStrategies.Tunnel, handledEventsToo: true);
        AddHandler(ContextRequestedEvent, (_, e) => contextTaken = e.Handled, RoutingStrategies.Bubble, handledEventsToo: true);
        AddHandler(PointerReleasedEvent, (_, e) =>
        { if (e.InitialPressMouseButton is MouseButton.Right or MouseButton.XButton1 && !contextTaken && _model.GoBack()) e.Handled = true; },
            RoutingStrategies.Bubble, handledEventsToo: true);
    }
    private void SystemBack(object? sender, RoutedEventArgs e) { if (_model.GoBack()) e.Handled = true; }
    // The menus show the page, never set it on their own: Browse is not among them (OneWay selection).
    private void PagePicked(object? sender, SelectionChangedEventArgs e)
    {
        if (e.AddedItems is [string page]) { _model.SelectedPage = page; _model.IsMenuOpen = false; }
    }
    private void MenuScrimTapped(object? sender, TappedEventArgs e) => _model.IsMenuOpen = false;
    private void SheetScrimTapped(object? sender, Avalonia.Input.TappedEventArgs e) => _model.Home.CloseSheet();
}
