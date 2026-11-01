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
        model.ErrorShown = TimeSpan.FromMilliseconds(50); model.BusyDelay = TimeSpan.Zero;
        var window = new Window { Width = 390, Height = 844, Content = view };
        try
        {
            window.Show(); model.SelectedPage = "Home"; window.UpdateLayout(); Dispatcher.UIThread.RunJobs();
            var home = Assert.Single(view.GetVisualDescendants().OfType<HomeView>());
            var top = home.TranslatePoint(new Point(), view)!.Value.Y;
            // A quick request shows nothing; one taking a moment pulses a dot after the title without moving the page.
            var dot = view.FindControl<Avalonia.Controls.Shapes.Ellipse>("BusyDot")!;
            model.BusyDelay = TimeSpan.FromMilliseconds(40);
            model.IsBusy = true; Dispatcher.UIThread.RunJobs(); Assert.False(dot.IsVisible);
            model.IsBusy = false; await Task.Delay(80); Dispatcher.UIThread.RunJobs(); Assert.False(dot.IsVisible);
            model.IsBusy = true;
            for (var i = 0; i < 100 && !dot.IsVisible; i++) { await Task.Delay(10); Dispatcher.UIThread.RunJobs(); }
            Assert.True(dot.IsVisible);
            model.IsBusy = false; Dispatcher.UIThread.RunJobs(); Assert.False(dot.IsVisible);
            model.BusyDelay = TimeSpan.Zero;
            model.IsBusy = true; window.UpdateLayout(); Dispatcher.UIThread.RunJobs();
            var progress = view.FindControl<Avalonia.Controls.Shapes.Ellipse>("BusyDot")!;
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
            // The shell has no padding; every page keeps 16 px from the window's sides itself (16 from the navigation when shown).
            var pages = view.FindControl<Grid>("Pages")!;
            var left = width < 700 ? 16 : view.FindControl<ListBox>("Navigation")!.TranslatePoint(new Point(view.FindControl<ListBox>("Navigation")!.Bounds.Width + 4 + 16, 0), window)!.Value.X;
            foreach (var page in new[] { "Home", "Queue", "Zones", "Sources", "Playlists", "Rooms", "Diagnostics", "Now Playing" })
            {
                model.SelectedPage = page; window.UpdateLayout(); Dispatcher.UIThread.RunJobs();
                var shown = pages.Children.Single(c => c.IsVisible);
                Assert.Equal(left, shown.TranslatePoint(new Point(), window)!.Value.X, 0.5);
                var side = view.FindControl<Border>("SideQueue")!;
                var right = side.IsVisible ? side.TranslatePoint(new Point(), window)!.Value.X - 20 : window.Bounds.Width - 16;
                Assert.Equal(right, shown.TranslatePoint(new Point(shown.Bounds.Width, 0), window)!.Value.X, 0.5);
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

    [AvaloniaFact]
    public void Library_updates_show_as_a_header_icon_until_the_indexer_is_done()
    {
        var view = new MainView(loadPreferences: false);
        var window = new Window { Width = 390, Height = 844, Content = view };
        try
        {
            window.Show(); Dispatcher.UIThread.RunJobs();
            var model = (ShellViewModel)view.DataContext!; model.SelectedPage = "Home"; var info = view.FindControl<Button>("InfoButton")!;
            // The first status is the starting point; a rescan finding nothing new shows nothing.
            model.LibraryProgress(new(Scanned: 100, Indexed: 40, Failed: 1, QueueDepth: 0, Passes: 3));
            model.LibraryProgress(new(200, 40, 1, 0, 4));
            Assert.False(info.IsVisible);
            // New music: the icon, its text and no button; nothing in the pages.
            model.LibraryProgress(new(210, 45, 1, 0, 4)); window.UpdateLayout();
            Assert.True(info.IsEffectivelyVisible); Assert.Equal(ShellViewModel.LibraryUpdating, model.Info); Assert.Equal("", model.Browser.Notice);
            Assert.IsType<TextBlock>(((Flyout)info.Flyout!).Content);
            // Still scanning, or a pass ended with folders waiting: it stays.
            model.LibraryProgress(new(220, 50, 1, 0, 4)); model.LibraryProgress(new(230, 50, 1, 1, 5));
            Assert.True(model.HasInfo);
            // The pass after that ends with nothing waiting: the icon goes (and Home is read again).
            model.LibraryProgress(new(240, 50, 1, 0, 6));
            Assert.False(model.HasInfo); Assert.False(info.IsVisible);
        }
        finally { window.Close(); }
    }

    [AvaloniaFact]
    public async Task Artist_page_looks_like_an_album_page_with_rows()
    {
        var artist = new BrowseNode("tidal", "artist:5", "Aurora", "artist:5", BrowseKind.Artist, Playable: true);
        var page = new BrowsePage([], BrowserTests.Empty, Node: artist, Sections:
        [
            new("Top tracks", [], BrowserTests.Empty with { Tracks = [new Track("tidal", "track:1", "One", "Aurora")] }),
            new("Albums", [new("tidal", "album:1", "Days", "album:1", BrowseKind.Album, "Aurora", Playable: true)], BrowserTests.Empty),
        ]);
        using var api = new MultiroomClient("http://fake", handler: new FakeHttp((_, _) => Task.FromResult(BrowserTests.Reply(page))));
        var browser = BrowserTests.Browser(api); await browser.OpenNodeAsync(new NodeReference("tidal", "artist:5"), "Aurora");
        var control = new BrowserView { DataContext = browser };
        var window = new Window { Width = 390, Height = 800, Content = control };
        try
        {
            window.Show(); window.UpdateLayout(); Dispatcher.UIThread.RunJobs();
            Assert.True(control.FindControl<ScrollViewer>("ArtistPage")!.IsEffectivelyVisible);
            Assert.False(control.FindControl<ScrollViewer>("AlbumPage")!.IsEffectivelyVisible);
            Assert.False(control.FindControl<ListBox>("MediaList")!.IsEffectivelyVisible);
            var cover = control.FindControl<Grid>("ArtistHeader")!;
            Assert.Equal(control.Bounds.Width, cover.Bounds.Width, 1); Assert.Equal(cover.Bounds.Width, cover.Bounds.Height, 1);
            Assert.Equal(default, control.FindControl<ArtworkView>("ArtistCover")!.CornerRadius);
            Assert.True(control.FindControl<Button>("ArtistPlay")!.IsEffectivelyVisible);
            // The tracks (no node of their own here) come first, then the albums, each as a row.
            var shown = control.GetVisualDescendants().OfType<TextBlock>().Where(t => t.IsEffectivelyVisible && t.Classes.Contains("section")).Select(t => t.Text ?? "").ToArray();
            Assert.Equal(["Top tracks", "Albums"], shown);
            var buttons = control.GetVisualDescendants().OfType<Button>().Where(b => b.IsEffectivelyVisible).ToArray();
            Assert.Contains(buttons, b => Avalonia.Automation.AutomationProperties.GetName(b) == "Days" && b.Command == browser.OpenRowCommand);
            Assert.Contains(buttons, b => Avalonia.Automation.AutomationProperties.GetName(b) == "One" && b.Command == browser.PlayTrackCommand);
        }
        finally { window.Close(); }
    }

    [AvaloniaFact]
    public async Task Album_cover_fills_the_width_with_centred_marks_and_no_back_button()
    {
        var node = new BrowseNode("local", "album:7", "Days", "album:days", BrowseKind.Album, "Aurora", Playable: true);
        using var api = new MultiroomClient("http://fake", handler: new FakeHttp((r, _) => Task.FromResult(r.Method == HttpMethod.Get
            ? BrowserTests.Reply(new BrowsePage([], new([new Track("local", "track:1", "One")], [], [], []), Node: node, Total: 1))
            : BrowserTests.Reply(new[] { new ItemStatus(LikeKind.Album, "local", "album:7", false, false) }))));
        var browser = BrowserTests.Browser(api); await browser.OpenNodeAsync(new NodeReference("local", "album:7"), "Days");
        var control = new BrowserView { DataContext = browser };
        var window = new Window { Width = 390, Height = 800, Content = control };
        try
        {
            window.Show(); window.UpdateLayout(); Dispatcher.UIThread.RunJobs();
            Assert.True(browser.CanMarkAlbum);
            var cover = control.FindControl<Grid>("AlbumHeader")!;
            // Outside the shell's phone layout (here: no shell) it fills the page up to 480, centred.
            Assert.Equal(control.Bounds.Width, cover.Bounds.Width, 1); Assert.Equal(cover.Bounds.Width, cover.Bounds.Height, 1);
            window.Width = 900; window.UpdateLayout(); Dispatcher.UIThread.RunJobs(); window.UpdateLayout();
            Assert.Equal(480, cover.Bounds.Width, 1);
            Assert.Equal((control.Bounds.Width - 480) / 2, cover.TranslatePoint(new Point(), control)!.Value.X, 1);
            window.Width = 390; window.UpdateLayout(); Dispatcher.UIThread.RunJobs(); window.UpdateLayout();
            Assert.DoesNotContain(control.GetVisualDescendants().OfType<Button>(), b => b.IsEffectivelyVisible &&
                (Equals(b.Content, "Back") || Avalonia.Automation.AutomationProperties.GetName(b) == "Back"));
            Assert.Equal(default, control.FindControl<ArtworkView>("AlbumCover")!.CornerRadius); // square, at any width
            // Each visible icon's drawn shape sits in the middle of its button.
            foreach (var name in new[] { "AlbumFavorite", "AlbumPlay", "AlbumLike" })
            {
                var button = control.FindControl<Button>(name)!;
                var path = button.GetVisualDescendants().OfType<Avalonia.Controls.Shapes.Path>().First(p => p.IsEffectivelyVisible);
                var drawn = path.RenderedGeometry!.Bounds.Center;
                var at = path.TranslatePoint(drawn, button)!.Value;
                var tolerance = name == "AlbumPlay" ? 1.1 : 0.5; // the triangle sits 1 px right, to look centred
                Assert.Equal(button.Bounds.Width / 2, at.X, tolerance: tolerance); Assert.Equal(button.Bounds.Height / 2, at.Y, tolerance: 0.5);
            }
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
            // A drawer from the left, over the whole height but not the whole width; no Browse in it.
            Assert.True(model.IsMenuOpen);
            var drawer = view.FindControl<Border>("MenuPanel")!;
            Assert.Equal(280, drawer.Bounds.Width); Assert.Equal(window.Bounds.Height, drawer.Bounds.Height, 1);
            Assert.True(drawer.IsEffectivelyVisible); Assert.Equal(0, drawer.Bounds.X);
            var picker = view.FindControl<ListBox>("PagePicker")!;
            Assert.DoesNotContain("Browse", picker.Items.OfType<string>()); Assert.Contains("Diagnostics", picker.Items.OfType<string>());
            picker.SelectedItem = "Diagnostics"; Dispatcher.UIThread.RunJobs();
            Assert.Equal("Diagnostics", model.SelectedPage); Assert.False(model.IsMenuOpen);
            // The dimmed rest and back close it too; an open album (Browse) leaves the menus unselected.
            model.IsMenuOpen = true; Assert.True(model.GoBack()); Assert.False(model.IsMenuOpen); Assert.Equal("Diagnostics", model.SelectedPage);
            model.SelectedPage = "Browse"; Dispatcher.UIThread.RunJobs();
            Assert.Equal("Browse", model.SelectedPage); Assert.Null(picker.SelectedItem);
            // No Now Playing icon in the top bar.
            Assert.DoesNotContain(view.GetVisualDescendants().OfType<Button>(), b => b.IsEffectivelyVisible
                && Avalonia.Automation.AutomationProperties.GetName(b) == "Now Playing");
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
