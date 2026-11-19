using System.Reflection;
using Avalonia;
using Xunit;

namespace Multiroom.App.Tests;

// The Android app turns on EGL partial presentation by setting Avalonia internals (Multiroom.App.Android/
// PartialPresentation.cs). An Avalonia update that renames them leaves the feature off without an error; this test
// fails instead. Members only Avalonia.Android has (the activity's view, its TopLevelImpl and Surfaces) are checked
// on the device by the log line (docs/android-rendering.md).
public class PartialPresentationTests
{
    private const BindingFlags Members = BindingFlags.Instance | BindingFlags.NonPublic | BindingFlags.Public;

    [Fact]
    public void AvaloniaInternalsTheAndroidAppSetsExist()
    {
        var openGl = Assembly.Load("Avalonia.OpenGL");
        var surface = openGl.GetType("Avalonia.OpenGL.Egl.EglGlPlatformSurface", throwOnError: true)!;
        Assert.Equal(typeof(bool), surface.GetField("_usePartialPresentation", Members)?.FieldType);

        var display = openGl.GetType("Avalonia.OpenGL.Egl.EglDisplay", throwOnError: true)!;
        Assert.Equal(typeof(bool), display.GetField("<SupportsBufferAge>k__BackingField", Members)?.FieldType);
        Assert.Equal(typeof(bool), display.GetProperty("SupportsPartialUpdate", Members)?.PropertyType);

        var current = typeof(AvaloniaLocator).GetProperty("Current", BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic);
        Assert.NotNull(current?.PropertyType.GetMethod("GetService", [typeof(Type)]));
    }
}
