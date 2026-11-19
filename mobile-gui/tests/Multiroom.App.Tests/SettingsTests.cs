using Avalonia.Controls;
using Avalonia.Headless.XUnit;
using Avalonia.Threading;
using Avalonia.VisualTree;
using Multiroom.App.Api;
using Xunit;

namespace Multiroom.App.Tests;

public sealed class SettingsTests
{
    private static string SettingsPath() => Path.Combine(Path.GetTempPath(), "galdrun-settings-" + Guid.NewGuid().ToString("N"), "settings.json");
    private static ShellViewModel Shell(string path) => new((_, _) => AsyncEnumerable.Empty<DiscoveredServer>(), path);

    [AvaloniaFact]
    public async Task Show_performance_is_loaded_and_saved_without_a_connection_keeping_the_rest()
    {
        var path = SettingsPath();
        try
        {
            await new Preferences("http://192.0.2.4:4725/api/v1/", "living", ShowPerformance: true).SaveAsync(path, TestContext.Current.CancellationToken);
            var model = Shell(path);
            await model.InitializeAsync();
            Assert.True(model.ShowPerformance);

            model.ShowPerformance = false;
            // The save runs on its own.
            Preferences? saved = null;
            for (var tries = 0; tries < 100 && saved?.ShowPerformance != false; tries++)
            {
                await Task.Delay(20, TestContext.Current.CancellationToken);
                try { saved = await Preferences.LoadAsync(path, TestContext.Current.CancellationToken); } catch (IOException) { }
            }
            Assert.Equal(new Preferences("http://192.0.2.4:4725/api/v1/", "living", ShowPerformance: false), saved);
        }
        finally { if (Directory.Exists(Path.GetDirectoryName(path))) Directory.Delete(Path.GetDirectoryName(path)!, true); }
    }

    [AvaloniaFact]
    public void The_settings_page_switches_the_performance_line_which_measures_only_while_shown()
    {
        var path = SettingsPath();
        var model = Shell(path);
        var view = new MainView(model, loadPreferences: false);
        var window = new Window { Width = 390, Height = 844, Content = view };
        try
        {
            window.Show();
            model.SelectedPage = "Settings"; window.UpdateLayout(); Dispatcher.UIThread.RunJobs();
            Assert.Contains("Settings", model.MenuPages);
            // Nothing on it comes from the server.
            Assert.DoesNotContain(view.GetVisualDescendants().OfType<Button>(), b => Equals(b.Content, "Refresh") && b.IsEffectivelyVisible);
            var line = view.FindControl<PerformanceView>("Performance")!;
            Assert.False(line.IsEffectivelyVisible);
            Assert.False(line.IsMeasuring);

            var toggle = view.GetVisualDescendants().OfType<ToggleSwitch>().Single(t => t.Name == "ShowPerformanceSwitch");
            Assert.True(toggle.IsEffectivelyVisible);
            toggle.IsChecked = true; Dispatcher.UIThread.RunJobs();
            Assert.True(model.ShowPerformance);
            Assert.True(line.IsEffectivelyVisible);
            Assert.True(line.IsMeasuring);

            model.ShowPerformance = false; Dispatcher.UIThread.RunJobs();
            Assert.False(line.IsMeasuring);
        }
        finally
        {
            window.Close();
            if (Directory.Exists(Path.GetDirectoryName(path))) Directory.Delete(Path.GetDirectoryName(path)!, true);
        }
    }
}
