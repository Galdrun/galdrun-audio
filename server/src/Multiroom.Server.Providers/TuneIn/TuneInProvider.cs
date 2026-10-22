using System.Collections.Concurrent;
using System.Net;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using Microsoft.Extensions.DependencyInjection;
using Multiroom.Server.Abstractions;
using Multiroom.Server.Library;
using Multiroom.Server.Providers.WebRadio;

namespace Multiroom.Server.Providers.TuneIn;

public static class TuneInServiceCollectionExtensions
{
    // `directory` holds the logo cache: <dataDirectory>/tunein.
    public static IServiceCollection AddTuneIn(this IServiceCollection services, TuneInSettings settings,
        string directory) =>
        services.AddProvider(TuneInProvider.ProviderId, _ => new TuneInProvider(settings, directory));
}

// TuneIn's directory of stations, through its OPML API (opml.radiotime.com:
// Browse.ashx, Search.ashx, Tune.ashx and Describe.ashx, with render=json),
// the one TuneIn's partner integrations and the open-source players use. It
// is not a documented public API and can change without notice; everything
// that knows its shape is in this folder.
//
// Ids: a station is "station:s<id>", a browse container "node:<query>", where
// the query is the parameters of TuneIn's own Browse.ashx link (c, id,
// filter, offset, pivot), checked before it is asked for. A station is a
// domain RadioStation of this provider, so the queue engine, favourites and
// the re-tune rule treat it as a webradio station. It is resolved at stage
// time and relayed by webradio's StationRelay.
//
// A stream URL goes in no message and no answer (rule 7), and neither does
// the partner id: messages name the station, never a URL.
public sealed partial class TuneInProvider : IProvider, ISearchProvider, IBrowseProvider, ILiveStreamResolver,
    IArtworkProvider, IStationTitles, IDisposable
{
    public const string ProviderId = "tunein";
    public const string StationPrefix = "station:";
    public const string NodePrefix = "node:";

    // Logos are small; a few hundred of them fit.
    public const long ArtworkCacheBytes = 16 * 1024 * 1024;

    // A .pls or .m3u TuneIn hands out is a few lines.
    private const int MaxPlaylistBytes = 64 * 1024;

    private const string Unplayable = "a TuneIn list is chosen from, not queued";

    private static readonly TimeSpan ConnectTimeout = TimeSpan.FromSeconds(5);
    private static readonly TimeSpan RequestTimeout = TimeSpan.FromSeconds(10);

    // The parameters of TuneIn's own browse links, and the only ones a node
    // id may carry.
    private static readonly HashSet<string> BrowseKeys = new(StringComparer.Ordinal) { "c", "id", "filter", "offset", "pivot" };

    private readonly TuneInSettings _settings;
    private readonly TuneInEndpoints _endpoints;
    private readonly int _bufferBytes;
    private readonly HttpClient _http;
    private readonly ConcurrentDictionary<StationRelay, byte> _relays = new();
    private readonly ArtworkCache _artwork;

    public TuneInProvider(TuneInSettings settings, string directory, TuneInEndpoints? endpoints = null,
        int bufferBytes = WebRadioProvider.DefaultBufferBytes)
    {
        ArgumentOutOfRangeException.ThrowIfLessThan(bufferBytes, StationRelay.ReadSize);
        _settings = settings;
        _endpoints = endpoints ?? TuneInEndpoints.Default;
        _bufferBytes = bufferBytes;
        _http = new HttpClient(new SocketsHttpHandler
        {
            // As webradio's: the station's bytes go on as they came.
            AutomaticDecompression = DecompressionMethods.None,
            ConnectTimeout = ConnectTimeout,
        })
        {
            // A station's body never ends; every other request is bounded by
            // its own token.
            Timeout = Timeout.InfiniteTimeSpan,
        };
        _http.DefaultRequestHeaders.UserAgent.ParseAdd(Branding.ServerName);
        _artwork = new ArtworkCache(Path.Combine(directory, "artwork"), ArtworkCacheBytes, _http);
    }

    public string Id => ProviderId;

    public string DisplayName => "TuneIn";

    public ProviderCapabilities Capabilities =>
        ProviderCapabilities.Search | ProviderCapabilities.Browse | ProviderCapabilities.LiveStream |
        ProviderCapabilities.Artwork;

    public long BufferBound => _bufferBytes + StationRelay.ReadSize;

    // Rule 6's status fields for the jitter buffers and the logo cache.
    public Task<ProviderStatus> GetStatusAsync(CancellationToken cancellationToken)
    {
        var buffered = _relays.Keys.Select(r => r.Buffered).ToList();
        var (logos, bytes) = _artwork.Held;
        return Task.FromResult(new ProviderStatus(ProviderState.Ready,
            $"{buffered.Count} relay(s) holding {buffered.Sum()} byte(s); at most {BufferBound} each; " +
            $"artwork {logos} image(s), {bytes} of {_artwork.Capacity} bytes"));
    }

    public string? CurrentTitle(string stationItemId) =>
        _relays.Keys.FirstOrDefault(r => r.ItemId == stationItemId && r.Title is not null)?.Title;

    public async Task<Result<CatalogItems, ProviderError>> SearchAsync(string text, SearchKinds kinds, int offset,
        int limit, CancellationToken cancellationToken)
    {
        if (!kinds.HasFlag(SearchKinds.Stations))
            return CatalogItems.Empty;
        var body = await GetAsync("Search.ashx", [("query", text), ("types", "station")], $"search {text}",
            cancellationToken).ConfigureAwait(false);
        if (!body.IsSuccess)
            return body.Failure;
        var stations = Outlines(body.Value.Body).Select(StationOf).OfType<RadioStation>().ToList();
        return CatalogItems.Empty with { Stations = [.. stations.Skip(offset).Take(limit)] };
    }

    // TuneIn's own tree (phase 14): links are nodes, stations are items. A
    // page whose outlines have children is sectioned, each section its first
    // `limit` and its "more" link. No sort, filter or letter; a total only
    // for a list TuneIn answered whole.
    public async Task<Result<BrowsePage, ProviderError>> BrowseAsync(BrowseRequest request,
        CancellationToken cancellationToken)
    {
        var r = request;
        if (r.Sort is not null || r.Filter is not null || r.Letter is not null)
            return Result.Fail(ProviderError.InvalidArgument, "TuneIn takes no sort, filter or letter");
        List<(string, string)> query = [];
        if (r.NodeId is not null)
        {
            if (!r.NodeId.StartsWith(NodePrefix, StringComparison.Ordinal) ||
                ParseQuery(r.NodeId[NodePrefix.Length..]) is not { Count: > 0 } parsed)
            {
                return Result.Fail(ProviderError.NotFound, $"no node {r.NodeId}");
            }
            query = parsed;
        }

        var answer = await GetAsync("Browse.ashx", query, r.NodeId ?? "the root", cancellationToken).ConfigureAwait(false);
        if (!answer.IsSuccess)
            return answer.Failure;
        var (title, body) = answer.Value;
        var root = r.NodeId is null;
        // The root has no header, as webradio's has none.
        var header = root ? null : new BrowseNode(ProviderId, r.NodeId!, title ?? "TuneIn", Kind: BrowseKind.Folder,
            Unplayable: Unplayable);

        var outlines = body.EnumerateArray().ToList();
        if (outlines.Any(o => o.TryGetProperty("children", out var c) && c.ValueKind == JsonValueKind.Array))
        {
            if (r.Offset != 0)
                return Result.Fail(ProviderError.InvalidArgument, "a sectioned page takes no offset; page a section's more");
            List<BrowseSection> sections = [];
            List<JsonElement> loose = [];
            foreach (var outline in outlines)
            {
                if (!outline.TryGetProperty("children", out var children) || children.ValueKind != JsonValueKind.Array)
                {
                    loose.Add(outline);
                    continue;
                }
                var parts = children.EnumerateArray().ToList();
                var more = parts.Where(IsMore).Select(o => NodeOf(o, root: false)).OfType<BrowseNode>().FirstOrDefault();
                var nodes = parts.Where(o => !IsMore(o)).Select(o => NodeOf(o, root: false)).OfType<BrowseNode>().ToList();
                var stations = parts.Select(StationOf).OfType<RadioStation>().ToList();
                if (nodes.Count == 0 && stations.Count == 0)
                    continue;
                sections.Add(new BrowseSection(Text(outline) ?? "", [.. nodes.Take(r.Limit)],
                    CatalogItems.Empty with { Stations = [.. stations.Take(r.Limit)] }, more,
                    more is null ? nodes.Count + stations.Count : null));
            }
            return new BrowsePage([.. loose.Select(o => NodeOf(o, root)).OfType<BrowseNode>()],
                CatalogItems.Empty with { Stations = [.. loose.Select(StationOf).OfType<RadioStation>()] },
                header, Sections: sections);
        }

        // One list: containers first, then stations, paged as one.
        var all = outlines.Where(o => !IsMore(o)).ToList();
        var containers = all.Select(o => NodeOf(o, root)).OfType<BrowseNode>().ToList();
        var items = all.Select(StationOf).OfType<RadioStation>().ToList();
        var total = containers.Count + items.Count;
        var pageNodes = containers.Skip(r.Offset).Take(r.Limit).ToList();
        var pageStations = items.Skip(Math.Max(0, r.Offset - containers.Count)).Take(r.Limit - pageNodes.Count).ToList();
        return new BrowsePage(pageNodes, CatalogItems.Empty with { Stations = pageStations },
            header is not null && items.Count > 0 && containers.Count == 0 ? header with { Kind = BrowseKind.Stations } : header,
            total);
    }

    // At stage time (the SPI's rule): Tune.ashx answers the station's streams.
    // The first plain HTTP(S) audio stream that connects is relayed; a .pls or
    // .m3u is read for its streams (the one place this provider follows a
    // playlist, because TuneIn hands them out); HLS is skipped, as webradio
    // does not play it.
    public async Task<Result<ResolvedStream, ProviderError>> ResolveAsync(RadioStation station,
        CancellationToken cancellationToken)
    {
        var itemId = station.ProviderItemId;
        if (GuideIdOf(itemId) is not { } guideId)
            return Result.Fail(ProviderError.NotFound, $"no station {itemId}");
        var tuned = await GetAsync("Tune.ashx", [("id", guideId)], station.Name, cancellationToken).ConfigureAwait(false);
        if (!tuned.IsSuccess)
            return tuned.Failure;

        Failure<ProviderError>? last = null;
        foreach (var entry in tuned.Value.Body.EnumerateArray())
        {
            if (String(entry, "url") is not { } text || HttpUri(text) is not { } url || IsHls(url, entry))
                continue;
            List<Uri> streams = [url];
            if (IsPlaylist(url))
            {
                var read = await ReadPlaylistAsync(url, station.Name, cancellationToken).ConfigureAwait(false);
                if (!read.IsSuccess)
                {
                    last = read.Failure;
                    continue;
                }
                streams = [.. read.Value.Where(u => !IsHls(u, null) && !IsPlaylist(u))];
            }
            foreach (var stream in streams)
            {
                var connected = await StationRelay.ConnectAsync(_http, itemId, station.Name, stream, _bufferBytes,
                    ConnectTimeout, r => _relays.TryRemove(r, out _), cancellationToken).ConfigureAwait(false);
                if (!connected.IsSuccess)
                {
                    last = connected.Failure;
                    continue;
                }
                var relay = connected.Value;
                _relays[relay] = 0;
                return ResolvedStream.Live(relay.ContentType, relay.Open, relay.Release);
            }
        }
        return last ?? Result.Fail(ProviderError.Unavailable, $"{station.Name}: TuneIn offers no stream this server plays");
    }

    // The station's logo, which Describe.ashx names, fetched once.
    public async Task<Result<Artwork, ProviderError>> GetArtworkAsync(ArtworkRef reference,
        CancellationToken cancellationToken)
    {
        if (reference.ProviderId != ProviderId || GuideIdOf(reference.Reference) is not { } guideId)
            return Result.Fail(ProviderError.NotFound, $"no artwork {reference.Reference}");
        return await _artwork.GetAsync(reference.Reference, async ct =>
        {
            var described = await GetAsync("Describe.ashx", [("id", guideId)], reference.Reference, ct).ConfigureAwait(false);
            if (!described.IsSuccess)
                return described.Failure;
            var logo = described.Value.Body.EnumerateArray().Select(e => String(e, "logo")).FirstOrDefault(l => l is not null);
            return logo is not null && HttpUri(logo) is { } uri
                ? await _artwork.FetchAsync(uri, reference.Reference, ct).ConfigureAwait(false)
                : Result.Fail(ProviderError.NotFound, $"{reference.Reference} has no logo");
        }, cancellationToken).ConfigureAwait(false);
    }

    public void Dispose()
    {
        foreach (var relay in _relays.Keys)
            relay.Dispose();
        _http.Dispose();
    }

    // "s<digits>" of "station:s<digits>", or null.
    public static string? GuideIdOf(string itemId) =>
        itemId.StartsWith(StationPrefix, StringComparison.Ordinal) && StationId().IsMatch(itemId[StationPrefix.Length..])
            ? itemId[StationPrefix.Length..]
            : null;

    // The browse parameters of a query, in order, when every one is a key
    // TuneIn's links carry with a plain value; otherwise null.
    private static List<(string Key, string Value)>? ParseQuery(string query)
    {
        List<(string, string)> pairs = [];
        foreach (var part in query.Split('&', StringSplitOptions.RemoveEmptyEntries))
        {
            var equals = part.IndexOf('=', StringComparison.Ordinal);
            if (equals <= 0)
                return null;
            var key = Uri.UnescapeDataString(part[..equals]);
            var value = Uri.UnescapeDataString(part[(equals + 1)..]);
            if (!BrowseKeys.Contains(key) || !PlainValue().IsMatch(value))
                return null;
            pairs.Add((key, value));
        }
        return pairs;
    }

    // A Browse.ashx link of TuneIn's own answer as a node id: only its browse
    // parameters are kept, and a link carrying anything else is left out. The
    // host is never asked: the node is browsed at the configured endpoint.
    private static string? NodeIdOf(string url)
    {
        if (!Uri.TryCreate(url, UriKind.Absolute, out var uri) ||
            !uri.AbsolutePath.EndsWith("/Browse.ashx", StringComparison.OrdinalIgnoreCase))
        {
            return null;
        }
        var kept = uri.Query.TrimStart('?').Split('&', StringSplitOptions.RemoveEmptyEntries)
            .Where(p => !p.StartsWith("render=", StringComparison.Ordinal) &&
                !p.StartsWith("partnerId=", StringComparison.Ordinal) && !p.StartsWith("locale=", StringComparison.Ordinal))
            .ToList();
        var query = string.Join('&', kept);
        return ParseQuery(query) is { Count: > 0 } ? NodePrefix + query : null;
    }

    // A link outline as a node; podcasts, shows and topics are left out, and
    // so are lists of them (filter=p, p:topic).
    private static BrowseNode? NodeOf(JsonElement outline, bool root)
    {
        if (String(outline, "type") != "link" || String(outline, "key") == "podcast" ||
            String(outline, "item") is "show" or "topic" ||
            String(outline, "guide_id") is { } guide && (guide.StartsWith('p') || guide.StartsWith('t')) ||
            String(outline, "URL") is not { } url || NodeIdOf(url) is not { } nodeId ||
            ParseQuery(nodeId[NodePrefix.Length..])!.Any(p => p.Key == "filter" && p.Value.StartsWith('p')))
        {
            return null;
        }
        return new BrowseNode(ProviderId, nodeId, Text(outline) ?? nodeId, Kind: root ? BrowseKind.Root : BrowseKind.Folder,
            Subtitle: String(outline, "subtext"), Unplayable: Unplayable);
    }

    // An audio outline of a station as the domain station; anything else null.
    private static RadioStation? StationOf(JsonElement outline)
    {
        if (String(outline, "type") != "audio" || String(outline, "guide_id") is not { } guide ||
            !StationId().IsMatch(guide) || String(outline, "item") is { } item && item != "station")
        {
            return null;
        }
        var itemId = StationPrefix + guide;
        return new RadioStation(ProviderId, itemId, Text(outline) ?? guide,
            Artwork: String(outline, "image") is null ? null : new ArtworkRef(ProviderId, itemId));
    }

    // A section's "more stations" link, TuneIn's own paging.
    private static bool IsMore(JsonElement outline) =>
        String(outline, "key") is { } key && key.StartsWith("next", StringComparison.Ordinal);

    private static IEnumerable<JsonElement> Outlines(JsonElement body) =>
        body.EnumerateArray().SelectMany<JsonElement, JsonElement>(o => o.TryGetProperty("children", out var c) && c.ValueKind == JsonValueKind.Array
            ? c.EnumerateArray()
            : [o]);

    private static string? Text(JsonElement outline) => String(outline, "text");

    private static string? String(JsonElement element, string name) =>
        element.ValueKind == JsonValueKind.Object && element.TryGetProperty(name, out var value) &&
        value.ValueKind == JsonValueKind.String && value.GetString() is { Length: > 0 } s
            ? s
            : null;

    private static Uri? HttpUri(string text) =>
        Uri.TryCreate(text, UriKind.Absolute, out var uri) && (uri.Scheme == Uri.UriSchemeHttp || uri.Scheme == Uri.UriSchemeHttps)
            ? uri
            : null;

    private static bool IsHls(Uri url, JsonElement? entry) =>
        url.AbsolutePath.EndsWith(".m3u8", StringComparison.OrdinalIgnoreCase) ||
        entry is { } e && (String(e, "media_type") == "hls" || String(e, "is_hls_advanced") == "true");

    private static bool IsPlaylist(Uri url) =>
        url.AbsolutePath.EndsWith(".pls", StringComparison.OrdinalIgnoreCase) ||
        url.AbsolutePath.EndsWith(".m3u", StringComparison.OrdinalIgnoreCase);

    private async Task<Result<List<Uri>, ProviderError>> ReadPlaylistAsync(Uri url, string name,
        CancellationToken cancellationToken)
    {
        using var bounded = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        bounded.CancelAfter(RequestTimeout);
        try
        {
            using var response = await _http.GetAsync(url, HttpCompletionOption.ResponseHeadersRead, bounded.Token)
                .ConfigureAwait(false);
            if (!response.IsSuccessStatusCode)
                return Result.Fail(ProviderError.Unavailable, $"{name}: its playlist answered HTTP {(int)response.StatusCode}");
            var stream = await response.Content.ReadAsStreamAsync(bounded.Token).ConfigureAwait(false);
            await using (stream.ConfigureAwait(false))
            {
                var buffer = new byte[MaxPlaylistBytes];
                var length = 0;
                int read;
                while (length < buffer.Length &&
                       (read = await stream.ReadAsync(buffer.AsMemory(length), bounded.Token).ConfigureAwait(false)) > 0)
                {
                    length += read;
                }
                var parsed = StationPlaylist.Parse(Encoding.UTF8.GetString(buffer, 0, length));
                return parsed.IsSuccess
                    ? parsed.Value.Select(s => s.StreamUrl).ToList()
                    : Result.Fail(ProviderError.Unavailable, $"{name}: its playlist cannot be read");
            }
        }
        catch (OperationCanceledException) when (!cancellationToken.IsCancellationRequested)
        {
            return Result.Fail(ProviderError.Unavailable, $"{name}: its playlist did not answer in time");
        }
        catch (HttpRequestException e)
        {
            return Result.Fail(ProviderError.Unavailable, $"{name}: its playlist cannot be reached ({e.HttpRequestError})");
        }
    }

    // One OPML call: the answer's title and body. TuneIn answers a fault
    // inside a 200 (head.status); 400 and 404 are an id it does not know.
    private async Task<Result<(string? Title, JsonElement Body), ProviderError>> GetAsync(string endpoint,
        IReadOnlyList<(string Key, string Value)> query, string name, CancellationToken cancellationToken)
    {
        var text = new StringBuilder(endpoint).Append('?');
        foreach (var (key, value) in query)
            text.Append(key).Append('=').Append(Uri.EscapeDataString(value)).Append('&');
        if (_settings.PartnerId is { } partner)
            text.Append("partnerId=").Append(Uri.EscapeDataString(partner)).Append('&');
        if (_settings.Locale is { } locale)
            text.Append("locale=").Append(Uri.EscapeDataString(locale)).Append('&');
        text.Append("render=json");

        using var bounded = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        bounded.CancelAfter(RequestTimeout);
        try
        {
            using var response = await _http.GetAsync(new Uri(_endpoints.Api, text.ToString()), bounded.Token)
                .ConfigureAwait(false);
            if (!response.IsSuccessStatusCode)
            {
                return Result.Fail(response.StatusCode is HttpStatusCode.NotFound or HttpStatusCode.BadRequest
                    ? ProviderError.NotFound
                    : ProviderError.Unavailable, $"{name}: TuneIn answered HTTP {(int)response.StatusCode}");
            }
            var bytes = await response.Content.ReadAsByteArrayAsync(bounded.Token).ConfigureAwait(false);
            using var document = JsonDocument.Parse(bytes);
            var root = document.RootElement;
            var head = root.TryGetProperty("head", out var h) ? h : default;
            var status = String(head, "status");
            if (status is not (null or "200"))
            {
                return Result.Fail(status is "400" or "404" ? ProviderError.NotFound : ProviderError.Unavailable,
                    $"{name}: TuneIn answered {status}{(String(head, "fault") is { } fault ? $" ({fault})" : "")}");
            }
            if (!root.TryGetProperty("body", out var body) || body.ValueKind != JsonValueKind.Array)
                return Result.Fail(ProviderError.Failed, $"{name}: TuneIn's answer has no body");
            return (String(head, "title"), body.Clone());
        }
        catch (OperationCanceledException) when (!cancellationToken.IsCancellationRequested)
        {
            return Result.Fail(ProviderError.Unavailable, $"{name}: TuneIn did not answer within {RequestTimeout.TotalSeconds:0} s");
        }
        catch (HttpRequestException e)
        {
            return Result.Fail(ProviderError.Unavailable, $"{name}: TuneIn cannot be reached ({e.HttpRequestError})");
        }
        catch (JsonException)
        {
            return Result.Fail(ProviderError.Failed, $"{name}: TuneIn's answer is not JSON");
        }
    }

    [GeneratedRegex("^s[0-9]+$")]
    private static partial Regex StationId();

    [GeneratedRegex("^[A-Za-z0-9_:.,-]+$")]
    private static partial Regex PlainValue();
}
