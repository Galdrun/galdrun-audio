using System.Reflection;
using Avalonia;

namespace Multiroom.App.Android;

// Turns on Avalonia's EGL partial presentation (AvaloniaUI/Avalonia#22501, Avalonia 12.2): the frame is drawn into
// the window buffer in place, only where it changed, and presented with its damage, instead of drawing into a
// full-screen layer and copying all of it every frame. Measured on a Mali/MediaTek phone for the playing bars: 55%
// less GPU time and about 6% less render-thread CPU (docs/android-rendering.md).
//
// Avalonia does not offer this on Android yet, so two internals are set, before the window's render target exists
// (MainActivity.OnCreate, ahead of Android's surface):
// - the EGL surface's opt-in flag, which only X11, Wayland and DRM have an option for;
// - buffer age support when the driver only advertises EGL_KHR_partial_update: that extension defines the same
//   query (EGL_BUFFER_AGE_KHR == EGL_BUFFER_AGE_EXT), but Avalonia checks only for EGL_EXT_buffer_age.
// Avalonia still checks the extensions itself and draws as before without them. If its internals change, nothing
// is set and the app draws as before; the log says which. Remove this once Avalonia has an Android option.
internal static class PartialPresentation
{
    private const BindingFlags Members = BindingFlags.Instance | BindingFlags.NonPublic | BindingFlags.Public;

    public static void Enable(object activity)
    {
        try
        {
            var bufferAge = EnableBufferAgeFromPartialUpdate();
            var surface = EnableOnSurface(activity);
            Log($"partial presentation: surface {(surface ? "enabled" : "not found")}, buffer age {bufferAge}");
        }
        catch (Exception e) when (e is TargetException or ArgumentException or MemberAccessException or InvalidCastException)
        {
            Log("partial presentation: left off, " + e.Message);
        }
    }

    private static string EnableBufferAgeFromPartialUpdate()
    {
        var locator = typeof(AvaloniaLocator).GetProperty("Current", BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic)?.GetValue(null);
        var graphics = locator?.GetType().GetMethod("GetService", [typeof(Type)])?.Invoke(locator, [typeof(Avalonia.Platform.IPlatformGraphics)]);
        var display = MembersOf(graphics).FirstOrDefault(v => v?.GetType().FullName == "Avalonia.OpenGL.Egl.EglDisplay");
        if (display is null) return "unknown (no EGL display)";
        var type = display.GetType();
        if (type.GetProperty("SupportsBufferAge", Members)?.GetValue(display) is true) return "supported";
        if (type.GetProperty("SupportsPartialUpdate", Members)?.GetValue(display) is not true) return "unsupported";
        if (type.GetField("<SupportsBufferAge>k__BackingField", Members) is not { } field) return "not settable";
        field.SetValue(display, true);
        return "from EGL_KHR_partial_update";
    }

    private static bool EnableOnSurface(object activity)
    {
        object? view = null;
        for (var type = activity.GetType(); type is not null && view is null; type = type.BaseType)
            view = type.GetField("_view", Members | BindingFlags.DeclaredOnly)?.GetValue(activity);
        var topLevel = view?.GetType().GetProperty("TopLevelImpl", Members)?.GetValue(view);
        if (topLevel?.GetType().GetProperty("Surfaces", Members)?.GetValue(topLevel) is not System.Collections.IEnumerable surfaces) return false;
        foreach (var surface in surfaces)
            if (surface?.GetType().FullName == "Avalonia.OpenGL.Egl.EglGlPlatformSurface" &&
                surface.GetType().GetField("_usePartialPresentation", Members) is { } flag)
            {
                flag.SetValue(surface, true);
                return true;
            }
        return false;
    }

    private static IEnumerable<object?> MembersOf(object? target)
    {
        for (var type = target?.GetType(); type is not null; type = type.BaseType)
        {
            foreach (var field in type.GetFields(Members | BindingFlags.DeclaredOnly)) yield return field.GetValue(target);
            foreach (var property in type.GetProperties(Members | BindingFlags.DeclaredOnly))
                if (property.GetIndexParameters().Length == 0 && property.GetMethod is not null) yield return property.GetValue(target);
        }
    }

    private static void Log(string message) => global::Android.Util.Log.Info("Galdrun", message);
}
