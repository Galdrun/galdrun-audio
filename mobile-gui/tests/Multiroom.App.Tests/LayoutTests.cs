using Avalonia;
using Avalonia.Controls;
using Avalonia.Headless;
using Avalonia.Headless.XUnit;
using Avalonia.Input;
using Avalonia.Threading;
using Avalonia.VisualTree;
using Multiroom.App.Api;

[assembly: AvaloniaTestApplication(typeof(Multiroom.App.Tests.TestApp))]

namespace Multiroom.App.Tests;

public static class TestApp
{
    public static AppBuilder BuildAvaloniaApp() => AppBuilder.Configure<App>().UseHeadless(new AvaloniaHeadlessPlatformOptions());
}

public sealed class LayoutTests
{
    [AvaloniaFact]
    public async Task Errors_show_as_a_toast_that_never_moves_the_page_and_hides_by_itself()
    {
        var view = new MainView(loadPreferences: false);
        var model = (ShellViewModel)view.DataContext!;
        model.ErrorShown = TimeSpan.FromMilliseconds(50);
        var window = new Window { Width = 390, Height = 844, Content = view };
        try
        {
            window.Show(); model.SelectedPage = "Home"; window.UpdateLayout(); Dispatcher.UIThread.RunJobs();
            var home = Assert.Single(view.GetVisualDescendants().OfType<HomeView>());
            var top = home.TranslatePoint(new Point(), view)!.Value.Y;
            // A request in flight shows the bar over the page's top edge without moving the page.
            model.IsBusy = true; window.UpdateLayout(); Dispatcher.UIThread.RunJobs();
            var progress = view.FindControl<ProgressBar>("TopProgress")!;
            Assert.True(progress.IsEffectivelyVisible); Assert.False(progress.IsHitTestVisible);
            Assert.Equal(top, home.TranslatePoint(new Point(), view)!.Value.Y);
            model.Error = "The server could not complete the request.";
            window.UpdateLayout(); Dispatcher.UIThread.RunJobs();
            model.IsBusy = false; window.UpdateLayout(); Dispatcher.UIThread.RunJobs();
            var toast = view.FindControl<Border>("ErrorToast")!;
            Assert.True(model.ShowError); Assert.Contains("shown", toast.Classes); Assert.False(toast.IsHitTestVisible);
            Assert.Equal(model.Error, view.FindControl<TextBlock>("ErrorText")!.Text);
            Assert.Equal(top, home.TranslatePoint(new Point(), view)!.Value.Y);
            for (var i = 0; i < 100 && model.ShowError; i++) { await Task.Delay(10); Dispatcher.UIThread.RunJobs(); }
            Assert.False(model.ShowError); Assert.DoesNotContain("shown", toast.Classes);
            Assert.NotEmpty(model.Error);
        }
        finally { window.Close(); }
    }

    [AvaloniaTheory]
    [InlineData(390, 844)] [InlineData(900, 850)] [InlineData(1440, 950)]
    public void Collection_room_and_diagnostics_pages_render_at_all_shell_sizes(int width, int height)
    {
        var view = new MainView(loadPreferences: false);
        var window = new Window { Width = width, Height = height, Content = view };
        try
        {
            window.Show(); Dispatcher.UIThread.RunJobs();
            var model = (ShellViewModel)view.DataContext!;
            model.Playback.Apply(PlaybackTests.Snapshot());
            model.Rooms.Apply(new([], [RoomsTests.Room()]));
            model.Collections.Recommendations = [new(new("fourth", "t:1", "Recommended song"), 1, [new("library", 1, "same artist")])];
            foreach (var page in new[] { "Playlists", "Favourites", "Recommendations", "Rooms", "Diagnostics" })
            {
                model.SelectedPage = page; window.UpdateLayout(); Dispatcher.UIThread.RunJobs();
                var content = view.GetVisualDescendants().OfType<Control>().Where(c => c.IsEffectivelyVisible).ToArray();
                Assert.Contains(content.OfType<TextBlock>(), t => t.Text == page);
                if (page is "Playlists" or "Favourites" or "Recommendations")
                {
                    var collection = Assert.Single(content.OfType<CollectionsView>());
                    Assert.True(collection.Bounds.Height > 200);
                    Assert.Contains(content.OfType<ListBox>(), l => l.Bounds.Height > 100 && l != view.FindControl<ListBox>("Navigation"));
                    Assert.Contains(content.OfType<Button>(), b => Equals(b.Content, "Play now") && b.Command == model.Collections.QueueCommand);
                }
                if (page == "Rooms") Assert.Contains(content.OfType<TextBlock>(), t => t.Text?.Contains("core reason EXACT", StringComparison.Ordinal) == true);
            }
        }
        finally { window.Close(); }
    }

    [AvaloniaFact]
    public async Task Phone_browser_keeps_content_space_with_album_header_and_filter_options()
    {
        using var api = new MultiroomClient("http://fake", handler: new FakeHttp((_, _) => Task.FromResult(BrowserTests.Reply(
            new BrowsePage([new("local", "album:1", "Album")], BrowserTests.Empty,
                Node: new("local", "albums", "Albums", Playable: true), Sorts: ["title"], Filterable: true, Letters: [new("A", 0)])))));
        var browser = BrowserTests.Browser(api); await browser.OpenProviderAsync(BrowserTests.Provider());
        var control = new BrowserView { DataContext = browser };
        // Available content height in the compact shell, after navigation and mini-player.
        var window = new Window { Width = 358, Height = 450, Content = control };
        try
        {
            window.Show(); window.UpdateLayout(); Dispatcher.UIThread.RunJobs();
            Assert.True(browser.IsCompact); Assert.False(browser.OptionsExpanded);
            Assert.True(control.FindControl<ListBox>("MediaList")!.Bounds.Height > 100,
                string.Join("; ", ((Grid)control.Content!).Children.Select(c => $"{c.GetType().Name}: {c.Bounds}, visible={c.IsVisible}")));
        }
        finally { window.Close(); }
    }

    [AvaloniaTheory]
    [InlineData(390, 844, true)] [InlineData(900, 850, false)] [InlineData(1440, 950, false)]
    public void Shell_resizes_navigation_and_keeps_player_visible(int width, int height, bool compact)
    {
        var view = new MainView(loadPreferences: false); var window = new Window { Width = width, Height = height, Content = view };
        try
        {
            window.Show(); Dispatcher.UIThread.RunJobs();
            var model = (ShellViewModel)view.DataContext!; model.Playback.Apply(PlaybackTests.Snapshot());
            model.SelectedPage = "Queue"; window.UpdateLayout(); Dispatcher.UIThread.RunJobs();
            Assert.Equal(compact, model.IsCompact);
            Assert.True(view.FindControl<Button>("HamburgerButton")!.IsVisible);
            Assert.Equal(!compact, view.FindControl<ListBox>("Navigation")!.IsVisible);
            Assert.True(view.FindControl<Grid>("ContentArea")!.Bounds.Height > 200);
            Assert.True(model.Playback.HasZone);
            foreach (var child in view.GetVisualDescendants().OfType<QueueView>().Where(c => c.IsEffectivelyVisible))
                Assert.True(child.Bounds.Height > 150);
            var hamburger = view.FindControl<Button>("HamburgerButton")!;
            var menu = hamburger.TranslatePoint(new Point(24, 24), window)!.Value;
            window.MouseDown(menu, MouseButton.Left); window.MouseUp(menu, MouseButton.Left); Dispatcher.UIThread.RunJobs();
            Assert.True(hamburger.Flyout!.IsOpen);
            var picker = view.FindControl<ListBox>("PagePicker")!;
            picker.SelectedItem = "Diagnostics"; Dispatcher.UIThread.RunJobs();
            Assert.Equal("Diagnostics", model.SelectedPage); Assert.False(hamburger.Flyout.IsOpen);
        }
        finally { window.Close(); }
    }

    [AvaloniaFact]
    public async Task Grid_sections_are_rendered_and_large_pages_are_virtualized()
    {
        using var api = new MultiroomClient("http://fake", handler: new FakeHttp((_, _) => Task.FromResult(BrowserTests.Reply(
            new BrowsePage(Enumerable.Range(0, 1000).Select(i => new BrowseNode("fourth", $"album:{i}", $"Album {i}", Kind: BrowseKind.Album, Playable: true)).ToArray(),
                BrowserTests.Empty, Layout: BrowseLayout.Grid)))));
        var model = BrowserTests.Browser(api); await model.OpenProviderAsync(BrowserTests.Provider());
        var browser = new BrowserView { DataContext = model }; var window = new Window { Width = 900, Height = 800, Content = browser };
        try
        {
            window.Show(); window.UpdateLayout(); Dispatcher.UIThread.RunJobs();
            Assert.True(model.Bands.Count >= 200);
            var list = browser.FindControl<ListBox>("MediaList")!;
            var realized = list.GetVisualDescendants().OfType<ListBoxItem>().Count();
            Assert.InRange(realized, 1, 30);
            Assert.Contains(browser.GetVisualDescendants().OfType<TextBlock>(), t => t.Text == "Album 0");
        }
        finally { window.Close(); }
    }
}
