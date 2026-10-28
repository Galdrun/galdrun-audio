using System.Text.Json;

namespace Multiroom.App.Api;

public sealed partial class MultiroomClient
{
    private async Task<T> SendTypedAsync<T>(HttpMethod method, string path, object body, CancellationToken ct) where T : class
    {
        var result = await SendAsync(method, path, body, ct).ConfigureAwait(false);
        try { return result?.Deserialize<T>(Json) ?? throw new JsonException(); }
        catch (JsonException) { throw new ApiException(System.Net.HttpStatusCode.OK, "invalidResponse", "The server returned an unexpected response."); }
    }
    public Task<IReadOnlyList<Playlist>> PlaylistsAsync(CancellationToken ct) => GetAsync<IReadOnlyList<Playlist>>("playlists", ct);
    public Task<Playlist> CreatePlaylistAsync(string name, CancellationToken ct) =>
        SendTypedAsync<Playlist>(HttpMethod.Post, "playlists", new { name }, ct);
    public Task<Playlist> PatchPlaylistAsync(long id, string? name, int? position, CancellationToken ct) =>
        SendTypedAsync<Playlist>(HttpMethod.Patch, $"playlists/{id}", new { name, position }, ct);
    public Task<JsonElement?> DeletePlaylistAsync(long id, CancellationToken ct) => SendAsync(HttpMethod.Delete, $"playlists/{id}", null, ct);
    public Task<IReadOnlyList<PlaylistEntry>> EntriesAsync(long id, CancellationToken ct) => GetAsync<IReadOnlyList<PlaylistEntry>>($"playlists/{id}/entries", ct);
    public Task<JsonElement?> AddEntriesAsync(long id, IReadOnlyList<Track> tracks, CancellationToken ct) =>
        SendAsync(HttpMethod.Post, $"playlists/{id}/entries", new { tracks }, ct);
    public Task<JsonElement?> MoveEntryAsync(long id, long entry, int index, CancellationToken ct) =>
        SendAsync(HttpMethod.Patch, $"playlists/{id}/entries/{entry}", new { index }, ct);
    public Task<JsonElement?> RemoveEntryAsync(long id, long entry, CancellationToken ct) =>
        SendAsync(HttpMethod.Delete, $"playlists/{id}/entries/{entry}", null, ct);
    public Task<IReadOnlyList<Favorite>> FavoritesAsync(CancellationToken ct) => GetAsync<IReadOnlyList<Favorite>>("favorites", ct);
    public Task<Favorite> AddFavoriteAsync(Track? track, RadioStation? station, CancellationToken ct) =>
        SendTypedAsync<Favorite>(HttpMethod.Post, "favorites", new { track, station }, ct);
    public Task<JsonElement?> RemoveFavoriteAsync(long id, CancellationToken ct) => SendAsync(HttpMethod.Delete, $"favorites/{id}", null, ct);
    public Task<IReadOnlyList<LikedItem>> LikesAsync(CancellationToken ct) => GetAsync<IReadOnlyList<LikedItem>>("likes", ct);
    public Task<ProposalsView> ProposalsAsync(CancellationToken ct) => GetAsync<ProposalsView>("ai/proposals", ct);
    public Task<HistoryPage> HistoryAsync(int limit, CancellationToken ct) => GetAsync<HistoryPage>($"history?limit={limit}", ct);
    public Task<IReadOnlyList<ItemStatus>> ItemStatusesAsync(IReadOnlyList<ItemRef> items, CancellationToken ct) =>
        SendTypedAsync<IReadOnlyList<ItemStatus>>(HttpMethod.Post, "items/status", new { items }, ct);
    public Task<LikedItem> AddLikeAsync(Track track, CancellationToken ct) =>
        SendTypedAsync<LikedItem>(HttpMethod.Post, "likes", new { track }, ct);
    public Task<LikedItem> AddRadioLikeAsync(Track track, CancellationToken ct) =>
        SendTypedAsync<LikedItem>(HttpMethod.Post, "likes", new { metadata = new { track.Title, track.Artist, track.Album } }, ct);
    public Task<JsonElement?> RemoveLikeAsync(long id, CancellationToken ct) =>
        SendAsync(HttpMethod.Delete, $"likes/{id}", null, ct);
    public Task<RecommendationAnswer> RecommendAsync(Track? track, long? playlist, CancellationToken ct) =>
        SendTypedAsync<RecommendationAnswer>(HttpMethod.Post, "recommendations", new { track, playlist, limit = 20 }, ct);
    public Task<JsonElement?> PatchRoomAsync(string client, RoomPatch patch, CancellationToken ct) =>
        SendAsync(HttpMethod.Patch, $"rooms/{Escape(client)}", patch, ct);
    public Task<JsonElement?> RejoinRoomAsync(string client, CancellationToken ct) =>
        SendAsync(HttpMethod.Post, $"rooms/{Escape(client)}/rejoin", null, ct);
    // A room that is not connected, gone for good (server phase 39).
    public Task<JsonElement?> ForgetRoomAsync(string client, CancellationToken ct) =>
        SendAsync(HttpMethod.Delete, $"rooms/{Escape(client)}", null, ct);
}
