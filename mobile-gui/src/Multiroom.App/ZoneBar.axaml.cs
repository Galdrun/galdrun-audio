using Avalonia;
using Avalonia.Animation;
using Avalonia.Animation.Easings;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Interactivity;
using Avalonia.Media;
using Avalonia.Threading;
using Avalonia.VisualTree;

namespace Multiroom.App;

public partial class ZoneBar : UserControl
{
    public ZoneBar()
    {
        InitializeComponent();
        MediaText.RenderTransform = _media; MediaPeek.RenderTransform = _mediaPeek;
        ZoneLabel.RenderTransform = _zone; ZonePeek.RenderTransform = _zonePeek;
        AttachCarousel(ArtworkButton); AttachCarousel(MediaButton); AttachCarousel(ZoneButton);
        ZoneSwipe.Attach(NowPlayingZoneSelector, direction =>
        { if (DataContext is ShellViewModel model) model.Playback.ChangeZoneCommand.Execute(direction); },
            swipeUp: () => (DataContext as ShellViewModel)?.NavigateCommand.Execute("Zone Setup"));
        SeekSlider.Attach(ZonePositionSlider, () => (DataContext as ShellViewModel)?.Playback);
        SizeChanged += (_, _) =>
        {
            var narrow = Bounds.Width < 520;
            MediaTitle.FontSize = narrow ? 16 : 18;
        };
        AddHandler(PointerPressedEvent, (_, _) => _held = false, RoutingStrategies.Tunnel, handledEventsToo: true);
    }

    // Carousel: the track and zone name follow a horizontal drag with the neighbouring zone's beside them;
    // a swipe as long as ZoneSwipe's slides the neighbour into place and then changes zone.
    private readonly TranslateTransform _media = new(), _mediaPeek = new(), _zone = new(), _zonePeek = new();
    private int _slide;
    private double SlideWidth => Math.Max(MediaSlides.Bounds.Width, ZoneSlides.Bounds.Width) + 24;

    private void AttachCarousel(Button button)
    {
        Point? press = null; IPointer? pointer = null; string? peeking = null;
        void Reset() { press = null; pointer = null; }
        button.AddHandler(PointerPressedEvent, (_, e) =>
        {
            if (pointer is null && e.GetCurrentPoint(button).Properties.IsLeftButtonPressed)
            { if (MediaPeek.IsVisible) FinishSlide(); press = e.GetPosition(button); pointer = e.Pointer; peeking = null; }
        }, RoutingStrategies.Tunnel);
        button.AddHandler(PointerMovedEvent, (_, e) =>
        {
            if (press is not { } start || pointer != e.Pointer) return;
            var delta = e.GetPosition(button) - start;
            if (peeking is null && (Math.Abs(delta.X) < 12 || Math.Abs(delta.X) <= Math.Abs(delta.Y))) return;
            var direction = delta.X < 0 ? "next" : "previous";
            if (direction != peeking && !ShowPeek(direction)) return;
            peeking = direction;
            Slide(delta.X, delta.X + (delta.X < 0 ? SlideWidth : -SlideWidth), animate: false);
        }, RoutingStrategies.Tunnel);
        button.AddHandler(PointerReleasedEvent, (_, e) =>
        {
            if (press is not { } start || pointer != e.Pointer) return;
            var delta = e.GetPosition(button) - start;
            Reset();
            if (Math.Abs(delta.X) < 12 && Math.Abs(delta.Y) < 12) { if (peeking is not null) SettleBack(); return; }
            e.Handled = true;
            e.Pointer.Capture(null); // A drag must not also activate the button.
            if (delta.Y <= -40 && Math.Abs(delta.Y) > Math.Abs(delta.X) * 1.5 && DataContext is ShellViewModel shell)
            {
                if (peeking is not null) SettleBack();
                OpenTapped(null, e);
                shell.NavigateCommand.Execute("Now Playing");
                return;
            }
            var side = delta.X < 0 ? 1 : -1;
            var direction = side > 0 ? "next" : "previous";
            if (Math.Abs(delta.X) < 40 || Math.Abs(delta.X) <= Math.Abs(delta.Y) * 1.5 || DataContext is not ShellViewModel model
                || (direction != peeking && !ShowPeek(direction)))
            { if (peeking is not null) SettleBack(); return; }
            // The neighbour's text slides fully in first; the zone changes once it has landed, as changing it
            // (artwork, queue, marks) is heavy UI-thread work that would stall the slide's start.
            _pendingZone = () => model.Playback.ChangeZoneCommand.Execute(direction);
            Slide(-side * SlideWidth, 0, animate: true);
        }, RoutingStrategies.Tunnel);
        button.PointerCaptureLost += (_, e) => { if (e.Pointer == pointer) { Reset(); if (peeking is not null) SettleBack(); } };
        button.DetachedFromVisualTree += (_, _) => { Reset(); FinishSlide(); };

        void SettleBack() => Slide(0, _mediaPeek.X < 0 ? -SlideWidth : SlideWidth, animate: true);
    }

    private bool ShowPeek(string direction)
    {
        if ((DataContext as ShellViewModel)?.Playback is not { Zones.Count: > 1 } playback || playback.PeekZone(direction) is not { } peek) return false;
        PeekTitle.Text = peek.Title; PeekSubtitle.Text = peek.Subtitle; PeekZoneName.Text = peek.ZoneName;
        MediaPeek.IsVisible = ZonePeek.IsVisible = true;
        return true;
    }

    private void Slide(double main, double peek, bool animate)
    {
        var slide = ++_slide;
        foreach (var (transform, x) in new[] { (_media, main), (_zone, main), (_mediaPeek, peek), (_zonePeek, peek) })
        {
            transform.Transitions = animate ? new Transitions { new DoubleTransition
                { Property = TranslateTransform.XProperty, Duration = TimeSpan.FromMilliseconds(220), Easing = new CubicEaseOut() } } : null;
            transform.X = x;
        }
        if (animate) DispatcherTimer.RunOnce(() => { if (slide == _slide) FinishSlide(); }, TimeSpan.FromMilliseconds(240));
    }

    // Ends a slide at once: changes to a swiped zone and puts its text back in the main layer.
    private Action? _pendingZone;
    private void FinishSlide()
    {
        ++_slide;
        var change = _pendingZone; _pendingZone = null;
        change?.Invoke();
        foreach (var transform in new[] { _media, _zone, _mediaPeek, _zonePeek }) { transform.Transitions = null; transform.X = 0; }
        MediaPeek.IsVisible = ZonePeek.IsVisible = false;
    }

    // Opening Now Playing from the bar (its buttons' Click, before their command shows the page, or a swipe up).
    private void OpenTapped(object? sender, RoutedEventArgs e) => this.FindAncestorOfType<MainView>()?.SlideInNowPlaying();

    // A long press switches the sleep timer off, and its release would tap it too.
    private bool _held;
    private PlaybackViewModel? Playback => (DataContext as ShellViewModel)?.Playback;
    private void SleepTapped(object? sender, RoutedEventArgs e)
    {
        if (_held) { _held = false; return; }
        Playback?.SleepCommand.Execute(null);
    }
    private void SleepHeld(object? sender, ContextRequestedEventArgs e)
    {
        e.Handled = true; _held = true;
        Playback?.SleepOffCommand.Execute(null);
    }
}
