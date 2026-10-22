using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Http;
using Multiroom.Server.Providers.Tidal;

namespace Multiroom.Server.Web.Api;

// Signing in to Tidal by device code. The apps it signs in and plays through
// are the config file's (providers.tidal), never set here.
public sealed partial class ApiEndpoint
{
    private void MapTidal(WebApplication app)
    {
        app.MapPost("/api/v1/providers/tidal/login", async (HttpContext c) =>
        {
            var tidal = providers.Get<TidalProvider>(TidalProvider.ProviderId);
            return tidal.IsSuccess ? Answer(await tidal.Value.StartLoginAsync(c.RequestAborted).ConfigureAwait(false)) : Error(tidal.Failure);
        });
        app.MapGet("/api/v1/providers/tidal/login", async (HttpContext c) =>
        {
            var tidal = providers.Get<TidalProvider>(TidalProvider.ProviderId);
            return tidal.IsSuccess ? Json(new { login = tidal.Value.Login,
                status = await tidal.Value.GetStatusAsync(c.RequestAborted).ConfigureAwait(false) }) : Error(tidal.Failure);
        });
    }
}

