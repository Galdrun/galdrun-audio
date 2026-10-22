using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Http;
using Multiroom.Server.Abstractions;
using Multiroom.Server.Library;
using Multiroom.Server.Providers.AddIns;

namespace Multiroom.Server.Web.Api;

// What one enrichment add-in holds in the library, on status.
public sealed record EnrichmentView(string AddIn, long Enriched, long Failed, long Pending);

// The add-in surface (phase 12): "more like this", merged over every
// recommendation add-in. An add-in that fails or does not answer in time is
// named in `failures`; the answer never fails because of one.
public sealed partial class ApiEndpoint
{
    private void MapAddIns(WebApplication app) =>
        app.MapPost("/api/v1/recommendations", (Delegate)RecommendAsync);

    private async Task<IResult> RecommendAsync(HttpContext context)
    {
        var body = await ApiJson.ReadAsync<RecommendRequest>(context.Request, optional: false, context.RequestAborted)
            .ConfigureAwait(false);
        if (!body.IsSuccess)
            return Error(body.Failure);
        var request = body.Value!;
        if (request.Limit is < 1 or > MaxLimit)
            return Error(Result.Fail(ApiRequestError.InvalidRequest, $"limit: 1 to {MaxLimit}, was {request.Limit}"));

        List<Track> seed;
        switch (request)
        {
            case { Track: { } track, Playlist: null }:
                seed = [track];
                break;
            case { Track: null, Playlist: { } playlist }:
                var entries = playlists.Entries(playlist);
                if (!entries.IsSuccess)
                    return Error(entries.Failure);
                if (entries.Value.Count == 0)
                    return Error(Result.Fail(LibraryError.InvalidArgument, $"playlist {playlist} is empty"));
                seed = [.. entries.Value.Select(e => e.Track).OfType<Track>()];
                break;
            default:
                return Error(Result.Fail(ApiRequestError.InvalidRequest, "exactly one of track or playlist"));
        }
        return Json(await addIns.RecommendAsync(seed, request.Limit, context.RequestAborted).ConfigureAwait(false));
    }

    private object AddInsStatus() => new
    {
        maxInFlight = AddInRegistry.MaxInFlight,
        registered = addIns.Status,
        enrichment = addIns.Enrichers
            .Select(e => (e.Id, Counts: enrichment.Counts(e.Id)))
            .Where(c => c.Counts.IsSuccess)
            .Select(c => new EnrichmentView(c.Id, c.Counts.Value.Enriched, c.Counts.Value.Failed, c.Counts.Value.Pending))
            .ToList(),
    };
}
