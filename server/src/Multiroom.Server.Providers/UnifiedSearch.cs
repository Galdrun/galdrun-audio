using System.Text.Json;
using Multiroom.Server.Abstractions;

namespace Multiroom.Server.Providers;

// A provider as the API lists it. DisplayName and Capabilities are absent for
// a disabled provider, which holds no instance to ask. Root is its browse
// root's nodes, when asked for.
public sealed record ProviderView(
    string Id,
    ProviderState State,
    string Detail,
    string? DisplayName = null,
    IReadOnlyList<string>? Capabilities = null,
    IReadOnlyList<BrowseNode>? Root = null);

// What one provider answered to a unified search.
public sealed record SearchSource(string Provider, int Tracks, int Albums, int Artists, int Stations, int Playlists);

// A provider that did not answer in time, or failed.
public sealed record SearchFailure(string Provider, string Error, string Message);

// Every search-capable provider's page, merged in registration order; each
// item names its provider. Nothing is deduplicated across providers.
public sealed record UnifiedSearchResult(
    string Query,
    CatalogItems Items,
    IReadOnlyList<SearchSource> Sources,
    IReadOnlyList<SearchFailure> Failures);

// Every provider asked at once: a search over
// all of them, and their list with their browse roots. Each provider is
// bounded by `timeout` (api.searchTimeout); one that is slow, disabled or
// failing is named and costs nothing more. The REST API and the MCP tools
// both call this, so they answer alike. The tracks `kept` names (KeptTracks:
// in a playlist, a queue, a favourite, liked) come first, each provider's in
// its order, then the rest as before.
public sealed class UnifiedSearch(ProviderRegistry providers, TimeSpan timeout,
    Func<IReadOnlyList<Track>, IReadOnlySet<(string ProviderId, string ProviderItemId)>>? kept = null)
{
    // The same bound as the routes that page: at most this many per provider.
    public const int MaxLimit = 100;

    public async Task<IReadOnlyList<ProviderView>> ProvidersAsync(bool withRoot, CancellationToken cancellationToken)
    {
        var statuses = await providers.GetStatusesAsync(cancellationToken).ConfigureAwait(false);
        var views = statuses.Select(async s => providers.Get<IProvider>(s.Id) is { IsSuccess: true } p
            ? new ProviderView(s.Id, s.Status.State, s.Status.Detail, p.Value.DisplayName,
                CapabilityNames(p.Value.Capabilities),
                withRoot ? await RootOfAsync(p.Value, cancellationToken).ConfigureAwait(false) : null)
            : new ProviderView(s.Id, s.Status.State, s.Status.Detail));
        return await Task.WhenAll(views).ConfigureAwait(false);
    }

    public Task<UnifiedSearchResult> SearchAsync(string text, SearchKinds kinds, int offset, int limit,
        CancellationToken cancellationToken) =>
        SearchAsync(text, kinds, offset, limit, null, cancellationToken);

    // `only` narrows the search to those providers (the song resolver's).
    public async Task<UnifiedSearchResult> SearchAsync(string text, SearchKinds kinds, int offset, int limit,
        IReadOnlyCollection<string>? only, CancellationToken cancellationToken)
    {
        var asked = new List<Task<SearchAnswer>>();
        foreach (var id in providers.Ids)
        {
            if (only is not null && !only.Contains(id))
                continue;
            var provider = providers.Get<IProvider>(id);
            if (!provider.IsSuccess)
                asked.Add(Task.FromResult(new SearchAnswer(id, null, Failed(id, provider.Failure))));
            else if (provider.Value is ISearchProvider searcher)
                asked.Add(SearchBoundedAsync(id, searcher, text, kinds, offset, limit, cancellationToken));
        }
        var answers = await Task.WhenAll(asked).ConfigureAwait(false);

        var tracks = new List<Track>();
        var albums = new List<Album>();
        var artists = new List<Artist>();
        var found = new List<RadioStation>();
        var playlists = new List<BrowseNode>();
        var sources = new List<SearchSource>();
        var failures = new List<SearchFailure>();
        foreach (var (id, items, failure) in answers)
        {
            if (items is null)
            {
                failures.Add(failure!);
                continue;
            }
            tracks.AddRange(items.Tracks);
            albums.AddRange(items.Albums);
            artists.AddRange(items.Artists);
            found.AddRange(items.Stations);
            playlists.AddRange(items.Playlists);
            sources.Add(new SearchSource(id, items.Tracks.Count, items.Albums.Count, items.Artists.Count,
                items.Stations.Count, items.Playlists.Count));
        }
        if (kept is not null && tracks.Count > 0)
        {
            var marked = kept(tracks);
            tracks = [.. tracks.OrderBy(t => marked.Contains((t.ProviderId, t.ProviderItemId)) ? 0 : 1)];
        }
        return new UnifiedSearchResult(text, new CatalogItems(tracks, albums, artists, found, playlists), sources,
            failures);
    }

    // Only the items of one media type (phase 30): tracks of that type
    // (music counting those nothing says anything about), stations for radio.
    // Albums, artists and playlists carry no type: radio answers none of
    // them, the other types all.
    public static UnifiedSearchResult OfType(UnifiedSearchResult result, MediaType type)
    {
        var items = result.Items;
        var radio = type == MediaType.Radio;
        return result with
        {
            Items = new CatalogItems(
                [.. items.Tracks.Where(t => type == MediaType.Music ? MediaTypeClassifier.IsMusic(t) : t.MediaType == type)],
                radio ? [] : items.Albums, radio ? [] : items.Artists, radio ? items.Stations : [],
                radio ? [] : items.Playlists),
        };
    }

    private static List<string> CapabilityNames(ProviderCapabilities capabilities) =>
    [
        .. Enum.GetValues<ProviderCapabilities>()
            .Where(f => f != ProviderCapabilities.None && capabilities.HasFlag(f))
            .Select(f => JsonNamingPolicy.CamelCase.ConvertName(f.ToString())),
    ];

    // Each provider's root nodes, so a UI draws its list of sources in one
    // call. Bounded like a search: a provider slow to answer has no Root.
    private async Task<IReadOnlyList<BrowseNode>?> RootOfAsync(IProvider provider, CancellationToken cancellationToken)
    {
        if (provider is not IBrowseProvider browser)
            return null;
        using var bounded = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        bounded.CancelAfter(timeout);
        try
        {
            var root = await browser.BrowseAsync(new BrowseRequest(null, 0, MaxLimit), bounded.Token)
                .WaitAsync(timeout, cancellationToken).ConfigureAwait(false);
            return root.IsSuccess ? root.Value.Nodes : null;
        }
        catch (Exception e) when (e is TimeoutException ||
            (e is OperationCanceledException && !cancellationToken.IsCancellationRequested))
        {
            return null;
        }
    }

    // A provider that ignores its cancellation is not waited for either.
    private async Task<SearchAnswer> SearchBoundedAsync(string id, ISearchProvider searcher, string text,
        SearchKinds kinds, int offset, int limit, CancellationToken cancellationToken)
    {
        using var bounded = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        bounded.CancelAfter(timeout);
        try
        {
            var answer = await searcher.SearchAsync(text, kinds, offset, limit, bounded.Token)
                .WaitAsync(timeout, cancellationToken).ConfigureAwait(false);
            return answer.IsSuccess
                ? new SearchAnswer(id, answer.Value, null)
                : new SearchAnswer(id, null, Failed(id, answer.Failure));
        }
        catch (Exception e) when (e is TimeoutException ||
            (e is OperationCanceledException && !cancellationToken.IsCancellationRequested))
        {
            return new SearchAnswer(id, null, new SearchFailure(id, "timeout",
                $"no answer within {timeout.TotalMilliseconds:0} ms"));
        }
#pragma warning disable CA1031 // A provider's fault is that provider's missing results, never the search's.
        catch (Exception e) when (!cancellationToken.IsCancellationRequested)
#pragma warning restore CA1031
        {
            return new SearchAnswer(id, null, new SearchFailure(id, "failed", e.Message));
        }
    }

    private static SearchFailure Failed(string id, Failure<ProviderError> failure) =>
        new(id, ApiError.From(failure).Error, failure.Detail);

    // Items or a failure, never both.
    private sealed record SearchAnswer(string Id, CatalogItems? Items, SearchFailure? Failure);
}
