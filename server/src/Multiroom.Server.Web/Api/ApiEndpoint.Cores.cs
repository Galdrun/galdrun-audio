using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Http;
using Multiroom.Server.Abstractions;
using Multiroom.Server.Playback.Core;

namespace Multiroom.Server.Web.Api;

// The server cores on the network and which one this server uses. Selected is
// the stored id, null for the configured address; Connected is where CoreLink
// is connected right now, null when it is not.
public sealed record CoresView(string? Selected, string Configured, string? Connected,
    IReadOnlyList<DiscoveredCore> Cores);

public sealed partial class ApiEndpoint
{
    private void MapCores(WebApplication app)
    {
        app.MapGet("/api/v1/cores", () => Json(CoresOf()));
        // {"coreId": "<id>"} uses that core from now on and after a restart;
        // {"coreId": null} goes back to core.host/core.port.
        app.MapPut("/api/v1/core/selection", (Delegate)((HttpContext c) =>
            WithBody<CoreSelectionRequest, CoresView, CoreError>(c, r => Task.FromResult(Select(r.CoreId)))));
    }

    private Result<CoresView, CoreError> Select(string? coreId)
    {
        var selected = cores.Select(coreId);
        return selected.IsSuccess ? CoresOf() : selected.Failure;
    }

    private CoresView CoresOf() =>
        new(cores.SelectedCoreId, $"{link.ConfiguredHost}:{link.ConfiguredPort}",
            link.IsConnected ? $"{link.Host}:{link.Port}" : null, cores.Cores);
}
