using Avalonia;
using Avalonia.VisualTree;

namespace Multiroom.App;

// Whether a control is shown: its own IsVisible and every ancestor's (Avalonia's IsEffectivelyVisibleChanged
// is internal). An endless animation must run only while its control is shown: Avalonia keeps its frame timer
// going for a hidden or paused animation too, and on a phone that cost the UI thread up to a whole core
// (measured). Styles mark such a control with TrackShown and run the animation only on the class "shown".
public sealed class VisibilityWatch
{
    public static readonly AttachedProperty<bool> TrackShownProperty =
        AvaloniaProperty.RegisterAttached<VisibilityWatch, Visual, bool>("TrackShown");
    public static bool GetTrackShown(Visual control) => control.GetValue(TrackShownProperty);
    public static void SetTrackShown(Visual control, bool value) => control.SetValue(TrackShownProperty, value);

    static VisibilityWatch() => TrackShownProperty.Changed.AddClassHandler<Visual>((control, e) =>
    {
        if (e.NewValue is true) Attach(control, () => control.Classes.Set("shown", control.IsEffectivelyVisible));
    });

    private VisibilityWatch() { }

    // Calls back when the control becomes shown or hidden, while it is in the visual tree.
    public static void Attach(Visual control, Action changed)
    {
        var watched = new List<Visual>();
        void OnChanged(object? sender, AvaloniaPropertyChangedEventArgs e) { if (e.Property == Visual.IsVisibleProperty) changed(); }
        void Watch()
        {
            foreach (var visual in control.GetSelfAndVisualAncestors()) { visual.PropertyChanged += OnChanged; watched.Add(visual); }
            changed();
        }
        control.AttachedToVisualTree += (_, _) => Watch();
        control.DetachedFromVisualTree += (_, _) =>
        {
            foreach (var visual in watched) visual.PropertyChanged -= OnChanged;
            watched.Clear();
            changed();
        };
        if (Avalonia.Controls.TopLevel.GetTopLevel(control) is not null) Watch();
    }
}
