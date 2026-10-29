using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Http;
using Multiroom.Server.Abstractions;
using Multiroom.Server.Library;
using Multiroom.Server.Playback;
using Multiroom.Server.Playback.Core;
using Multiroom.Server.Providers;

namespace Multiroom.Server.Web.Api;

public sealed partial class ApiEndpoint
{
    private void MapZones(WebApplication app)
    {
        app.MapGet("/api/v1/zones", () => Json(house.Zones()));
        app.MapGet("/api/v1/zones/{zone}", (string zone) => Answer(house.Zone(zone)));
        app.MapDelete("/api/v1/zones/{zone}", async (string zone, HttpContext c) =>
        {
            var deleted = await zones.DeleteAsync(zone, c.RequestAborted).ConfigureAwait(false);
            return deleted.IsSuccess ? Results.NoContent() : Error(deleted.Failure);
        });
        app.MapMethods("/api/v1/zones/{zone}", [HttpMethods.Patch], PatchZoneAsync);
        app.MapGet("/api/v1/history", (Delegate)History);
        app.MapGet("/api/v1/books/positions", (Delegate)BookPositionsPage);
        // The reference is the book's ("local:book:3", "local:folder:<path>"),
        // percent-encoded where it holds a slash; ?user= whose place, absent
        // the household's.
        app.MapDelete("/api/v1/books/positions/{**reference}", (string reference, HttpContext c) =>
        {
            if (UnknownQuery(c.Request, "user") is { } unknown)
                return Error(unknown);
            var user = users.Resolve(c.Request.Query["user"].ToString());
            if (!user.IsSuccess)
                return Error(user.Failure);
            var deleted = bookPositions.Delete(Uri.UnescapeDataString(reference), user.Value);
            return deleted.IsSuccess ? Results.NoContent() : Error(deleted.Failure);
        });
        app.MapPut("/api/v1/zones/{zone}/clients", (string zone, HttpContext c) =>
            WithBody<ClientsRequest>(c, r => zones.SetClientsAsync(zone, r.Clients, c.RequestAborted)));

        app.MapPost("/api/v1/zones/{zone}/play", PlayAsync);
        app.MapPost("/api/v1/zones/{zone}/pause", (string zone, HttpContext c) =>
            Transport(c, () => queues.PauseAsync(zone, c.RequestAborted)));
        app.MapPost("/api/v1/zones/{zone}/stop", (string zone, HttpContext c) =>
            Transport(c, () => queues.StopAsync(zone, c.RequestAborted)));
        app.MapPost("/api/v1/zones/{zone}/next", (string zone, HttpContext c) =>
            Transport(c, () => queues.NextAsync(zone, c.RequestAborted)));
        app.MapPost("/api/v1/zones/{zone}/previous", (string zone, HttpContext c) =>
            Transport(c, () => queues.PreviousAsync(zone, c.RequestAborted)));
        app.MapPost("/api/v1/zones/{zone}/seek", (string zone, HttpContext c) =>
            WithBody<SeekRequest>(c, r => r.PositionMs < 0
                ? Task.FromResult<Result<QueueState, QueueError>>(Result.Fail(QueueError.InvalidArgument,
                    $"positionMs {r.PositionMs} is negative"))
                : queues.SeekAsync(zone, new QueueItemId(r.Item), TimeSpan.FromMilliseconds(r.PositionMs),
                    c.RequestAborted)));

        app.MapGet("/api/v1/zones/{zone}/queue", (string zone) => Answer(queues.GetQueue(zone)));
        app.MapMethods("/api/v1/zones/{zone}/queue", [HttpMethods.Patch], PatchQueueAsync);
        app.MapPost("/api/v1/zones/{zone}/queue/items", AddItemsAsync);
        // The queue as a stored playlist, in a folder or at the top: 409
        // alreadyExists for a name in use there, unless overwrite.
        app.MapPost("/api/v1/zones/{zone}/queue/save", (string zone, HttpContext c) =>
            Sync<QueueSaveRequest, QueueSaved>(c, r => SaveQueue(zone, r.Name, r.Overwrite, r.Folder), StatusCodes.Status200OK));
        app.MapMethods("/api/v1/zones/{zone}/queue/items/{item}", [HttpMethods.Patch],
            (string zone, string item, HttpContext c) =>
            {
                var id = IdOf(item, "item");
                return id.IsSuccess
                    ? WithBody<ItemMove>(c, r => queues.MoveAsync(zone, new QueueItemId(id.Value), r.Index, c.RequestAborted))
                    : Task.FromResult(Error(id.Failure));
            });
        app.MapDelete("/api/v1/zones/{zone}/queue/items/{item}", async (string zone, string item, HttpContext c) =>
        {
            var id = IdOf(item, "item");
            if (!id.IsSuccess)
                return Error(id.Failure);
            return Answer(await queues.RemoveAsync(zone, new QueueItemId(id.Value), c.RequestAborted)
                .ConfigureAwait(false));
        });
    }

    // Phase 15: the zone's own volume and mute (House.SetZoneLevelAsync);
    // phase 26: its AutoFill (House.SetAutoFill), set first, as it needs no
    // core. Either alone, or both.
    private async Task<IResult> PatchZoneAsync(string zone, HttpContext context)
    {
        var body = await ApiJson.ReadAsync<ZonePatch>(context.Request, optional: false, context.RequestAborted)
            .ConfigureAwait(false);
        if (!body.IsSuccess)
            return Error(body.Failure);
        var patch = body.Value!;
        if (patch is { Volume: null, Muted: null, AutoFill: null })
            return Error(Result.Fail(QueueError.InvalidArgument,
                "a zone patch needs volume, muted, autoFill, or more of them"));
        if (patch.AutoFill is { } autoFill)
        {
            var set = house.SetAutoFill(zone, autoFill);
            if (!set.IsSuccess)
                return Error(set.Failure);
        }
        if (patch.Volume is null && patch.Muted is null)
            return Answer(house.Zone(zone));
        return Answer(await house.SetZoneLevelAsync(zone, patch.Volume, patch.Muted, context.RequestAborted)
            .ConfigureAwait(false));
    }

    // Phase 26: what the house played, newest first; one zone's with ?zone=,
    // in a user's scope with ?user= (phase 36).
    private IResult History(HttpContext context)
    {
        if (UnknownQuery(context.Request, "zone", "user", "only", "offset", "limit") is { } unknown)
            return Error(unknown);
        var page = Page(context.Request);
        if (!page.IsSuccess)
            return Error(page.Failure);
        var only = Only(context.Request);
        if (!only.IsSuccess)
            return Error(only.Failure);
        var zone = context.Request.Query["zone"].ToString();
        var read = history.Page(zone.Length == 0 ? null : zone, page.Value.Offset, page.Value.Limit,
            context.Request.Query["user"].ToString(), only.Value);
        return read.IsSuccess
            ? Json(new HistoryPage(read.Value.Items, page.Value.Offset, read.Value.Total))
            : Error(read.Failure);
    }

    // Phase 33: where a listener stopped in each book, newest first, in a
    // user's scope with ?user=.
    private IResult BookPositionsPage(HttpContext context)
    {
        if (UnknownQuery(context.Request, "user", "offset", "limit") is { } unknown)
            return Error(unknown);
        var page = Page(context.Request);
        if (!page.IsSuccess)
            return Error(page.Failure);
        var read = bookPositions.Page(page.Value.Offset, page.Value.Limit, context.Request.Query["user"].ToString());
        return read.IsSuccess
            ? Json(new BookPositionPage(read.Value.Items, page.Value.Offset, read.Value.Total))
            : Error(read.Failure);
    }

    private async Task<IResult> PlayAsync(string zone, HttpContext context)
    {
        var body = await ApiJson.ReadAsync<PlayRequest>(context.Request, optional: true, context.RequestAborted)
            .ConfigureAwait(false);
        if (!body.IsSuccess)
            return Error(body.Failure);
        var item = body.Value?.Item is { } id ? new QueueItemId(id) : (QueueItemId?)null;
        return Answer(await queues.PlayAsync(zone, item, context.RequestAborted, body.Value?.FromStart ?? false)
            .ConfigureAwait(false));
    }

    // Transport takes no body: fields sent with it would be ignored.
    private static async Task<IResult> Transport(HttpContext context,
        Func<Task<Result<QueueState, QueueError>>> operation)
    {
        if (context.Request.ContentLength > 0)
            return Error(Result.Fail(ApiRequestError.InvalidRequest, $"{context.Request.Path} takes no body"));
        return Answer(await operation().ConfigureAwait(false));
    }

    private Task<IResult> PatchQueueAsync(string zone, HttpContext context) =>
        WithBody<QueuePatch>(context, async patch =>
        {
            if (patch.Repeat is null && patch.Shuffle is null)
                return Result.Fail(QueueError.InvalidArgument, "a queue patch needs repeat, shuffle, or both");
            var state = queues.GetQueue(zone);
            if (patch.Repeat is { } repeat)
            {
                state = await queues.SetRepeatAsync(zone, repeat, context.RequestAborted).ConfigureAwait(false);
                if (!state.IsSuccess)
                    return state;
            }
            if (patch.Shuffle is { } shuffle)
                state = await queues.SetShuffleAsync(zone, shuffle, context.RequestAborted).ConfigureAwait(false);
            return state;
        });

    // Tracks or a station by reference, as a search or a browse answered them;
    // or a stored playlist or favourite, which the playlist player checks.
    private async Task<IResult> AddItemsAsync(string zone, HttpContext context)
    {
        var body = await ApiJson.ReadAsync<AddItemsRequest>(context.Request, optional: false, context.RequestAborted)
            .ConfigureAwait(false);
        if (!body.IsSuccess)
            return Error(body.Failure);
        var r = body.Value!;
        var ct = context.RequestAborted;

        var kinds = (r.Tracks is null ? 0 : 1) + (r.Station is null ? 0 : 1) + (r.Playlist is null ? 0 : 1) +
            (r.Favorite is null ? 0 : 1) + (r.Node is null ? 0 : 1);
        if (kinds != 1)
            return Error(Result.Fail(ApiRequestError.InvalidRequest, "exactly one of tracks, station, playlist, favorite or node"));
        if (r.Next && r.Replace)
            return Error(Result.Fail(ApiRequestError.InvalidRequest, "next and replace place differently: one of them"));
        if (r.Index is not null && (r.Next || r.Replace))
            return Error(Result.Fail(ApiRequestError.InvalidRequest, "index places at the end's turn, not with next or replace"));
        var placement = r.Replace ? QueuePlacement.Replace : r.Next ? QueuePlacement.Next : QueuePlacement.End;

        if (r.Playlist is { } playlist)
            return Answer(await player.EnqueuePlaylistAsync(zone, playlist, placement, ct, r.FromStart, r.Index).ConfigureAwait(false));
        if (r.Favorite is { } favorite)
            return Answer(await player.EnqueueFavoriteAsync(zone, favorite, placement, ct, r.FromStart, r.Index).ConfigureAwait(false));
        if (r.Node is { } node)
        {
            return Answer(await player.EnqueueNodeAsync(zone, node.Provider, node.Id, placement, ct, r.Group, r.FromStart, r.Index)
                .ConfigureAwait(false));
        }
        if (r.Station is { } station)
        {
            return Answer(placement switch
            {
                QueuePlacement.Next => await queues.AddNextAsync(zone, station, ct).ConfigureAwait(false),
                QueuePlacement.Replace => await queues.ReplaceAsync(zone, station, ct).ConfigureAwait(false),
                _ => await queues.AddAsync(zone, station, r.Index, ct).ConfigureAwait(false),
            });
        }
        if (r.Tracks!.Count == 0)
            return Error(Result.Fail(ApiRequestError.InvalidRequest, "tracks is empty"));
        return Answer(placement switch
        {
            QueuePlacement.Next => await queues.AddNextAsync(zone, r.Tracks, ct).ConfigureAwait(false),
            QueuePlacement.Replace => await queues.ReplaceAsync(zone, r.Tracks, ct).ConfigureAwait(false),
            _ => await queues.AddAsync(zone, r.Tracks, r.Index, ct).ConfigureAwait(false),
        });
    }

    // The body read strictly, then the operation, answered with `status`.
    private static async Task<IResult> WithBody<T, TValue, TCode>(HttpContext context,
        Func<T, Task<Result<TValue, TCode>>> operation, int status = StatusCodes.Status200OK)
        where T : class
        where TCode : struct, Enum
    {
        var body = await ApiJson.ReadAsync<T>(context.Request, optional: false, context.RequestAborted)
            .ConfigureAwait(false);
        if (!body.IsSuccess)
            return Error(body.Failure);
        return Answer(await operation(body.Value!).ConfigureAwait(false), status);
    }

    private static Task<IResult> WithBody<T>(HttpContext context, Func<T, Task<Result<QueueState, QueueError>>> operation)
        where T : class => WithBody<T, QueueState, QueueError>(context, operation);

    private Result<QueueSaved, LibraryError> SaveQueue(string zone, string name, bool overwrite, long? folder)
    {
        var queue = queues.GetQueue(zone);
        if (!queue.IsSuccess)
            return Result.Fail(LibraryError.NotFound, queue.Failure.Detail);
        var items = queue.Value.Items;
        var saved = playlists.SaveAs(name, items, overwrite, folder);
        return saved.IsSuccess
            ? new QueueSaved(saved.Value, items.Count(i => i.Track is null && i.Station is null && i.Group is not { Parts.Count: > 0 }))
            : saved.Failure;
    }
}

// Phase 26: one page of the play history, newest first.
public sealed record HistoryPage(IReadOnlyList<PlayedTrack> Items, int Offset, int Total);

// Phase 33: one page of where a listener stopped in each book, newest first.
public sealed record BookPositionPage(IReadOnlyList<BookPosition> Items, int Offset, int Total);
