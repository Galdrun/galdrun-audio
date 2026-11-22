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
    public async Task An_album_page_counts_its_tracks_marks_the_album_and_offers_play_choices()
    {
        var node = new BrowseNode("local", "album:7", "Days", "album:days", BrowseKind.Album, "Aurora · 2015 · 2 tracks · 1:12:00", Playable: true);
        var tracks = new[]
        {
            new Track("local", "track:1", "One", "Aurora", "Days", TimeSpan.FromMinutes(30)),
            new Track("local", "track:2", "Two", "Aurora", "Days", TimeSpan.FromMinutes(42)),
        };
        var liked = false; var posted = new List<string>();
        using var api = new MultiroomClient("http://fake", handler: new FakeHttp(async (r, ct) =>
        {
            var path = r.RequestUri!.AbsolutePath;
            if (r.Method == HttpMethod.Get) return Reply(new BrowsePage([], new(tracks, [], [], []), Node: node, Total: 2));
            if (path.EndsWith("/items/status", StringComparison.Ordinal))
            {
                using var body = JsonDocument.Parse(await r.Content!.ReadAsStringAsync(ct));
                Assert.Equal("album", body.RootElement.GetProperty("items")[0].GetProperty("kind").GetString());
                return Reply(new[] { new ItemStatus(LikeKind.Album, "local", "album:7", liked, false, liked ? 3 : null) });
            }
            posted.Add($"{r.Method} {path} {await r.Content!.ReadAsStringAsync(ct)}");
            liked = true;
            return FakeHttp.Json("""{"id":3,"added":"2026-10-31T00:00:00Z","item":{"kind":"album","providerId":"local","providerItemId":"album:7","title":"Days"}}""");
        }));
        var vm = Browser(api);
        HomeItem? sheet = null; var marksChanged = 0;
        vm.SheetRequested += item => sheet = item; vm.MarksChanged += () => marksChanged++;
        await vm.OpenNodeAsync(new NodeReference("local", "album:7"), "Days");

        Assert.True(vm.IsAlbum); Assert.False(vm.IsBrowsing); Assert.False(vm.ShowHeader); Assert.False(vm.ShowCompactHeader);
        Assert.Equal(("Days", "Aurora", "album:days"), (vm.Album!.Title, vm.Album.Artist, vm.Album.ArtworkId));
        Assert.Equal("2 tracks · 1 h 12 min", vm.AlbumSummary);
        Assert.Equal([(1, "One", "30:00"), (2, "Two", "42:00")], vm.AlbumTracks.Select(t => (t.Number, t.Title, t.Length)));
        Assert.True(vm.CanMarkAlbum); Assert.False(vm.AlbumLiked);

        await vm.ToggleAlbumLikeCommand.ExecuteAsync(null);
        Assert.True(vm.AlbumLiked); Assert.Equal(1, marksChanged);
        Assert.StartsWith("POST /api/v1/likes {\"album\":{\"providerId\":\"local\",\"providerItemId\":\"album:7\"", Assert.Single(posted));

        vm.PlayAlbumCommand.Execute(null);
        Assert.Equal(("Album", new NodeReference("local", "album:7")), (sheet!.Kind, sheet.Items.Node));
        // A track plays the whole album from it.
        vm.PlayTrackCommand.Execute(vm.AlbumTracks[1]);
        Assert.Equal(tracks, sheet!.Items.Tracks!); Assert.Equal(tracks[1], sheet.Start); Assert.Equal(tracks[1], sheet.Media);
    }

    [Fact]
    public async Task A_folder_of_tracks_is_no_album_page()
    {
        var node = new BrowseNode("local", "folder:1", "Inbox", Kind: BrowseKind.Folder);
        using var api = new MultiroomClient("http://fake", handler: new FakeHttp((_, _) => Task.FromResult(
            Reply(new BrowsePage([], new([new Track("local", "track:1", "One")], [], [], []), Node: node)))));
        var vm = Browser(api);
        await vm.OpenNodeAsync(new NodeReference("local", "folder:1"), "Inbox");
        Assert.False(vm.IsAlbum); Assert.True(vm.IsBrowsing); Assert.Empty(vm.AlbumTracks);
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
    public async Task A_complete_section_has_no_more_row_and_a_section_list_shows_as_rows_in_server_order()
    {
        var older = new BrowseNode("tidal", "album:1", "Older", Kind: BrowseKind.Album, Subtitle: "1990", Playable: true);
        var newer = new BrowseNode("tidal", "album:2", "Newer", Kind: BrowseKind.Album, Subtitle: "2020", Playable: true);
        var section = new BrowseNode("tidal", "artist:7/albums", "Albums", Kind: BrowseKind.Section);
        using var api = new MultiroomClient("http://fake", handler: new FakeHttp((r, _) => Task.FromResult(Reply(
            r.RequestUri!.Query.Contains("albums", StringComparison.Ordinal)
                ? new BrowsePage([older, newer], Empty, Node: section, Total: 2)
                : new BrowsePage([], Empty, Sections: [new BrowseSection("Albums", [older, newer], Empty, More: section, Total: 2)])))));
        var vm = Browser(api); await vm.OpenProviderAsync(Provider("tidal"));
        Assert.Equal(["Albums", "Older", "Newer"], vm.Items.Select(i => i.Title));

        await vm.OpenNodeAsync(new NodeReference("tidal", "artist:7/albums"), "Albums");
        Assert.True(vm.IsSectionList); Assert.False(vm.IsBrowsing); Assert.False(vm.ShowHeader);
        Assert.Equal(["Older", "Newer"], vm.SectionList!.Rows.Select(r => r.Title));
    }

    [Fact]
    public async Task Opening_a_row_or_going_back_shows_nothing_of_browse_until_the_page_has_loaded()
    {
        var artist = new BrowseNode("tidal", "artist:5", "Aurora", Kind: BrowseKind.Artist, Playable: true);
        var album = new BrowseNode("tidal", "album:1", "Days", Kind: BrowseKind.Album, Playable: true);
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var slow = false;
        using var api = new MultiroomClient("http://fake", handler: new FakeHttp(async (r, _) =>
        {
            if (slow) await release.Task;
            return Reply(r.RequestUri!.Query.Contains("album", StringComparison.Ordinal)
                ? new BrowsePage([], Empty with { Tracks = [new("tidal", "track:1", "One")] }, Node: album)
                : new BrowsePage([], Empty, Node: artist, Sections: [new("Albums", [album], Empty)]));
        }));
        var vm = Browser(api); await vm.OpenNodeAsync(new NodeReference("tidal", "artist:5"), "Aurora");
        slow = true;
        var opening = vm.OpenRowCommand.ExecuteAsync(vm.ArtistSections[0].Rows[0]);
        Assert.False(vm.IsBrowsing);
        release.SetResult(); await opening;
        Assert.True(vm.IsAlbum); Assert.False(vm.IsBrowsing);
        release = new(TaskCreationOptions.RunContinuationsAsynchronously);
        var back = vm.BackCommand.ExecuteAsync(null);
        Assert.False(vm.IsBrowsing);
        release.SetResult(); await back;
        Assert.True(vm.IsArtist);
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
        var requests = new List<string>();
        using var api = new MultiroomClient("http://fake", handler: new FakeHttp((r, _) =>
        {
            requests.Add(r.RequestUri!.PathAndQuery);
            return Task.FromResult(Reply(new SearchResult("Night", new CatalogItems(
                [new("local", "track:1", "Night"), new("tidal", "track:1", "Night")], [], [], []),
                [new("local", 1, 0, 0, 0), new("tidal", 1, 0, 0, 0)], [new("fourth", "timeout", "too slow")])));
        }));
        var vm = new SearchViewModel(action => action(api, TestContext.Current.CancellationToken)) { Query = "Night" };
        await vm.SearchCommand.ExecuteAsync(null);
        Assert.Equal("/api/v1/search?q=Night&offset=0&limit=60&types=tracks&mediaType=music", requests[^1]);
        Assert.Equal(["local", "tidal"], vm.Results.Select(r => ((Track)r.Item!.Media).ProviderId));
        Assert.Contains("fourth: timeout", vm.Notice); Assert.Equal("2 results for “Night”", vm.Status);
        Assert.Equal(["local", "tidal"], vm.Results.Select(r => r.Source));
        // Opening a result keeps the search among the kind's recent ones, the newest first, once.
        vm.OpenCommand.Execute(vm.Results[0]); vm.OpenCommand.Execute(vm.Results[1]);
        Assert.Equal(["Night"], vm.RecentOfKind);
        // The "…" kinds search their own types, at once for the query already typed.
        vm.ChooseKindCommand.Execute(SearchViewModel.Radio); await Task.Yield();
        Assert.True(vm.IsMore); Assert.Equal("Radio", vm.MoreLabel);
        Assert.Equal("/api/v1/search?q=Night&offset=0&limit=60&types=stations", requests[^1]);
        // One source searched alone.
        vm.ChooseScopeCommand.Execute(new SearchScope("tidal", "Tidal")); await Task.Yield();
        Assert.True(vm.IsOneSource);
        Assert.Equal("/api/v1/search?q=Night&offset=0&limit=60&types=stations&providers=tidal", requests[^1]);
    }

    [Fact]
    public async Task Audiobook_search_lists_whole_books_that_open_as_their_node()
    {
        using var api = new MultiroomClient("http://fake", handler: new FakeHttp((r, _) =>
        {
            Assert.Equal("/api/v1/search?q=long&offset=0&limit=60&types=books", r.RequestUri!.PathAndQuery);
            return Task.FromResult(Reply(new SearchResult("long", new CatalogItems([], [new("local", "book:4", "Long Book", "Narrator")], [], []),
                [new("local", 0, 1, 0, 0)], [])));
        }));
        var vm = new SearchViewModel(action => action(api, TestContext.Current.CancellationToken)) { Query = "long", Kind = SearchViewModel.Audiobooks };
        await vm.SearchCommand.ExecuteAsync(null);
        var book = Assert.Single(vm.Results);
        Assert.Equal(("Book", "local"), (book.Item!.Kind, book.Source));
        NodeReference? opened = null; vm.OpenRequested += (node, _) => opened = node;
        vm.OpenCommand.Execute(book);
        Assert.Equal(new NodeReference("local", "book:4"), opened);
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

    [Fact]
    public async Task An_artist_page_lists_albums_newest_first_singles_and_similar_artists_without_its_top_tracks()
    {
        var artist = new BrowseNode("tidal", "artist:5", "Aurora", "artist:5", BrowseKind.Artist, Playable: true);
        Track[] top = [new("tidal", "track:1", "One", "Aurora") { ArtworkId = "album:9" }, new("tidal", "track:2", "Two", "Aurora")];
        BrowseNode Album(string id, string title, int year) => new("tidal", id, title, id, BrowseKind.Album, $"Aurora · {year} · 10 tracks", Playable: true);
        var allTop = new BrowseNode("tidal", "artist:5/toptracks", "Top tracks", Kind: BrowseKind.Section, Playable: true);
        var more = new BrowseNode("tidal", "artist:5/albums", "Albums", Kind: BrowseKind.Section);
        // As Tidal answers: top tracks first, then albums (oldest first, with more), singles, similar artists.
        var page = new BrowsePage([], Empty, Node: artist, Sections:
        [
            new("Top tracks", [], Empty with { Tracks = top }, allTop),
            new("Albums", [Album("album:1", "Days", 2015), Album("album:2", "Nights", 2019), Album("album:4", "Mid", 2017)], Empty, more),
            new("Singles & EPs", [Album("album:3", "Single", 2020)], Empty),
            new("Similar artists", [new("tidal", "artist:6", "Other", Kind: BrowseKind.Artist, Playable: true)], Empty),
        ]);
        var opened = new List<string>();
        using var api = new MultiroomClient("http://fake", handler: new FakeHttp((r, _) =>
        { opened.Add(r.RequestUri!.Query); return Task.FromResult(Reply(page)); }));
        var vm = Browser(api); HomeItem? sheet = null; vm.SheetRequested += item => sheet = item;
        await vm.OpenNodeAsync(new NodeReference("tidal", "artist:5"), "Aurora");

        Assert.True(vm.IsArtist); Assert.True(vm.IsDetail); Assert.False(vm.IsAlbum); Assert.False(vm.IsBrowsing); Assert.False(vm.ShowOptions);
        Assert.Equal(["Albums", "Singles & EPs", "Similar artists"], vm.ArtistSections.Select(s => s.Title));
        Assert.DoesNotContain(vm.ArtistSections.SelectMany(s => s.Rows), r => r.Title == "Top tracks");
        // Albums newest first, then their "More"; the artist's own name is left out of each line.
        Assert.Equal(["Nights", "Mid", "Days", "More Albums"], vm.ArtistSections[0].Rows.Select(r => r.Title));
        Assert.Equal("2019 · 10 tracks", vm.ArtistSections[0].Rows[0].Subtitle);
        Assert.Equal("Other", Assert.Single(vm.ArtistSections[2].Rows).Title);

        vm.PlayArtistCommand.Execute(null);
        Assert.Equal(("Artist", new NodeReference("tidal", "artist:5")), (sheet!.Kind, sheet.Items.Node));
        // A row opens at a tap; back returns to the artist.
        await vm.OpenRowCommand.ExecuteAsync(vm.ArtistSections[0].Rows[0]);
        Assert.Contains("node=album%3A2", opened[^1]); Assert.True(vm.CanBack);
        await vm.BackCommand.ExecuteAsync(null);
        Assert.True(vm.IsArtist);
    }

    [Fact]
    public async Task An_artist_without_a_node_for_its_tracks_lists_them_and_a_track_plays_them_from_it()
    {
        var artist = new BrowseNode("local", "artist:5", "Aurora", Kind: BrowseKind.Artist, Playable: true);
        Track[] list = [new("local", "track:1", "One"), new("local", "track:2", "Two")];
        var page = new BrowsePage([], Empty, Node: artist, Sections: [new("Tracks", [], Empty with { Tracks = list })]);
        using var api = new MultiroomClient("http://fake", handler: new FakeHttp((_, _) => Task.FromResult(Reply(page))));
        var vm = Browser(api); HomeItem? sheet = null; vm.SheetRequested += item => sheet = item;
        await vm.OpenNodeAsync(new NodeReference("local", "artist:5"), "Aurora");
        var section = Assert.Single(vm.ArtistSections);
        Assert.Equal("Tracks", section.Title); Assert.Empty(section.Rows);
        vm.PlayTrackCommand.Execute(section.Tracks[1]);
        Assert.Equal(list, sheet!.Items.Tracks!); Assert.Equal(list[1], sheet.Start);
    }

    [Fact]
    public async Task Opening_another_page_never_shows_the_last_one_while_it_loads()
    {
        var album = new BrowseNode("local", "album:7", "Days", Kind: BrowseKind.Album, Playable: true);
        var slow = new TaskCompletionSource<HttpResponseMessage>(); var calls = 0;
        // The album's marks are read too: answered at once, only the second page waits.
        using var api = new MultiroomClient("http://fake", handler: new FakeHttp((r, _) =>
            r.RequestUri!.AbsolutePath.EndsWith("/items/status", StringComparison.Ordinal) ? Task.FromResult(FakeHttp.Json("[]"))
            : ++calls == 1 ? Task.FromResult(Reply(new BrowsePage([], Empty with { Tracks = [new Track("local", "track:1", "One")] }, Node: album)))
            : slow.Task));
        var vm = Browser(api);
        await vm.OpenNodeAsync(new NodeReference("local", "album:7"), "Days");
        Assert.True(vm.IsAlbum);
        var loading = vm.OpenNodeAsync(new NodeReference("local", "artist:5"), "Aurora");
        Assert.False(vm.IsAlbum); Assert.Empty(vm.Items); Assert.False(vm.ShowEmpty);
        slow.SetResult(Reply(new BrowsePage([], Empty, Node: new("local", "artist:5", "Aurora", Kind: BrowseKind.Artist))));
        await loading;
    }
}
