using Avalonia;
using Avalonia.Controls;
using Avalonia.Headless;
using Avalonia.Headless.XUnit;
using Avalonia.Input;
using Avalonia.Threading;
using Avalonia.VisualTree;

namespace Multiroom.App.Tests;

public sealed class SearchViewTests
{
    [AvaloniaFact]
    public void The_source_button_is_left_of_the_kinds_and_opens_the_sources()
    {
        var model = new SearchViewModel(_ => Task.CompletedTask) { Scopes = [SearchViewModel.AllSources, new("tidal", "Tidal")] };
        var view = new SearchView { DataContext = model };
        var window = new Window { Width = 390, Height = 844, Content = view };
        try
        {
            window.Show(); window.UpdateLayout(); Dispatcher.UIThread.RunJobs();
            var source = view.FindControl<Button>("SourceTab")!;
            var tracks = view.FindControl<Button>("TracksTab")!;
            Assert.True(source.TranslatePoint(default, window)!.Value.X < tracks.TranslatePoint(default, window)!.Value.X);

            var at = source.TranslatePoint(new Point(source.Bounds.Width / 2, source.Bounds.Height / 2), window)!.Value;
            window.MouseDown(at, MouseButton.Left); window.MouseUp(at, MouseButton.Left); Dispatcher.UIThread.RunJobs();
            // The menu opens with every source (it once stayed shut: filled only as it opened, it was empty).
            Assert.Equal(["All sources", "Tidal"], window.GetVisualDescendants().OfType<MenuItem>().Select(Avalonia.Automation.AutomationProperties.GetName));
        }
        finally { window.Close(); }
    }

    [AvaloniaFact]
    public void Every_source_has_an_icon_and_the_button_shows_the_picked_one()
    {
        foreach (var id in new string?[] { null, "local", "webradio", "tunein", "tidal", "another" })
            Assert.False(new SearchScope(id, "x").IconGeometry.Bounds.Width == 0);
        var model = new SearchViewModel(_ => Task.CompletedTask) { Scopes = [SearchViewModel.AllSources, new("tidal", "Tidal")] };
        var view = new SearchView { DataContext = model };
        var window = new Window { Width = 390, Height = 844, Content = view };
        try
        {
            window.Show(); Dispatcher.UIThread.RunJobs();
            Avalonia.Controls.Shapes.Path Icon() => Assert.Single(view.FindControl<Button>("SourceTab")!.GetVisualDescendants().OfType<Avalonia.Controls.Shapes.Path>());
            Assert.Equal(SearchViewModel.AllSources.IconGeometry.Bounds, Icon().Data!.Bounds);
            var before = Icon().Data;
            model.ChooseScopeCommand.Execute(model.Scopes[1]); Dispatcher.UIThread.RunJobs();
            Assert.NotSame(before, Icon().Data);
            Assert.Equal(new SearchScope("tidal", "Tidal").IconGeometry.Bounds, Icon().Data!.Bounds);
        }
        finally { window.Close(); }
    }
}
