using Avalonia;
using Avalonia.Controls.Primitives;
using Avalonia.Input;
using Avalonia.Media;
using Avalonia.Rendering;

namespace Multiroom.App;

// The volume page's big dial: a 270° ring showing Value (0-100). Dragging the knob turns it around the ring;
// swiping up or right elsewhere turns it up, down or left turns it down; the wheel and the arrow keys step it.
// Values are whole percents.
public sealed class VolumeDial : RangeBase, ICustomHitTest
{
    public static readonly StyledProperty<bool> IsMutedProperty = AvaloniaProperty.Register<VolumeDial, bool>(nameof(IsMuted));
    public bool IsMuted { get => GetValue(IsMutedProperty); set => SetValue(IsMutedProperty, value); }
    private const double StartAngle = 135, Sweep = 270, Ring = 10;
    private static readonly IBrush Track = new SolidColorBrush(Color.Parse("#2E3638"));
    private static readonly IBrush Accent = new SolidColorBrush(Color.Parse("#CBEC97"));
    private static readonly IBrush MutedAccent = new SolidColorBrush(Color.Parse("#5E6B66"));
    private static readonly IBrush Glow = new SolidColorBrush(Color.Parse("#CBEC97"), 0.16);
    private static readonly IBrush Knob = new SolidColorBrush(Color.Parse("#EEF1ED"));
    private static readonly IBrush Face = new RadialGradientBrush
    {
        GradientStops = { new(Color.Parse("#252D30"), 0), new(Color.Parse("#171C1E"), 1) },
        GradientOrigin = new RelativePoint(0.5, 0.3, RelativeUnit.Relative),
    };
    private static readonly IPen FaceRim = new Pen(new SolidColorBrush(Color.Parse("#2E3638")), 1);
    private Point? _from;
    private bool _turning;
    // The whole square takes swipes, not only what is drawn.
    public bool HitTest(Point point) => new Rect(Bounds.Size).Contains(point);
    private double _fromValue;

    static VolumeDial()
    {
        MaximumProperty.OverrideDefaultValue<VolumeDial>(100);
        FocusableProperty.OverrideDefaultValue<VolumeDial>(true);
        SmallChangeProperty.OverrideDefaultValue<VolumeDial>(1);
        LargeChangeProperty.OverrideDefaultValue<VolumeDial>(10);
        AffectsRender<VolumeDial>(ValueProperty, IsMutedProperty, IsEnabledProperty);
    }

    public override void Render(DrawingContext context)
    {
        var size = Math.Min(Bounds.Width, Bounds.Height);
        if (size <= 2 * Ring + 20) return;
        var (center, radius) = Geometry();
        var fraction = Fraction();
        context.DrawEllipse(Face, FaceRim, center, radius - Ring - 8, radius - Ring - 8);
        context.DrawGeometry(null, new Pen(Track, Ring, lineCap: PenLineCap.Round), Arc(center, radius, Sweep));
        if (fraction > 0)
        {
            var arc = Arc(center, radius, Sweep * fraction);
            if (IsEnabled && !IsMuted) context.DrawGeometry(null, new Pen(Glow, Ring * 2.4, lineCap: PenLineCap.Round), arc);
            context.DrawGeometry(null, new Pen(IsMuted || !IsEnabled ? MutedAccent : Accent, Ring, lineCap: PenLineCap.Round), arc);
        }
        context.DrawEllipse(Knob, null, PointAt(center, radius, StartAngle + Sweep * fraction), Ring * 0.95, Ring * 0.95);
    }

    private (Point Center, double Radius) Geometry() =>
        (new Point(Bounds.Width / 2, Bounds.Height / 2), Math.Min(Bounds.Width, Bounds.Height) / 2 - Ring - 6);

    private double Fraction()
    {
        var range = Maximum - Minimum;
        return range > 0 ? Math.Clamp((Value - Minimum) / range, 0, 1) : 0;
    }

    private static Point PointAt(Point center, double radius, double degrees)
    {
        var angle = degrees * Math.PI / 180;
        return new(center.X + radius * Math.Cos(angle), center.Y + radius * Math.Sin(angle));
    }

    private static StreamGeometry Arc(Point center, double radius, double sweep)
    {
        var geometry = new StreamGeometry();
        using var context = geometry.Open();
        context.BeginFigure(PointAt(center, radius, StartAngle), false);
        context.ArcTo(PointAt(center, radius, StartAngle + Math.Max(sweep, 0.01)), new Size(radius, radius), 0, sweep > 180, SweepDirection.Clockwise);
        context.EndFigure(false);
        return geometry;
    }

    private void SetValueRounded(double value) => SetCurrentValue(ValueProperty, Math.Clamp(Math.Round(value), Minimum, Maximum));

    protected override void OnPointerPressed(PointerPressedEventArgs e)
    {
        base.OnPointerPressed(e);
        if (!e.GetCurrentPoint(this).Properties.IsLeftButtonPressed) return;
        _from = e.GetPosition(this); _fromValue = Value;
        // A touch on the knob (with a finger-sized margin) turns the dial around the ring.
        var (center, radius) = Geometry();
        var knob = PointAt(center, radius, StartAngle + Sweep * Fraction());
        _turning = Point.Distance(_from.Value, knob) <= Ring * 3;
        e.Pointer.Capture(this); Focus(); e.Handled = true;
    }

    protected override void OnPointerMoved(PointerEventArgs e)
    {
        base.OnPointerMoved(e);
        if (_from is not { } from) return;
        var at = e.GetPosition(this);
        if (_turning)
        {
            var (center, _) = Geometry();
            var angle = (Math.Atan2(at.Y - center.Y, at.X - center.X) * 180 / Math.PI - StartAngle + 720) % 360;
            // In the gap at the bottom the knob stays at the end it came from, so it never jumps between 0 and 100.
            var fraction = angle <= Sweep ? angle / Sweep : Fraction() >= 0.5 ? 1 : 0;
            SetValueRounded(Minimum + fraction * (Maximum - Minimum));
            e.Handled = true;
            return;
        }
        // A swipe across the dial's width (at least 240 px) spans the whole range.
        var span = Math.Max(240, Math.Min(Bounds.Width, Bounds.Height));
        SetValueRounded(_fromValue + ((at.X - from.X) - (at.Y - from.Y)) / span * (Maximum - Minimum));
        e.Handled = true;
    }

    protected override void OnPointerReleased(PointerReleasedEventArgs e)
    {
        base.OnPointerReleased(e);
        if (_from is null) return;
        _from = null; e.Pointer.Capture(null); e.Handled = true;
    }

    protected override void OnPointerCaptureLost(PointerCaptureLostEventArgs e) { base.OnPointerCaptureLost(e); _from = null; }

    protected override void OnPointerWheelChanged(PointerWheelEventArgs e)
    {
        base.OnPointerWheelChanged(e);
        SetValueRounded(Value + (e.Delta.Y + e.Delta.X) * SmallChange * 2); e.Handled = true;
    }

    protected override void OnKeyDown(KeyEventArgs e)
    {
        base.OnKeyDown(e);
        double? step = e.Key switch
        {
            Key.Up or Key.Right or Key.Add or Key.OemPlus => SmallChange,
            Key.Down or Key.Left or Key.Subtract or Key.OemMinus => -SmallChange,
            Key.PageUp => LargeChange, Key.PageDown => -LargeChange,
            Key.Home => Minimum - Value, Key.End => Maximum - Value,
            _ => null,
        };
        if (step is not { } delta) return;
        SetValueRounded(Value + delta); e.Handled = true;
    }
}
