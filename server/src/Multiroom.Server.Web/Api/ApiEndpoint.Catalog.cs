using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Http;
using Multiroom.Server.Abstractions;
using Multiroom.Server.Providers;

namespace Multiroom.Server.Web.Api;

public sealed partial class ApiEndpoint
{
    private void MapCatalog(WebApplication app)
    {
        app.MapGet("/api/v1/providers", (Delegate)(async (HttpContext c) =>
            Json(await search.ProvidersAsync(withRoot: true, c.RequestAborted).ConfigureAwait(false))));
        app.MapGet("/api/v1/providers/{id}/browse", BrowseAsync);
        app.MapGet("/api/v1/providers/{id}/search", SearchOneAsync);
        app.MapGet("/api/v1/providers/{id}/artwork", ArtworkAsync);
        app.MapGet("/api/v1/search", (Delegate)SearchAllAsync);
    }

    // The page of a node, with the options it takes: a sort, a text filter,
    // a letter to start at. The provider refuses what the node does not take.
    private async Task<IResult> BrowseAsync(string id, HttpContext context)
    {
        var request = context.Request;
        if (UnknownQuery(request, "node", "sort", "filter", "letter", "offset", "limit") is { } unknown)
            return Error(unknown);
        var page = Page(request);
        if (!page.IsSuccess)
            return Error(page.Failure);
        var browser = providers.Get<IBrowseProvider>(id);
        if (!browser.IsSuccess)
            return Error(browser.Failure);
        string? Text(string name) => request.Query[name].ToString() is { Length: > 0 } text ? text : null;
        return Answer(await browser.Value.BrowseAsync(new BrowseRequest(Text("node"), page.Value.Offset,
            page.Value.Limit, Text("sort"), Text("filter"), Text("letter")), context.RequestAborted).ConfigureAwait(false));
    }

    private async Task<IResult> SearchOneAsync(string id, HttpContext context)
    {
        var query = SearchQuery(context.Request);
        if (!query.IsSuccess)
            return Error(query.Failure);
        var searcher = providers.Get<ISearchProvider>(id);
        if (!searcher.IsSuccess)
            return Error(searcher.Failure);
        var (text, kinds, offset, limit) = query.Value;
        return Answer(await searcher.Value.SearchAsync(text, kinds, offset, limit, context.RequestAborted)
            .ConfigureAwait(false));
    }

    // Every provider that can search, in parallel (UnifiedSearch): one that
    // is slow, disabled or failing is named in Failures.
    private async Task<IResult> SearchAllAsync(HttpContext context)
    {
        var query = SearchQuery(context.Request, "mediaType");
        if (!query.IsSuccess)
            return Error(query.Failure);
        var (text, kinds, offset, limit) = query.Value;
        var type = MediaTypeQuery(context.Request);
        if (!type.IsSuccess)
            return Error(type.Failure);
        var found = await search.SearchAsync(text, kinds, offset, limit, context.RequestAborted).ConfigureAwait(false);
        return Json(type.Value is { } only ? UnifiedSearch.OfType(found, only) : found);
    }

    private async Task<IResult> ArtworkAsync(string id, HttpContext context)
    {
        var request = context.Request;
        if (UnknownQuery(request, "reference") is { } unknown)
            return Error(unknown);
        var reference = request.Query["reference"].ToString();
        if (reference.Length == 0)
            return Error(Result.Fail(ApiRequestError.InvalidRequest, "reference is required"));
        var artwork = providers.Get<IArtworkProvider>(id);
        if (!artwork.IsSuccess)
            return Error(artwork.Failure);
        var fetched = await artwork.Value.GetArtworkAsync(new ArtworkRef(id, reference), context.RequestAborted)
            .ConfigureAwait(false);
        return fetched.IsSuccess
            ? Results.Bytes(fetched.Value.Bytes, fetched.Value.ContentType)
            : Error(fetched.Failure);
    }

    private static Result<(string Text, SearchKinds Kinds, int Offset, int Limit), ApiRequestError> SearchQuery(
        HttpRequest request, string? also = null)
    {
        if (UnknownQuery(request, also is null ? ["q", "types", "offset", "limit"] : ["q", "types", "offset", "limit", also])
            is { } unknown)
        {
            return unknown;
        }
        var text = request.Query["q"].ToString().Trim();
        if (text.Length == 0)
            return Result.Fail(ApiRequestError.InvalidRequest, "q is required");
        var kinds = KindsQuery(request);
        if (!kinds.IsSuccess)
            return kinds.Failure;
        var page = Page(request);
        return page.IsSuccess ? (text, kinds.Value, page.Value.Offset, page.Value.Limit) : page.Failure;
    }

    // types: a comma-separated list of tracks, albums, artists, playlists,
    // stations or all, in any case (Tidal's ARTISTS,ALBUMS too); all when absent.
    private static Result<SearchKinds, ApiRequestError> KindsQuery(HttpRequest request)
    {
        var text = request.Query["types"].ToString();
        if (text.Length == 0)
            return SearchKinds.All;
        var kinds = SearchKinds.None;
        foreach (var name in text.Split(',', StringSplitOptions.TrimEntries))
        {
            if (!Enum.TryParse<SearchKinds>(name, ignoreCase: true, out var kind) || kind == SearchKinds.None ||
                !name.All(char.IsAsciiLetter))
            {
                return Result.Fail(ApiRequestError.InvalidRequest,
                    $"types: tracks, albums, artists, playlists, stations or all, was {name}");
            }
            kinds |= kind;
        }
        return kinds;
    }

    // mediaType: music, audiobook or radio (phase 30); none when absent.
    private static Result<MediaType?, ApiRequestError> MediaTypeQuery(HttpRequest request)
    {
        var text = request.Query["mediaType"].ToString();
        if (text.Length == 0)
            return (MediaType?)null;
        return Enum.TryParse<MediaType>(text, ignoreCase: true, out var type) && text.All(char.IsAsciiLetter)
            ? type
            : Result.Fail(ApiRequestError.InvalidRequest, $"mediaType: music, audiobook or radio, was {text}");
    }

    private static Result<(int Offset, int Limit), ApiRequestError> Page(HttpRequest request)
    {
        var offset = IntQuery(request, "offset", 0, 0, int.MaxValue);
        if (!offset.IsSuccess)
            return offset.Failure;
        var limit = IntQuery(request, "limit", DefaultLimit, 1, MaxLimit);
        return limit.IsSuccess ? (offset.Value, limit.Value) : limit.Failure;
    }
}
