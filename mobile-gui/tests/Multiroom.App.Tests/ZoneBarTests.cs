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
            Assert.Equal(8, volume.TranslatePoint(new Point(), bar)!.Value.Y - play.TranslatePoint(new Point(0, play.Bounds.Height), bar)!.Value.Y);
            Assert.Equal(100, art.Bounds.Height);
            Assert.Equal(art.Bounds.Height, bar.Bounds.Height);
            Assert.Equal(new Point(0, 0), art.TranslatePoint(new Point(), bar));
            Assert.Equal(width < 520 ? 16 : 18, bar.FindControl<TextBlock>("MediaTitle")!.FontSize);
            Assert.Equal(media.Command, artButton.Command); Assert.Equal(media.CommandParameter, artButton.CommandParameter);
            Assert.Equal(new CornerRadius(0), art.CornerRadius);
            Assert.Equal(new CornerRadius(20), play.CornerRadius);
            Assert.All(new[] { bar.FindControl<TextBlock>("MediaTitle")!, bar.FindControl<TextBlock>("MediaSubtitle")! }, text => Assert.Equal(Avalonia.Media.TextAlignment.Left, text.TextAlignment));
            Assert.Equal(new Size(40, 40), play.Bounds.Size);
            var mediaGrid = bar.FindControl<Grid>("MediaGrid")!;
            Assert.Same(mediaGrid, media.Parent); Assert.Contains(selector, media.GetVisualDescendants());
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
    [InlineData("MediaButton")] [InlineData("ZoneName")]
    public void Media_and_nested_zone_info_swipe_between_zones_and_tap_opens_selected_now_playing(string row)
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
            Assert.Equal("Now Playing", model.SelectedPage); Assert.Equal("kitchen", model.Playback.SelectedZone);
            Assert.Equal("Radio", model.Playback.Title);
            model.SelectedPage = "Home";
            window.MouseDown(center, MouseButton.Left); window.MouseUp(center, MouseButton.Left);
            Dispatcher.UIThread.RunJobs(); Assert.Equal("Now Playing", model.SelectedPage);
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
        var snapshot = PlaybackTests.Snapshot(); vm.Apply(snapshot);
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
    public async Task Zone_volume_uses_zone_endpoint_preserves_drafts_and_obeys_cancellation()
    {
        var bodies = new List<string>();
        var snapshot = PlaybackTests.Snapshot();
        var desired = new ZoneDesired([], 0.6, false);
        snapshot = snapshot with { Zones = snapshot.Zones.Select(z => z with { Desired = desired }).ToArray() };
        using var api = new MultiroomClient("http://fake", handler: new FakeHttp(async (r, ct) =>
        {
            ct.ThrowIfCancellationRequested(); Assert.Equal(HttpMethod.Patch, r.Method);
            Assert.Equal("/api/v1/zones/living", r.RequestUri!.AbsolutePath);
            var body = await r.Content!.ReadAsStringAsync(ct); bodies.Add(body);
            return BrowserTests.Reply(snapshot.Zones[0] with { Desired = body.Contains("volume", StringComparison.Ordinal) ? desired with { Volume = 0.35 } : desired with { Muted = true } });
        }));
        var vm = new PlaybackViewModel(a => a(api, TestContext.Current.CancellationToken)); vm.Apply(snapshot);
        Assert.Equal(60, vm.RequestedZoneVolume); vm.RequestedZoneVolume = 35;
        vm.Apply(snapshot); Assert.Equal(35, vm.RequestedZoneVolume);
        await vm.SetZoneVolumeCommand.ExecuteAsync(null); Assert.Equal(0.35, vm.ZoneSettings!.Volume);
        await vm.MuteZoneCommand.ExecuteAsync(null); Assert.Equal("Unmute zone", vm.ZoneMuteLabel);
        Assert.Equal(new[] { "{\"volume\":0.35}", "{\"muted\":true}" }, bodies);
        vm.ChangeZoneCommand.Execute("next"); Assert.Equal(60, vm.RequestedZoneVolume);
        using var cancelled = new CancellationTokenSource(); cancelled.Cancel();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => api.SetZoneVolumeAsync("living", 0.1, null, cancelled.Token));
        Assert.Equal(2, bodies.Count);
    }

}
