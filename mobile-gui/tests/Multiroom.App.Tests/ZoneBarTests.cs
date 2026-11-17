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
            Assert.Equal(new Size(bar.Bounds.Width, 10), seek.Bounds.Size);
            var track = seek.GetVisualDescendants().OfType<Border>().First(border => border.Name == "TrackBackground");
            Assert.Equal(0, track.TranslatePoint(new Point(), bar)!.Value.Y, 1); Assert.Equal(3, track.Bounds.Height);
            var thumb = Assert.Single(seek.GetVisualDescendants().OfType<Avalonia.Controls.Primitives.Thumb>());
            Assert.Equal(new Size(10, 10), thumb.Bounds.Size);
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
            Assert.Equal(new CornerRadius(20), play.CornerRadius);
            Assert.All(new[] { bar.FindControl<TextBlock>("MediaTitle")!, bar.FindControl<TextBlock>("MediaSubtitle")! }, text => Assert.Equal(Avalonia.Media.TextAlignment.Left, text.TextAlignment));
            Assert.Equal(new Size(40, 40), play.Bounds.Size);
            var mediaGrid = bar.FindControl<Grid>("MediaGrid")!;
            Assert.Same(mediaGrid, media.Parent); Assert.DoesNotContain(selector, media.GetVisualDescendants());
            var playLeft = play.TranslatePoint(new Point(), bar)!.Value.X;
            Assert.Equal(16, bar.Bounds.Width - playLeft - play.Bounds.Width);
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
                Dispatcher.UIThread.RunJobs();
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
            Dispatcher.UIThread.RunJobs(); Assert.Equal("kitchen", model.Playback.SelectedZone); Assert.Equal("Home", model.SelectedPage);
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
            var span = Math.Max(240, dial.Bounds.Width);
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
            Assert.Equal("Zone Setup", model.SelectedPage); Assert.False(model.ShowZoneBar);
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
