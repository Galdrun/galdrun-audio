using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Interactivity;

namespace Multiroom.App;

internal static class ZoneSwipe
{
    public static void Attach(Control control, Action<string> changeZone)
    {
        Point? press = null;
        IPointer? pointer = null;
        control.AddHandler(InputElement.PointerPressedEvent, (_, e) =>
        {
            if (pointer is null && e.GetCurrentPoint(control).Properties.IsLeftButtonPressed)
            { press = e.GetPosition(control); pointer = e.Pointer; }
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
        }, RoutingStrategies.Tunnel);
        control.PointerCaptureLost += (_, e) => { if (e.Pointer == pointer) { press = null; pointer = null; } };
        control.DetachedFromVisualTree += (_, _) => { press = null; pointer = null; };
    }
}
