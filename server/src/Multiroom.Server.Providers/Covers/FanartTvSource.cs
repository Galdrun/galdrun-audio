using System.Globalization;
using System.Text.Json;
using Multiroom.Server.Abstractions;
using static Multiroom.Server.Providers.Covers.ServiceJson;

namespace Multiroom.Server.Providers.Covers;

// fanart.tv's music API (v3), by MusicBrainz ids only: an album's covers
// (`music/albums/<release group>`: albumcover) and an artist's thumbnails
// (`music/<artist>`: artistthumb), each most liked first. Its images are
// made for players, 1000 px square, so it is asked before the Cover Art
// Archive and Wikidata. It takes a project key (artwork.fanartTvKey), which
// goes in every request's query and so in no message: a failure names the
// host only (ServiceJson).
public sealed class FanartTvSource(HttpClient http, string apiKey, Uri? api = null) : IArtworkSource
{
    public const string SourceId = "fanarttv";

    public static Uri DefaultApi { get; } = new("https://webservice.fanart.tv/v3/");

    private readonly Uri _api = api ?? DefaultApi;

    public string Id => SourceId;

    public async Task<Result<IReadOnlyList<Uri>, ProviderError>> CoversAsync(AlbumQuery album,
        CancellationToken cancellationToken)
    {
        if (MusicBrainzSource.Mbid(album.ReleaseGroupId) is not { } group)
            return Array.Empty<Uri>();
        var answer = await GetAsync(http, Uri($"music/albums/{group}"), cancellationToken).ConfigureAwait(false);
        if (!answer.IsSuccess)
            return answer.Failure;
        using var document = answer.Value;
        return document is not null && document.RootElement.TryGetProperty("albums", out var albums) &&
            albums.ValueKind == JsonValueKind.Object && albums.TryGetProperty(group, out var found)
            ? Best(found, "albumcover")
            : [];
    }

    public async Task<Result<IReadOnlyList<Uri>, ProviderError>> PicturesAsync(string artistId,
        CancellationToken cancellationToken)
    {
        if (MusicBrainzSource.Mbid(artistId) is not { } id)
            return Array.Empty<Uri>();
        var answer = await GetAsync(http, Uri($"music/{id}"), cancellationToken).ConfigureAwait(false);
        if (!answer.IsSuccess)
            return answer.Failure;
        using var document = answer.Value;
        return document is null ? [] : Best(document.RootElement, "artistthumb");
    }

    private Uri Uri(string path) => new(_api, $"{path}?api_key={System.Uri.EscapeDataString(apiKey)}");

    // The images of a kind, most liked first; only http(s) URLs.
    private static List<Uri> Best(JsonElement holder, string kind) =>
    [
        .. Items(holder, kind)
            .Select(i => (Url: String(i, "url"),
                Likes: int.TryParse(String(i, "likes"), NumberStyles.Integer, CultureInfo.InvariantCulture, out var n) ? n : 0))
            .Where(i => System.Uri.TryCreate(i.Url, UriKind.Absolute, out var u) && u.Scheme is "http" or "https")
            .OrderByDescending(i => i.Likes)
            .Select(i => new Uri(i.Url!)),
    ];
}
