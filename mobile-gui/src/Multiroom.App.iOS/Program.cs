using Avalonia.iOS;
using Foundation;
using UIKit;

namespace Multiroom.App.iOS;

[Register("AppDelegate")]
public class AppDelegate : AvaloniaAppDelegate<App>;

internal static class Program
{
    public static void Main(string[] args) => UIApplication.Main(args, null, typeof(AppDelegate));
}
