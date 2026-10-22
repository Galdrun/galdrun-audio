using System.Net;
using System.Net.Http.Headers;
using System.Net.Http.Json;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace Multiroom.App.Api;

public sealed class ApiException(HttpStatusCode status, string code, string message) : Exception(message)
{
    public HttpStatusCode Status { get; } = status;
    public string Code { get; } = code;
}

// A client is one immutable connection configuration. Replace the whole session on address/token changes.
public sealed partial class MultiroomClient : IDisposable
{
    public static readonly JsonSerializerOptions Json = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        RespectNullableAnnotations = true,
        RespectRequiredConstructorParameters = true,
        DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull,
        // A list entry's item names its kind; where it does, it does not matter.
        AllowOutOfOrderMetadataProperties = true,
        Converters = { new JsonStringEnumConverter(JsonNamingPolicy.CamelCase, false) }
    };
    private readonly HttpClient _http;
    private readonly string? _token;
    public Uri BaseUri { get; }

    public MultiroomClient(string address, string? token = null, HttpMessageHandler? handler = null)
    {
        BaseUri = NormalizeAddress(address);
        if (token?.Any(char.IsControl) == true) throw new ArgumentException("The API token contains invalid control characters.");
        _token = string.IsNullOrWhiteSpace(token) ? null : token;
        _http = new HttpClient(handler ?? new HttpClientHandler { AllowAutoRedirect = false }, true)
        { Timeout = Timeout.InfiniteTimeSpan };
    }

    public static Uri NormalizeAddress(string address)
    {
        if (!Uri.TryCreate(address.Trim(), UriKind.Absolute, out var uri) ||
            uri.Scheme is not ("http" or "https") || uri.UserInfo.Length != 0 ||
            uri.Query.Length != 0 || uri.Fragment.Length != 0 ||
            uri.Port is >= 4711 and <= 4714 || uri.AbsolutePath.TrimEnd('/') is not ("" or "/api/v1"))
            throw new ArgumentException("Enter the high-end server HTTP(S) address, using its outward API port (normally 4725).");
        return new Uri(uri.GetLeftPart(UriPartial.Authority) + "/api/v1/");
    }

    public static string Escape(string value) => Uri.EscapeDataString(value);
    internal HttpRequestMessage Request(HttpMethod method, string path, object? body = null)
    {
        if (path.StartsWith('/') || path.Contains("://", StringComparison.Ordinal) || path.Split('?')[0].Split('/').Contains(".."))
            throw new ArgumentException("Only relative API routes are accepted.");
        var request = new HttpRequestMessage(method, new Uri(BaseUri, path));
        if (_token is not null) request.Headers.Authorization = new AuthenticationHeaderValue("Bearer", _token);
        if (body is not null) request.Content = JsonContent.Create(body, body.GetType(), options: Json);
        return request;
    }

    internal async Task<HttpResponseMessage> OpenAsync(HttpRequestMessage request, CancellationToken ct)
    {
        var response = await _http.SendAsync(request, HttpCompletionOption.ResponseHeadersRead, ct).ConfigureAwait(false);
        if (response.IsSuccessStatusCode) return response;
        using (response)
        {
            ApiError? error = null;
            try { error = await response.Content.ReadFromJsonAsync<ApiError>(Json, ct).ConfigureAwait(false); }
            catch (JsonException) { }
            // Do not display server messages that may echo user-entered secrets (validation can echo input).
            var message = response.StatusCode switch
            {
                HttpStatusCode.Unauthorized => "The server requires a valid API token.",
                HttpStatusCode.Forbidden => "Access was refused. Check the account credentials.",
                HttpStatusCode.NotFound => "This item no longer exists. Refresh the page.",
                HttpStatusCode.Gone => "This media is no longer available.",
                HttpStatusCode.ServiceUnavailable => "The server or source is temporarily unavailable. Try again.",
                HttpStatusCode.BadRequest => "The server rejected the supplied values. Check the form and try again.",
                _ => "The server could not complete the request."
            };
            throw new ApiException(response.StatusCode, error?.Error ?? "httpError", message);
        }
    }

    public async Task<T> GetAsync<T>(string path, CancellationToken ct = default)
    {
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(ct);
        timeout.CancelAfter(TimeSpan.FromSeconds(30));
        using var request = Request(HttpMethod.Get, path);
        using var response = await OpenAsync(request, timeout.Token).ConfigureAwait(false);
        try { return await response.Content.ReadFromJsonAsync<T>(Json, timeout.Token).ConfigureAwait(false)
            ?? throw new JsonException("Empty response."); }
        catch (JsonException) { throw new ApiException(response.StatusCode, "invalidResponse", "The server returned an unexpected response."); }
    }

    internal async Task<JsonElement?> SendAsync(HttpMethod method, string path, object? body, CancellationToken ct)
    {
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(ct);
        timeout.CancelAfter(TimeSpan.FromSeconds(30));
        using var request = Request(method, path, body);
        using var response = await OpenAsync(request, timeout.Token).ConfigureAwait(false);
        if (response.StatusCode == HttpStatusCode.NoContent) return null;
        try { return await response.Content.ReadFromJsonAsync<JsonElement>(Json, timeout.Token).ConfigureAwait(false); }
        catch (JsonException) { throw new ApiException(response.StatusCode, "invalidResponse", "The server returned an unexpected response."); }
    }

    public Task<ServerStatus> StatusAsync(CancellationToken ct) => GetAsync<ServerStatus>("status", ct);
    public Task<IReadOnlyList<ProviderView>> ProvidersAsync(CancellationToken ct) => GetAsync<IReadOnlyList<ProviderView>>("providers", ct);
    public Task<IReadOnlyList<ZoneView>> ZonesAsync(CancellationToken ct) => GetAsync<IReadOnlyList<ZoneView>>("zones", ct);
    public Task<IReadOnlyList<RoomView>> RoomsAsync(CancellationToken ct) => GetAsync<IReadOnlyList<RoomView>>("rooms", ct);
    public Task<BrowsePage> BrowseAsync(string provider, string? node, int offset, string? sort, string? filter,
        string? letter, CancellationToken ct) => GetAsync<BrowsePage>($"providers/{Escape(provider)}/browse?limit=60&offset={offset}" +
            (node is null ? "" : $"&node={Escape(node)}") + (sort is null ? "" : $"&sort={Escape(sort)}") +
            (string.IsNullOrEmpty(filter) ? "" : $"&filter={Escape(filter)}") + (letter is null ? "" : $"&letter={Escape(letter)}"), ct);
    public Task<SearchResult> SearchAsync(string query, int offset, CancellationToken ct) =>
        GetAsync<SearchResult>($"search?q={Escape(query)}&offset={offset}&limit=60", ct);
    public Task<JsonElement?> TransportAsync(string zone, string action, long? item, CancellationToken ct)
    {
        if (action is not ("play" or "pause" or "stop" or "previous" or "next")) throw new ArgumentException("Unknown transport action.");
        return SendAsync(HttpMethod.Post, $"zones/{Escape(zone)}/{action}", action == "play" && item is not null ? new { item } : null, ct);
    }
    public Task<JsonElement?> SeekAsync(string zone, long item, long positionMs, CancellationToken ct) =>
        SendAsync(HttpMethod.Post, $"zones/{Escape(zone)}/seek", new { item, positionMs }, ct);
    public Task<JsonElement?> QueueAsync(string zone, AddItems items, CancellationToken ct) =>
        SendAsync(HttpMethod.Post, $"zones/{Escape(zone)}/queue/items", items, ct);
    public Task<JsonElement?> QueueModeAsync(string zone, RepeatMode? repeat, bool? shuffle, CancellationToken ct) =>
        SendAsync(HttpMethod.Patch, $"zones/{Escape(zone)}/queue", new { repeat, shuffle }, ct);
    public Task<JsonElement?> MoveQueueAsync(string zone, long item, int index, CancellationToken ct) =>
        SendAsync(HttpMethod.Patch, $"zones/{Escape(zone)}/queue/items/{item}", new { index }, ct);
    public Task<JsonElement?> RemoveQueueAsync(string zone, long item, CancellationToken ct) =>
        SendAsync(HttpMethod.Delete, $"zones/{Escape(zone)}/queue/items/{item}", null, ct);
    public Task<JsonElement?> SetClientsAsync(string zone, IReadOnlyList<string> clients, CancellationToken ct) =>
        SendAsync(HttpMethod.Put, $"zones/{Escape(zone)}/clients", new { clients }, ct);
    public Task<JsonElement?> DeleteZoneAsync(string zone, CancellationToken ct) =>
        SendAsync(HttpMethod.Delete, $"zones/{Escape(zone)}", null, ct);
    public Task<JsonElement?> SetRoomVolumeAsync(string client, double volume, CancellationToken ct) =>
        SendAsync(HttpMethod.Patch, $"rooms/{Escape(client)}", new { volume }, ct);
    public Task<ZoneView> SetZoneVolumeAsync(string zone, double? volume, bool? muted, CancellationToken ct) =>
        SendTypedAsync<ZoneView>(HttpMethod.Patch, $"zones/{Escape(zone)}", new { volume, muted }, ct);
    public void Dispose() => _http.Dispose();
}
