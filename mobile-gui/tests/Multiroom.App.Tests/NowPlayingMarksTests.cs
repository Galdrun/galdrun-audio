using System.Net;
using System.Text.Json;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Headless;
using Avalonia.Headless.XUnit;
using Avalonia.Input;
using Avalonia.Threading;
using Avalonia.VisualTree;
using Multiroom.App.Api;

namespace Multiroom.App.Tests;

public sealed class NowPlayingMarksTests
{
    private static CancellationToken Ct => TestContext.Current.CancellationToken;
    private static HttpResponseMessage Status(bool liked, bool favorite, string id = "track:1", LikeKind kind = LikeKind.Track) => BrowserTests.Reply(
        new[] { new ItemStatus(kind, kind == LikeKind.Station ? "webradio" : "local", id, liked, favorite, liked ? 7 : null, favorite ? 9 : null) });

    [Fact]
    public async Task Radio_favourites_mark_the_station_but_likes_mark_the_current_song()
    {
        var liked = false; var favorite = false; var writes = 0;
        var station = PlaybackTests.Snapshot().Zones[1].Queue!.Items[0].Station!;
        var song = new Track("radio-metadata", "song:1", "Song", "Artist", "Album");
        using var api = new MultiroomClient("http://fake", handler: new FakeHttp(async (request, ct) =>
        {
            var route = request.Method + " " + request.RequestUri!.AbsolutePath;
            if (route == "POST /api/v1/items/status")
            {
                Assert.Equal("{\"items\":[{\"kind\":\"station\",\"providerId\":\"webradio\",\"providerItemId\":\"station:1\"},{\"kind\":\"track\",\"providerId\":\"radio-metadata\",\"providerItemId\":\"song:1\"}]}", await request.Content!.ReadAsStringAsync(ct));
                return BrowserTests.Reply(new[] { new ItemStatus(LikeKind.Station, "webradio", "station:1", true, favorite, 123, favorite ? 9 : null),
                    new ItemStatus(LikeKind.Track, song.ProviderId, song.ProviderItemId, liked, false, liked ? 7 : null) });
            }
            writes++;
            if (route == "POST /api/v1/favorites")
                Assert.Equal("{\"station\":{\"providerId\":\"webradio\",\"providerItemId\":\"station:1\",\"name\":\"Radio\"}}", await request.Content!.ReadAsStringAsync(ct));
            if (route == "POST /api/v1/likes")
                Assert.Equal("{\"metadata\":{\"title\":\"Song\",\"artist\":\"Artist\",\"album\":\"Album\"}}", await request.Content!.ReadAsStringAsync(ct));
            switch (route)
            {
                case "POST /api/v1/favorites": favorite = true; return BrowserTests.Reply(new Favorite(9, DateTimeOffset.UnixEpoch, station));
                case "POST /api/v1/likes": liked = true; return BrowserTests.Reply(new LikedItem(7, DateTimeOffset.UnixEpoch, song));
                case "DELETE /api/v1/favorites/9": favorite = false; return new(HttpStatusCode.NoContent);
                case "DELETE /api/v1/likes/7": liked = false; return new(HttpStatusCode.NoContent);
                default: throw new InvalidOperationException("Unexpected request");
            }
        }));
        var snapshot = PlaybackTests.Snapshot();
        snapshot = snapshot with { Zones = snapshot.Zones.Select(z => z.Zone == "kitchen" ? z with { RadioTrack = song } : z).ToArray() };
        var model = new PlaybackViewModel(action => action(api, Ct)); model.Apply(snapshot); model.SelectedZone = "kitchen";
        await model.RefreshMarksAsync(api, Ct);
        Assert.True(model.FavoriteCurrentCommand.CanExecute(null)); Assert.True(model.LikeCurrentCommand.CanExecute(null)); Assert.False(model.CanScrub);
        await model.FavoriteCurrentCommand.ExecuteAsync(null); Assert.True(model.FavoriteSaved);
        await model.LikeCurrentCommand.ExecuteAsync(null); Assert.True(model.Liked);
        await model.FavoriteCurrentCommand.ExecuteAsync(null); Assert.False(model.FavoriteSaved);
        await model.LikeCurrentCommand.ExecuteAsync(null); Assert.False(model.Liked); Assert.Equal(4, writes);
        model.Apply(snapshot with { Zones = snapshot.Zones.Select(z => z.Zone == "kitchen" ? z with { RadioTrack = null } : z).ToArray() });
        using var noMetadataApi = new MultiroomClient("http://fake", handler: new FakeHttp((_, _) =>
            Task.FromResult(Status(true, false, "station:1", LikeKind.Station))));
        Assert.False(model.CanLikeCurrent); await model.RefreshMarksAsync(noMetadataApi, Ct);
        Assert.True(model.CanFavoriteCurrent); Assert.False(model.CanLikeCurrent); Assert.False(model.Liked);
    }

    [Fact]
    public async Task Existing_marks_are_read_and_toggled_using_server_ids_then_reread()
    {
        var liked = true; var favorite = true; var routes = new List<string>();
        var track = PlaybackTests.Snapshot().Zones[0].Queue!.Items[0].Track!;
        using var api = new MultiroomClient("http://fake", "marks-token", new FakeHttp(async (request, ct) =>
        {
            Assert.Equal("Bearer marks-token", request.Headers.Authorization?.ToString());
            var route = request.Method + " " + request.RequestUri!.AbsolutePath; routes.Add(route);
            switch (route)
            {
                case "POST /api/v1/items/status":
                    Assert.Equal("{\"items\":[{\"kind\":\"track\",\"providerId\":\"local\",\"providerItemId\":\"track:1\"}]}", await request.Content!.ReadAsStringAsync(ct));
                    return Status(liked, favorite);
                case "DELETE /api/v1/favorites/9": Assert.Null(request.Content); favorite = false; return new(HttpStatusCode.NoContent);
                case "DELETE /api/v1/likes/7": Assert.Null(request.Content); liked = false; return new(HttpStatusCode.NoContent);
                case "POST /api/v1/favorites":
                case "POST /api/v1/likes":
                    Assert.Equal("{\"track\":{\"providerId\":\"local\",\"providerItemId\":\"track:1\",\"title\":\"First\"}}", await request.Content!.ReadAsStringAsync(ct));
                    if (route.EndsWith("favorites", StringComparison.Ordinal))
                    { favorite = true; return BrowserTests.Reply(new Favorite(9, DateTimeOffset.UnixEpoch, track)); }
                    liked = true; return BrowserTests.Reply(new LikedItem(7, DateTimeOffset.UnixEpoch, track));
                default: throw new InvalidOperationException("Unexpected request");
            }
        }));
        var model = new PlaybackViewModel(action => action(api, Ct)); model.Apply(PlaybackTests.Snapshot());
        Assert.False(model.FavoriteCurrentCommand.CanExecute(null)); Assert.False(model.LikeCurrentCommand.CanExecute(null));
        await model.RefreshMarksAsync(api, Ct); Assert.True(model.FavoriteSaved); Assert.True(model.Liked);
        model.SelectedItem = model.Items[1];
        await model.FavoriteCurrentCommand.ExecuteAsync(null); Assert.False(model.FavoriteSaved); Assert.True(model.Liked);
        await model.LikeCurrentCommand.ExecuteAsync(null); Assert.False(model.Liked);
        await model.FavoriteCurrentCommand.ExecuteAsync(null); Assert.True(model.FavoriteSaved);
        await model.LikeCurrentCommand.ExecuteAsync(null); Assert.True(model.Liked);
        Assert.Equal(new[] { "POST /api/v1/items/status", "DELETE /api/v1/favorites/9", "POST /api/v1/items/status",
            "DELETE /api/v1/likes/7", "POST /api/v1/items/status", "POST /api/v1/favorites", "POST /api/v1/items/status",
            "POST /api/v1/likes", "POST /api/v1/items/status" }, routes);
        liked = favorite = false; await model.RefreshMarksAsync(api, Ct);
        Assert.False(model.FavoriteSaved); Assert.False(model.Liked);
    }

    [Theory]
    [InlineData(false)] [InlineData(true)]
    public async Task Refusal_and_cancellation_preserve_authoritative_like_state(bool cancel)
    {
        using var stop = new CancellationTokenSource();
        using var api = new MultiroomClient("http://fake", handler: new FakeHttp((request, ct) =>
        {
            ct.ThrowIfCancellationRequested();
            return Task.FromResult(request.RequestUri!.AbsolutePath == "/api/v1/items/status" ? Status(true, true)
                : FakeHttp.Json("{\"error\":\"unavailable\",\"message\":\"secret\"}", HttpStatusCode.ServiceUnavailable));
        }));
        var model = new PlaybackViewModel(action => action(api, stop.Token)); model.Apply(PlaybackTests.Snapshot());
        await model.RefreshMarksAsync(api, Ct); if (cancel) stop.Cancel();
        if (cancel) await Assert.ThrowsAnyAsync<OperationCanceledException>(() => model.LikeCurrentCommand.ExecuteAsync(null));
        else
        {
            var error = await Assert.ThrowsAsync<ApiException>(() => model.LikeCurrentCommand.ExecuteAsync(null));
            Assert.DoesNotContain("secret", error.Message);
        }
        Assert.True(model.Liked); Assert.True(model.FavoriteSaved);
    }

    [Theory]
    [InlineData("[]")]
    [InlineData("[{\"kind\":\"track\",\"providerId\":\"local\",\"providerItemId\":\"wrong\",\"liked\":false,\"favorite\":false}]")]
    [InlineData("[{\"kind\":\"track\",\"providerId\":\"local\",\"providerItemId\":\"track:1\",\"liked\":true,\"favorite\":false}]")]
    [InlineData("[{\"kind\":\"track\",\"providerId\":\"local\",\"providerItemId\":\"track:1\",\"liked\":false,\"favorite\":true}]")]
    [InlineData("not-json")]
    public async Task Invalid_status_is_refused_and_never_enables_unknown_mark_actions(string response)
    {
        using var api = new MultiroomClient("http://fake", handler: new FakeHttp((_, _) => Task.FromResult(FakeHttp.Json(response))));
        var model = new PlaybackViewModel(action => action(api, Ct)); model.Apply(PlaybackTests.Snapshot());
        var error = await Assert.ThrowsAsync<ApiException>(() => model.RefreshMarksAsync(api, Ct));
        Assert.Equal("invalidResponse", error.Code); Assert.False(model.Liked); Assert.False(model.FavoriteSaved);
        Assert.False(model.LikeCurrentCommand.CanExecute(null)); Assert.False(model.FavoriteCurrentCommand.CanExecute(null));
    }

    [Fact]
    public async Task Track_change_cancels_old_lookup_and_a_late_answer_cannot_replace_new_marks()
    {
        var started = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var cancelled = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        using var api = new MultiroomClient("http://fake", handler: new FakeHttp(async (request, ct) =>
        {
            using var body = JsonDocument.Parse(await request.Content!.ReadAsStringAsync(ct));
            var id = body.RootElement.GetProperty("items")[0].GetProperty("providerItemId").GetString();
            if (id == "track:2") return Status(false, true, id);
            using var registration = ct.Register(() => cancelled.TrySetResult());
            started.SetResult(); await release.Task; return Status(true, false);
        }));
        var model = new PlaybackViewModel(action => action(api, Ct)); var snapshot = PlaybackTests.Snapshot(); model.Apply(snapshot);
        var old = model.RefreshMarksAsync(api, Ct); await started.Task.WaitAsync(TimeSpan.FromSeconds(5), Ct);
        model.Apply(snapshot with { Zones = snapshot.Zones.Select(z => z.Zone == "living" ? z with { Queue = z.Queue! with { Current = 2 } } : z).ToArray() });
        await cancelled.Task.WaitAsync(TimeSpan.FromSeconds(5), Ct); Assert.False(model.CanLikeCurrent);
        await model.RefreshMarksAsync(api, Ct); release.SetResult(); await old;
        Assert.False(model.Liked); Assert.True(model.FavoriteSaved); Assert.Equal("Second", model.Title);
        using var stopped = new CancellationTokenSource(); stopped.Cancel();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => model.RefreshMarksAsync(api, stopped.Token));
    }

    [AvaloniaTheory]
    [InlineData(false)] [InlineData(true)]
    public async Task Page_entry_track_change_and_like_events_refresh_marks_and_the_heart_removes_a_like(bool radio)
    {
        using var events = new FixtureEventStream();
        var path = Path.Combine(Path.GetTempPath(), "galdrun-marks-" + Guid.NewGuid().ToString("N") + ".json");
        var snapshot = PlaybackTests.Snapshot(); var liked = true; var favorite = true; var removed = 0;
        if (radio) snapshot = snapshot with { Zones = snapshot.Zones.Select(zone => zone.Zone == "living" ? zone with
        { RadioTrack = new("local", "track:1", "Song", "Artist"), Queue = zone.Queue! with { Items = zone.Queue.Items.Select(item => item.Id == 1
            ? item with { Item = new RadioStation("webradio", "station:1", "Radio") } : item).ToArray() } } : zone).ToArray() };
        var model = new ShellViewModel(preferencesPath: path, clientFactory: (address, token) => new MultiroomClient(address, token,
            new FakeHttp(async (request, ct) =>
            {
                switch (request.RequestUri!.AbsolutePath)
                {
                    case "/api/v1/status": return FakeHttp.Json("""{"version":"0.1","core":{"connected":true,"connections":1,"zones":1,"rooms":0,"queuedEvents":0,"droppedEvents":0},"media":{},"library":{},"providers":[],"addIns":{},"events":{"subscribers":0,"held":0,"dropped":0}}""");
                    case "/api/v1/providers": case "/api/v1/favorites": case "/api/v1/rooms": return FakeHttp.Json("[]");
                    case "/api/v1/zones": return BrowserTests.Reply(snapshot.Zones);
                    case "/api/v1/events": return new(HttpStatusCode.OK) { Content = new StreamContent(events) { Headers = { ContentType = new("text/event-stream") } } };
                    case "/api/v1/items/status":
                        using (var body = JsonDocument.Parse(await request.Content!.ReadAsStringAsync(ct)))
                        {
                            return BrowserTests.Reply(body.RootElement.GetProperty("items").EnumerateArray().Select(item =>
                                new ItemStatus(item.GetProperty("kind").GetString() == "station" ? LikeKind.Station : LikeKind.Track,
                                    item.GetProperty("providerId").GetString()!, item.GetProperty("providerItemId").GetString()!,
                                    liked, favorite, liked ? 7 : null, favorite ? 9 : null)).ToArray());
                        }
                    case "/api/v1/likes/7": Assert.Equal(HttpMethod.Delete, request.Method); liked = false; removed++; return new(HttpStatusCode.NoContent);
                    default: throw new InvalidOperationException("Unexpected request");
                }
            })));
        var view = new MainView(model, loadPreferences: false); var window = new Window { Width = 390, Height = 844, Content = view };
        async Task WaitFor(Func<bool> condition)
        {
            using var deadline = CancellationTokenSource.CreateLinkedTokenSource(Ct); deadline.CancelAfter(TimeSpan.FromSeconds(5));
            while (!condition()) { Dispatcher.UIThread.RunJobs(); await Task.Delay(10, deadline.Token); }
            Dispatcher.UIThread.RunJobs();
        }
        try
        {
            await model.ConnectCommand.ExecuteAsync(null); model.Playback.Apply(snapshot); window.Show(); model.SelectedPage = "Now Playing";
            await WaitFor(() => model.Playback.Liked && model.Playback.FavoriteSaved);
            var page = view.GetVisualDescendants().OfType<NowPlayingView>().Single(); var heart = page.FindControl<Button>("LikeButton")!;
            var star = page.FindControl<Button>("FavoriteButton")!;
            Assert.True(heart.IsVisible); Assert.True(star.IsVisible);
            window.UpdateLayout(); var center = heart.TranslatePoint(new Point(24, 24), window)!.Value;
            window.MouseDown(center, MouseButton.Left); window.MouseUp(center, MouseButton.Left); Dispatcher.UIThread.RunJobs();
            if (model.Playback.LikeCurrentCommand.ExecutionTask is { } task) await task;
            Assert.Equal(1, removed); Assert.False(model.Playback.Liked); Assert.True(model.Playback.FavoriteSaved);
            liked = true; events.Publish("event: like.changed\ndata: {\"id\":7}\n\n");
            await WaitFor(() => model.Playback.Liked);
            liked = favorite = false;
            if (radio)
            {
                snapshot = snapshot with { Zones = snapshot.Zones.Select(z => z.Zone == "living" ? z with
                    { RadioTrack = new("radio-metadata", "song:2", "Another song", "Another artist") } : z).ToArray() };
                events.Publish("event: zone.metadataChanged\ndata: {\"zone\":\"living\"}\n\n");
                await WaitFor(() => model.Playback.CurrentLikeItem?.ProviderItemId == "song:2" && model.Playback.CanLikeCurrent);
                Assert.False(model.Playback.Liked);
            }
            snapshot = snapshot with { Zones = snapshot.Zones.Select(z => z.Zone == "living" ? z with { Queue = z.Queue! with { Current = 2 } } : z).ToArray() };
            events.Publish("event: queue.changed\ndata: {\"zone\":\"living\"}\n\n");
            await WaitFor(() => model.Playback.Title == "Second" && model.Playback.CanLikeCurrent);
            Assert.False(model.Playback.Liked); Assert.False(model.Playback.FavoriteSaved);
            model.SelectedPage = "Home"; favorite = true; model.SelectedPage = "Now Playing";
            await WaitFor(() => model.Playback.FavoriteSaved); Assert.False(model.Playback.Liked); Assert.Empty(model.Error);
            if (radio)
            {
                snapshot = snapshot with { Zones = snapshot.Zones.Select(z => z.Zone == "living" ? z with
                    { RadioTrack = null, Queue = z.Queue! with { Current = 1 } } : z).ToArray() };
                events.Publish("event: zone.metadataChanged\ndata: {\"zone\":\"living\"}\n\n");
                await WaitFor(() => model.Playback.Current?.Id == 1 && model.Playback.CanFavoriteCurrent);
                Assert.False(heart.IsVisible); Assert.True(star.IsVisible);
            }
            model.Playback.CancelMarks(); Dispatcher.UIThread.RunJobs();
            Assert.False(heart.IsVisible); Assert.False(star.IsVisible);
        }
        finally { window.Close(); await model.DisconnectAsync(); if (File.Exists(path)) File.Delete(path); }
    }
}
