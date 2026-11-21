using Avalonia;
using Avalonia.Controls;
using Avalonia.Headless;
using Avalonia.Headless.XUnit;
using Avalonia.Input;
using Avalonia.Threading;
using Avalonia.VisualTree;
using Multiroom.App.Api;

namespace Multiroom.App.Tests;

public sealed class ZoneBarTests
{
    [AvaloniaTheory]
    [InlineData(320)] [InlineData(390)] [InlineData(900)] [InlineData(1440)]
    public void Bottom_bar_groups_media_and_zone_beneath_the_title_with_clear_space_before_play(int width)
    {
        var view = new MainView(loadPreferences: false);
        var window = new Window { Width = width, Height = 950, Content = view };
        try
        {
            window.Show(); Dispatcher.UIThread.RunJobs();
            var model = (ShellViewModel)view.DataContext!;
            model.Playback.Apply(PlaybackTests.Snapshot()); model.SelectedPage = "Sources";
            window.UpdateLayout(); Dispatcher.UIThread.RunJobs();
            var bar = view.FindControl<ZoneBar>("BottomZoneBar")!;
            Assert.True(bar.IsVisible);
            var card = bar.FindControl<Border>("ZoneCard")!;
            Assert.Equal(new CornerRadius(0), card.CornerRadius); Assert.Equal(new Thickness(0), card.Margin);
            Assert.Equal(bar.Bounds.Width, card.Bounds.Width);
            Assert.Equal(Avalonia.Media.Color.Parse("#252A2C"), ((Avalonia.Media.ISolidColorBrush)card.Background!).Color);
            Assert.Null(bar.FindControl<ItemsControl>("ZoneDots")); Assert.Null(bar.FindControl<ProgressBar>("TrackProgress"));
            var art = bar.FindControl<ArtworkView>("ZoneArtwork")!;
            var artButton = bar.FindControl<Button>("ArtworkButton")!;
            var media = bar.FindControl<Button>("MediaButton")!;
            var selector = bar.FindControl<TextBlock>("ZoneName")!;
            var play = bar.FindControl<Button>("PlayPauseButton")!;
            var volume = bar.FindControl<Button>("VolumeButton")!;
            Assert.Null(bar.FindControl<Slider>("VolumeSlider"));
            Assert.Equal(new Size(32, 32), volume.Bounds.Size);
            var playCenter = play.TranslatePoint(new Point(play.Bounds.Width / 2, play.Bounds.Height / 2), bar)!.Value;
            var volumeCenter = volume.TranslatePoint(new Point(volume.Bounds.Width / 2, volume.Bounds.Height / 2), bar)!.Value;
            Assert.Equal(playCenter.X, volumeCenter.X, 1);
            var volumeIcon = bar.FindControl<Avalonia.Controls.Shapes.Path>("VolumeIcon")!;
            Assert.Equal(volumeCenter, volumeIcon.TranslatePoint(new Point(volumeIcon.Bounds.Width / 2, volumeIcon.Bounds.Height / 2), bar)!.Value);
            Assert.Equal(new Thickness(0), volume.BorderThickness);
            Assert.Same(model.OpenVolumeCommand, volume.Command);
            var strip = bar.FindControl<Border>("ZoneStrip")!;
            Assert.Equal(Avalonia.Media.Colors.Black, ((Avalonia.Media.ISolidColorBrush)strip.Background!).Color);
            var stripTopLeft = strip.TranslatePoint(new Point(), bar)!.Value;
            Assert.Equal(artButton.Bounds.Width, stripTopLeft.X); Assert.Equal(bar.Bounds.Width, stripTopLeft.X + strip.Bounds.Width);
            Assert.Equal(bar.Bounds.Height, stripTopLeft.Y + strip.Bounds.Height, 0);
            Assert.Contains(volume, strip.GetVisualDescendants()); Assert.Contains(bar.FindControl<TextBlock>("ZoneName")!, strip.GetVisualDescendants());
            Assert.True(play.TranslatePoint(new Point(0, play.Bounds.Height), bar)!.Value.Y <= stripTopLeft.Y);
            Assert.Equal(100, art.Bounds.Height);
            var seek = bar.FindControl<Slider>("ZonePositionSlider")!;
            Assert.True(seek.IsEffectivelyVisible); Assert.Equal(model.Playback.CanScrub, seek.IsEnabled);
            Assert.Equal(model.Playback.ProgressMaximum, seek.Maximum); Assert.Equal(model.Playback.ProgressSeconds, seek.Value);
            Assert.Equal(new Size(bar.Bounds.Width, 14), seek.Bounds.Size);
            var track = seek.GetVisualDescendants().OfType<Border>().First(border => border.Name == "TrackBackground");
            Assert.Equal(0, track.TranslatePoint(new Point(), bar)!.Value.Y, 1); Assert.Equal(3, track.Bounds.Height);
            var thumb = Assert.Single(seek.GetVisualDescendants().OfType<Avalonia.Controls.Primitives.Thumb>());
            Assert.Equal(new Size(14, 14), thumb.Bounds.Size);
            Assert.True(thumb.TranslatePoint(new Point(0, thumb.Bounds.Height), bar)!.Value.Y > art.TranslatePoint(new Point(), bar)!.Value.Y);
            var cardGrid = (Grid)card.Child!; Assert.Same(seek, cardGrid.Children[^1]); // Drawn over the artwork.
            var bubbles = bar.FindControl<Grid>("ScrubBubbles")!; Assert.False(bubbles.IsVisible);
            var snapshot = PlaybackTests.Snapshot();
            model.Playback.Apply(snapshot with { Zones = [snapshot.Zones[0] with { Queue = snapshot.Zones[0].Queue! with
                { Items = [new(1, new Track("local", "track:1", "First", Duration: TimeSpan.FromSeconds(200)))], PositionMs = 65000, Transport = QueueTransport.Paused } }] });
            model.Playback.BeginScrub(); Assert.True(bubbles.IsVisible);
            Assert.Equal("1:05", bar.FindControl<TextBlock>("ScrubPosition")!.Text);
            Assert.Equal("3:20", bar.FindControl<TextBlock>("ScrubLength")!.Text);
            model.Playback.CancelScrub(); Assert.False(bubbles.IsVisible);
            model.Playback.Apply(snapshot);
            // The dot overhangs the bar's top edge; nothing between it and the page may clip it.
            Assert.All(thumb.GetVisualAncestors().TakeWhile(visual => visual != view).Where(visual => visual.Bounds.Height > seek.Bounds.Height), visual => Assert.False(visual.ClipToBounds));
            Assert.Equal(art.Bounds.Height + 3, bar.Bounds.Height, 0);
            Assert.Equal(new Point(0, 3), art.TranslatePoint(new Point(), bar));
            Assert.Equal(width < 520 ? 16 : 18, bar.FindControl<TextBlock>("MediaTitle")!.FontSize);
            Assert.Equal(media.Command, artButton.Command); Assert.Equal(media.CommandParameter, artButton.CommandParameter);
            Assert.Equal(new CornerRadius(0), art.CornerRadius);
            Assert.Equal(new CornerRadius(17), play.CornerRadius);
            Assert.All(new[] { bar.FindControl<TextBlock>("MediaTitle")!, bar.FindControl<TextBlock>("MediaSubtitle")! }, text => Assert.Equal(Avalonia.Media.TextAlignment.Left, text.TextAlignment));
            Assert.Equal(new Size(34, 34), play.Bounds.Size);
            var playIcon = play.GetVisualDescendants().OfType<Viewbox>().Single();
            Assert.Equal(play.TranslatePoint(new Point(17, 17), bar)!.Value, playIcon.TranslatePoint(new Point(playIcon.Bounds.Width / 2, playIcon.Bounds.Height / 2), bar)!.Value);
            var mediaGrid = bar.FindControl<Grid>("MediaGrid")!;
            Assert.Same(mediaGrid, media.Parent); Assert.DoesNotContain(selector, media.GetVisualDescendants());
            var playLeft = play.TranslatePoint(new Point(), bar)!.Value.X;
            Assert.Equal(19, bar.Bounds.Width - playLeft - play.Bounds.Width); // Centred over the volume button.
            foreach (var name in new[] { "MediaTitle", "MediaSubtitle" })
            {
                var text = bar.FindControl<TextBlock>(name)!;
                var right = text.TranslatePoint(new Point(text.Bounds.Width, 0), bar)!.Value.X;
                Assert.True(playLeft - right >= 10);
                Assert.Equal(10, media.Margin.Right);
            }
            var subtitle = bar.FindControl<TextBlock>("MediaSubtitle")!;
            var subtitleBottom = subtitle.TranslatePoint(new Point(0, subtitle.Bounds.Height), bar)!.Value.Y;
            Assert.True(bar.FindControl<TextBlock>("ZoneName")!.TranslatePoint(new Point(), bar)!.Value.Y >= subtitleBottom);
            Assert.True(media.Bounds.Width >= 44); Assert.Equal(2, model.Playback.Zones.Count);
            Assert.Equal(12, bar.FindControl<TextBlock>("ZoneName")!.FontSize);
            Assert.True(bar.FindControl<Avalonia.Controls.Shapes.Path>("ZoneIcon")!.Bounds.Height <= bar.FindControl<TextBlock>("ZoneName")!.Bounds.Height);
            foreach (var control in new Control[] { artButton, media, play, volume, selector })
            {
                var left = control.TranslatePoint(new Point(), bar)!.Value;
                Assert.True(left.X >= 0); Assert.True(left.X + control.Bounds.Width <= bar.Bounds.Width);
            }
            Assert.Equal("living", model.Playback.SelectedZone);
            var bottom = bar.TranslatePoint(new Point(0, bar.Bounds.Height), view)!.Value.Y;
            Assert.Equal(view.Bounds.Height, bottom, 0);
            Assert.Equal(view.Bounds.Width, bar.Bounds.Width, 0);
            model.SelectedPage = "Now Playing"; Assert.True(model.ShowZoneBar);
            window.UpdateLayout(); Assert.False(seek.IsEffectivelyVisible);
            foreach (var page in new[] { "Zones", "Volume", "Servers", "Server" })
            { model.SelectedPage = page; Assert.False(model.ShowZoneBar); }
            model.SelectedPage = "Search"; Assert.True(model.ShowZoneBar);
            volume.Command!.Execute(null); Assert.Equal("Volume", model.SelectedPage);
            model.CloseVolumeCommand.Execute(null); Assert.Equal("Search", model.SelectedPage);
            model.Playback.Apply(new([], [])); Assert.False(model.ShowZoneBar);
        }
        finally { window.Close(); }
    }

    [AvaloniaTheory]
    [InlineData("MediaButton", "Now Playing")] [InlineData("ZoneName", "Zone Setup")]
    public void Media_and_nested_zone_info_swipe_between_zones_and_tap_opens_their_page_for_the_selected_zone(string row, string page)
    {
        var model = new ShellViewModel { SelectedPage = "Home" }; model.Playback.Apply(PlaybackTests.Snapshot());
        var bar = new ZoneBar { DataContext = model };
        var window = new Window { Width = 390, Height = 140, Content = bar };
        try
        {
            window.Show(); window.UpdateLayout(); Dispatcher.UIThread.RunJobs();
            var selector = bar.FindControl<Control>(row)!;
            var center = selector.TranslatePoint(new Point(selector.Bounds.Width / 2, selector.Bounds.Height / 2), window)!.Value;
            void Swipe(double x, double y = 0)
            {
                using var touch = window.TouchBegin(center);
                window.TouchMove(touch, center + new Vector(x, y));
                window.TouchEnd(touch, center + new Vector(x, y));
                Settle(bar);
            }
            Swipe(-55); Assert.Equal("kitchen", model.Playback.SelectedZone); Assert.Equal("Live artist", model.Playback.Subtitle);
            Assert.Equal("Home", model.SelectedPage);
            Swipe(-55); Assert.Equal("living", model.Playback.SelectedZone); // Wrap past the last zone.
            Swipe(55); Assert.Equal("kitchen", model.Playback.SelectedZone); // Wrap before the first zone.
            Swipe(55); Assert.Equal("living", model.Playback.SelectedZone); Assert.Equal("Home", model.SelectedPage);
            Swipe(0, 45); Assert.Equal("living", model.Playback.SelectedZone); Assert.Equal("Home", model.SelectedPage);
            Swipe(20); Assert.Equal("living", model.Playback.SelectedZone); Assert.Equal("Home", model.SelectedPage);
            Swipe(55, 45); Assert.Equal("living", model.Playback.SelectedZone); Assert.Equal("Home", model.SelectedPage);
            window.MouseDown(center, MouseButton.Left);
            window.MouseMove(center + new Vector(-55, 0)); window.MouseUp(center + new Vector(-55, 0), MouseButton.Left);
            window.MouseMove(new Point(5, 135)); // Off the bar, so no tooltip opens over it while the slide finishes.
            Settle(bar); Assert.Equal("kitchen", model.Playback.SelectedZone); Assert.Equal("Home", model.SelectedPage);
            bar.FindControl<Button>("MediaButton")!.Focus(); window.KeyPress(Key.Left, RawInputModifiers.None, PhysicalKey.ArrowLeft, null);
            window.KeyRelease(Key.Left, RawInputModifiers.None, PhysicalKey.ArrowLeft, null);
            Assert.Equal("living", model.Playback.SelectedZone);
            Swipe(-55);
            using (var tap = window.TouchBegin(center)) window.TouchEnd(tap, center + new Vector(3, 0));
            Dispatcher.UIThread.RunJobs();
            Assert.Equal(page, model.SelectedPage); Assert.Equal("kitchen", model.Playback.SelectedZone);
            Assert.Equal("Radio", model.Playback.Title);
            model.SelectedPage = "Home";
            window.MouseDown(center, MouseButton.Left); window.MouseUp(center, MouseButton.Left);
            Dispatcher.UIThread.RunJobs(); Assert.Equal(page, model.SelectedPage);
        }
        finally { window.Close(); }
    }

    [AvaloniaFact]
    public void Dragging_the_media_shows_the_neighbouring_zone_beside_it_and_a_swipe_slides_it_in()
    {
        var model = new ShellViewModel { SelectedPage = "Home" }; model.Playback.Apply(PlaybackTests.Snapshot());
        var bar = new ZoneBar { DataContext = model };
        var window = new Window { Width = 390, Height = 140, Content = bar };
        try
        {
            window.Show(); window.UpdateLayout(); Dispatcher.UIThread.RunJobs();
            var media = bar.FindControl<Button>("MediaButton")!;
            var text = (Avalonia.Media.TranslateTransform)bar.FindControl<StackPanel>("MediaText")!.RenderTransform!;
            var peek = bar.FindControl<StackPanel>("MediaPeek")!;
            var living = model.Playback.Title;
            var center = media.TranslatePoint(new Point(media.Bounds.Width / 2, media.Bounds.Height / 2), window)!.Value;
            using (var touch = window.TouchBegin(center))
            {
                window.TouchMove(touch, center + new Vector(-30, 0));
                Assert.True(peek.IsVisible); Assert.Equal(-30, text.X);
                Assert.Equal("Radio", bar.FindControl<TextBlock>("PeekTitle")!.Text);
                Assert.Equal("kitchen", bar.FindControl<TextBlock>("PeekZoneName")!.Text);
                window.TouchEnd(touch, center + new Vector(-30, 0)); // Too short: stays.
            }
            Dispatcher.UIThread.RunJobs(); Assert.Equal("living", model.Playback.SelectedZone); Assert.Equal("Home", model.SelectedPage);
            using (var touch = window.TouchBegin(center))
            {
                window.TouchMove(touch, center + new Vector(-55, 0));
                window.TouchEnd(touch, center + new Vector(-55, 0));
            }
            Dispatcher.UIThread.RunJobs();
            // The zone changes only once the neighbour has slid in, so the change cannot stall the slide.
            Assert.Equal("living", model.Playback.SelectedZone); Assert.True(peek.IsVisible);
            Settle(bar);
            Assert.Equal("kitchen", model.Playback.SelectedZone); Assert.Equal("Radio", bar.FindControl<TextBlock>("MediaTitle")!.Text);
            Assert.False(peek.IsVisible); Assert.Equal(0, text.X); Assert.NotEqual(living, model.Playback.Title);
            Assert.Equal("Home", model.SelectedPage);
        }
        finally { window.Close(); }
    }

    [AvaloniaTheory]
    [InlineData("ArtworkButton")] [InlineData("MediaButton")] [InlineData("ZoneButton")]
    public void Swiping_the_bar_up_opens_now_playing_rising_from_the_bottom(string part)
    {
        var view = new MainView(loadPreferences: false);
        var model = (ShellViewModel)view.DataContext!; model.Playback.Apply(PlaybackTests.Snapshot());
        var window = new Window { Width = 390, Height = 844, Content = view };
        try
        {
            window.Show(); model.SelectedPage = "Home"; window.UpdateLayout(); Dispatcher.UIThread.RunJobs();
            var bar = view.FindControl<ZoneBar>("BottomZoneBar")!;
            var start = bar.FindControl<Button>(part)!;
            var at = start.TranslatePoint(new Point(start.Bounds.Width / 2, start.Bounds.Height / 2), window)!.Value;
            using (var touch = window.TouchBegin(at))
            {
                window.TouchMove(touch, at + new Vector(5, -60));
                window.TouchEnd(touch, at + new Vector(5, -60));
            }
            Dispatcher.UIThread.RunJobs();
            Assert.Equal("Now Playing", model.SelectedPage); Assert.Equal("living", model.Playback.SelectedZone);
            var page = view.FindControl<Grid>("NowPlayingPage")!;
            var rise = Assert.IsType<Avalonia.Media.TranslateTransform>(page.RenderTransform);
            Assert.True(rise.Y > 0); // Starts below and rises to its place.
            for (var i = 0; i < 100 && rise.Y != 0; i++) { Thread.Sleep(10); Dispatcher.UIThread.RunJobs(); }
            Assert.Equal(0, rise.Y);
        }
        finally { window.Close(); }
    }

    [AvaloniaFact]
    public void Swiping_now_playing_down_closes_it_sliding_down_and_any_close_slides()
    {
        var view = new MainView(loadPreferences: false);
        var model = (ShellViewModel)view.DataContext!; model.Playback.Apply(PlaybackTests.Snapshot());
        var window = new Window { Width = 390, Height = 844, Content = view };
        try
        {
            window.Show(); model.SelectedPage = "Home"; model.SelectedPage = "Now Playing";
            window.UpdateLayout(); Dispatcher.UIThread.RunJobs();
            var page = view.FindControl<Grid>("NowPlayingPage")!;
            Assert.True(page.IsVisible);
            var cover = page.GetVisualDescendants().OfType<Button>().First(b => b.Name == "CoverButton");
            var at = cover.TranslatePoint(new Point(cover.Bounds.Width / 2, cover.Bounds.Height / 2), window)!.Value;
            using (var touch = window.TouchBegin(at))
            {
                window.TouchMove(touch, at + new Vector(-5, 80));
                window.TouchEnd(touch, at + new Vector(-5, 80));
            }
            Dispatcher.UIThread.RunJobs();
            Assert.Equal("Home", model.SelectedPage); Assert.Equal("living", model.Playback.SelectedZone);
            // Still shown while it slides down over Home, then hidden.
            Assert.True(page.IsVisible); Assert.False(page.IsHitTestVisible);
            Assert.True(((Avalonia.Media.TranslateTransform)page.RenderTransform!).Y < view.Bounds.Height);
            for (var i = 0; i < 100 && page.IsVisible; i++) { Thread.Sleep(10); Dispatcher.UIThread.RunJobs(); }
            Assert.False(page.IsVisible);
            // Leaving it any other way slides it down too.
            model.SelectedPage = "Now Playing"; Dispatcher.UIThread.RunJobs(); Assert.True(page.IsVisible);
            Assert.True(model.GoBack()); Dispatcher.UIThread.RunJobs();
            Assert.True(page.IsVisible); Assert.IsType<Avalonia.Media.TranslateTransform>(page.RenderTransform);
            // A short or sideways drag does not close it.
            model.SelectedPage = "Now Playing"; Dispatcher.UIThread.RunJobs(); window.UpdateLayout();
            using (var touch = window.TouchBegin(at)) { window.TouchMove(touch, at + new Vector(0, 30)); window.TouchEnd(touch, at + new Vector(0, 30)); }
            Dispatcher.UIThread.RunJobs(); Assert.Equal("Now Playing", model.SelectedPage);
        }
        finally { window.Close(); }
    }

    [AvaloniaTheory]
    [InlineData("TrackTitle", 80)] [InlineData("TrackSubtitle", 80)] [InlineData("TrackSubtitle", 250)] [InlineData("PositionLabel", 120)]
    public void Swiping_down_over_the_track_info_closes_now_playing(string part, int distance)
    {
        var view = new MainView(loadPreferences: false);
        var model = (ShellViewModel)view.DataContext!; model.Playback.Apply(PlaybackTests.Snapshot());
        var window = new Window { Width = 390, Height = 844, Content = view };
        try
        {
            window.Show(); model.SelectedPage = "Home"; model.SelectedPage = "Now Playing";
            window.UpdateLayout(); Dispatcher.UIThread.RunJobs();
            var start = view.GetVisualDescendants().OfType<Control>().First(c => c.Name == part);
            // Its left edge: beside a centred text there is only the page itself.
            var at = start.TranslatePoint(new Point(4, start.Bounds.Height / 2), window)!.Value;
            using (var touch = window.TouchBegin(at))
            {
                window.TouchMove(touch, at + new Vector(0, distance)); // May end past the page, on the zone bar.
                window.TouchEnd(touch, at + new Vector(0, distance));
            }
            Dispatcher.UIThread.RunJobs();
            Assert.Equal("Home", model.SelectedPage);
        }
        finally { window.Close(); }
    }

    [AvaloniaTheory]
    [InlineData("TrackTitle")] [InlineData("TrackSubtitle")]
    public void Swiping_the_track_info_on_now_playing_changes_zone_as_the_cover_does(string part)
    {
        var view = new MainView(loadPreferences: false);
        var model = (ShellViewModel)view.DataContext!; model.Playback.Apply(PlaybackTests.Snapshot());
        var window = new Window { Width = 390, Height = 844, Content = view };
        try
        {
            window.Show(); model.SelectedPage = "Now Playing"; window.UpdateLayout(); Dispatcher.UIThread.RunJobs();
            var text = view.GetVisualDescendants().OfType<Control>().First(c => c.Name == part);
            var at = text.TranslatePoint(new Point(text.Bounds.Width / 2, text.Bounds.Height / 2), window)!.Value;
            void Swipe(double x)
            {
                using var touch = window.TouchBegin(at);
                window.TouchMove(touch, at + new Vector(x, 4)); window.TouchEnd(touch, at + new Vector(x, 4));
                Dispatcher.UIThread.RunJobs();
            }
            Swipe(-150); Assert.Equal("kitchen", model.Playback.SelectedZone); // Ends past the text: still a swipe.
            Swipe(60); Assert.Equal("living", model.Playback.SelectedZone);
            Assert.Equal("Now Playing", model.SelectedPage);
        }
        finally { window.Close(); }
    }

    [AvaloniaFact]
    public void For_you_see_all_has_flat_kind_tabs_over_its_covers()
    {
        var shelf = new ShellViewModel().Home.ForYou;
        Track T(int i) => new("local", $"t{i}", $"Song {i}");
        shelf.Set([HomeItem.Of(T(1)), HomeItem.Of(new Album("local", "a1", "Record")), HomeItem.Of(T(2)), HomeItem.Of(new Artist("local", "r1", "Singer"))]);
        Assert.Equal(["All", "Tracks", "Albums", "Artists"], shelf.Tabs.Select(t => t.Label));
        Assert.Equal(["", "2", "1", "1"], shelf.Tabs.Select(t => t.Count));
        Assert.True(shelf.Tabs[1].IsSelected); Assert.Equal(["Song 1", "Song 2"], shelf.Shown.Select(i => i.Title)); // Tracks first.
        shelf.ShowKindCommand.Execute("Album"); Assert.Equal(["Record"], shelf.Shown.Select(i => i.Title));
        shelf.ShowKindCommand.Execute(null); Assert.Equal(4, shelf.Shown.Count); Assert.True(shelf.Tabs[0].IsSelected);
        shelf.Set([HomeItem.Of(T(1))]); Assert.False(shelf.HasTabs); Assert.Single(shelf.Shown); // One kind: no tabs.
        var favourites = new ShellViewModel().Home.Favourites;
        favourites.Set([HomeItem.Of(T(1)), HomeItem.Of(new Album("local", "a1", "Record"))]); Assert.False(favourites.HasTabs); Assert.Equal(2, favourites.Shown.Count);

        var view = new MainView(loadPreferences: false); var model = (ShellViewModel)view.DataContext!;
        model.Home.ForYou.Set([HomeItem.Of(T(1)), HomeItem.Of(new Album("local", "a1", "Record")), HomeItem.Of(T(2))]);
        var window = new Window { Width = 390, Height = 844, Content = view };
        try
        {
            window.Show(); model.SelectedPage = "Home"; model.Home.OpenShelf = model.Home.ForYou; window.UpdateLayout(); Dispatcher.UIThread.RunJobs();
            var tabs = view.GetVisualDescendants().OfType<ItemsControl>().First(c => c.Name == "ShelfTabs");
            var grid = view.GetVisualDescendants().OfType<RecyclingRowPanel>().First(c => c.Name == "ShelfItems");
            Assert.True(tabs.IsEffectivelyVisible);
            int Covers() { window.UpdateLayout(); return grid.GetVisualDescendants().OfType<ArtworkView>().Count(a => a.IsEffectivelyVisible); }
            Assert.True(tabs.TranslatePoint(new Point(0, tabs.Bounds.Height), view)!.Value.Y <= grid.TranslatePoint(new Point(), view)!.Value.Y);
            var buttons = tabs.GetVisualDescendants().OfType<Button>().ToArray();
            Assert.Equal(3, buttons.Length); Assert.All(buttons, b => Assert.Equal(new CornerRadius(0), b.CornerRadius));
            // Full width, equal tabs, divided by vertical lines (none before the first); the picked one is filled, not underlined.
            Assert.Equal(grid.Bounds.Width - 14, tabs.Bounds.Width, 1.0); Assert.Equal(buttons[0].Bounds.Width, buttons[2].Bounds.Width, 1.0);
            Assert.Equal(new Thickness(0), buttons[0].BorderThickness); Assert.Equal(new Thickness(1, 0, 0, 0), buttons[1].BorderThickness);
            Assert.Contains("selected", buttons[1].Classes);
            Assert.Equal(Avalonia.Media.Color.Parse("#2C372F"), ((Avalonia.Media.ISolidColorBrush)buttons[1].Background!).Color);
            Assert.Equal(Avalonia.Media.Colors.Transparent, ((Avalonia.Media.ISolidColorBrush)buttons[0].Background!).Color);
            Assert.Equal(2, Covers());
            var albums = buttons[2]; var at = albums.TranslatePoint(new Point(albums.Bounds.Width / 2, albums.Bounds.Height / 2), window)!.Value;
            window.MouseDown(at, MouseButton.Left); window.MouseUp(at, MouseButton.Left); Dispatcher.UIThread.RunJobs();
            Assert.Equal(1, Covers());
            // Virtualized by rows: a long shelf builds only the covers on screen.
            model.Home.ForYou.Set(Enumerable.Range(1, 90).Select(i => HomeItem.Of(T(i))).Append(HomeItem.Of(new Album("local", "a1", "Record"))));
            model.Home.ForYou.ShowKindCommand.Execute("Track");
            Assert.InRange(Covers(), 3, 45);
            // Rows are a cover high once the columns are known (not the one-column height measured first), with no gaps.
            var rows = grid.Children.Where(r => r.IsVisible).OrderBy(r => r.Bounds.Y).Take(3).ToArray();
            Assert.InRange(rows[1].Bounds.Y - rows[0].Bounds.Y, 100, 170); Assert.Equal(rows[1].Bounds.Y - rows[0].Bounds.Y, rows[2].Bounds.Y - rows[1].Bounds.Y, 1.0);
            // The refresh sits on the title's line, at the right edge.
            var refresh = view.GetVisualDescendants().OfType<Button>().First(b => b.Name == "RefreshProposals");
            Assert.True(refresh.IsEffectivelyVisible);
            Assert.True(refresh.TranslatePoint(new Point(refresh.Bounds.Width, 0), view)!.Value.X > 390 - 30);
        }
        finally { window.Close(); }
    }

    private static void Settle(ZoneBar bar)
    {
        Dispatcher.UIThread.RunJobs();
        for (var i = 0; i < 100 && bar.FindControl<StackPanel>("MediaPeek")!.IsVisible; i++) { Thread.Sleep(10); Dispatcher.UIThread.RunJobs(); }
    }

    [AvaloniaTheory]
    [InlineData(24)] [InlineData(76)]
    public void Both_left_rows_open_now_playing_for_the_selected_zone(int y)
    {
        var model = new ShellViewModel { SelectedPage = "Home" }; model.Playback.Apply(PlaybackTests.Snapshot());
        model.Playback.SelectedZone = "kitchen";
        var bar = new ZoneBar { DataContext = model };
        var window = new Window { Width = 390, Height = 140, Content = bar };
        try
        {
            window.Show(); window.UpdateLayout(); Dispatcher.UIThread.RunJobs();
            var artwork = bar.FindControl<Button>("ArtworkButton")!;
            var position = artwork.TranslatePoint(new Point(50, y), window)!.Value;
            using (var tap = window.TouchBegin(position)) window.TouchEnd(tap, position);
            Dispatcher.UIThread.RunJobs();
            Assert.Equal("Now Playing", model.SelectedPage); Assert.Equal("kitchen", model.Playback.SelectedZone);
            model.SelectedPage = "Home";
            window.MouseDown(position, MouseButton.Left); window.MouseUp(position, MouseButton.Left);
            Dispatcher.UIThread.RunJobs(); Assert.Equal("Now Playing", model.SelectedPage);
        }
        finally { window.Close(); }
    }

    [Fact]
    public async Task Play_pause_uses_authoritative_state_and_swapping_zones_never_controls_transport()
    {
        var paths = new List<string>();
        using var api = new MultiroomClient("http://fake", handler: new FakeHttp((r, _) =>
        { paths.Add($"{r.Method} {r.RequestUri!.AbsolutePath}"); return Task.FromResult(FakeHttp.Json("{}")); }));
        var vm = new PlaybackViewModel(a => a(api, TestContext.Current.CancellationToken));
        var snapshot = PlaybackTests.Snapshot() with { Rooms = [new("speaker", new(Zone: "kitchen"), new("speaker", Connected: true, Member: true))] }; vm.Apply(snapshot);
        Assert.True(vm.IsPlaying); Assert.Equal("Pause", vm.PlayPauseLabel);
        await vm.PlayPauseCommand.ExecuteAsync(null); Assert.Equal("POST /api/v1/zones/living/pause", Assert.Single(paths));
        snapshot = snapshot with { Zones = snapshot.Zones.Select(z => z with { Queue = z.Queue! with { Transport = QueueTransport.Paused } }).ToArray() };
        vm.Apply(snapshot); Assert.Equal("Play", vm.PlayPauseLabel);
        vm.ChangeZoneCommand.Execute("next"); Assert.Single(paths); Assert.Equal("kitchen", vm.SelectedZone);
        await vm.PlayPauseCommand.ExecuteAsync(null); Assert.Equal("POST /api/v1/zones/kitchen/play", paths[1]);
        vm.Apply(snapshot with { Zones = [snapshot.Zones[0]] });
        Assert.Equal("living", vm.SelectedZone); Assert.Equal("living", Assert.Single(vm.Zones));
        vm.Apply(new([], [])); vm.ChangeZoneCommand.Execute("next"); await vm.PlayPauseCommand.ExecuteAsync(null);
        Assert.Empty(vm.Zones); Assert.Equal(2, paths.Count);
    }

    [Fact]
    public async Task Play_with_every_device_off_fails_without_a_request()
    {
        var paths = new List<string>();
        using var api = new MultiroomClient("http://fake", handler: new FakeHttp((r, _) =>
        { paths.Add($"{r.Method} {r.RequestUri!.AbsolutePath}"); return Task.FromResult(FakeHttp.Json("{}")); }));
        var vm = new PlaybackViewModel(a => a(api, TestContext.Current.CancellationToken));
        var snapshot = PlaybackTests.Snapshot();
        vm.Apply(snapshot with { Zones = [snapshot.Zones[0] with { Queue = snapshot.Zones[0].Queue! with { Transport = QueueTransport.Paused } }],
            Rooms = [new("speaker", new(Zone: "living"), new("speaker", Connected: false, Member: false))] });
        var error = await Assert.ThrowsAsync<ApiException>(() => vm.PlayPauseCommand.ExecuteAsync(null));
        Assert.Equal("No device in living is connected. Turn a device on and try again.", error.Message); Assert.Empty(paths);
        // Woken by its POWER button only: still off as far as play goes.
        vm.Apply(snapshot with { Zones = [snapshot.Zones[0] with { Queue = snapshot.Zones[0].Queue! with { Transport = QueueTransport.Paused } }],
            Rooms = [new("speaker", new(Zone: "living"), new("speaker", Connected: false, Member: false, PowerState: "sleepButtonOnly"))] });
        await Assert.ThrowsAsync<ApiException>(() => vm.PlayPauseCommand.ExecuteAsync(null)); Assert.Empty(paths);
        // In deep sleep with Wake-on-LAN: the core wakes it, so play is asked without a warning.
        vm.Apply(snapshot with { Zones = [snapshot.Zones[0] with { Queue = snapshot.Zones[0].Queue! with { Transport = QueueTransport.Paused } }],
            Rooms = [new("speaker", new(Zone: "living"), new("speaker", Connected: false, Member: false, PowerState: "sleep"))] });
        await vm.PlayPauseCommand.ExecuteAsync(null);
        Assert.Equal(["POST /api/v1/zones/living/play"], paths);
    }

    [Fact]
    public async Task Zone_volume_sends_the_dials_newest_value_one_request_at_a_time_and_mutes()
    {
        var bodies = new List<string>();
        var first = new TaskCompletionSource();
        var snapshot = PlaybackTests.Snapshot();
        var desired = new ZoneDesired([], 0.6, false);
        snapshot = snapshot with { Zones = snapshot.Zones.Select(z => z with { Desired = desired }).ToArray() };
        using var api = new MultiroomClient("http://fake", handler: new FakeHttp(async (r, ct) =>
        {
            ct.ThrowIfCancellationRequested(); Assert.Equal(HttpMethod.Patch, r.Method);
            Assert.Equal("/api/v1/zones/living", r.RequestUri!.AbsolutePath);
            var body = await r.Content!.ReadAsStringAsync(ct); bodies.Add(body);
            if (bodies.Count == 1) await first.Task;
            using var json = System.Text.Json.JsonDocument.Parse(body);
            return BrowserTests.Reply(snapshot.Zones[0] with { Desired = json.RootElement.TryGetProperty("volume", out var v)
                ? desired with { Volume = v.GetDouble() } : desired with { Muted = true } });
        }));
        var vm = new PlaybackViewModel(a => a(api, TestContext.Current.CancellationToken)); vm.Apply(snapshot);
        Assert.Equal(60, vm.ZoneVolume);
        // Turned on while the first value is still on its way: only the newest follows, and no snapshot moves it back.
        vm.ZoneVolume = 35; vm.ZoneVolume = 40; vm.ZoneVolume = 45;
        vm.Apply(snapshot); Assert.Equal(45, vm.ZoneVolume);
        first.SetResult();
        for (var i = 0; i < 200 && vm.ZoneSettings!.Volume != 0.45; i++) await Task.Delay(10, TestContext.Current.CancellationToken);
        Assert.Equal(new[] { "{\"volume\":0.35}", "{\"volume\":0.45}" }, bodies);
        Assert.Equal(45, vm.ZoneVolume);
        await vm.MuteZoneCommand.ExecuteAsync(null); Assert.True(vm.ZoneMuted); Assert.Equal("Unmute zone", vm.ZoneMuteLabel);
        Assert.Equal("{\"muted\":true}", bodies[^1]);
        vm.ChangeZoneCommand.Execute("next"); Assert.Equal(60, vm.ZoneVolume); Assert.Equal(3, bodies.Count);
    }

    [AvaloniaFact]
    public void Volume_page_puts_rooms_above_the_dial_which_swipes_up_or_right_louder()
    {
        // The server takes what is sent.
        using var api = new MultiroomClient("http://fake", handler: new FakeHttp(async (r, ct) =>
        {
            using var json = System.Text.Json.JsonDocument.Parse(await r.Content!.ReadAsStringAsync(ct));
            return BrowserTests.Reply(new ZoneView("living", Desired: new ZoneDesired([], json.RootElement.GetProperty("volume").GetDouble(), false)));
        }));
        var vm = new PlaybackViewModel(a => a(api, CancellationToken.None));
        var snapshot = PlaybackTests.Snapshot();
        vm.Apply(snapshot with { Zones = snapshot.Zones.Select(z => z with { Desired = new ZoneDesired([], 0.5, false) }).ToArray(),
            Rooms = [new("a", new(Zone: "living", Volume: 0.8)), new("b", new(Zone: "living", Volume: 0.4))] });
        var view = new VolumeView { DataContext = vm };
        var window = new Window { Width = 390, Height = 800, Content = view };
        try
        {
            window.Show(); Dispatcher.UIThread.RunJobs(); window.UpdateLayout();
            var dial = view.FindControl<VolumeDial>("ZoneDial")!;
            var list = view.FindControl<ItemsControl>("RoomVolumeList")!;
            Assert.Equal(2, list.GetVisualDescendants().OfType<Slider>().Count());
            Assert.True(list.TranslatePoint(new Point(), view)!.Value.Y < dial.TranslatePoint(new Point(), view)!.Value.Y);
            Assert.Equal("50%", view.FindControl<TextBlock>("ZoneVolumeText")!.Text);
            var center = dial.TranslatePoint(new Point(dial.Bounds.Width / 2, dial.Bounds.Height / 2), window)!.Value;
            var span = VolumeDial.SwipeSpan(dial.Bounds.Width);
            window.MouseDown(center, MouseButton.Left);
            window.MouseMove(center + new Point(0, -span / 10)); // up: louder
            Assert.Equal(60, vm.ZoneVolume);
            window.MouseMove(center + new Point(-span / 5, -span / 10)); // then left: quieter
            Assert.Equal(40, vm.ZoneVolume);
            window.MouseUp(center, MouseButton.Left); Dispatcher.UIThread.RunJobs();
            dial.Focus(); window.KeyPress(Key.Up, RawInputModifiers.None, PhysicalKey.ArrowUp, null);
            Assert.Equal(41, vm.ZoneVolume);
            // The knob turns around the ring: straight up is the middle, right is five sixths, the bottom gap holds the end.
            var radius = Math.Min(dial.Bounds.Width, dial.Bounds.Height) / 2 - 16;
            var knobAngle = (135 + 270 * 0.41) * Math.PI / 180;
            window.MouseDown(center + new Point(radius * Math.Cos(knobAngle), radius * Math.Sin(knobAngle)), MouseButton.Left);
            window.MouseMove(center + new Point(0, -radius));
            Assert.Equal(50, vm.ZoneVolume);
            window.MouseMove(center + new Point(radius, 0));
            Assert.Equal(83, vm.ZoneVolume);
            window.MouseMove(center + new Point(0, radius));
            Assert.Equal(100, vm.ZoneVolume);
            window.MouseUp(center, MouseButton.Left);
        }
        finally { window.Close(); }
    }

    [AvaloniaFact]
    public async Task Zone_setup_rises_on_a_swipe_up_over_now_playing_and_closes_on_a_swipe_down_or_an_update()
    {
        using var events = new FixtureEventStream();
        var saved = 0;
        var model = new ShellViewModel(preferencesPath: Path.Combine(Path.GetTempPath(), "galdrun-zone-" + Guid.NewGuid().ToString("N") + ".json"),
            clientFactory: (address, token) => new MultiroomClient(address, token, new FakeHttp((request, _) => Task.FromResult(request.RequestUri!.AbsolutePath switch
            {
                "/api/v1/status" => FakeHttp.Json("""{"version":"0.1","core":{"connected":true,"connections":1,"zones":0,"rooms":0,"queuedEvents":0,"droppedEvents":0},"media":{},"library":{},"providers":[],"addIns":{},"events":{"subscribers":0,"held":0,"dropped":0}}"""),
                "/api/v1/events" => new HttpResponseMessage(System.Net.HttpStatusCode.OK) { Content = new StreamContent(events) { Headers = { ContentType = new("text/event-stream") } } },
                "/api/v1/zones/living/clients" => Saved(),
                _ => FakeHttp.Json("[]"),
            }))));
        HttpResponseMessage Saved() { saved++; return FakeHttp.Json("{}"); }
        var view = new MainView(model, loadPreferences: false);
        var window = new Window { Width = 390, Height = 844, Content = view };
        try
        {
            await model.ConnectCommand.ExecuteAsync(null);
            for (var i = 0; i < 20; i++) { await Task.Delay(10); Dispatcher.UIThread.RunJobs(); } // The session's first reads.
            model.Playback.Apply(PlaybackTests.Snapshot());
            window.Show(); model.SelectedPage = "Now Playing"; window.UpdateLayout(); Dispatcher.UIThread.RunJobs();
            Assert.True(model.ShowZoneBar);
            var nowPlaying = view.FindControl<Grid>("NowPlayingPage")!; var setup = view.FindControl<Grid>("ZoneSetupPage")!;
            Assert.False(setup.IsVisible);
            double Rise() => (setup.RenderTransform as Avalonia.Media.TranslateTransform)?.Y ?? 0;
            void Settle(Func<bool> done) { for (var i = 0; i < 200 && !done(); i++) { Thread.Sleep(10); Avalonia.Headless.AvaloniaHeadlessPlatform.ForceRenderTimerTick(); Dispatcher.UIThread.RunJobs(); } window.UpdateLayout(); }
            void Swipe(Control on, double dy)
            {
                var at = on.TranslatePoint(new Point(on.Bounds.Width / 2, Math.Min(on.Bounds.Height / 2, 200)), window)!.Value;
                using var touch = window.TouchBegin(at);
                window.TouchMove(touch, at + new Vector(4, dy)); window.TouchEnd(touch, at + new Vector(4, dy));
                Dispatcher.UIThread.RunJobs();
            }
            // A swipe up on Now Playing's zone selector: Zone Setup rises over Now Playing, which stays until it has.
            var cover = view.GetVisualDescendants().OfType<Control>().First(c => c.Name == "CoverSpace");
            var coverAt = cover.TranslatePoint(new Point(), view)!.Value;
            Swipe(view.GetVisualDescendants().OfType<Button>().First(b => b.Name == "NowPlayingZoneSelector"), -80);
            Assert.Equal("Zone Setup", model.SelectedPage);
            Assert.True(setup.IsVisible); Assert.True(nowPlaying.IsVisible); Assert.True(Rise() > 0);
            // Now Playing keeps its place beneath, and Zone Setup is as wide as the window, over the zone bar too.
            window.UpdateLayout();
            Assert.Equal(coverAt, cover.TranslatePoint(new Point(), view)!.Value);
            Assert.Equal(window.Bounds.Width, setup.Bounds.Width); Assert.Equal(0, setup.Bounds.X);
            var bar = view.FindControl<ZoneBar>("BottomZoneBar")!;
            Assert.Equal(bar.Bounds.Bottom, setup.Bounds.Bottom, 0.5);
            Settle(() => Rise() == 0 && !nowPlaying.IsVisible);
            Assert.Equal(0, Rise()); Assert.False(nowPlaying.IsVisible);
            // A swipe down closes it back to Now Playing, which shows beneath at once while Zone Setup slides away.
            Swipe(setup, 120);
            Assert.Equal("Now Playing", model.SelectedPage); Assert.True(nowPlaying.IsVisible); Assert.True(setup.IsVisible);
            Settle(() => !setup.IsVisible); Assert.False(setup.IsVisible);
            // Update zone saves and closes it.
            model.NavigateCommand.Execute("Zone Setup"); Settle(() => Rise() == 0);
            var update = view.GetVisualDescendants().OfType<Button>().First(b => b.Name == "UpdateZoneButton");
            Assert.Equal(new CornerRadius(0), update.CornerRadius);
            await model.Playback.SaveMembershipCommand.ExecuteAsync(null);
            Assert.Equal(1, saved); Assert.Equal("Now Playing", model.SelectedPage);
        }
        finally { window.Close(); await model.DisconnectAsync(); }
    }

    [AvaloniaFact]
    public void Zone_setup_opens_from_the_zone_name_and_sends_the_ticked_rooms()
    {
        var sent = new List<string>();
        using var api = new MultiroomClient("http://fake", handler: new FakeHttp(async (r, ct) =>
        { sent.Add($"{r.Method} {r.RequestUri!.AbsolutePath} {await r.Content!.ReadAsStringAsync(ct)}"); return FakeHttp.Json("{}"); }));
        var snapshot = PlaybackTests.Snapshot() with { Rooms = [
            new("a", new(Zone: "living"), Name: "Sofa"), new("b", new(Zone: "kitchen"), Name: "Stove"), new("c", new(), Name: "Desk")] };
        var model = new ShellViewModel { SelectedPage = "Now Playing" }; model.Playback.Apply(snapshot);
        var bar = new ZoneBar { DataContext = model };
        var window = new Window { Width = 390, Height = 140, Content = bar };
        try
        {
            window.Show(); window.UpdateLayout(); Dispatcher.UIThread.RunJobs();
            var selector = bar.FindControl<Button>("NowPlayingZoneSelector")!;
            var at = selector.TranslatePoint(new Point(selector.Bounds.Width / 2, selector.Bounds.Height / 2), window)!.Value;
            window.MouseDown(at, MouseButton.Left); window.MouseUp(at, MouseButton.Left); Dispatcher.UIThread.RunJobs();
            Assert.Equal("Zone Setup", model.SelectedPage); Assert.True(model.ShowZoneBar); // Covered by Zone Setup (MainView).
            Assert.True(model.GoBack()); Assert.Equal("Now Playing", model.SelectedPage);
        }
        finally { window.Close(); }

        var vm = new PlaybackViewModel(a => a(api, CancellationToken.None)); vm.Apply(snapshot);
        var view = new ZoneSetupView { DataContext = vm };
        window = new Window { Width = 390, Height = 800, Content = view };
        try
        {
            window.Show(); window.UpdateLayout(); Dispatcher.UIThread.RunJobs();
            Assert.Equal("living", view.FindControl<TextBlock>("ZoneSetupName")!.Text);
            var rows = view.FindControl<ItemsControl>("AllRooms")!.GetVisualDescendants().OfType<Avalonia.Controls.Primitives.ToggleButton>().ToArray();
            Assert.Equal([true, false, false], rows.Select(r => r.IsChecked == true));
            Click(rows[2]); Click(rows[0]); // Desk in, Sofa out
            Assert.Equal([false, false, true], rows.Select(r => r.IsChecked == true));
            Click(view.FindControl<Button>("UpdateZoneButton")!);
            for (var i = 0; i < 200 && sent.Count == 0; i++) { Dispatcher.UIThread.RunJobs(); Thread.Sleep(10); }
            Assert.Equal(["PUT /api/v1/zones/living/clients {\"clients\":[\"c\"]}"], sent);
        }
        finally { window.Close(); }

        void Click(Control control)
        {
            window.UpdateLayout(); Dispatcher.UIThread.RunJobs();
            var p = control.TranslatePoint(new Point(control.Bounds.Width / 2, control.Bounds.Height / 2), window)!.Value;
            window.MouseDown(p, MouseButton.Left); window.MouseUp(p, MouseButton.Left); Dispatcher.UIThread.RunJobs();
        }
    }

    [Fact]
    public async Task Room_volumes_list_the_zones_rooms_and_set_volume_and_mute_per_room()
    {
        var sent = new List<string>();
        using var api = new MultiroomClient("http://fake", handler: new FakeHttp(async (r, ct) =>
        {
            sent.Add($"{r.Method} {r.RequestUri!.AbsolutePath} {await r.Content!.ReadAsStringAsync(ct)}");
            return FakeHttp.Json("{}");
        }));
        var snapshot = PlaybackTests.Snapshot() with { Rooms = [
            new("a", new(Zone: "living", Volume: 0.8, Muted: false), Name: "Sofa"),
            new("b", new(Zone: "living", Volume: 0.5, Muted: true)),
            new("c", new(Zone: "kitchen", Volume: 0.2))] };
        var vm = new PlaybackViewModel(a => a(api, TestContext.Current.CancellationToken)); vm.Apply(snapshot);
        Assert.Equal(["a", "b"], vm.RoomVolumes.Select(r => r.Client));
        var sofa = vm.RoomVolumes[0];
        Assert.Equal(80, sofa.Volume); Assert.False(sofa.Muted); Assert.True(vm.RoomVolumes[1].Muted);
        sofa.Volume = 30;
        for (var i = 0; i < 200 && sent.Count == 0; i++) await Task.Delay(10, TestContext.Current.CancellationToken);
        await sofa.ToggleMuteCommand.ExecuteAsync(null);
        // Accepted values stay until the server's next snapshot says otherwise.
        Assert.Equal(30, sofa.Volume); Assert.True(sofa.Muted);
        Assert.Equal(["PATCH /api/v1/rooms/a {\"volume\":0.3}", "PATCH /api/v1/rooms/a {\"muted\":true}"], sent);
        vm.ChangeZoneCommand.Execute("next");
        Assert.Equal(["c"], vm.RoomVolumes.Select(r => r.Client)); Assert.Equal(20, vm.RoomVolumes[0].Volume);
    }

}
