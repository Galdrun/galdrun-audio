using System.Net;
using System.Text.Json;
using Multiroom.App.Api;

namespace Multiroom.App.Tests;

public sealed class CollectionsTests
{
    private static CancellationToken Ct => TestContext.Current.CancellationToken;
    private static readonly Track Track = new("fourth", "track:1", "Song");
    private static readonly Playlist Playlist = new(7, "Evening", 0, DateTimeOffset.UnixEpoch, 2);

    [Fact]
    public async Task Collection_routes_bodies_and_typed_responses_match_the_server()
    {
        var requests = new List<(string Route, string? Body)>();
        using var api = new MultiroomClient("http://fake", "secret", new FakeHttp(async (r, ct) =>
        {
            Assert.Equal("Bearer secret", r.Headers.Authorization!.ToString());
            requests.Add(($"{r.Method} {r.RequestUri!.PathAndQuery}", r.Content is null ? null : await r.Content.ReadAsStringAsync(ct)));
            if (r.Method == HttpMethod.Get) return FakeHttp.Json(r.RequestUri.AbsolutePath.EndsWith("entries", StringComparison.Ordinal)
                ? """[{"id":4,"item":{"kind":"track","providerId":"fourth","providerItemId":"track:1","title":"Song"}}]"""
                : r.RequestUri.AbsolutePath.EndsWith("favorites", StringComparison.Ordinal)
                ? """[{"id":9,"added":"2026-09-25T00:00:00Z","item":{"kind":"station","providerId":"radio","providerItemId":"s:1","name":"Radio"}}]"""
                : """[{"id":7,"name":"Evening","position":0,"created":"2026-09-25T00:00:00Z","count":2}]""");
            if (r.Method == HttpMethod.Delete) return new HttpResponseMessage(HttpStatusCode.NoContent);
            if (r.RequestUri.AbsolutePath.EndsWith("favorites", StringComparison.Ordinal))
                return FakeHttp.Json("""{"id":9,"added":"2026-09-25T00:00:00Z","item":{"kind":"track","providerId":"fourth","providerItemId":"track:1","title":"Song"}}""");
            return BrowserTests.Reply(Playlist);
        }));
        Assert.Equal(7, Assert.Single(await api.PlaylistsAsync(Ct)).Id);
        Assert.Equal("Song", Assert.Single(await api.EntriesAsync(7, Ct)).Track!.Title);
        Assert.Equal("Radio", Assert.Single(await api.FavoritesAsync(Ct)).Title);
        Assert.Equal(7, (await api.CreatePlaylistAsync("Evening", Ct)).Id);
        await api.PatchPlaylistAsync(7, "Night", 1, Ct);
        await api.AddEntriesAsync(7, [Track], Ct);
        await api.MoveEntryAsync(7, 4, 1, Ct);
        await api.RemoveEntryAsync(7, 4, Ct);
        Assert.Equal(9, (await api.AddFavoriteAsync(Track, null, Ct)).Id);
        await api.AddFavoriteAsync(null, new("radio", "s:1", "Radio"), Ct);
        await api.RemoveFavoriteAsync(9, Ct); await api.DeletePlaylistAsync(7, Ct);
        Assert.Equal(new[] { "GET /api/v1/playlists", "GET /api/v1/playlists/7/entries", "GET /api/v1/favorites",
            "POST /api/v1/playlists", "PATCH /api/v1/playlists/7", "POST /api/v1/playlists/7/entries",
            "PATCH /api/v1/playlists/7/entries/4", "DELETE /api/v1/playlists/7/entries/4",
            "POST /api/v1/favorites", "POST /api/v1/favorites", "DELETE /api/v1/favorites/9", "DELETE /api/v1/playlists/7" }, requests.Select(r => r.Route));
        Assert.Equal("{\"name\":\"Evening\"}", requests[3].Body);
        Assert.Equal("{\"name\":\"Night\",\"position\":1}", requests[4].Body);
        Assert.Equal("{\"tracks\":[{\"providerId\":\"fourth\",\"providerItemId\":\"track:1\",\"title\":\"Song\"}]}", requests[5].Body);
        Assert.Equal("{\"index\":1}", requests[6].Body);
        using var favorite = JsonDocument.Parse(requests[9].Body!);
        Assert.False(favorite.RootElement.TryGetProperty("track", out _));
        Assert.Equal("s:1", favorite.RootElement.GetProperty("station").GetProperty("providerItemId").GetString());
        Assert.Null(requests[7].Body); Assert.Null(requests[10].Body);
    }

    [Fact]
    public async Task Refused_reorders_preserve_order_and_refresh_picks_up_external_changes()
    {
        IReadOnlyList<Playlist> playlists = [Playlist, Playlist with { Id = 8, Name = "Morning", Position = 1 }];
        IReadOnlyList<PlaylistEntry> entries = [new(1, Track), new(2, Track with { Title = "Second" })];
        using var api = new MultiroomClient("http://fake", handler: new FakeHttp((r, _) => Task.FromResult(
            r.Method == HttpMethod.Patch ? FakeHttp.Json("{\"error\":\"unavailable\",\"message\":\"secret\"}", HttpStatusCode.ServiceUnavailable) :
            r.RequestUri!.AbsolutePath.EndsWith("entries", StringComparison.Ordinal) ? BrowserTests.Reply(entries) :
            r.RequestUri.AbsolutePath.EndsWith("favorites", StringComparison.Ordinal) ? FakeHttp.Json("[]") : BrowserTests.Reply(playlists))));
        var vm = new CollectionsViewModel(a => a(api, Ct), () => "living");
        await vm.RefreshAsync(); vm.SelectedPlaylist = vm.Playlists[0]; await vm.RefreshAsync(); vm.SelectedEntry = vm.Entries[0];
        await Assert.ThrowsAsync<ApiException>(() => vm.MovePlaylistCommand.ExecuteAsync("down"));
        await Assert.ThrowsAsync<ApiException>(() => vm.MoveEntryCommand.ExecuteAsync("down"));
        Assert.Equal([7L, 8L], vm.Playlists.Select(p => p.Id)); Assert.Equal([1L, 2L], vm.Entries.Select(e => e.Id));
        playlists = [playlists[1], Playlist with { Name = "Changed elsewhere", Position = 1 }]; entries = [entries[1]];
        await vm.RefreshAsync(); Assert.Equal("Changed elsewhere", vm.SelectedPlaylist!.Name);
        Assert.Equal(2, Assert.Single(vm.Entries).Id); Assert.Null(vm.SelectedEntry);
        playlists = []; await vm.RefreshAsync(); Assert.Null(vm.SelectedPlaylist); Assert.Empty(vm.Entries);
    }

    [Fact]
    public async Task Recommendations_keep_reasons_sources_and_successes_when_an_addin_fails()
    {
        var bodies = new List<string>();
        using var api = new MultiroomClient("http://fake", handler: new FakeHttp(async (r, ct) =>
        {
            Assert.Equal("/api/v1/recommendations", r.RequestUri!.AbsolutePath);
            bodies.Add(await r.Content!.ReadAsStringAsync(ct));
            return FakeHttp.Json("""{"items":[{"track":{"providerId":"fourth","providerItemId":"track:2","title":"Next"},"score":1,"sources":[{"addIn":"library","score":1,"reason":"same artist"}]}],"sources":[{"addIn":"library","tracks":1}],"failures":[{"addIn":"slow","error":"timeout","message":"secret"}]}""");
        }));
        var vm = new CollectionsViewModel(a => a(api, Ct), () => "living") { PendingMedia = new("Song", "", Track: Track) };
        vm.RecommendationsRequested += () => vm.Page = "Recommendations";
        await vm.MoreLikeCommand.ExecuteAsync("pending");
        Assert.Equal("library: same artist", Assert.Single(vm.Recommendations).Explanation);
        Assert.Contains("slow: Timed out", vm.RecommendationStatus); Assert.DoesNotContain("secret", vm.RecommendationStatus);
        Assert.Contains("\"track\":", bodies[0]); Assert.DoesNotContain("playlist", bodies[0]);
        await api.RecommendAsync(null, 7, Ct); Assert.Equal("{\"playlist\":7,\"limit\":20}", bodies[1]);
    }

    [Theory]
    [InlineData("Playlists", "now", "playlist")]
    [InlineData("Favourites", "next", "favorite")]
    [InlineData("Recommendations", "end", "tracks")]
    public async Task Collection_queue_actions_send_references_and_play_only_after_replace(string page, string placement, string field)
    {
        var paths = new List<string>(); var bodies = new List<string?>();
        using var api = new MultiroomClient("http://fake", handler: new FakeHttp(async (r, ct) =>
        {
            paths.Add(r.RequestUri!.AbsolutePath); bodies.Add(r.Content is null ? null : await r.Content.ReadAsStringAsync(ct));
            return r.Method == HttpMethod.Get ? FakeHttp.Json("[]") : FakeHttp.Json("{\"refused\":[{}]}");
        }));
        var vm = new CollectionsViewModel(a => a(api, Ct), () => "living / room")
        { Page = page, SelectedPlaylist = Playlist, SelectedFavorite = new(9, DateTimeOffset.UnixEpoch, Track), SelectedRecommendation = new(Track, 1, []) };
        paths.Clear(); bodies.Clear(); await vm.QueueCommand.ExecuteAsync(placement);
        Assert.Equal("/api/v1/zones/living%20%2F%20room/queue/items", paths[0]);
        using var body = JsonDocument.Parse(bodies[0]!);
        Assert.True(body.RootElement.TryGetProperty(field, out _));
        Assert.Equal(placement == "next", body.RootElement.GetProperty("next").GetBoolean());
        Assert.Equal(placement == "now", body.RootElement.GetProperty("replace").GetBoolean());
        Assert.Equal(placement == "now" ? 2 : 1, paths.Count);
        if (placement == "now") { Assert.EndsWith("/play", paths[1]); Assert.Null(bodies[1]); }
        Assert.Contains("1 items refused", vm.Notice);
    }

    [Fact]
    public async Task New_requests_are_cancellable_and_malformed_recommendations_are_safe_errors()
    {
        using var stop = new CancellationTokenSource(); stop.Cancel();
        using var api = new MultiroomClient("http://fake", handler: new FakeHttp((_, ct) =>
        { ct.ThrowIfCancellationRequested(); return Task.FromResult(FakeHttp.Json("{}")); }));
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => api.PlaylistsAsync(stop.Token));
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => api.RecommendAsync(Track, null, stop.Token));
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => api.PatchRoomAsync("room", new(Volume: 0.5), stop.Token));
        Assert.Equal("invalidResponse", (await Assert.ThrowsAsync<ApiException>(() => api.RecommendAsync(Track, null, Ct))).Code);
    }
}
