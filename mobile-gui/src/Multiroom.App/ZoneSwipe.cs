using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Interactivity;

namespace Multiroom.App;

internal static class ZoneSwipe
{
    // swipeUp: what a swipe up does (Now Playing's zone selector opens Zone Setup), if anything.
    public static void Attach(Control control, Action<string> changeZone, Action? swipeUp = null)
    {
        Point? press = null;
        IPointer? pointer = null;
        control.AddHandler(InputElement.PointerPressedEvent, (_, e) =>
        {
            if (pointer is null && e.GetCurrentPoint(control).Properties.IsLeftButtonPressed)
            {
                press = e.GetPosition(control); pointer = e.Pointer;
                // Over text nothing else captures it, and the release would go to wherever the swipe ends.
                // Not from a button inside it (the like button beside a title): that button needs its release.
                if (Avalonia.VisualTree.VisualExtensions.FindAncestorOfType<Button>(e.Source as Visual, includeSelf: true) is not { } inner || inner == control)
                    e.Pointer.Capture(control);
            }
        }, RoutingStrategies.Tunnel);
        control.AddHandler(InputElement.PointerReleasedEvent, (_, e) =>
        {
            if (press is not { } start || pointer != e.Pointer) return;
            var delta = e.GetPosition(control) - start;
            press = null; pointer = null;
            if (Math.Abs(delta.X) < 12 && Math.Abs(delta.Y) < 12) return;
            e.Handled = true;
            e.Pointer.Capture(null); // A drag must not also activate the button.
            if (Math.Abs(delta.X) >= 40 && Math.Abs(delta.X) > Math.Abs(delta.Y) * 1.5)
                changeZone(delta.X < 0 ? "next" : "previous");
            else if (swipeUp is not null && delta.Y <= -40 && -delta.Y > Math.Abs(delta.X) * 1.5)
                swipeUp();
        }, RoutingStrategies.Tunnel);
        control.PointerCaptureLost += (_, e) => { if (e.Pointer == pointer) { press = null; pointer = null; } };
        control.DetachedFromVisualTree += (_, _) => { press = null; pointer = null; };
    }
}
