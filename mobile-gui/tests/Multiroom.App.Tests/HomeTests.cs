using System.Text.Json;
using Avalonia.Controls;
using Avalonia.Headless.XUnit;
using Avalonia.Threading;
using Avalonia.VisualTree;
using Multiroom.App.Api;

namespace Multiroom.App.Tests;

public sealed class HomeTests
{
    private static readonly Favorite Song = new(9, DateTimeOffset.UnixEpoch,
        new Track("fourth", "song:1", "Favourite song", "An artist") { ArtworkId = "album:1" });
    private static readonly Favorite Station = new(3, DateTimeOffset.UnixEpoch,
        new RadioStation("radio", "station:1", "Favourite radio", "Jazz"));

    [Fact]
    public async Task Refresh_preserves_server_order_and_reads_external_changes()
    {
        Favorite[] favourites = [Song, Station];
        using var api = new MultiroomClient("http://fake", "test-token", new FakeHttp((request, _) =>
        {
            Assert.Equal(HttpMethod.Get, request.Method);
            Assert.Equal("/api/v1/favorites", request.RequestUri!.AbsolutePath);
            Assert.Equal("Bearer test-token", request.Headers.Authorization?.ToString());
            return Task.FromResult(BrowserTests.Reply(favourites));
        }));
        var home = new HomeViewModel(action => action(api, TestContext.Current.CancellationToken), () => "living");
        await home.RefreshAsync();
        Assert.Equal([9L, 3L], home.Favourites.Select(item => item.Id));
        Assert.Equal("An artist", home.Favourites[0].Subtitle);
        Assert.Equal(Song.Item.ArtworkId, home.Favourites[0].Artwork);
        Assert.Equal("Jazz", home.Favourites[1].Subtitle);
        Assert.False(home.NoFavourites);
        favourites = [Station];
        await home.RefreshAsync();
        Assert.Equal(3, Assert.Single(home.Favourites).Id);
        home.Reset(); Assert.True(home.NoFavourites); Assert.Empty(home.Favourites);
    }

    [Theory]
    [InlineData(true)] [InlineData(false)]
    public async Task Favourite_actions_queue_the_stored_reference_and_play_after_replace(bool play)
    {
        var requests = new List<string>();
        using var api = new MultiroomClient("http://fake", handler: new FakeHttp(async (request, ct) =>
        {
            requests.Add(request.RequestUri!.AbsolutePath);
            if (requests.Count == 1)
            {
                using var body = JsonDocument.Parse(await request.Content!.ReadAsStringAsync(ct));
                Assert.Equal(9, body.RootElement.GetProperty("favorite").GetInt64());
                Assert.Equal(play, body.RootElement.GetProperty("replace").GetBoolean());
                Assert.False(body.RootElement.TryGetProperty("tracks", out _));
            }
            else Assert.Null(request.Content);
            return FakeHttp.Json("{}");
        }));
        var home = new HomeViewModel(action => action(api, TestContext.Current.CancellationToken), () => "living / room");
        if (play) await home.PlayCommand.ExecuteAsync(HomeItem.From(Song));
        else await home.QueueCommand.ExecuteAsync(HomeItem.From(Song));
        Assert.Equal("/api/v1/zones/living%20%2F%20room/queue/items", requests[0]);
        Assert.Equal(play ? 2 : 1, requests.Count);
        if (play) Assert.EndsWith("/play", requests[1]);
    }

    [Fact]
    public async Task No_zone_and_rejected_queue_never_start_playback()
    {
        var calls = 0;
        using var api = new MultiroomClient("http://fake", handler: new FakeHttp((_, _) =>
        {
            calls++;
            return Task.FromResult(FakeHttp.Json("{\"error\":\"unavailable\",\"message\":\"private\"}",
                System.Net.HttpStatusCode.ServiceUnavailable));
        }));
        string? zone = null;
        var home = new HomeViewModel(action => action(api, TestContext.Current.CancellationToken), () => zone);
        await Assert.ThrowsAsync<ArgumentException>(() => home.PlayCommand.ExecuteAsync(HomeItem.From(Song)));
        Assert.Equal(0, calls);
        zone = "living";
        await Assert.ThrowsAsync<ApiException>(() => home.PlayCommand.ExecuteAsync(HomeItem.From(Song)));
        Assert.Equal(1, calls);
    }

    [Fact]
    public async Task A_cancelled_refresh_preserves_the_previous_shelf()
    {
        using var stop = CancellationTokenSource.CreateLinkedTokenSource(TestContext.Current.CancellationToken);
        using var api = new MultiroomClient("http://fake", handler: new FakeHttp(async (_, ct) =>
        { stop.Cancel(); await Task.Delay(Timeout.Infinite, ct); return FakeHttp.Json("[]"); }));
        var home = new HomeViewModel(action => action(api, stop.Token), () => "living");
        home.Favourites.Add(HomeItem.From(Song));
        await Assert.ThrowsAnyAsync<OperationCanceledException>(home.RefreshAsync);
        Assert.Equal(9, Assert.Single(home.Favourites).Id);
        Assert.False(home.IsLoading);
    }

    [AvaloniaTheory]
    [InlineData(390, 844)] [InlineData(900, 850)] [InlineData(1440, 950)]
    public void Home_shelves_render_and_navigation_updates_visibility(int width, int height)
    {
        var view = new MainView(loadPreferences: false);
        var model = (ShellViewModel)view.DataContext!;
        model.Playback.Apply(PlaybackTests.Snapshot());
        model.Home.Favourites.Add(HomeItem.From(Song));
        model.Home.Favourites.Add(HomeItem.From(Station));
        var window = new Window { Width = width, Height = height, Content = view };
        try
        {
            window.Show(); Dispatcher.UIThread.RunJobs();
            model.SelectedPage = "Home"; window.UpdateLayout(); Dispatcher.UIThread.RunJobs();
            var home = Assert.Single(view.GetVisualDescendants().OfType<HomeView>());
            Assert.True(home.IsEffectivelyVisible); Assert.True(home.Bounds.Height > 200);
            var shelf = home.FindControl<ListBox>("FavouriteShelf")!;
            Assert.True(shelf.IsEffectivelyVisible); Assert.Equal(2, shelf.ItemCount);
            Assert.InRange(shelf.Bounds.Width, 150, width);
            var labels = home.GetVisualDescendants().OfType<TextBlock>().Select(label => label.Text);
            Assert.Contains("Last played", labels); Assert.Contains("Proposals", labels);
            Assert.Equal(2, labels.Count(text => text == "Coming soon"));
            Assert.DoesNotContain(view.GetVisualDescendants().OfType<TextBlock>(), label => label.IsEffectivelyVisible && label.Text == "GALDRUN");
            Assert.DoesNotContain(view.GetVisualDescendants().OfType<Button>(), button => button.IsEffectivelyVisible && Equals(button.Content, "Refresh"));
            var more = Assert.Single(home.GetVisualDescendants().OfType<Button>(), button => Equals(button.Content, "See all"));
            Assert.Same(model.NavigateCommand, more.Command);
            more.Command!.Execute(more.CommandParameter);
            window.UpdateLayout(); Dispatcher.UIThread.RunJobs();
            Assert.Equal("Favourites", model.SelectedPage); Assert.False(home.IsEffectivelyVisible);
            Assert.Contains(view.GetVisualDescendants().OfType<Button>(), button => button.IsEffectivelyVisible && Equals(button.Content, "Refresh"));
        }
        finally { window.Close(); }
    }
}
