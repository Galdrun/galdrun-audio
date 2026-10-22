using System.Net;
using System.Text.Json;
using Multiroom.App.Api;

namespace Multiroom.App.Tests;

public sealed class BrowserTests
{
    internal static CatalogItems Empty => new([], [], [], []);
    internal static ProviderView Provider(string id = "fourth") => new(id, ProviderState.Ready, "Ready", "Independent source", ["browse", "expand"]);
    internal static HttpResponseMessage Reply(object value) => FakeHttp.Json(JsonSerializer.Serialize(value, MultiroomClient.Json));
    internal static BrowserViewModel Browser(MultiroomClient api, string? zone = "living") =>
        new(action => action(api, TestContext.Current.CancellationToken), () => zone);

    [Fact]
    public async Task Unknown_provider_renders_grid_header_and_queues_node_without_expansion()
    {
        var node = new BrowseNode("fourth", "album:1", "Quiet places", Kind: BrowseKind.Album, Subtitle: "An artist · 2026", Playable: true);
        var requests = new List<string>();
        using var api = new MultiroomClient("http://fake", handler: new FakeHttp(async (r, ct) =>
        {
            requests.Add(r.Method + " " + r.RequestUri!.PathAndQuery);
            if (r.Method == HttpMethod.Get) return Reply(new BrowsePage([node], Empty, Node: node, Total: 1, Layout: BrowseLayout.Grid));
            if (r.RequestUri.AbsolutePath.EndsWith("/items", StringComparison.Ordinal))
            {
                using var body = JsonDocument.Parse(await r.Content!.ReadAsStringAsync(ct));
                Assert.Equal("fourth", body.RootElement.GetProperty("node").GetProperty("provider").GetString());
                Assert.Equal("album:1", body.RootElement.GetProperty("node").GetProperty("id").GetString());
                Assert.True(body.RootElement.GetProperty("replace").GetBoolean());
                Assert.False(body.RootElement.TryGetProperty("tracks", out _));
                return FakeHttp.Json("""{"queue":{},"queued":2,"refused":[],"truncated":false}""");
            }
            return FakeHttp.Json("{}");
        }));
        var vm = Browser(api);
        await vm.OpenProviderAsync(Provider());
        Assert.True(vm.IsGrid); Assert.True(vm.HasHeader); Assert.False(vm.HasMore);
        Assert.Single(vm.Items); Assert.True(vm.Items[0].Grid);
        vm.SelectedItem = vm.Header;
        await vm.QueueCommand.ExecuteAsync("now");
        Assert.Equal(3, requests.Count);
        Assert.Equal("POST /api/v1/zones/living/play", requests[^1]);
    }

    [Fact]
    public async Task Sections_keep_server_order_layout_unplayable_reason_and_more_reference()
    {
        var album = new BrowseNode("tidal", "album:42", "Night", Playable: false, Unplayable: "No stream rights");
        var more = new BrowseNode("tidal", "artist-albums:7", "Albums");
        var calls = new List<string>();
        using var api = new MultiroomClient("http://fake", handler: new FakeHttp((r, _) =>
        {
            calls.Add(r.RequestUri!.PathAndQuery);
            return Task.FromResult(Reply(new BrowsePage([], Empty, Sections:
            [new BrowseSection("Albums", [album], Empty, More: more, Layout: BrowseLayout.Grid),
             new BrowseSection("Top tracks", [], new CatalogItems([new("tidal", "track:1", "One")], [], [], []))])));
        }));
        var vm = Browser(api); await vm.OpenProviderAsync(Provider("tidal"));
        Assert.Equal(["Albums", "Night", "More Albums", "Top tracks", "One"], vm.Items.Select(i => i.Title));
        Assert.True(vm.Items[1].Grid); Assert.False(vm.HasMore);
        vm.SelectedItem = vm.Items[1]; Assert.False(vm.CanQueue); Assert.Contains("No stream rights", vm.SelectedItem.Subtitle);
        vm.SelectedItem = vm.Items[2]; await vm.OpenCommand.ExecuteAsync(null);
        Assert.Contains("node=artist-albums%3A7", calls[^1]); Assert.True(vm.CanBack);
        await vm.BackCommand.ExecuteAsync(null); Assert.False(vm.CanBack);
    }

    [Fact]
    public async Task Letter_jump_and_subsequent_page_use_server_offset_without_counting_header()
    {
        var calls = new List<string>();
        using var api = new MultiroomClient("http://fake", handler: new FakeHttp((r, _) =>
        {
            calls.Add(r.RequestUri!.Query);
            return Task.FromResult(Reply(new BrowsePage([new("local", "album:1", "Tide")], Empty,
                Node: new("local", "albums", "Albums"), Total: 100, Sorts: ["title"], Sort: "title", Filterable: true,
                Letters: [new("T", 40)], Layout: BrowseLayout.Grid)));
        }));
        var vm = Browser(api); await vm.OpenProviderAsync(Provider("local"));
        vm.SelectedLetter = vm.Letters[0]; vm.Filter = "a&b";
        await vm.ApplyCommand.ExecuteAsync(null);
        Assert.Contains("offset=0", calls[^1]); Assert.Contains("letter=T", calls[^1]); Assert.Contains("filter=a%26b", calls[^1]);
        await vm.MoreCommand.ExecuteAsync(null);
        Assert.Contains("offset=41", calls[^1]); Assert.DoesNotContain("letter=", calls[^1]);
        Assert.Equal(2, vm.Items.Count);
    }

    [Fact]
    public async Task Unknown_total_stops_after_short_page()
    {
        var call = 0;
        using var api = new MultiroomClient("http://fake", handler: new FakeHttp((_, _) =>
        {
            var count = call++ == 0 ? 60 : 3;
            return Task.FromResult(Reply(new BrowsePage(Enumerable.Range(0, count).Select(i => new BrowseNode("fourth", i.ToString(), $"Album {i}")).ToArray(), Empty)));
        }));
        var vm = Browser(api); await vm.OpenProviderAsync(Provider()); Assert.True(vm.HasMore);
        await vm.MoreCommand.ExecuteAsync(null); Assert.False(vm.HasMore); Assert.Equal(63, vm.Items.Count);
    }

    [Fact]
    public async Task Search_keeps_provider_identity_and_partial_failures()
    {
        using var api = new MultiroomClient("http://fake", handler: new FakeHttp((r, _) =>
        {
            Assert.Equal("/api/v1/search?q=Night&offset=0&limit=60", r.RequestUri!.PathAndQuery);
            return Task.FromResult(Reply(new SearchResult("Night", new CatalogItems(
                [new("local", "track:1", "Night"), new("tidal", "track:1", "Night")], [], [], []),
                [new("local", 1, 0, 0, 0), new("tidal", 1, 0, 0, 0)], [new("fourth", "timeout", "too slow")])));
        }));
        var vm = Browser(api); vm.Query = "Night"; await vm.SearchCommand.ExecuteAsync(null);
        Assert.Equal(2, vm.Items.Count(i => !i.IsHeading)); Assert.Equal("Tracks", vm.Items[0].Title);
        Assert.Contains("fourth: timeout", vm.Notice); Assert.Contains("local: 1", vm.Summary);
    }

    [Fact]
    public async Task Superseded_browse_cannot_replace_new_page_even_if_handler_ignores_cancellation()
    {
        var started = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        using var api = new MultiroomClient("http://fake", handler: new FakeHttp(async (r, _) =>
        {
            if (r.RequestUri!.AbsolutePath.Contains("slow", StringComparison.Ordinal)) { started.SetResult(); await release.Task; }
            return Reply(new BrowsePage([new("fourth", "one", r.RequestUri.AbsolutePath)], Empty));
        }));
        var vm = Browser(api);
        var slow = vm.OpenProviderAsync(Provider("slow")); await started.Task.WaitAsync(TestContext.Current.CancellationToken);
        await vm.OpenProviderAsync(Provider("fast")); release.SetResult(); await slow;
        Assert.Contains("fast", Assert.Single(vm.Items).Title);
    }

    [Theory]
    [InlineData("end", false, false)] [InlineData("next", true, false)] [InlineData("now", false, true)]
    public async Task Track_placement_maps_to_server_body(string placement, bool next, bool replace)
    {
        using var api = new MultiroomClient("http://fake", handler: new FakeHttp(async (r, ct) =>
        {
            if (r.RequestUri!.AbsolutePath.EndsWith("/items", StringComparison.Ordinal))
            {
                using var body = JsonDocument.Parse(await r.Content!.ReadAsStringAsync(ct));
                Assert.Equal(next, body.RootElement.GetProperty("next").GetBoolean());
                Assert.Equal(replace, body.RootElement.GetProperty("replace").GetBoolean());
                Assert.Equal("track:1", body.RootElement.GetProperty("tracks")[0].GetProperty("providerItemId").GetString());
            }
            return FakeHttp.Json("{}");
        }));
        var vm = Browser(api); vm.SelectedItem = new("One", "", Track: new("local", "track:1", "One"));
        await vm.QueueCommand.ExecuteAsync(placement);
    }

    [Fact]
    public async Task Failed_replace_does_not_start_playback()
    {
        var calls = 0;
        using var api = new MultiroomClient("http://fake", handler: new FakeHttp((_, _) =>
        { calls++; return Task.FromResult(FakeHttp.Json("""{"error":"unavailable","message":"no"}""", HttpStatusCode.ServiceUnavailable)); }));
        var vm = Browser(api); vm.SelectedItem = new("One", "", Track: new("local", "track:1", "One"));
        await Assert.ThrowsAsync<ApiException>(() => vm.QueueCommand.ExecuteAsync("now")); Assert.Equal(1, calls);
    }
}
