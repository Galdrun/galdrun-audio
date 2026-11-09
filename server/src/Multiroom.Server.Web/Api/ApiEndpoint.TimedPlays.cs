using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Http;
using Multiroom.Server.Library;

namespace Multiroom.Server.Web.Api;

// Timed plays: a playlist started and stopped in some rooms by a weekly
// schedule (Playback/TimedPlays.cs). A PUT replaces the whole definition;
// the scheduler picks a change up at its next tick. A delete stops what the
// timed play started.
public sealed partial class ApiEndpoint
{
    private void MapTimedPlays(WebApplication app)
    {
        app.MapGet("/api/v1/timed-plays", () => Answer(timedPlayStore.All()));
        app.MapPost("/api/v1/timed-plays", (Delegate)((HttpContext c) =>
            Sync<TimedPlayDefinition, TimedPlay>(c, timedPlayStore.Create, StatusCodes.Status201Created)));
        app.MapGet("/api/v1/timed-plays/{id}", (string id) =>
            WithId(id, "timed play", i => Task.FromResult(Answer(timedPlayStore.Get(i)))));
        app.MapPut("/api/v1/timed-plays/{id}", (string id, HttpContext c) =>
            WithId(id, "timed play", i => Sync<TimedPlayDefinition, TimedPlay>(c, r => timedPlayStore.Update(i, r))));
        app.MapDelete("/api/v1/timed-plays/{id}", (string id, HttpContext c) =>
            WithId(id, "timed play", async i => Deleted(await timedPlays.DeleteAsync(i, c.RequestAborted)
                .ConfigureAwait(false))));
    }
}
