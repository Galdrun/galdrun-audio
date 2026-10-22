using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Http;
using Microsoft.AspNetCore.Http.Features;
using Multiroom.Server.Abstractions;
using Multiroom.Server.Library;

namespace Multiroom.Server.Web.Api;

// This server's name (GALDRUN-SERVER-#### until set) and whether it has an image.
public sealed record ServerView(string Name, bool HasImage);

public sealed record ServerPatch(string Name);

// The server's name, and the images of the server and of each room, kept in
// the library database (ServerProfileStore). An image is PUT as the raw bytes
// of a JPEG, PNG, GIF or WebP (its type read from them, whatever the
// Content-Type says) and answered with its type. A room's image is kept by
// its client name, whether or not the core knows the room.
//
// A change publishes server.changed (the ServerView) or room.imageChanged
// ({client, hasImage}); removing an image that is not there publishes nothing.
public sealed partial class ApiEndpoint
{
    private void MapServer(WebApplication app)
    {
        app.MapGet("/api/v1/server", () => Answer(ServerViewOf()));
        // The house's playback settings (groupBooks, resumeBooks, findRadioLikes), kept in playback.json;
        // a change publishes settings.playbackChanged.
        app.MapGet("/api/v1/settings/playback", () => Json(player.Settings.Current));
        app.MapMethods("/api/v1/settings/playback", [HttpMethods.Patch], (Delegate)(async (HttpContext c) =>
        {
            var body = await ApiJson.ReadAsync<PlaybackSettingsPatch>(c.Request, optional: false, c.RequestAborted)
                .ConfigureAwait(false);
            if (!body.IsSuccess)
                return Error(body.Failure);
            if (body.Value! is { GroupBooks: null, ResumeBooks: null, FindRadioLikes: null })
            {
                return Error(Result.Fail(ApiRequestError.InvalidRequest,
                    "a playback settings patch needs groupBooks, resumeBooks, findRadioLikes, or several"));
            }
            var before = player.Settings.Current;
            var after = player.Settings.Set(body.Value.GroupBooks, body.Value.ResumeBooks, body.Value.FindRadioLikes);
            if (after != before)
                events.Publish("settings.playbackChanged", after);
            return Json(after);
        }));
        app.MapMethods("/api/v1/server", [HttpMethods.Patch], (Delegate)((HttpContext c) =>
            WithBody<ServerPatch, ServerView, LibraryError>(c, p =>
            {
                var named = profile.SetName(p.Name);
                return Task.FromResult(named.IsSuccess ? ServerChanged() : named.Failure);
            })));
        MapImage(app, "/api/v1/server/image", _ => ServerProfileStore.ServerOwner, (_, _) => ServerChanged());
        MapImage(app, "/api/v1/rooms/{client}/image",
            c => ServerProfileStore.RoomOwner(ClientOf(c)),
            (c, hasImage) => events.Publish("room.imageChanged", new { client = ClientOf(c), hasImage }));
    }

    private static string ClientOf(HttpContext context) => (string)context.Request.RouteValues["client"]!;

    // The view, published as server.changed when it could be read.
    private Result<ServerView, LibraryError> ServerChanged()
    {
        var view = ServerViewOf();
        if (view.IsSuccess)
            events.Publish("server.changed", view.Value);
        return view;
    }

    // `changed` is told whether the owner now has an image.
    private void MapImage(WebApplication app, string route, Func<HttpContext, string> owner,
        Action<HttpContext, bool> changed)
    {
        app.MapGet(route, (Delegate)((HttpContext c) =>
        {
            var image = profile.Image(owner(c));
            return image.IsSuccess ? Results.Bytes(image.Value.Bytes, image.Value.ContentType) : Error(image.Failure);
        }));
        app.MapPut(route, (Delegate)(async (HttpContext c) =>
        {
            var bytes = await ReadImageAsync(c).ConfigureAwait(false);
            if (!bytes.IsSuccess)
                return Error(bytes.Failure);
            var set = profile.SetImage(owner(c), bytes.Value);
            if (!set.IsSuccess)
                return Error(set.Failure);
            changed(c, true);
            return Json(new { contentType = set.Value });
        }));
        app.MapDelete(route, (Delegate)((HttpContext c) =>
        {
            var removed = profile.RemoveImage(owner(c));
            if (!removed.IsSuccess)
                return Error(removed.Failure);
            if (removed.Value)
                changed(c, false);
            return Results.NoContent();
        }));
    }

    private Result<ServerView, LibraryError> ServerViewOf()
    {
        var name = profile.Name();
        if (!name.IsSuccess)
            return name.Failure;
        var image = profile.HasImage(ServerProfileStore.ServerOwner);
        return image.IsSuccess ? new ServerView(name.Value, image.Value) : image.Failure;
    }

    // The body, at most one byte past MaxImageBytes so a larger one is refused
    // by the store's rule without being read whole. An image may be larger than
    // the server-wide ApiJson.MaxBodyBytes, and this read is bounded already,
    // so Kestrel's limit is lifted for it.
    private static async Task<Result<byte[], LibraryError>> ReadImageAsync(HttpContext context)
    {
        var limit = ServerProfileStore.MaxImageBytes + 1;
        if (context.Features.Get<IHttpMaxRequestBodySizeFeature>() is { IsReadOnly: false } bodySize)
            bodySize.MaxRequestBodySize = null;
        using var buffer = new MemoryStream();
        var chunk = new byte[81920];
        int read;
        while (buffer.Length < limit &&
               (read = await context.Request.Body.ReadAsync(chunk.AsMemory(0, (int)Math.Min(chunk.Length, limit - buffer.Length)),
                   context.RequestAborted).ConfigureAwait(false)) > 0)
        {
            buffer.Write(chunk, 0, read);
        }
        return buffer.Length == 0
            ? Result.Fail(LibraryError.InvalidArgument, "the image's bytes are required")
            : buffer.ToArray();
    }
}
