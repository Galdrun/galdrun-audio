using System.Globalization;
using System.Net;
using System.Text.Json;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Logging;
using Multiroom.Server.Abstractions;

namespace Multiroom.Server.Providers.Tidal;

public static class TidalServiceCollectionExtensions
{
    // `directory` holds the tokens and the caches: <dataDirectory>/tidal.
    // `client`, when it answers one, is the app set through the API, used
    // instead of the config's.
    public static IServiceCollection AddTidal(this IServiceCollection services, TidalSettings settings,
        string directory) =>
        services.AddProvider(TidalProvider.ProviderId, s =>
        {
            var provider = new TidalProvider(settings, directory,
                s.GetRequiredService<ILoggerFactory>().CreateLogger<TidalProvider>());
            provider.Start();
            return provider;
        });
}

// Tidal, through the API its own apps use (api.tidal.com/v1): search, the
// media browser (Tidal's own pages, the account's collection, artist pages)
// and queueing a container whole, tracks reassembled into a bounded cache and
// served with their length, and cover art. The account is linked
// by device authorization (TidalSession); until it is, the status is AuthRequired
// with the URL to open, and every call is Unavailable.
//
// Ids: a track is "track:<id>", an album "album:<id>", an artist
// "artist:<id>" and a playlist "playlist:<uuid>"; the media browser's nodes
// are in TidalProvider.Browse.cs. Artwork references are Tidal's image ids.
public sealed partial class TidalProvider : IProvider, ISearchProvider, IBrowseProvider, IStreamResolver,
    IArtworkProvider, IContainerExpander, IDisposable
{
    public const string ProviderId = "tidal";
    public const string TrackPrefix = "track:";
    public const string AlbumPrefix = "album:";
    public const string ArtistPrefix = "artist:";
    public const string PlaylistPrefix = "playlist:";

    // Cover art is asked for at this size; Tidal serves square JPEGs.
    private const string ArtworkSize = "640x640";

    private static readonly TimeSpan RequestTimeout = TimeSpan.FromSeconds(30);

    // What Tidal's search names each kind it holds; it holds no stations.
    private static readonly (SearchKinds Kind, string Name)[] SearchTypes =
    [
        (SearchKinds.Artists, "ARTISTS"), (SearchKinds.Albums, "ALBUMS"), (SearchKinds.Tracks, "TRACKS"),
        (SearchKinds.Playlists, "PLAYLISTS"),
    ];

    private readonly TidalSettings _settings;
    private readonly TidalEndpoints _endpoints;
    private readonly HttpClient _http;
    private readonly ILogger _log;
    private readonly TrackCache _tracks;
    private readonly ArtworkCache _artwork;
    private readonly TidalSession _session;
    private volatile string? _granted;
    private int _disposed;

    public TidalProvider(TidalSettings settings, string directory, ILogger log, TidalEndpoints? endpoints = null)
    {
        _settings = settings;
        _endpoints = endpoints ?? TidalEndpoints.Default;
        _http = new HttpClient(new SocketsHttpHandler
        {
            // The audio goes on as it came: nothing on the media path is
            // compressed or decompressed.
            AutomaticDecompression = DecompressionMethods.None,
            ConnectTimeout = TimeSpan.FromSeconds(10),
        })
        {
            Timeout = RequestTimeout,
        };
        _http.DefaultRequestHeaders.UserAgent.ParseAdd(Branding.ServerName);
        _log = log;
        _session = new TidalSession(_http, settings, _endpoints, Path.Combine(directory, "token.json"), log);
        _tracks = new TrackCache(Path.Combine(directory, "tracks"), settings.CacheBytes, _http, log);
        _artwork = new ArtworkCache(Path.Combine(directory, "artwork"), settings.ArtworkCacheBytes, _http);
    }

    public string Id => ProviderId;

    public string DisplayName => "Tidal";

    public ProviderCapabilities Capabilities => ProviderCapabilities.Search | ProviderCapabilities.Browse |
        ProviderCapabilities.Stream | ProviderCapabilities.Artwork | ProviderCapabilities.Expand;

    // Reads the kept account link, or waits for a device login.
    public void Start() => _session.Start();

    public TidalLoginStart? Login => _session.Login;

    public Task<Result<TidalLoginStart, ProviderError>> StartLoginAsync(CancellationToken cancellationToken) =>
        _session.StartLoginAsync(cancellationToken);

    public Task<ProviderStatus> GetStatusAsync(CancellationToken cancellationToken)
    {
        var (tracks, bytes, inUse) = _tracks.Held;
        var (images, imageBytes) = _artwork.Held;
        var caches = $"track cache {tracks} track(s), {inUse} in use, {bytes} of {_tracks.Capacity} bytes; " +
            $"artwork {images} image(s), {imageBytes} of {_artwork.Capacity} bytes";
        var token = _session.Token;
        return Task.FromResult(token is null
            ? new ProviderStatus(ProviderState.AuthRequired, $"{_session.Waiting}; {caches}")
            : new ProviderStatus(ProviderState.Ready,
                $"user {token.UserId} ({token.CountryCode}), quality {_settings.Quality}" +
                (_granted is { } granted && granted != QualityName(_settings.Quality) ? $" (Tidal grants {granted})" : "") +
                $"; {caches}"));
    }

    public async Task<Result<CatalogItems, ProviderError>> SearchAsync(string text, SearchKinds kinds, int offset,
        int limit, CancellationToken cancellationToken)
    {
        var types = string.Join(',', SearchTypes.Where(t => kinds.HasFlag(t.Kind)).Select(t => t.Name));
        if (string.IsNullOrWhiteSpace(text) || types.Length == 0)
            return CatalogItems.Empty;
        var found = await _session.GetAsync("search",
            [("query", text), ("types", types), .. Page(offset, limit)], "search",
            cancellationToken).ConfigureAwait(false);
        if (!found.IsSuccess)
            return found.Failure;
        var json = found.Value;
        IEnumerable<JsonElement> Of(SearchKinds kind, string name) => kinds.HasFlag(kind) ? Items(json.Get(name)) : [];
        return new CatalogItems(
            [.. Of(SearchKinds.Tracks, "tracks").Select(ToTrack).OfType<Track>()],
            [.. Of(SearchKinds.Albums, "albums").Select(ToAlbum).OfType<Album>()],
            [.. Of(SearchKinds.Artists, "artists").Select(ToArtist).OfType<Artist>()],
            [],
            [.. Of(SearchKinds.Playlists, "playlists").Select(PlaylistNode).OfType<BrowseNode>()]);
    }

    // Through the track cache: a track played before, in the quality asked
    // for, is answered from disk without asking Tidal.
    public Task<Result<ResolvedStream, ProviderError>> ResolveAsync(Track track, CancellationToken cancellationToken)
    {
        if (IdOf(track.ProviderItemId, TrackPrefix) is not { } id)
            return Task.FromResult<Result<ResolvedStream, ProviderError>>(
                Result.Fail(ProviderError.NotFound, $"no track {track.ProviderItemId}"));
        var quality = QualityName(_settings.Quality);
        var key = $"{id}-{quality}";
        return _tracks.GetAsync(key, track.Title, ct => ManifestAsync(id, quality, track.Title, ct), cancellationToken);
    }

    public Task<Result<Artwork, ProviderError>> GetArtworkAsync(ArtworkRef reference,
        CancellationToken cancellationToken)
    {
        var id = reference.Reference;
        if (reference.ProviderId != ProviderId || !IsUuid(id))
            return Task.FromResult<Result<Artwork, ProviderError>>(Result.Fail(ProviderError.NotFound, $"no artwork {id}"));
        var url = new Uri(_endpoints.Images, $"{id.Replace('-', '/')}/{ArtworkSize}.jpg");
        return _artwork.GetAsync(id, url, id, cancellationToken);
    }

    public void Dispose()
    {
        if (Interlocked.Exchange(ref _disposed, 1) != 0)
            return;
        _tracks.Dispose();
        _session.Dispose();
        _http.Dispose();
    }

    private async Task<Result<TidalManifest, ProviderError>> ManifestAsync(long id, string quality, string name,
        CancellationToken cancellationToken)
    {
        var info = await _session.GetAsync($"tracks/{id}/playbackinfopostpaywall",
            [("audioquality", quality), ("playbackmode", "STREAM"), ("assetpresentation", "FULL")], name,
            cancellationToken).ConfigureAwait(false);
        if (!info.IsSuccess)
            return info.Failure;
        var json = info.Value;
        if (json.Text("assetPresentation") is { } presentation && presentation != "FULL")
            return Result.Fail(ProviderError.Unavailable, $"{name}: Tidal offers it only as {presentation}");
        var manifest = TidalManifest.Parse(json.Text("manifestMimeType"), json.Text("manifest"), name);
        if (!manifest.IsSuccess)
            return manifest;

        // Tidal answers with the tier it grants, which the account, the
        // client id or the track may hold below the one asked for.
        var granted = json.Text("audioQuality");
        if (granted is not null && granted != quality && _granted != granted)
            LogDowngraded(_log, quality, granted, name);
        _granted = granted;
        return manifest.Value with { Grant = GrantOf(granted, json.Number("sampleRate"), json.Number("bitDepth")) };
    }

    // The grant as the cache keeps it in a file name: the tier, and for a
    // lossless tier the sample rate and bit depth when Tidal names them
    // ("HI_RES_LOSSLESS-96000-24").
    private static string GrantOf(string? quality, long? sampleRate, long? bitDepth)
    {
        if (quality is null || !quality.All(c => char.IsAsciiLetterUpper(c) || c == '_'))
            return TidalManifest.UnknownGrant;
        return FormatOf(quality)?.Codec == "FLAC" && sampleRate is > 0 and < 10_000_000 && bitDepth is > 0 and <= 64
            ? $"{quality}-{Invariant(sampleRate.Value)}-{Invariant(bitDepth.Value)}"
            : quality;
    }

    // What a cached file holds, from its grant; null when it is not known.
    internal static AudioFormat? FormatOfGrant(string grant)
    {
        var parts = grant.Split('-');
        var format = FormatOf(parts[0]);
        if (format is null || parts.Length != 3)
            return format;
        return int.TryParse(parts[1], NumberStyles.None, CultureInfo.InvariantCulture, out var rate) &&
               int.TryParse(parts[2], NumberStyles.None, CultureInfo.InvariantCulture, out var bits)
            ? format with { SampleRate = rate, BitDepth = bits }
            : format;
    }

    [LoggerMessage(Level = LogLevel.Warning, Message = "tidal: asked for {Asked}, Tidal grants {Granted} ({Name})")]
    private static partial void LogDowngraded(ILogger log, string asked, string granted, string name);

    private Task<Result<JsonElement, ProviderError>> Get(string path, int offset, int limit, string what,
        CancellationToken cancellationToken) =>
        _session.GetAsync(path, Page(offset, limit), what, cancellationToken);

    private static (string, string)[] Page(int offset, int limit) =>
    [
        ("offset", Math.Max(0, offset).ToString(CultureInfo.InvariantCulture)),
        ("limit", Math.Clamp(limit, 1, 100).ToString(CultureInfo.InvariantCulture)),
    ];

    // A playlist's items may be wrapped ({"item": track, "type": "track"}).
    private static JsonElement PlaylistItem(JsonElement item) => item.Get("item") ?? item;

    private static IEnumerable<JsonElement> Items(JsonElement? page) => page is { } found ? found.Items() : [];

    private static Track? ToTrack(JsonElement json)
    {
        if (json.Number("id") is not { } id || json.Text("title") is not { } title)
            return null;
        if (json.Text("version") is { } version)
            title = $"{title} ({version})";
        var album = json.Get("album");
        var artist = json.Get("artist")?.Text("name") ??
            (json.Get("artists") is { } artists ? artists.EnumerateArray().Select(a => a.Text("name")).FirstOrDefault(n => n is not null) : null);
        // Tidal's app API streams music only: no audiobooks, no episodes
        // (phase 30), and its videos are left out.
        var type = MediaTypeClassifier.Classify(new MediaTypeEvidence(Provider: MediaType.Music));
        return new Track(ProviderId, TrackPrefix + Invariant(id), title, artist, album?.Text("title"),
            json.Number("duration") is { } seconds ? TimeSpan.FromSeconds(seconds) : null,
            ArtworkOf(album?.Text("cover")), FormatOf(json.Text("audioQuality")), type.Type, type.Source);
    }

    // The best tier Tidal holds the track in; it plays in that tier or in
    // providers.tidal.quality, whichever is lower. A hi-res track's sample
    // rate is known only once it is played.
    private static AudioFormat? FormatOf(string? audioQuality) => audioQuality switch
    {
        "LOW" => new AudioFormat("AAC", Channels: 2, Bitrate: 96),
        "HIGH" => new AudioFormat("AAC", Channels: 2, Bitrate: 320),
        "LOSSLESS" => new AudioFormat("FLAC", 44100, 16, 2),
        "HI_RES" or "HI_RES_LOSSLESS" => new AudioFormat("FLAC", BitDepth: 24, Channels: 2),
        _ => null,
    };

    private static Album? ToAlbum(JsonElement json)
    {
        if (json.Number("id") is not { } id || json.Text("title") is not { } title)
            return null;
        var released = json.Text("releaseDate");
        int? year = released is { Length: >= 4 } && int.TryParse(released.AsSpan(0, 4), NumberStyles.None,
            CultureInfo.InvariantCulture, out var y) ? y : null;
        return new Album(ProviderId, AlbumPrefix + Invariant(id), title, json.Get("artist")?.Text("name"), year,
            ArtworkOf(json.Text("cover")));
    }

    private static Artist? ToArtist(JsonElement json) =>
        json.Number("id") is { } id && json.Text("name") is { } name
            ? new Artist(ProviderId, ArtistPrefix + Invariant(id), name, ArtworkOf(json.Text("picture")))
            : null;

    private static ArtworkRef? ArtworkOf(string? imageId) =>
        imageId is not null && IsUuid(imageId) ? new ArtworkRef(ProviderId, imageId) : null;

    private static long? IdOf(string itemId, string prefix) =>
        itemId.StartsWith(prefix, StringComparison.Ordinal) &&
        long.TryParse(itemId.AsSpan(prefix.Length), NumberStyles.None, CultureInfo.InvariantCulture, out var id)
            ? id
            : null;

    // Tidal's playlist and image ids: hex digits and dashes. Anything else
    // never reaches a URL.
    private static bool IsUuid(string text) =>
        text.Length is >= 8 and <= 64 && text.All(c => char.IsAsciiHexDigit(c) || c == '-');

    private static string QualityName(TidalQuality quality) => quality switch
    {
        TidalQuality.Low => "LOW",
        TidalQuality.High => "HIGH",
        TidalQuality.Lossless => "LOSSLESS",
        TidalQuality.HiResLossless => "HI_RES_LOSSLESS",
        _ => throw new ArgumentOutOfRangeException(nameof(quality)),
    };

    private static string Invariant(long id) => id.ToString(CultureInfo.InvariantCulture);
}
