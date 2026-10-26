using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Http;
using Multiroom.Server.Abstractions;
using Multiroom.Server.Library;
using Multiroom.Server.Providers.WebRadio;

namespace Multiroom.Server.Web.Api;

// Playlists, favourites and radio stations: the library's stores. A station
// is answered as the domain RadioStation, never with its stream URL (rule 7);
// the stations are searched and browsed through the webradio provider, and
// listed here with the folders they lie in.
public sealed partial class ApiEndpoint
{
    private void MapCollections(WebApplication app)
    {
        app.MapGet("/api/v1/playlists", () => Answer(playlists.Playlists()));
        app.MapPost("/api/v1/playlists", (Delegate)((HttpContext c) =>
            Sync<PlaylistCreate, Playlist>(c, r => playlists.CreatePlaylist(r.Name, Folder(r.Folder)), StatusCodes.Status201Created)));
        app.MapMethods("/api/v1/playlists/{playlist}", [HttpMethods.Patch], (string playlist, HttpContext c) =>
            WithId(playlist, "playlist", id => Sync<PlaylistPatch, Playlist>(c, r => PatchPlaylist(id, r))));
        app.MapDelete("/api/v1/playlists/{playlist}", (string playlist) =>
            WithId(playlist, "playlist", id => Task.FromResult(Deleted(playlists.DeletePlaylist(id)))));

        app.MapGet("/api/v1/playlist-folders", () => Answer(playlists.Folders()));
        app.MapPost("/api/v1/playlist-folders", (Delegate)((HttpContext c) =>
            Sync<FolderCreate, CollectionFolder>(c, r => playlists.CreateFolder(r.Name, Folder(r.Parent)),
                StatusCodes.Status201Created)));
        app.MapMethods("/api/v1/playlist-folders/{folder}", [HttpMethods.Patch], (string folder, HttpContext c) =>
            WithId(folder, "playlist folder", id => Sync<FolderPatch, CollectionFolder>(c,
                r => PatchFolder(id, r, playlists.RenameFolder, playlists.MoveFolder))));
        app.MapDelete("/api/v1/playlist-folders/{folder}", (string folder) =>
            WithId(folder, "playlist folder", id => Task.FromResult(Deleted(playlists.DeleteFolder(id)))));

        app.MapGet("/api/v1/playlists/{playlist}/entries", (string playlist) =>
            WithId(playlist, "playlist", id => Task.FromResult(Answer(playlists.Entries(id)))));
        app.MapPost("/api/v1/playlists/{playlist}/entries", (string playlist, HttpContext c) =>
            WithId(playlist, "playlist", id => AddEntriesAsync(id, c)));
        app.MapMethods("/api/v1/playlists/{playlist}/entries/{entry}", [HttpMethods.Patch],
            (string playlist, string entry, HttpContext c) => WithId(playlist, "playlist", id =>
                WithId(entry, "entry", e => Sync<ItemMove, IReadOnlyList<PlaylistEntry>>(c,
                    r => playlists.MoveEntry(id, e, r.Index)))));
        app.MapDelete("/api/v1/playlists/{playlist}/entries/{entry}", (string playlist, string entry) =>
            WithId(playlist, "playlist", id =>
                WithId(entry, "entry", e => Task.FromResult(Answer(playlists.RemoveEntry(id, e))))));

        app.MapGet("/api/v1/favorites", (Delegate)((HttpContext c) =>
        {
            if (UnknownQuery(c.Request, "user", "only") is { } unknown)
                return Error(unknown);
            var only = Only(c.Request);
            return only.IsSuccess
                ? Answer(playlists.Favorites(c.Request.Query["user"].ToString(), only.Value))
                : Error(only.Failure);
        }));
        app.MapPost("/api/v1/favorites", (Delegate)((HttpContext c) =>
            WithBody<FavoriteRequest, Favorite, LibraryError>(c, r => AddFavoriteAsync(r, c.RequestAborted),
                StatusCodes.Status201Created)));
        app.MapDelete("/api/v1/favorites/{favorite}", (string favorite) =>
            WithId(favorite, "favorite", id => Task.FromResult(Deleted(playlists.RemoveFavorite(id)))));

        // Every station of the webradio provider by name, each with its folder.
        app.MapGet("/api/v1/stations", () =>
        {
            var all = stations.Stations(0, int.MaxValue);
            return all.IsSuccess ? Json(all.Value.Select(ToEntry).ToList()) : Error(all.Failure);
        });
        app.MapPost("/api/v1/stations", (Delegate)((HttpContext c) => Sync<NewStation, RadioStation>(c,
            r => Station(stations.Add(r with { Folder = Folder(r.Folder) })), StatusCodes.Status201Created)));
        app.MapPost("/api/v1/stations/import", (Delegate)((HttpContext c) => Sync<StationImport, List<RadioStation>>(c,
            r =>
            {
                var parsed = StationPlaylist.Parse(r.Playlist);
                if (!parsed.IsSuccess)
                    return parsed.Failure;
                var imported = stations.Import([.. parsed.Value.Select(s => s with { Folder = Folder(r.Folder) })]);
                if (!imported.IsSuccess)
                    return imported.Failure;
                var added = imported.Value.Select(WebRadioProvider.ToStation).ToList();
                return added;
            }, StatusCodes.Status201Created)));
        app.MapDelete("/api/v1/stations/{station}", (string station) =>
            WebRadioProvider.IdOf(station) is { } id
                ? Deleted(stations.Remove(id))
                : Error(Result.Fail(LibraryError.NotFound, $"no station {station}")));
        app.MapMethods("/api/v1/stations/{station}", [HttpMethods.Patch], (string station, HttpContext c) =>
            WebRadioProvider.IdOf(station) is { } id
                ? Sync<StationPatch, StationEntry>(c, r =>
                {
                    var moved = stations.MoveTo(id, Folder(r.Folder));
                    return moved.IsSuccess ? ToEntry(moved.Value) : moved.Failure;
                })
                : Task.FromResult(Error(Result.Fail(LibraryError.NotFound, $"no station {station}"))));

        app.MapGet("/api/v1/station-folders", () => Answer(stations.Folders()));
        app.MapPost("/api/v1/station-folders", (Delegate)((HttpContext c) =>
            Sync<FolderCreate, CollectionFolder>(c, r => stations.CreateFolder(r.Name, Folder(r.Parent)),
                StatusCodes.Status201Created)));
        app.MapMethods("/api/v1/station-folders/{folder}", [HttpMethods.Patch], (string folder, HttpContext c) =>
            WithId(folder, "station folder", id => Sync<FolderPatch, CollectionFolder>(c,
                r => PatchFolder(id, r, stations.RenameFolder, stations.MoveFolder))));
        app.MapDelete("/api/v1/station-folders/{folder}", (string folder) =>
            WithId(folder, "station folder", id => Task.FromResult(Deleted(stations.DeleteFolder(id)))));
    }

    private static StationEntry ToEntry(StoredStation s) => new(WebRadioProvider.ToStation(s), s.Folder);

    private Result<Playlist, LibraryError> PatchPlaylist(long id, PlaylistPatch patch)
    {
        if (patch.Name is null && patch.Position is null && patch.Folder is null)
            return Result.Fail(LibraryError.InvalidArgument, "a playlist patch needs name, position or folder");
        Result<Playlist, LibraryError> result = default;
        if (patch.Name is { } name)
        {
            result = playlists.RenamePlaylist(id, name);
            if (!result.IsSuccess)
                return result;
        }
        if (patch.Folder is { } folder)
        {
            result = playlists.MovePlaylistTo(id, Folder(folder));
            if (!result.IsSuccess)
                return result;
        }
        if (patch.Position is { } position)
            result = playlists.MovePlaylist(id, position);
        return result;
    }

    // A playlist or station folder renamed, moved or both, by its store.
    private static Result<CollectionFolder, LibraryError> PatchFolder(long id, FolderPatch patch,
        Func<long, string, Result<CollectionFolder, LibraryError>> rename,
        Func<long, long?, Result<CollectionFolder, LibraryError>> move)
    {
        if (patch.Name is null && patch.Parent is null)
            return Result.Fail(LibraryError.InvalidArgument, "a folder patch needs name, parent or both");
        Result<CollectionFolder, LibraryError> result = default;
        if (patch.Name is { } name)
        {
            result = rename(id, name);
            if (!result.IsSuccess)
                return result;
        }
        if (patch.Parent is { } parent)
            result = move(id, Folder(parent));
        return result;
    }

    // A folder as the API names it: 0 (or none) is the top.
    private static long? Folder(long? folder) => folder is null or 0 ? null : folder;

    // Tracks as they are given, or a node expanded by its provider: a book
    // (phase 31) is one group entry, its parts stored as they are now.
    private async Task<IResult> AddEntriesAsync(long playlist, HttpContext context)
    {
        var body = await ApiJson.ReadAsync<EntriesRequest>(context.Request, optional: false, context.RequestAborted)
            .ConfigureAwait(false);
        if (!body.IsSuccess)
            return Error(body.Failure);
        var r = body.Value!;
        if ((r.Tracks is null) == (r.Node is null))
            return Error(Result.Fail(ApiRequestError.InvalidRequest, "exactly one of tracks or node"));
        if (r.Tracks is { } tracks)
            return Answer(playlists.AddEntries(playlist, tracks, r.Index));

        var expanded = await player.ExpandAsync(r.Node!.Provider, r.Node.Id, r.Group ?? player.Settings.GroupBooks,
                context.RequestAborted)
            .ConfigureAwait(false);
        if (!expanded.IsSuccess)
            return Error(expanded.Failure);
        return Answer(expanded.Value.Group is { } group
            ? playlists.AddGroup(playlist, group, r.Index)
            : playlists.AddEntries(playlist, expanded.Value.Tracks, r.Index));
    }

    private async Task<Result<Favorite, LibraryError>> AddFavoriteAsync(FavoriteRequest request,
        CancellationToken cancellationToken) => request switch
    {
        { Track: { } track, Station: null, Node: null } => playlists.AddFavorite(track, request.User),
        { Track: null, Station: { } station, Node: null } => playlists.AddFavorite(station, request.User),
        { Track: null, Station: null, Node: { } node } =>
            await FavoriteBookAsync(node, request.User, cancellationToken).ConfigureAwait(false),
        _ => Result.Fail(LibraryError.InvalidArgument, "exactly one of track, station or node"),
    };

    // A node is a favourite only as a book: expanded and grouped as the
    // queue groups it (PlaylistPlayer.ExpandAsync).
    private async Task<Result<Favorite, LibraryError>> FavoriteBookAsync(NodeReference node, string? user,
        CancellationToken cancellationToken)
    {
        var expanded = await player.ExpandAsync(node.Provider, node.Id, group: true, cancellationToken).ConfigureAwait(false);
        if (!expanded.IsSuccess)
        {
            return Result.Fail(expanded.Failure.Code == QueueError.NotFound ? LibraryError.NotFound : LibraryError.InvalidArgument,
                expanded.Failure.Detail);
        }
        return expanded.Value.Group is { } book
            ? playlists.AddFavorite(book, user)
            : Result.Fail(LibraryError.InvalidArgument,
                $"{node.Id} is no book (two or more audiobook tracks): make its tracks favourites");
    }

    private static Result<RadioStation, LibraryError> Station(Result<StoredStation, LibraryError> stored) =>
        stored.IsSuccess ? WebRadioProvider.ToStation(stored.Value) : stored.Failure;

    private static IResult Deleted(Result<bool, LibraryError> result) =>
        result.IsSuccess ? Results.NoContent() : Error(result.Failure);

    private static Task<IResult> WithId(string text, string what, Func<long, Task<IResult>> then)
    {
        var id = IdOf(text, what);
        return id.IsSuccess ? then(id.Value) : Task.FromResult(Error(id.Failure));
    }

    // The stores answer synchronously.
    private static Task<IResult> Sync<T, TValue>(HttpContext context, Func<T, Result<TValue, LibraryError>> operation,
        int status = StatusCodes.Status200OK)
        where T : class =>
        WithBody<T, TValue, LibraryError>(context, r => Task.FromResult(operation(r)), status);
}
