using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Http;
using Multiroom.Server.Abstractions;

namespace Multiroom.Server.Web.Api;

// AI proposals (phase 38, ProposalRunner): the stored lists a view reads,
// a refresh queued (202, the run; a scope already running answers that
// run), and an item dismissed. Liking or playing a proposal uses the routes
// that exist. Changes are announced as proposals.running and
// proposals.changed.
public sealed partial class ApiEndpoint
{
    private void MapProposals(WebApplication app)
    {
        app.MapGet("/api/v1/ai/proposals", (Delegate)((HttpContext c) =>
            UnknownQuery(c.Request, "user") is { } unknown
                ? Error(unknown)
                : Answer(proposals.View(c.Request.Query["user"].ToString()))));
        app.MapPost("/api/v1/ai/proposals/refresh", (Delegate)(async (HttpContext c) =>
        {
            var body = await ApiJson.ReadAsync<ProposalRefresh>(c.Request, optional: true, c.RequestAborted)
                .ConfigureAwait(false);
            if (!body.IsSuccess)
                return Error(body.Failure);
            return Answer(proposals.Refresh(body.Value?.User), StatusCodes.Status202Accepted);
        }));
        app.MapDelete("/api/v1/ai/proposals/{proposal}", (string proposal) =>
            WithId(proposal, "proposal", id =>
            {
                var dismissed = proposals.Dismiss(id);
                return Task.FromResult(dismissed.IsSuccess ? Results.NoContent() : Error(dismissed.Failure));
            }));
    }
}
