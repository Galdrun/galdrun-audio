using System.Collections.Concurrent;
using System.Globalization;
using System.Net;
using Microsoft.Extensions.DependencyInjection;
using Multiroom.Server.Abstractions;
using Multiroom.Server.Library;

namespace Multiroom.Server.Providers.WebRadio;

public static class WebRadioServiceCollectionExtensions
{
    // The library (AddLibrary) must be registered too: the stations are in
    // its database. `directory` holds the logo cache: <dataDirectory>/webradio.
    public static IServiceCollection AddWebRadio(this IServiceCollection services, string directory) =>
        services.AddProvider(WebRadioProvider.ProviderId,
            s => new WebRadioProvider(s.GetRequiredService<StationStore>(), Path.Combine(directory, "artwork")));
}

// Internet radio: the stations of the StationStore, searched and browsed (by
// name and by genre) like any provider's items, and each resolved to a live
// relay connected at stage time (StationRelay). A station's ProviderItemId is "station:<id>". A
// station with a logo has artwork, referenced by its ProviderItemId: the logo
// is fetched once and kept in a bounded cache (ArtworkCache).
public sealed class WebRadioProvider : IProvider, ISearchProvider, IBrowseProvider, ILiveStreamResolver,
    IArtworkProvider, IDisposable
{
    public const string ProviderId = "webradio";
    public const string StationPrefix = "station:";
    public const string StationsNode = "stations";
    public const string GenrePrefix = "genre:";

    // 8 s of a 128 kbit/s station, 3 s of a 320 kbit/s one.
    public const int DefaultBufferBytes = 128 * 1024;

    // Logos are small; a few hundred of them fit.
    public const long ArtworkCacheBytes = 16 * 1024 * 1024;

    private static readonly TimeSpan ConnectTimeout = TimeSpan.FromSeconds(5);

    private readonly StationStore _store;
    private readonly int _bufferBytes;
    private readonly HttpClient _http;
    private readonly ConcurrentDictionary<StationRelay, byte> _relays = new();
    private readonly ArtworkCache _artwork;

    public WebRadioProvider(StationStore store, string artworkDirectory, int bufferBytes = DefaultBufferBytes)
    {
        ArgumentOutOfRangeException.ThrowIfLessThan(bufferBytes, StationRelay.ReadSize);
        _store = store;
        _bufferBytes = bufferBytes;
        _http = new HttpClient(new SocketsHttpHandler
        {
            // Nothing on the media path is compressed or decompressed: no
            // Accept-Encoding goes upstream, and the bytes go on as they came.
            AutomaticDecompression = DecompressionMethods.None,
            ConnectTimeout = ConnectTimeout,
        })
        {
            // The body never ends. ConnectAsync bounds the wait for the headers.
            Timeout = Timeout.InfiniteTimeSpan,
        };
        _http.DefaultRequestHeaders.UserAgent.ParseAdd(Branding.ServerName);
        _artwork = new ArtworkCache(artworkDirectory, ArtworkCacheBytes, _http);
    }

    public string Id => ProviderId;

    public string DisplayName => "Web radio";

    public ProviderCapabilities Capabilities =>
        ProviderCapabilities.Search | ProviderCapabilities.Browse | ProviderCapabilities.LiveStream |
        ProviderCapabilities.Artwork;

    // Each relay's jitter buffer holds at most this.
    public long BufferBound => _bufferBytes + StationRelay.ReadSize;

    // What the open relays hold now, each relay's buffer in turn.
    public IReadOnlyList<long> Buffered => [.. _relays.Keys.Select(r => r.Buffered)];

    // Rule 6's status fields for the jitter buffers and the logo cache.
    public Task<ProviderStatus> GetStatusAsync(CancellationToken cancellationToken)
    {
        var buffered = Buffered;
        var (logos, bytes) = _artwork.Held;
        return Task.FromResult(new ProviderStatus(ProviderState.Ready,
            $"{buffered.Count} relay(s) holding {buffered.Sum()} byte(s); at most {BufferBound} each; " +
            $"artwork {logos} image(s), {bytes} of {_artwork.Capacity} bytes"));
    }

    // The title the station's stream last announced (ICY StreamTitle), while
    // a relay of it is open. For the UI only: the rooms keep the station name.
    public string? CurrentTitle(string stationItemId) =>
        _relays.Keys.FirstOrDefault(r => r.ItemId == stationItemId && r.Title is not null)?.Title;

    public Task<Result<CatalogItems, ProviderError>> SearchAsync(string text, SearchKinds kinds, int offset,
        int limit, CancellationToken cancellationToken)
    {
        if (!kinds.HasFlag(SearchKinds.Stations))
            return Task.FromResult<Result<CatalogItems, ProviderError>>(CatalogItems.Empty);
        var found = _store.Search(text, offset, limit);
        return Task.FromResult<Result<CatalogItems, ProviderError>>(found.IsSuccess
            ? CatalogItems.Empty with { Stations = [.. found.Value.Select(ToStation)] }
            : ToProviderError(found.Failure));
    }

    // By name and by genre (phase 14): the root holds "stations" (every
    // station) and a "genre:<name>" node per genre; each lists its stations
    // by name, filtered by text and indexed by letter. A list of stations is
    // chosen from, not queued as a whole.
    public Task<Result<BrowsePage, ProviderError>> BrowseAsync(BrowseRequest request,
        CancellationToken cancellationToken) => Task.FromResult(Browse(request));

    private Result<BrowsePage, ProviderError> Browse(BrowseRequest r)
    {
        if (r.NodeId is null)
        {
            if (r.Sort is not null || r.Filter is not null || r.Letter is not null)
                return Result.Fail(ProviderError.InvalidArgument, "the root takes no sort, filter or letter");
            var genres = _store.Genres();
            if (!genres.IsSuccess)
                return ToProviderError(genres.Failure);
            List<BrowseNode> nodes =
            [
                Node(StationsNode, "All stations", BrowseKind.Stations),
                .. genres.Value.OrderBy(g => g.Genre, BrowseIndex.Order).Select(g =>
                    Node(GenrePrefix + g.Genre, g.Genre, BrowseKind.Genre, $"{g.Stations} station{(g.Stations == 1 ? "" : "s")}")),
            ];
            return new BrowsePage([.. nodes.Skip(r.Offset).Take(r.Limit)], CatalogItems.Empty, Total: nodes.Count);
        }

        string? genre = null;
        BrowseNode header;
        if (r.NodeId == StationsNode)
        {
            header = Node(StationsNode, "All stations", BrowseKind.Stations);
        }
        else if (r.NodeId.StartsWith(GenrePrefix, StringComparison.Ordinal) && r.NodeId.Length > GenrePrefix.Length)
        {
            genre = r.NodeId[GenrePrefix.Length..];
            header = Node(r.NodeId, genre, BrowseKind.Genre);
        }
        else
        {
            return Result.Fail(ProviderError.NotFound, $"no node {r.NodeId}");
        }

        if (r.Sort is not (null or "name"))
            return Result.Fail(ProviderError.InvalidArgument, $"{r.NodeId} sorts by name, not {r.Sort}");
        if (r.Letter is { } letter && (!BrowseIndex.IsLetter(letter) || r.Offset != 0))
            return Result.Fail(ProviderError.InvalidArgument, $"letter: one letter or #, without an offset; was {letter}");
        var all = _store.All(genre);
        if (!all.IsSuccess)
            return ToProviderError(all.Failure);
        if (genre is not null && all.Value.Count == 0)
            return Result.Fail(ProviderError.NotFound, $"no genre {genre}");

        var ordered = all.Value.OrderBy(s => s.Name, BrowseIndex.Order).ThenBy(s => s.Id)
            .Where(s => r.Filter is null || BrowseIndex.Matches(r.Filter, s.Name, s.Genre)).ToList();
        var letters = BrowseIndex.Of(ordered.Select(s => s.Name));
        var offset = r.Letter is { } jump ? BrowseIndex.OffsetOf(letters, jump, ordered.Count) : r.Offset;
        return new BrowsePage([], CatalogItems.Empty with { Stations = [.. ordered.Skip(offset).Take(r.Limit).Select(ToStation)] },
            header, ordered.Count, Sorts: ["name"], Sort: "name", Filterable: true, Letters: letters);
    }

    private static BrowseNode Node(string id, string title, BrowseKind kind, string? subtitle = null) =>
        new(ProviderId, id, title, Kind: kind, Subtitle: subtitle, Unplayable: "a station list is chosen from, not queued");

    public async Task<Result<ResolvedStream, ProviderError>> ResolveAsync(RadioStation station,
        CancellationToken cancellationToken)
    {
        var itemId = station.ProviderItemId;
        if (IdOf(itemId) is not { } id)
            return Result.Fail(ProviderError.NotFound, $"no station {itemId}");
        var stored = _store.Station(id);
        if (!stored.IsSuccess)
            return ToProviderError(stored.Failure);

        var connected = await StationRelay.ConnectAsync(_http, itemId, stored.Value.Name, stored.Value.StreamUrl,
            _bufferBytes, ConnectTimeout, r => _relays.TryRemove(r, out _), cancellationToken).ConfigureAwait(false);
        if (!connected.IsSuccess)
            return connected.Failure;
        var relay = connected.Value;
        _relays[relay] = 0;
        return ResolvedStream.Live(relay.ContentType, relay.Open, relay.Release);
    }

    // The logo of the station the reference names, fetched once. The key is
    // the station and its logo URL, so a changed logo is fetched anew.
    public async Task<Result<Artwork, ProviderError>> GetArtworkAsync(ArtworkRef reference,
        CancellationToken cancellationToken)
    {
        if (reference.ProviderId != ProviderId || IdOf(reference.Reference) is not { } id)
            return Result.Fail(ProviderError.NotFound, $"no artwork {reference.Reference}");
        var stored = _store.Station(id);
        if (!stored.IsSuccess)
            return ToProviderError(stored.Failure);
        if (stored.Value.Logo is not { } logo)
            return Result.Fail(ProviderError.NotFound, $"{stored.Value.Name} has no logo");
        return await _artwork.GetAsync($"{reference.Reference} {logo}", logo, stored.Value.Name, cancellationToken)
            .ConfigureAwait(false);
    }

    public void Dispose()
    {
        foreach (var relay in _relays.Keys)
            relay.Dispose();
        _http.Dispose();
    }

    // The domain station of a stored one: no stream URL, which stays here.
    public static RadioStation ToStation(StoredStation s)
    {
        var itemId = StationPrefix + s.Id.ToString(CultureInfo.InvariantCulture);
        return new(ProviderId, itemId, s.Name, s.Genre, s.Homepage,
            s.Logo is null ? null : new ArtworkRef(ProviderId, itemId));
    }

    // The store id of "station:<id>", or null.
    public static long? IdOf(string itemId) =>
        itemId.StartsWith(StationPrefix, StringComparison.Ordinal) &&
        long.TryParse(itemId.AsSpan(StationPrefix.Length), NumberStyles.None, CultureInfo.InvariantCulture, out var id)
            ? id
            : null;

    private static Failure<ProviderError> ToProviderError(Failure<LibraryError> failure) =>
        Result.Fail(failure.Code == LibraryError.NotFound ? ProviderError.NotFound : ProviderError.Failed, failure.Detail);
}
