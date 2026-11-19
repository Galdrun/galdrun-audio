using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Interactivity;

namespace Multiroom.App;

public partial class MainView : UserControl
{
    private readonly ShellViewModel _model;
    /// <summary>Now Playing rises from the bottom zone bar it was opened from (a tap or a swipe up on it).</summary>
    public void SlideInNowPlaying() => _nowPlaying.SlideIn();

    // Now Playing and Zone Setup rise from the bottom and slide back down when they close, however they are
    // closed; a swipe down closes them. Now Playing rises only when opened from the zone bar (SlideInNowPlaying),
    // Zone Setup always. Zone Setup covers Now Playing: opening it from there leaves Now Playing in place
    // beneath it until it has risen, and closing it back to Now Playing uncovers it.
    private readonly SlidingPage _nowPlaying, _zoneSetup;
    private sealed class SlidingPage
    {
        private static readonly TimeSpan Duration = TimeSpan.FromMilliseconds(260);
        private readonly MainView _view;
        private readonly Control _page;
        private readonly Func<bool> _isOpen;
        private int _generation;
        public SlidingPage(MainView view, Control page, Func<bool> isOpen)
        {
            _view = view; _page = page; _isOpen = isOpen;
            page.IsVisible = isOpen();
            AttachSwipeDown();
        }
        public void SlideIn() => Slide(Math.Max(_view.Bounds.Height, 1), 0);
        // Its page was opened or left; covered: another page rises over it, so it stays until that has.
        public void Changed(bool covered = false)
        {
            var generation = ++_generation;
            if (_isOpen()) { Unpin(); _page.IsVisible = true; _page.IsHitTestVisible = true; return; }
            if (!_page.IsVisible) return;
            _page.IsHitTestVisible = false;
            if (!covered) Slide((_page.RenderTransform as Avalonia.Media.TranslateTransform)?.Y ?? 0, Math.Max(_view.Bounds.Height, 1));
            // Covered, it keeps its height: the zone bar beneath changes with the page, and its content must not move.
            else { _page.Height = _page.Bounds.Height; _page.VerticalAlignment = Avalonia.Layout.VerticalAlignment.Top; }
            Avalonia.Threading.DispatcherTimer.RunOnce(() =>
            {
                if (generation != _generation) return;
                _page.IsVisible = false; _page.RenderTransform = null; Unpin();
            }, Duration + TimeSpan.FromMilliseconds(20));
        }
        private void Unpin()
        {
            _page.Height = double.NaN; _page.VerticalAlignment = Avalonia.Layout.VerticalAlignment.Stretch;
        }
        private void Slide(double from, double to)
        {
            var shift = new Avalonia.Media.TranslateTransform(0, from);
            _page.RenderTransform = shift;
            shift.Transitions = new Avalonia.Animation.Transitions { new Avalonia.Animation.DoubleTransition
                { Property = Avalonia.Media.TranslateTransform.YProperty, Duration = Duration, Easing = new Avalonia.Animation.Easings.CubicEaseOut() } };
            shift.Y = to;
        }

        // A swipe down closes it: not one that starts on a slider (it drags its value) or in a list scrolled
        // down (it scrolls back up).
        private void AttachSwipeDown()
        {
            Point? press = null; IPointer? pointer = null;
            _page.AddHandler(PointerPressedEvent, (_, e) =>
            {
                var source = e.Source as Visual;
                if (pointer is null && e.GetCurrentPoint(_page).Properties.IsLeftButtonPressed
                    && Avalonia.VisualTree.VisualExtensions.FindAncestorOfType<Slider>(source, includeSelf: true) is null
                    && Avalonia.VisualTree.VisualExtensions.FindAncestorOfType<ScrollViewer>(source, includeSelf: true) is not { Offset.Y: > 0 })
                { press = e.GetPosition(_page); pointer = e.Pointer; }
            }, RoutingStrategies.Tunnel);
            // On the whole view: over text nothing captures the pointer, so the release goes to wherever it ends,
            // which may be past the page (the zone bar).
            _view.AddHandler(PointerReleasedEvent, (_, e) =>
            {
                if (press is not { } start || pointer != e.Pointer) return;
                press = null; pointer = null;
                var delta = e.GetPosition(_page) - start;
                if (delta.Y < 60 || delta.Y <= Math.Abs(delta.X) * 1.5 || !_isOpen()) return;
                e.Handled = true;
                e.Pointer.Capture(null); // Not also a tap on the button it started on.
                if (!_view._model.GoBack()) _view._model.SelectedPage = "Home";
            }, RoutingStrategies.Tunnel);
            _page.PointerCaptureLost += (_, e) => { if (e.Pointer == pointer) { press = null; pointer = null; } };
        }
    }

    public MainView() : this(true) { }
    public MainView(bool loadPreferences) : this(new ShellViewModel(), loadPreferences) { }
    public MainView(ShellViewModel model, bool loadPreferences = true)
    {
        InitializeComponent();
        _model = model;
        DataContext = _model;
        _nowPlaying = new SlidingPage(this, NowPlayingPage, () => _model.IsNowPlaying);
        _zoneSetup = new SlidingPage(this, ZoneSetupPage, () => _model.IsZoneSetup);
        if (loadPreferences) AttachedToVisualTree += async (_, _) => await _model.InitializeAsync();
        SizeChanged += (_, _) =>
        {
            var compact = Bounds.Width < 700;
            Navigation.IsVisible = !compact;
            Body.ColumnDefinitions[0].Width = new GridLength(compact ? 0 : 190);
            _model.SetWidth(Bounds.Width);
        };
        DetachedFromVisualTree += async (_, _) => await _model.DisconnectAsync();
        _model.PropertyChanged += (_, e) =>
        {
            if (e.PropertyName == nameof(ShellViewModel.HasInfo) && !_model.HasInfo) InfoButton.Flyout?.Hide();
            if (e.PropertyName == nameof(ShellViewModel.IsNowPlaying)) _nowPlaying.Changed(covered: _model.IsZoneSetup);
            if (e.PropertyName == nameof(ShellViewModel.IsZoneSetup))
            {
                _zoneSetup.Changed();
                if (_model.IsZoneSetup) _zoneSetup.SlideIn();
            }
            // The queue opens on the playing track, once it is laid out.
            if (e.PropertyName == nameof(ShellViewModel.IsQueue) && _model.IsQueue)
                Avalonia.Threading.Dispatcher.UIThread.Post(QueuePage.ScrollToCurrent, Avalonia.Threading.DispatcherPriority.Loaded);
        };
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
        // A touch swipe rightwards from the left edge is back too (a mouse has the right click). From the edge
        // only, so the cover's zone swipe and the queue rows' swipes keep theirs.
        Point? swipeStart = null; IPointer? swipePointer = null;
        AddHandler(PointerPressedEvent, (_, e) =>
        {
            var at = e.GetPosition(this);
            if (e.Pointer.Type != PointerType.Mouse && at.X <= BackEdge) { swipeStart = at; swipePointer = e.Pointer; }
        }, RoutingStrategies.Tunnel, handledEventsToo: true);
        AddHandler(PointerReleasedEvent, (_, e) =>
        {
            if (swipeStart is not { } start || swipePointer != e.Pointer) return;
            var delta = e.GetPosition(this) - start;
            swipeStart = null; swipePointer = null;
            if (delta.X < BackSwipe || delta.X < Math.Abs(delta.Y) * 1.5 || !_model.GoBack()) return;
            e.Handled = true;
            e.Pointer.Capture(null); // The swipe must not also activate what it started on.
        }, RoutingStrategies.Tunnel, handledEventsToo: true);
    }
    private const double BackEdge = 24, BackSwipe = 60;
    private void SystemBack(object? sender, RoutedEventArgs e) { if (_model.GoBack()) e.Handled = true; }
    // The menus show the page, never set it on their own: Browse is not among them (OneWay selection).
    private void PagePicked(object? sender, SelectionChangedEventArgs e)
    {
        if (e.AddedItems is [string page]) { _model.SelectedPage = page; _model.IsMenuOpen = false; }
    }
    // On the queue already, its button brings the playing track back to the middle, as opening the queue does.
    private void QueueTapped(object? sender, RoutedEventArgs e) { if (_model.IsQueue) QueuePage.ScrollToCurrent(); }
    private void MenuScrimTapped(object? sender, TappedEventArgs e) => _model.IsMenuOpen = false;
    private void SheetScrimTapped(object? sender, Avalonia.Input.TappedEventArgs e) => _model.Home.CloseSheet();
}
