using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Http;
using Multiroom.Server.Abstractions;
using Multiroom.Server.Library;

namespace Multiroom.Server.Web.Api;

// One item's marks, in the order asked: whether it is liked and a favourite,
// with the id to DELETE likes/{id} or favorites/{id} by.
public sealed record ItemStatus(LikeKind Kind, string ProviderId, string ProviderItemId, bool Liked, long? LikeId,
    bool Favorite, long? FavoriteId);

// Likes (LikeStore): tracks, albums, artists and stations the household
// liked, the taste signal for recommendations. A new like publishes
// like.changed with the like, a removed one {id, removed: true}; liking an
// item already liked answers its like and publishes nothing.
//
// items/status answers, for a list of items as a UI shows them, which are
// liked and which are favourites.
//
// Users (phase 36, UserStore): a like, a favourite or a play may be one
// person's. A write's `user` is whose it is, absent or "" the household's; a
// read's is a scope: the household's alone, `user=alice` hers with the
// household's (an item both marked answered once, as hers), `user=*` every
// row with its user.
public sealed partial class ApiEndpoint
{
    private void MapLikes(WebApplication app)
    {
        app.MapGet("/api/v1/likes", (Delegate)((HttpContext c) =>
        {
            if (UnknownQuery(c.Request, "kind", "user") is { } unknown)
                return Error(unknown);
            var user = c.Request.Query["user"].ToString();
            var text = c.Request.Query["kind"].ToString();
            if (text.Length == 0)
                return Answer(likes.Likes(null, user));
            return Enum.TryParse<LikeKind>(text, ignoreCase: true, out var kind) && Enum.IsDefined(kind)
                ? Answer(likes.Likes(kind, user))
                : Error(Result.Fail(ApiRequestError.InvalidRequest, $"kind: track, album, artist, station or audiobook, was {text}"));
        }));
        app.MapGet("/api/v1/users", () => Answer(users.Users()));
        app.MapPost("/api/v1/users", (Delegate)((HttpContext c) =>
            Sync<UserCreate, HouseholdUser>(c, r => users.Add(r.Name), StatusCodes.Status201Created)));
        app.MapDelete("/api/v1/users/{name}", (string name) => Deleted(users.Remove(name)));
        app.MapPost("/api/v1/likes", (Delegate)((HttpContext c) =>
            WithBody<LikeRequest, LikedItem, LibraryError>(c, r => AddLikeAsync(r, c.RequestAborted),
                StatusCodes.Status201Created)));
        app.MapDelete("/api/v1/likes/{like}", (string like) => WithId(like, "like", id =>
        {
            return Task.FromResult(Deleted(likes.Remove(id)));
        }));
        app.MapPost("/api/v1/items/status", (Delegate)((HttpContext c) =>
            Sync<ItemStatusRequest, List<ItemStatus>>(c, ItemStatuses)));
    }

    // Phase 37: a node is liked as a book, and an album the server classifies
    // as a book is stored and answered as one (AudiobookLikes); a client need
    // not know media types.
    private async Task<Result<LikedItem, LibraryError>> AddLikeAsync(LikeRequest request,
        CancellationToken cancellationToken)
    {
        if (request.Metadata is { } metadata)
        {
            if (request.Track is not null || request.Album is not null || request.Artist is not null ||
                request.Station is not null || request.Node is not null)
            {
                return Result.Fail(LibraryError.InvalidArgument, "metadata must be the only liked item");
            }
            var track = RadioTrackMetadata.FromMetadata(metadata);
            if (track is null) return Result.Fail(LibraryError.InvalidArgument, "metadata.title must not be empty");
            request = new LikeRequest(Track: track, User: request.User);
        }
        var user = request.User;
        var added = request switch
        {
            { Track: { } track, Album: null, Artist: null, Station: null, Node: null } => likes.Add(track, user),
            { Track: null, Album: { } album, Artist: null, Station: null, Node: null } =>
                await audiobookLikes.LikeAlbumAsync(album, user, cancellationToken).ConfigureAwait(false),
            { Track: null, Album: null, Artist: { } artist, Station: null, Node: null } => likes.Add(artist, user),
            { Track: null, Album: null, Artist: null, Station: { } station, Node: null } => likes.Add(station, user),
            { Track: null, Album: null, Artist: null, Station: null, Node: { } node } =>
                await audiobookLikes.LikeBookAsync(node.Provider, node.Id, user, cancellationToken).ConfigureAwait(false),
            _ => Result.Fail(LibraryError.InvalidArgument, "exactly one of track, album, artist, station or node"),
        };
        return added.IsSuccess ? added.Value.Like : added.Failure;
    }

    private Result<List<ItemStatus>, LibraryError> ItemStatuses(ItemStatusRequest request)
    {
        if (request.Items.Count is 0 or > LikeStore.MaxLookup)
            return Result.Fail(LibraryError.InvalidArgument, $"items: 1 to {LikeStore.MaxLookup}");
        if (request.Items.Any(i => !Enum.IsDefined(i.Kind)))
            return Result.Fail(LibraryError.InvalidArgument, "items.kind: track, album, artist, station or audiobook");
        var liked = likes.Liked(request.Items, request.User);
        if (!liked.IsSuccess)
            return liked.Failure;
        var favorited = playlists.Favorited(request.Items, request.User);
        if (!favorited.IsSuccess)
            return favorited.Failure;
        return request.Items.Select(i =>
        {
            long? like = liked.Value.TryGetValue(i, out var l) ? l : null;
            long? favorite = favorited.Value.TryGetValue(i, out var f) ? f : null;
            return new ItemStatus(i.Kind, i.ProviderId, i.ProviderItemId, like is not null, like, favorite is not null,
                favorite);
        }).ToList();
    }
}
