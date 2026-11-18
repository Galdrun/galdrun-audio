using Android.App;
using Android.Content.PM;
using Android.Runtime;
using Avalonia;
using Avalonia.Android;
using Avalonia.Threading;

namespace Multiroom.App.Android;

[Activity(Label = Branding.Name, MainLauncher = true, Theme = "@style/Theme.AppCompat.DayNight.NoActionBar",
    ConfigurationChanges = ConfigChanges.Orientation | ConfigChanges.ScreenSize | ConfigChanges.UiMode)]
public class MainActivity : AvaloniaMainActivity;

[Application]
public class AndroidApp(nint reference, JniHandleOwnership transfer) : AvaloniaAndroidApplication<App>(reference, transfer)
{
    // A list's glide after a fling runs at Input priority and starved behind rendering until Avalonia's
    // default 1 s starvation timeout let it through: the list stopped for a second, then moved on (measured).
    protected override AppBuilder CustomizeAppBuilder(AppBuilder builder) =>
        base.CustomizeAppBuilder(builder).With(new DispatcherOptions { InputStarvationTimeout = TimeSpan.FromMilliseconds(50) });
}
