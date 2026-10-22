using Android.App;
using Android.Content.PM;
using Android.Runtime;
using Avalonia.Android;

namespace Multiroom.App.Android;

[Activity(Label = Branding.Name, MainLauncher = true,
    ConfigurationChanges = ConfigChanges.Orientation | ConfigChanges.ScreenSize | ConfigChanges.UiMode)]
public class MainActivity : AvaloniaMainActivity;

[Application]
public class AndroidApp(nint reference, JniHandleOwnership transfer) : AvaloniaAndroidApplication<App>(reference, transfer);
