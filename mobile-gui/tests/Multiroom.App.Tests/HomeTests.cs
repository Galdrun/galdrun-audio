using System.Text.Json;
using Avalonia;
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
            Assert.Equal("Bearer test-token", request.Headers.Authorization?.ToString());
            return Task.FromResult(request.RequestUri!.AbsolutePath == "/api/v1/favorites" ? BrowserTests.Reply(favourites)
                : FakeHttp.Json("{\"error\":\"notFound\",\"message\":\"\"}", System.Net.HttpStatusCode.NotFound));
        }));
        var home = new HomeViewModel(action => action(api, TestContext.Current.CancellationToken), () => "living");
        await home.RefreshAsync();
        Assert.Equal([9L, 3L], home.Favourites.Items.Select(item => item.Items.Favorite!.Value));
        Assert.Equal("An artist", home.Favourites.Items[0].Subtitle);
        Assert.Equal(Song.Item.ArtworkId, home.Favourites.Items[0].Artwork);
        Assert.Equal("Jazz", home.Favourites.Items[1].Subtitle);
        Assert.False(home.Favourites.IsEmpty);
        // A server without proposals, likes or history still shows its favourites.
        Assert.True(home.ForYou.IsEmpty); Assert.True(home.Liked.IsEmpty); Assert.True(home.Recent.IsEmpty);
        favourites = [Station];
        await home.RefreshAsync();
        Assert.Equal(3, Assert.Single(home.Favourites.Items).Items.Favorite);
        home.Reset(); Assert.True(home.Favourites.IsEmpty);
    }

    [Fact]
    public async Task For_you_liked_and_recent_shelves_queue_what_each_item_is()
    {
        var album = new Album("tidal", "album:7", "Night", "Band");
        var book = new Audiobook("local:book:3", "A book");
        using var api = new MultiroomClient("http://fake", handler: new FakeHttp((request, _) => Task.FromResult(request.RequestUri!.AbsolutePath switch
        {
            "/api/v1/favorites" => FakeHttp.Json("[]"),
            "/api/v1/ai/proposals" => BrowserTests.Reply(new ProposalsView(true,
                [new(1, new Track("local", "track:1", "Proposed"), "You like this")], [new(2, album)], [], [new(3, book)])),
            "/api/v1/likes" => BrowserTests.Reply(new LikedItem[] {
                new(4, DateTimeOffset.UnixEpoch, new Track("radio-metadata", "x", "Heard")),
                new(5, DateTimeOffset.UnixEpoch, new Track("radio-metadata", "y", "Found"), new(new Track("tidal", "track:9", "Found it"), false)),
                new(6, DateTimeOffset.UnixEpoch, new Artist("local", "artist:1", "Singer")) }),
            "/api/v1/history" => BrowserTests.Reply(new HistoryPage([
                new(1, "zone-1", new Track("local", "track:1", "Again", "Band"), DateTimeOffset.UnixEpoch),
                new(2, "zone-1", new Track("local", "track:1", "Again", "Band"), DateTimeOffset.UnixEpoch)], 0, 2)),
            _ => throw new InvalidOperationException(request.RequestUri.ToString())
        })));
        var home = new HomeViewModel(action => action(api, TestContext.Current.CancellationToken), () => "zone-1", _ => "Kitchen");
        await home.RefreshAsync();
        Assert.Equal(["Proposed", "Night", "A book"], home.ForYou.Items.Select(i => i.Title));
        Assert.Equal("You like this", home.ForYou.Items[0].Reason);
        Assert.Equal(new NodeReference("tidal", "album:7"), home.ForYou.Items[1].Items.Node);
        Assert.Equal(new NodeReference("local", "book:3"), home.ForYou.Items[2].Items.Node);
        Assert.Equal(["Singer", "Found it"], home.Liked.Items.Select(i => i.Title));
        Assert.Equal("Band · Kitchen", Assert.Single(home.Recent.Items).Subtitle);
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
        home.Favourites.Set([HomeItem.From(Song)]);
        await Assert.ThrowsAnyAsync<OperationCanceledException>(home.RefreshAsync);
        Assert.Equal(9, Assert.Single(home.Favourites.Items).Items.Favorite);
        Assert.False(home.IsLoading);
    }

    [AvaloniaTheory]
    [InlineData(390, 844)] [InlineData(900, 850)] [InlineData(1440, 950)]
    public void Home_shelves_render_and_navigation_updates_visibility(int width, int height)
    {
        var view = new MainView(loadPreferences: false);
        var model = (ShellViewModel)view.DataContext!;
        model.Playback.Apply(PlaybackTests.Snapshot());
        model.Home.Favourites.Set([HomeItem.From(Song), HomeItem.From(Station)]);
        var window = new Window { Width = width, Height = height, Content = view };
        try
        {
            window.Show(); Dispatcher.UIThread.RunJobs();
            model.SelectedPage = "Home"; window.UpdateLayout(); Dispatcher.UIThread.RunJobs();
            var home = Assert.Single(view.GetVisualDescendants().OfType<HomeView>());
            Assert.True(home.IsEffectivelyVisible); Assert.True(home.Bounds.Height > 200);
            // Home starts right under the toolbar and the page margin, with no empty status row above it.
            Assert.Equal(48 + 16, home.TranslatePoint(new Point(), view)!.Value.Y, 1);
            var shelf = Assert.Single(home.GetVisualDescendants().OfType<ListBox>(), list => list.IsEffectivelyVisible);
            Assert.Equal(2, shelf.ItemCount);
            Assert.InRange(shelf.Bounds.Width, 150, width);
            var labels = home.GetVisualDescendants().OfType<TextBlock>().Select(label => label.Text).ToArray();
            Assert.Equal(["For you", "Favourites", "Liked", "Last played"], labels.Where(text => text is "For you" or "Favourites" or "Liked" or "Last played"));
            Assert.DoesNotContain(view.GetVisualDescendants().OfType<TextBlock>(), label => label.IsEffectivelyVisible && label.Text == "GALDRUN");
            Assert.DoesNotContain(view.GetVisualDescendants().OfType<Button>(), button => button.IsEffectivelyVisible && Equals(button.Content, "Refresh"));
            var more = Assert.Single(home.GetVisualDescendants().OfType<Button>(), button => button.IsVisible && Equals(button.Content, "See all"));
            more.Command!.Execute(more.CommandParameter);
            window.UpdateLayout(); Dispatcher.UIThread.RunJobs();
            // "See all" shows the shelf's covers as a grid on Home under a fixed header with back on its right.
            Assert.Same(model.Home.Favourites, model.Home.OpenShelf);
            Assert.False(home.FindControl<ScrollViewer>("HomeScroll")!.IsEffectivelyVisible);
            var grid = home.FindControl<ScrollViewer>("ShelfGrid")!;
            Assert.True(grid.IsEffectivelyVisible);
            var divider = home.FindControl<Border>("ShelfDivider")!;
            Assert.True(divider.IsEffectivelyVisible); Assert.Equal(1, divider.Bounds.Height);
            Assert.True(divider.TranslatePoint(new Point(), home)!.Value.Y < grid.TranslatePoint(new Point(), home)!.Value.Y);
            Assert.Equal(2, grid.GetVisualDescendants().OfType<ArtworkView>().Count());
            // The columns fill the width: each cover scales up to its column, never below its shelf size.
            var columns = home.GridColumns;
            var cells = home.FindControl<ItemsControl>("ShelfItems")!.GetRealizedContainers().Select(c => c.GetVisualDescendants().OfType<Viewbox>().First()).ToArray();
            Assert.Equal(2, cells.Length);
            Assert.All(cells, cell => Assert.Equal((grid.Bounds.Width + 14) / columns - 14, cell.Bounds.Width, tolerance: 1.0));
            Assert.All(cells, cell => Assert.True(cell.Bounds.Width >= 100));
            if (width == 390) Assert.Equal(3, columns);
            // A few covers keep their own height: rows are not spread over the page.
            var items = home.FindControl<ItemsControl>("ShelfItems")!;
            Assert.Equal(items.DesiredSize.Height - items.Margin.Bottom, items.Bounds.Height, tolerance: 1.0);
            var back = home.FindControl<Button>("ShelfBack")!;
            Assert.True(back.IsEffectivelyVisible); Assert.Equal("Back", back.Content);
            Assert.DoesNotContain(back, grid.GetVisualDescendants());
            var corner = back.TranslatePoint(new Point(back.Bounds.Width, 0), home)!.Value;
            Assert.Equal(home.Bounds.Width, corner.X, 1); Assert.True(corner.Y < grid.TranslatePoint(new Point(), home)!.Value.Y);
            // No scrollbars anywhere on Home; swiping still scrolls.
            Assert.All(home.GetVisualDescendants().OfType<ScrollViewer>(), viewer =>
            {
                Assert.NotEqual(Avalonia.Controls.Primitives.ScrollBarVisibility.Auto, viewer.VerticalScrollBarVisibility);
                Assert.NotEqual(Avalonia.Controls.Primitives.ScrollBarVisibility.Auto, viewer.HorizontalScrollBarVisibility);
                Assert.NotEqual(Avalonia.Controls.Primitives.ScrollBarVisibility.Visible, viewer.VerticalScrollBarVisibility);
                Assert.NotEqual(Avalonia.Controls.Primitives.ScrollBarVisibility.Visible, viewer.HorizontalScrollBarVisibility);
            });
            back.Command!.Execute(null);
            window.UpdateLayout(); Dispatcher.UIThread.RunJobs();
            Assert.Null(model.Home.OpenShelf); Assert.True(home.FindControl<ScrollViewer>("HomeScroll")!.IsEffectivelyVisible);
            model.SelectedPage = "Favourites";
            window.UpdateLayout(); Dispatcher.UIThread.RunJobs();
            Assert.False(home.IsEffectivelyVisible);
            Assert.Contains(view.GetVisualDescendants().OfType<Button>(), button => button.IsEffectivelyVisible && Equals(button.Content, "Refresh"));
        }
        finally { window.Close(); }
    }
}
