using System.Net;
using System.Text.Json;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.Primitives;
using Avalonia.Headless;
using Avalonia.Headless.XUnit;
using Avalonia.Input;
using Avalonia.Threading;
using Avalonia.VisualTree;
using Multiroom.App.Api;

namespace Multiroom.App.Tests;

public sealed class NowPlayingTests
{
    private sealed class Clock : TimeProvider
    {
        public long Milliseconds { get; set; }
        public override long TimestampFrequency => 1000;
        public override long GetTimestamp() => Milliseconds;
    }

    private static HttpResponseMessage Marks(bool favorite = false, bool liked = false, string id = "track:1") => BrowserTests.Reply(
        new[] { new ItemStatus(LikeKind.Track, "local", id, liked, favorite, liked ? 7 : null, favorite ? 9 : null) });

    private static PlaybackSnapshot Snapshot(long? position = 12000, QueueTransport transport = QueueTransport.Playing)
    {
        var snapshot = PlaybackTests.Snapshot();
        return snapshot with { Zones = snapshot.Zones.Select(zone => zone.Zone == "living" ? zone with
        {
            Queue = zone.Queue! with
            {
                PositionMs = position, Transport = transport,
                Items = zone.Queue.Items.Select(item => item.Track is { } track
                    ? item with { Item = track with { Duration = TimeSpan.FromMinutes(3) } } : item).ToArray()
            }
        } : zone).ToArray() };
    }

    [Fact]
    public void Position_runs_only_while_playing_and_unknown_position_is_not_invented()
    {
        var clock = new Clock();
        var model = new PlaybackViewModel(_ => throw new InvalidOperationException("Unexpected network"), clock);
        var snapshot = Snapshot(); model.Apply(snapshot);
        Assert.Equal("0:12", model.PositionText); Assert.Equal("3:00", model.LengthText);
        clock.Milliseconds += 3500; model.RefreshProgress(); Assert.Equal(15.5, model.ProgressSeconds);
        model.Apply(snapshot); Assert.Equal(15.5, model.ProgressSeconds); // A local edit must not reset the clock anchor.
        model.Apply(Snapshot(16000, QueueTransport.Paused)); clock.Milliseconds += 5000;
        model.RefreshProgress(); Assert.Equal(16, model.ProgressSeconds);
        model.Apply(Snapshot(null)); clock.Milliseconds += 5000; model.RefreshProgress();
        Assert.Equal("—:—", model.PositionText); Assert.Equal(0, model.ProgressSeconds);
        model.Apply(Snapshot(179000)); clock.Milliseconds += 3000; model.RefreshProgress();
        Assert.Equal(180, model.ProgressSeconds);
        model.SelectedZone = "kitchen"; Assert.False(model.CanScrub); Assert.Equal("LIVE", model.LengthText);
    }

    [Fact]
    public async Task Scrubbing_previews_without_requests_then_seeks_once_and_uses_the_server_response()
    {
        var snapshot = Snapshot(); var clock = new Clock(); var calls = 0;
        using var api = new MultiroomClient("http://fake", "seek-token", new FakeHttp(async (request, ct) =>
        {
            Assert.Equal("/api/v1/zones/living/seek", request.RequestUri!.AbsolutePath);
            Assert.Equal(HttpMethod.Post, request.Method); Assert.Equal("Bearer seek-token", request.Headers.Authorization?.ToString());
            Assert.Equal("{\"item\":1,\"positionMs\":45500}", await request.Content!.ReadAsStringAsync(ct));
            calls++; return BrowserTests.Reply(snapshot.Zones[0].Queue! with { PositionMs = 45000 });
        }));
        var model = new PlaybackViewModel(action => action(api, TestContext.Current.CancellationToken), clock);
        model.Apply(snapshot); model.BeginScrub(); model.ProgressSeconds = 45.5;
        clock.Milliseconds += 4000; model.RefreshProgress();
        Assert.Equal(45.5, model.ProgressSeconds); Assert.Equal("0:45", model.PositionText); Assert.Equal(0, calls);
        model.Apply(Snapshot(16000)); Assert.Equal(45.5, model.ProgressSeconds); // SSE refresh during a drag.
        await model.CommitScrubCommand.ExecuteAsync(null);
        Assert.Equal(1, calls); Assert.Equal(45, model.ProgressSeconds);
        await model.CommitScrubCommand.ExecuteAsync(null); Assert.Equal(1, calls);
    }

    [Theory]
    [InlineData(true)] [InlineData(false)]
    public async Task Changing_zone_or_track_cancels_the_pending_seek(bool zoneChanged)
    {
        var calls = 0;
        using var api = new MultiroomClient("http://fake", handler: new FakeHttp((_, _) =>
        { calls++; return Task.FromResult(FakeHttp.Json("{}")); }));
        var model = new PlaybackViewModel(action => action(api, TestContext.Current.CancellationToken));
        var snapshot = Snapshot(); model.Apply(snapshot); model.BeginScrub(); model.ProgressSeconds = 90;
        if (zoneChanged) model.SelectedZone = "kitchen";
        else model.Apply(snapshot with { Zones = snapshot.Zones.Select(z => z.Zone == "living"
            ? z with { Queue = z.Queue! with { Current = 2, PositionMs = 0 } } : z).ToArray() });
        await model.CommitScrubCommand.ExecuteAsync(null); Assert.Equal(0, calls);
    }

    [Fact]
    public async Task Failed_and_cancelled_seeks_restore_the_reported_position()
    {
        using var api = new MultiroomClient("http://fake", handler: new FakeHttp((_, _) => Task.FromResult(
            FakeHttp.Json("{\"error\":\"unavailable\",\"message\":\"secret\"}", HttpStatusCode.ServiceUnavailable))));
        var model = new PlaybackViewModel(action => action(api, TestContext.Current.CancellationToken), new Clock());
        model.Apply(Snapshot()); model.BeginScrub(); model.ProgressSeconds = 90;
        await Assert.ThrowsAsync<ApiException>(() => model.CommitScrubCommand.ExecuteAsync(null));
        Assert.Equal(12, model.ProgressSeconds);
        using var cancelled = new CancellationTokenSource(); cancelled.Cancel();
        var stopped = new PlaybackViewModel(action => action(api, cancelled.Token), new Clock());
        stopped.Apply(Snapshot()); stopped.BeginScrub(); stopped.ProgressSeconds = 90;
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => stopped.CommitScrubCommand.ExecuteAsync(null));
        Assert.Equal(12, stopped.ProgressSeconds);
    }

    [Fact]
    public async Task Favorite_saves_the_current_track_and_does_not_follow_a_zone_change_during_the_request()
    {
        var received = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var calls = 0; var snapshot = Snapshot(); var track = snapshot.Zones[0].Queue!.Items[0].Track!;
        using var api = new MultiroomClient("http://fake", "favorite-token", new FakeHttp(async (request, ct) =>
        {
            if (request.RequestUri!.AbsolutePath == "/api/v1/items/status") return Marks(favorite: calls != 0);
            Assert.Equal(HttpMethod.Post, request.Method); Assert.Equal("/api/v1/favorites", request.RequestUri!.AbsolutePath);
            Assert.Equal("Bearer favorite-token", request.Headers.Authorization?.ToString());
            using var body = JsonDocument.Parse(await request.Content!.ReadAsStringAsync(ct));
            Assert.Equal("track:1", body.RootElement.GetProperty("track").GetProperty("providerItemId").GetString());
            Assert.Equal("local", body.RootElement.GetProperty("track").GetProperty("providerId").GetString());
            Assert.False(body.RootElement.TryGetProperty("station", out _));
            calls++; received.SetResult(); await release.Task.WaitAsync(ct);
            return BrowserTests.Reply(new Favorite(9, DateTimeOffset.UnixEpoch, track));
        }));
        var model = new PlaybackViewModel(action => action(api, TestContext.Current.CancellationToken)); model.Apply(snapshot);
        await model.RefreshMarksAsync(api, TestContext.Current.CancellationToken);
        model.SelectedItem = model.Items[1]; // Queue selection must not replace the playing track.
        var save = model.FavoriteCurrentCommand.ExecuteAsync(null);
        await received.Task.WaitAsync(TestContext.Current.CancellationToken);
        Assert.False(model.FavoriteSaved); model.SelectedZone = "kitchen";
        release.SetResult(); await save;
        Assert.Equal(1, calls); Assert.False(model.FavoriteSaved); Assert.Equal("Add to favourites", model.FavoriteLabel);
        model.SelectedZone = "living"; await model.RefreshMarksAsync(api, TestContext.Current.CancellationToken);
        Assert.True(model.FavoriteSaved); Assert.Equal("Remove from favourites", model.FavoriteLabel);
        model.Apply(new([], [])); Assert.False(model.FavoriteSaved); Assert.False(model.FavoriteCurrentCommand.CanExecute(null));
        await model.FavoriteCurrentCommand.ExecuteAsync(null); Assert.Equal(1, calls);
    }

    [Theory]
    [InlineData(false)] [InlineData(true)]
    public async Task Refused_or_cancelled_favorites_do_not_show_success(bool cancel)
    {
        using var stopped = new CancellationTokenSource(); if (cancel) stopped.Cancel();
        using var api = new MultiroomClient("http://fake", handler: new FakeHttp((request, ct) =>
        {
            ct.ThrowIfCancellationRequested();
            if (request.RequestUri!.AbsolutePath == "/api/v1/items/status") return Task.FromResult(Marks());
            return Task.FromResult(FakeHttp.Json("{\"error\":\"unavailable\",\"message\":\"secret\"}", HttpStatusCode.ServiceUnavailable));
        }));
        var model = new PlaybackViewModel(action => action(api, stopped.Token)); model.Apply(Snapshot());
        await model.RefreshMarksAsync(api, TestContext.Current.CancellationToken);
        if (cancel) await Assert.ThrowsAnyAsync<OperationCanceledException>(() => model.FavoriteCurrentCommand.ExecuteAsync(null));
        else await Assert.ThrowsAsync<ApiException>(() => model.FavoriteCurrentCommand.ExecuteAsync(null));
        Assert.False(model.FavoriteSaved); Assert.Equal("Add to favourites", model.FavoriteLabel);
    }

    [AvaloniaTheory]
    [InlineData(390, 844)] [InlineData(900, 850)] [InlineData(1440, 950)]
    [InlineData(900, 500)] [InlineData(320, 480)]
    public async Task Now_playing_has_transport_buttons_and_a_zone_name_and_dots_pane(int width, int height)
    {
        var view = new MainView(loadPreferences: false);
        var model = (ShellViewModel)view.DataContext!; model.Playback.Apply(Snapshot());
        var window = new Window { Width = width, Height = height, Content = view };
        try
        {
            window.Show(); model.SelectedPage = "Now Playing"; window.UpdateLayout(); Dispatcher.UIThread.RunJobs();
            using var api = new MultiroomClient("http://fake", handler: new FakeHttp((_, _) => Task.FromResult(Marks(favorite: true, liked: true))));
            await model.Playback.RefreshMarksAsync(api, TestContext.Current.CancellationToken); Dispatcher.UIThread.RunJobs();
            var page = Assert.Single(view.GetVisualDescendants().OfType<NowPlayingView>());
            Assert.True(page.IsEffectivelyVisible); Assert.True(page.Bounds.Height > 200);
            Assert.Empty(page.GetVisualDescendants().OfType<ScrollViewer>());
            var coverSpace = page.FindControl<Viewbox>("CoverSpace")!;
            var cover = page.FindControl<Button>("CoverButton")!;
            var topLeft = cover.TranslatePoint(new Point(), page)!.Value;
            var bottomRight = cover.TranslatePoint(new Point(cover.Bounds.Width, cover.Bounds.Height), page)!.Value;
            var coverSize = bottomRight.X - topLeft.X;
            Assert.Equal(coverSize, bottomRight.Y - topLeft.Y, 1);
            Assert.Equal(Math.Min(coverSpace.Bounds.Width, coverSpace.Bounds.Height), coverSize, 1);
            Assert.True(coverSize > 0);
            Assert.InRange(topLeft.X, 0, page.Bounds.Width); Assert.InRange(bottomRight.X, 0, page.Bounds.Width);
            Assert.InRange(topLeft.Y, 0, page.Bounds.Height); Assert.InRange(bottomRight.Y, 0, page.Bounds.Height);
            var artwork = page.FindControl<ArtworkView>("CoverArtwork")!;
            Assert.IsType<Viewbox>(artwork.Child); // Missing artwork has a scalable record placeholder.
            var transportBounds = page.GetVisualDescendants().OfType<TransportView>().Single();
            var controlsBottom = transportBounds.TranslatePoint(new Point(transportBounds.Bounds.Width, transportBounds.Bounds.Height), page)!.Value;
            Assert.InRange(controlsBottom.X, 0, page.Bounds.Width); Assert.InRange(controlsBottom.Y, 0, page.Bounds.Height);
            Assert.Empty(page.GetVisualDescendants().OfType<ComboBox>());
            Assert.DoesNotContain(view.GetVisualDescendants().OfType<TextBlock>(), label => label.IsEffectivelyVisible && label.Text == "GALDRUN");
            Assert.DoesNotContain(view.GetVisualDescendants().OfType<Button>(), button => button.IsEffectivelyVisible && Equals(button.Content, "Refresh"));
            Assert.DoesNotContain(page.GetVisualDescendants().OfType<TextBlock>(), label => label.Text is "Room volume" or "Playing");
            Assert.True(page.FindControl<Slider>("PositionSlider")!.IsEnabled);
            var transport = Assert.Single(page.GetVisualDescendants().OfType<TransportView>());
            Assert.Equal("next", transport.FindControl<Button>("NextButton")!.CommandParameter);
            Assert.Same(model.Playback.PlayPauseCommand, transport.FindControl<Button>("TogglePlaybackButton")!.Command);
            var favoriteButton = page.FindControl<Button>("FavoriteButton")!;
            Assert.Null(transport.FindControl<Button>("FavoriteButton"));
            Assert.Same(model.Playback.FavoriteCurrentCommand, favoriteButton.Command); Assert.True(favoriteButton.IsEffectivelyEnabled);
            var title = page.FindControl<TextBlock>("TrackTitle")!;
            var like = page.FindControl<Button>("LikeButton")!;
            var favoriteRight = favoriteButton.TranslatePoint(new Point(favoriteButton.Bounds.Width, 24), page)!.Value;
            var titleLeft = title.TranslatePoint(new Point(), page)!.Value;
            var titleRight = title.TranslatePoint(new Point(title.Bounds.Width, title.Bounds.Height), page)!.Value;
            var likeLeft = like.TranslatePoint(new Point(0, 24), page)!.Value;
            Assert.Equal(titleLeft.X, favoriteRight.X, 1); Assert.Equal(titleRight.X, likeLeft.X, 1);
            Assert.Equal(favoriteRight.Y, likeLeft.Y, 1);
            Assert.Equal((titleLeft.Y + titleRight.Y) / 2, likeLeft.Y, 1);
            Assert.Same(model.Playback.LikeCurrentCommand, like.Command); Assert.True(like.IsEffectivelyEnabled);
            Assert.True(page.FindControl<Avalonia.Controls.Shapes.Path>("FavoriteFill")!.IsVisible);
            Assert.True(page.FindControl<Avalonia.Controls.Shapes.Path>("LikeFill")!.IsVisible);
            Assert.Null(transport.FindControl<Button>("StopButton"));
            Assert.Same(model.Playback.RepeatCommand, transport.FindControl<Button>("RepeatButton")!.Command);
            Assert.Same(model.Playback.ShuffleCommand, transport.FindControl<ToggleButton>("ShuffleButton")!.Command);
            Assert.False(transport.FindControl<ToggleButton>("ShuffleButton")!.IsChecked);
            var controls = new Button[] { transport.FindControl<ToggleButton>("ShuffleButton")!,
                transport.FindControl<Button>("PreviousButton")!, transport.FindControl<Button>("TogglePlaybackButton")!,
                transport.FindControl<Button>("NextButton")!, transport.FindControl<Button>("RepeatButton")! };
            var previousRight = 0d;
            foreach (var control in controls)
            {
                var left = control.TranslatePoint(new Point(), transport)!.Value;
                var right = control.TranslatePoint(new Point(control.Bounds.Width, control.Bounds.Height), transport)!.Value;
                Assert.Equal(32, (left.Y + right.Y) / 2, 1);
                Assert.True(control.Bounds.Width >= 44); Assert.True(left.X >= previousRight);
                Assert.InRange(right.X, 0, transport.Bounds.Width); previousRight = right.X;
            }
            var bar = view.FindControl<ZoneBar>("BottomZoneBar")!;
            Assert.True(bar.IsEffectivelyVisible); Assert.Equal(64, bar.Bounds.Height);
            Assert.Equal(view.Bounds.Width, bar.Bounds.Width);
            Assert.Equal(view.Bounds.Height, bar.TranslatePoint(new Point(0, bar.Bounds.Height), view)!.Value.Y, 1);
            Assert.Equal("#ff252a2c", bar.FindControl<Border>("ZoneCard")!.Background!.ToString());
            var zoneName = bar.FindControl<TextBlock>("NowPlayingZoneName")!;
            var dots = bar.FindControl<ItemsControl>("NowPlayingZoneDots")!;
            Assert.True(zoneName.IsEffectivelyVisible); Assert.True(dots.IsEffectivelyVisible);
            Assert.Equal("living", zoneName.Text); Assert.Equal(18, zoneName.FontSize);
            Assert.Equal(new[] { "#ffc7ccce", "#ff50575a" }, dots.GetVisualDescendants().OfType<Avalonia.Controls.Shapes.Ellipse>().Select(dot => dot.Fill!.ToString()));
            Assert.True(dots.TranslatePoint(new Point(), bar)!.Value.Y >= zoneName.TranslatePoint(new Point(0, zoneName.Bounds.Height), bar)!.Value.Y);
            Assert.False(bar.FindControl<Button>("ArtworkButton")!.IsVisible);
            Assert.False(bar.FindControl<Button>("MediaButton")!.IsVisible);
            Assert.False(bar.FindControl<Button>("PlayPauseButton")!.IsVisible);
            Assert.Null(bar.FindControl<Button>("ZoneSelector"));
            Assert.False(bar.FindControl<Button>("VolumeButton")!.IsVisible); Assert.Null(bar.FindControl<Slider>("VolumeSlider"));
            Assert.Null(bar.FindControl<ItemsControl>("ZoneDots"));
            Assert.Null(bar.FindControl<Button>("LikeButton"));
            var zoneSelector = bar.FindControl<Button>("NowPlayingZoneSelector")!;
            var zoneCenter = zoneSelector.TranslatePoint(new Point(zoneSelector.Bounds.Width / 2, zoneSelector.Bounds.Height / 2), window)!.Value;
            using (var swipe = window.TouchBegin(zoneCenter))
            {
                window.TouchMove(swipe, zoneCenter + new Vector(-55, 0));
                window.TouchEnd(swipe, zoneCenter + new Vector(-55, 0));
            }
            Dispatcher.UIThread.RunJobs(); Assert.Equal("kitchen", model.Playback.SelectedZone);
            Assert.Equal("Now Playing", model.SelectedPage); Assert.False(favoriteButton.IsEffectivelyEnabled);
            window.UpdateLayout(); Dispatcher.UIThread.RunJobs();
            Assert.Equal("kitchen", zoneName.Text);
            Assert.Equal(new[] { "#ff50575a", "#ffc7ccce" }, dots.GetVisualDescendants().OfType<Avalonia.Controls.Shapes.Ellipse>().Select(dot => dot.Fill!.ToString()));
            model.SelectedPage = "Home"; window.UpdateLayout(); Dispatcher.UIThread.RunJobs(); Assert.Equal(100, bar.Bounds.Height);
            Assert.False(dots.IsEffectivelyVisible);
            Assert.False(like.IsEffectivelyVisible); Assert.False(favoriteButton.IsEffectivelyVisible);
        }
        finally { window.Close(); }
    }

    [AvaloniaFact]
    public void Busy_now_playing_uses_an_artwork_spinner_without_moving_the_layout_or_blocking_swipes()
    {
        var view = new MainView(loadPreferences: false); var model = (ShellViewModel)view.DataContext!;
        model.Playback.Apply(Snapshot()); model.SelectedPage = "Now Playing";
        var window = new Window { Width = 390, Height = 844, Content = view };
        try
        {
            window.Show(); window.UpdateLayout(); Dispatcher.UIThread.RunJobs();
            var page = view.GetVisualDescendants().OfType<NowPlayingView>().Single();
            var coverSpace = page.FindControl<Viewbox>("CoverSpace")!; var bounds = coverSpace.Bounds;
            var overlay = page.FindControl<Border>("ArtworkProgress")!;
            var spinner = page.FindControl<Avalonia.Controls.Shapes.Path>("LoadingSpinner")!;
            var topProgress = view.FindControl<ProgressBar>("TopProgress")!;
            Assert.False(overlay.IsVisible); Assert.False(topProgress.IsVisible);
            model.IsBusy = true; window.UpdateLayout(); Dispatcher.UIThread.RunJobs();
            Assert.True(overlay.IsEffectivelyVisible); Assert.True(spinner.IsEffectivelyVisible);
            Assert.False(topProgress.IsVisible); Assert.False(overlay.IsHitTestVisible);
            Assert.IsType<Avalonia.Media.RotateTransform>(spinner.RenderTransform);
            Assert.Equal(bounds, coverSpace.Bounds);
            var cover = page.FindControl<Button>("CoverButton")!;
            var center = cover.TranslatePoint(new Point(320, 320), window)!.Value;
            using (var touch = window.TouchBegin(center))
            { window.TouchMove(touch, center + new Vector(-70, 0)); window.TouchEnd(touch, center + new Vector(-70, 0)); }
            Dispatcher.UIThread.RunJobs(); Assert.Equal("kitchen", model.Playback.SelectedZone);
            model.IsBusy = false; window.UpdateLayout(); Dispatcher.UIThread.RunJobs();
            Assert.False(overlay.IsVisible); Assert.False(spinner.IsVisible); Assert.Equal(bounds, coverSpace.Bounds);
            model.SelectedPage = "Sources"; model.IsBusy = true; window.UpdateLayout(); Dispatcher.UIThread.RunJobs();
            Assert.True(topProgress.IsEffectivelyVisible); Assert.False(overlay.IsVisible);
            model.SelectedPage = "Now Playing"; window.UpdateLayout(); Dispatcher.UIThread.RunJobs();
            Assert.False(topProgress.IsVisible); Assert.True(overlay.IsEffectivelyVisible);
            model.IsBusy = false; Dispatcher.UIThread.RunJobs(); Assert.False(overlay.IsVisible);
        }
        finally { window.Close(); }
    }

    [AvaloniaFact]
    public void Cover_swipes_change_zone_and_vertical_drags_do_not()
    {
        var model = new ShellViewModel { SelectedPage = "Now Playing" }; model.Playback.Apply(Snapshot());
        var page = new NowPlayingView { DataContext = model };
        var window = new Window { Width = 390, Height = 750, Content = page };
        try
        {
            window.Show(); window.UpdateLayout(); Dispatcher.UIThread.RunJobs();
            var cover = page.FindControl<Button>("CoverButton")!;
            var center = cover.TranslatePoint(new Point(120, 120), window)!.Value;
            using (var touch = window.TouchBegin(center))
            { window.TouchMove(touch, center + new Vector(-70, 0)); window.TouchEnd(touch, center + new Vector(-70, 0)); }
            Dispatcher.UIThread.RunJobs(); Assert.Equal("kitchen", model.Playback.SelectedZone);
            Assert.False(model.Playback.FavoriteCurrentCommand.CanExecute(null));
            window.MouseDown(center, MouseButton.Left); window.MouseMove(center + new Vector(70, 0));
            window.MouseUp(center + new Vector(70, 0), MouseButton.Left); Dispatcher.UIThread.RunJobs();
            Assert.Equal("living", model.Playback.SelectedZone); Assert.Equal("Now Playing", model.SelectedPage);
            using (var touch = window.TouchBegin(center))
            { window.TouchMove(touch, center + new Vector(0, 60)); window.TouchEnd(touch, center + new Vector(0, 60)); }
            Dispatcher.UIThread.RunJobs(); Assert.Equal("living", model.Playback.SelectedZone);
        }
        finally { window.Close(); }
    }

    [AvaloniaTheory]
    [InlineData(false)] [InlineData(true)]
    public async Task Slider_updates_the_preview_and_sends_one_seek_on_release(bool touchInput)
    {
        var path = Path.Combine(Path.GetTempPath(), "galdrun-nowplaying-" + Guid.NewGuid().ToString("N") + ".json");
        var snapshot = Snapshot(12000, QueueTransport.Paused); var seeks = 0; long requested = 0;
        using var events = new FixtureEventStream();
        var model = new ShellViewModel(preferencesPath: path, clientFactory: (address, token) =>
            new MultiroomClient(address, token, new FakeHttp(async (request, ct) =>
            {
                switch (request.RequestUri!.AbsolutePath)
                {
                    case "/api/v1/status": return FakeHttp.Json("""{"version":"0.1","core":{"connected":true,"connections":1,"zones":1,"rooms":0,"queuedEvents":0,"droppedEvents":0},"media":{},"library":{},"providers":[],"addIns":{},"events":{"subscribers":0,"held":0,"dropped":0}}""");
                    case "/api/v1/providers": case "/api/v1/favorites": case "/api/v1/rooms": return FakeHttp.Json("[]");
                    case "/api/v1/zones": return BrowserTests.Reply(snapshot.Zones);
                    case "/api/v1/items/status": return Marks();
                    case "/api/v1/events": return new HttpResponseMessage(HttpStatusCode.OK) { Content = new StreamContent(events) { Headers = { ContentType = new("text/event-stream") } } };
                    case "/api/v1/zones/living/seek":
                        using (var body = JsonDocument.Parse(await request.Content!.ReadAsStringAsync(ct)))
                        { Assert.Equal(1, body.RootElement.GetProperty("item").GetInt64()); requested = body.RootElement.GetProperty("positionMs").GetInt64(); }
                        seeks++;
                        snapshot = snapshot with { Zones = snapshot.Zones.Select(z => z.Zone == "living" ? z with { Queue = z.Queue! with { PositionMs = requested } } : z).ToArray() };
                        return BrowserTests.Reply(snapshot.Zones[0].Queue!);
                    default: throw new InvalidOperationException("Unexpected request");
                }
            })));
        var page = new NowPlayingView { DataContext = model };
        var window = new Window { Width = 390, Height = 750, Content = page };
        try
        {
            await model.ConnectCommand.ExecuteAsync(null); model.Playback.Apply(snapshot); model.SelectedPage = "Now Playing";
            window.Show(); window.UpdateLayout(); Dispatcher.UIThread.RunJobs();
            var slider = page.FindControl<Slider>("PositionSlider")!;
            var thumb = Assert.Single(slider.GetVisualDescendants().OfType<Thumb>());
            var start = thumb.TranslatePoint(new Point(thumb.Bounds.Width / 2, thumb.Bounds.Height / 2), window)!.Value;
            var end = start + new Vector(100, 0);
            var touch = touchInput ? window.TouchBegin(start) : null;
            if (!touchInput) window.MouseDown(start, MouseButton.Left);
            try
            {
                if (touch is not null) window.TouchMove(touch, end); else window.MouseMove(end);
                Dispatcher.UIThread.RunJobs();
                Assert.Equal(0, seeks); Assert.True(model.Playback.ProgressSeconds > 12);
                Assert.Equal(model.Playback.PositionText, page.FindControl<TextBlock>("PositionLabel")!.Text);
                if (touch is not null) window.TouchEnd(touch, end); else window.MouseUp(end, MouseButton.Left);
            }
            finally { touch?.Dispose(); }
            Dispatcher.UIThread.RunJobs();
            if (model.Playback.CommitScrubCommand.ExecutionTask is { } task) await task;
            Dispatcher.UIThread.RunJobs();
            Assert.Equal(1, seeks); Assert.True(requested > 12000); Assert.Equal("Now Playing", model.SelectedPage);
        }
        finally { window.Close(); await model.DisconnectAsync(); if (File.Exists(path)) File.Delete(path); }
    }
}
