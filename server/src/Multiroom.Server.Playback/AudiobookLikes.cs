using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;
using Multiroom.Server.Abstractions;
using Multiroom.Server.Library;

namespace Multiroom.Server.Playback;

// Phase 37: whether a liked album is a book is the server's to say, by the
// classifier PlaylistPlayer.ExpandAsync groups by (phase 30's media type,
// Tidal's heuristic of phase 31). New likes are classified as they come
// (Classify); the album likes from before this phase are asked once, at
// start, on this service's task. One whose provider cannot answer now
// (unavailable) is asked again at the next start; any other answer is final.
public sealed partial class AudiobookLikes(LikeStore likes, PlaylistPlayer player, ILogger<AudiobookLikes> log)
    : BackgroundService
{
    // The book an album is, or null for music and for an album that cannot
    // be expanded (Failure says why).
    public async Task<(Audiobook? Book, Failure<QueueError>? Failure)> ClassifyAsync(string provider, string node,
        CancellationToken cancellationToken)
    {
        var expanded = await player.ExpandAsync(provider, node, group: true, cancellationToken).ConfigureAwait(false);
        if (!expanded.IsSuccess)
            return (null, expanded.Failure);
        return (expanded.Value.Group is { } group ? Audiobook.Of(group) : null, null);
    }

    // A node liked as a book: expanded as FavoriteBookAsync expands one, and
    // refused by name when it is no book.
    public async Task<Result<(LikedItem Like, bool Added), LibraryError>> LikeBookAsync(string provider, string node,
        string? user, CancellationToken cancellationToken)
    {
        var (book, failure) = await ClassifyAsync(provider, node, cancellationToken).ConfigureAwait(false);
        if (failure is { } f)
        {
            return Result.Fail(f.Code == QueueError.NotFound ? LibraryError.NotFound : LibraryError.InvalidArgument,
                f.Detail);
        }
        return book is null
            ? Result.Fail(LibraryError.InvalidArgument,
                $"{node} is no book (two or more audiobook tracks): like its tracks or its album")
            : likes.Add(book, user);
    }

    // An album liked: a book when the server says so, else an album. One
    // that cannot be expanded now is kept as the album it was given as.
    public async Task<Result<(LikedItem Like, bool Added), LibraryError>> LikeAlbumAsync(Album album, string? user,
        CancellationToken cancellationToken)
    {
        var (book, _) = await ClassifyAsync(album.ProviderId, album.ProviderItemId, cancellationToken).ConfigureAwait(false);
        return book is null ? likes.Add(album, user) : likes.Add(book, user);
    }

    protected override async Task ExecuteAsync(CancellationToken stoppingToken)
    {
        var albums = likes.UncheckedAlbums();
        if (!albums.IsSuccess)
        {
            LogUnread(log, albums.Failure.Detail);
            return;
        }
        var (books, later) = (0, 0);
        foreach (var like in albums.Value)
        {
            stoppingToken.ThrowIfCancellationRequested();
            var (book, failure) = await ClassifyAsync(like.Album!.ProviderId, like.Album.ProviderItemId, stoppingToken)
                .ConfigureAwait(false);
            if (failure?.Code == QueueError.Unavailable)
            {
                later++;
                continue;
            }
            var checkedLike = likes.Checked(like.Id, book);
            if (checkedLike is { IsSuccess: true, Value: true })
                books++;
        }
        if (albums.Value.Count > 0)
            LogChecked(log, albums.Value.Count, books, later);
    }

    [LoggerMessage(Level = LogLevel.Information,
        Message = "liked albums checked for books: {Albums} asked, {Books} now audiobook likes, {Later} asked again at the next start")]
    private static partial void LogChecked(ILogger log, int albums, int books, int later);

    [LoggerMessage(Level = LogLevel.Warning, Message = "liked albums could not be checked for books: {Reason}")]
    private static partial void LogUnread(ILogger log, string reason);
}
