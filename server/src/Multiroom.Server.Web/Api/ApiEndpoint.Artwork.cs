using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Http;
using Multiroom.Server.Abstractions;
using Multiroom.Server.Library;
using Multiroom.Server.Providers.Covers;

namespace Multiroom.Server.Web.Api;

// The artwork settings and how far the add-in got: the tracks it answered,
// failed on, and has not asked about yet; and the artwork store (phase 40):
// its pictures and bytes, the ids waiting to be looked for, those not found.
public sealed record ArtworkView(MusicBrainzView MusicBrainz, FanartTvView FanartTv, EnrichmentCounts? Progress,
    ArtworkStoreStats Store);

public sealed record ArtworkPatch(bool? MusicBrainz = null, bool? FanartTv = null);

// fanart.tv's project key, in the body. Prints without it (rule 7).
public sealed record ArtworkKeyRequest(string Key)
{
    public override string ToString() => nameof(ArtworkKeyRequest);
}

// Where the library's missing art is looked for: MusicBrainz on or off,
// fanart.tv on or off and its key. What is set here wins over the config
// and counts at once; each change asks again about what was not found.
//
// GET artwork/{id} (phase 40) is the picture of any item a list answered,
// by its artworkId (ArtworkService); notFound while there is none, and an
// `artwork.changed {id}` event when one is found.
public sealed partial class ApiEndpoint
{
    private void MapArtwork(WebApplication app)
    {
        app.MapGet("/api/v1/artwork", () => Json(ArtworkNow(artwork.Current)));
        app.MapMethods("/api/v1/artwork", [HttpMethods.Patch], (Delegate)(async (HttpContext c) =>
        {
            var body = await ApiJson.ReadAsync<ArtworkPatch>(c.Request, optional: false, c.RequestAborted)
                .ConfigureAwait(false);
            if (!body.IsSuccess)
                return Error(body.Failure);
            if (body.Value! is { MusicBrainz: null, FanartTv: null })
            {
                return Error(Result.Fail(ApiRequestError.InvalidRequest,
                    "an artwork patch needs musicBrainz, fanartTv, or both"));
            }
            return ArtworkAnswer(artwork.Set(body.Value.MusicBrainz, body.Value.FanartTv));
        }));
        app.MapPut("/api/v1/artwork/fanarttv/key", (Delegate)((HttpContext c) =>
            WithBody<ArtworkKeyRequest, ArtworkView, ProviderError>(c, r =>
                Task.FromResult(ArtworkResult(artwork.SetFanartTvKey(r.Key))))));
        app.MapDelete("/api/v1/artwork/fanarttv/key", () => ArtworkAnswer(artwork.ClearFanartTvKey()));
        app.MapGet("/api/v1/artwork/{id}", PictureAsync);
    }

    private async Task<IResult> PictureAsync(string id, HttpContext context)
    {
        if (UnknownQuery(context.Request) is { } unknown)
            return Error(unknown);
        var found = await pictures.GetAsync(id, context.RequestAborted).ConfigureAwait(false);
        return found.IsSuccess ? Results.Bytes(found.Value.Bytes, found.Value.ContentType) : Error(found.Failure);
    }

    private IResult ArtworkAnswer(Result<ArtworkSettingsView, ProviderError> changed) =>
        Answer(ArtworkResult(changed));

    private Result<ArtworkView, ProviderError> ArtworkResult(Result<ArtworkSettingsView, ProviderError> changed) =>
        changed.IsSuccess ? ArtworkNow(changed.Value) : changed.Failure;

    private ArtworkView ArtworkNow(ArtworkSettingsView settings) =>
        new(settings.MusicBrainz, settings.FanartTv,
            enrichment.Counts(ArtworkAddIn.AddInId) is { IsSuccess: true } counts ? counts.Value : null, pictures.Stats);
}
