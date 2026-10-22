namespace Multiroom.Server.Providers.Tidal;

// The quality tiers Tidal's playback API names (LOW, HIGH, LOSSLESS,
// HI_RES_LOSSLESS). A track not offered in the tier asked for comes in the
// best tier below it.
public enum TidalQuality
{
    Low,
    High,
    Lossless,
    HiResLossless,
}

// providers.tidal, resolved. A class, not a record: a record's ToString would
// print the client secrets (rule 7). Without a client id, login is unavailable.
//
// The client signs in by device code. The optional playback client, when
// set, refreshes the tokens instead.
public sealed class TidalSettings(string? clientId, string? clientSecret, TidalQuality quality, long cacheBytes,
    long artworkCacheBytes, string? playbackClientId = null, string? playbackClientSecret = null)
{
    public const long DefaultCacheBytes = 2L * 1024 * 1024 * 1024;
    public const long MinimumCacheBytes = 256L * 1024 * 1024;
    public const long DefaultArtworkCacheBytes = 64L * 1024 * 1024;
    public const long MinimumArtworkCacheBytes = 1024 * 1024;

    public string? ClientId { get; } = clientId;

    public string? ClientSecret { get; } = clientSecret;

    public string? PlaybackClientId { get; } = playbackClientId;

    public string? PlaybackClientSecret { get; } = playbackClientSecret;

    public TidalQuality Quality { get; } = quality;

    // The reassembled tracks, least recently played evicted first.
    public long CacheBytes { get; } = cacheBytes;

    public long ArtworkCacheBytes { get; } = artworkCacheBytes;
}

// Where Tidal is. Only a test points these anywhere else.
public sealed record TidalEndpoints(Uri Auth, Uri Api, Uri Images)
{
    public static TidalEndpoints Default { get; } = new(
        new Uri("https://auth.tidal.com/v1/"),
        new Uri("https://api.tidal.com/v1/"),
        new Uri("https://resources.tidal.com/images/"));
}
