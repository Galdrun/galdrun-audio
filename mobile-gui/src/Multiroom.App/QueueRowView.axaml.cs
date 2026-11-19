using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.Shapes;
using Avalonia.Data;
using Avalonia.Rendering.Composition;
using Avalonia.Threading;

namespace Multiroom.App;

public partial class QueueRowView : UserControl
{
    // Each bar's height, as part of its 14: at rest, and from/to while moving, with its time for one way.
    private readonly record struct BarMotion(double Rest, double From, double To, double Seconds);
    private static readonly BarMotion[] Motion =
        [new(7 / 14d, 4 / 14d, 13 / 14d, 0.9), new(1, 1, 5 / 14d, 0.6), new(10 / 14d, 6 / 14d, 1, 0.75), new(5 / 14d, 11 / 14d, 3 / 14d, 1.05)];
    // Frames a second for the bars: an equaliser icon needs few, and each frame costs the render thread the same
    // whatever it changes (it walks the whole tree and copies the whole screen layer on Android).
    // 10 to save render work (was 15, which moved more smoothly; set it back if 10 looks too choppy).
    private const double BarFrames = 10;
    private readonly UiTimer _barTimer;
    private readonly System.Diagnostics.Stopwatch _barClock = new();
    private CompositionVisual[] _barVisuals = [];
    private bool? _moving;
    private bool _attached;

    public QueueRowView()
    {
        InitializeComponent();
        _barTimer = new UiTimer(StepBars);
    }

    // The queue's own values (its remove command, its artwork, whether it plays) come with the row: it asks no
    // ancestor for them, once per row instead of once per row and item.
    public QueueRowView(PlaybackViewModel queue) : this()
    {
        Delete.Command = queue.RemoveItemCommand;
        Picture.Bind(ArtworkView.CacheProperty, new Binding(nameof(PlaybackViewModel.Artwork)) { Source = queue });
        Queue = queue;
        void Playing(object? sender, System.ComponentModel.PropertyChangedEventArgs e)
        { if (e.PropertyName is null or nameof(PlaybackViewModel.IsPlaying)) UpdateBars(); }
        AttachedToVisualTree += (_, _) => { _attached = true; queue.PropertyChanged += Playing; UpdateBars(); };
        DetachedFromVisualTree += (_, _) => { _attached = false; queue.PropertyChanged -= Playing; UpdateBars(); };
        VisibilityWatch.Attach(Bars, UpdateBars);
    }

    private PlaybackViewModel? Queue { get; }

    // Whether the playing bars move (tests).
    public bool AreBarsMoving => _moving == true;

    protected override void OnPropertyChanged(AvaloniaPropertyChangedEventArgs change)
    {
        base.OnPropertyChanged(change);
        if (change.Property == RecyclingRowPanel.IsInViewProperty) UpdateBars();
    }

    // The bars move while the zone plays and they show: on the playing row, on a shown page, inside the viewport.
    // A timer sets their compositor scale BarFrames times a second: no layout and one small frame per step. The style
    // animation they had changed their height every frame, laid the row out and kept Avalonia's frame timer and its
    // calls into Java going (a quarter of a core on a phone); a compositor animation draws at the display's 120 Hz
    // (half a core on the render thread). Measured. Nothing runs while they do not show.
    private void UpdateBars()
    {
        var moving = _attached && Queue?.IsPlaying == true && Bars.IsEffectivelyVisible && RecyclingRowPanel.GetIsInView(this);
        if (moving == _moving) return;
        if (!moving) { _barTimer.Stop(); _barClock.Stop(); } // Also when the row left the tree and has no visuals.
        var bars = Bars.Children.OfType<Rectangle>().ToArray();
        var visuals = bars.Select(ElementComposition.GetElementVisual).ToArray();
        if (visuals.Any(v => v is null)) { _moving = null; return; } // Not drawn yet: asked again once shown.
        _moving = moving;
        _barVisuals = visuals!;
        for (var i = 0; i < bars.Length; i++) _barVisuals[i].CenterPoint = new Vector3D(bars[i].Width / 2, bars[i].Height, 0);
        if (moving) { _barClock.Start(); _barTimer.Start(TimeSpan.FromSeconds(1 / BarFrames)); StepBars(); }
        else
            for (var i = 0; i < _barVisuals.Length && i < Motion.Length; i++) _barVisuals[i].Scale = new Vector3D(1, Motion[i].Rest, 1);
    }

    // Each bar goes from From to To and back, eased like a sine, one way in its Seconds.
    private void StepBars()
    {
        var time = _barClock.Elapsed.TotalSeconds;
        for (var i = 0; i < _barVisuals.Length && i < Motion.Length; i++)
        {
            var (_, from, to, seconds) = Motion[i];
            var phase = time / seconds % 2;
            var progress = phase <= 1 ? phase : 2 - phase;
            _barVisuals[i].Scale = new Vector3D(1, from + (to - from) * (1 - Math.Cos(Math.PI * progress)) / 2, 1);
        }
    }
}
