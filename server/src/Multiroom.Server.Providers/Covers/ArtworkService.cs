using System.Collections.Concurrent;
using System.Threading.Channels;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;
using Multiroom.Server.Abstractions;
using Multiroom.Server.Library;
using Multiroom.Server.Providers.AddIns;

namespace Multiroom.Server.Providers.Covers;

// The artwork store's size, what waits to be looked for and what was not found.
public sealed record ArtworkStoreStats(int Images, long Bytes, int Pending, int NotFound);

// What a shown item says about its picture (phase 40): the names to look it
// up by and its provider's artwork, any of them null.
public sealed record ArtworkHint(string? Title, string? Artist, string? Album, ArtworkRef? Candidate)
{
    public static ArtworkHint Of(MediaItem item) => item switch
    {
        Track t => new(t.Title, t.Artist, t.Album, t.Artwork),
        Album a => new(null, a.Artist, a.Title, a.Artwork),
        Artist a => new(null, a.Name, null, a.Artwork),
        QueueGroup g => new(null, g.Artist, g.Title, g.Artwork),
        Audiobook b => new(null, b.Author, b.Title, b.Artwork),
        BrowseNode n => new(null, n.Kind == BrowseKind.Artist ? n.Title : null, null, n.Artwork),
        RadioStation s => new(null, null, null, s.Logo),
        _ => new(null, null, null, null),
    };
}

// The items the outward API has shown, by artwork id: what a request for an
// id is answered from when the store does not hold it yet. Ids derive from
// what they name, so one table serves every server in a process (the tests
// run several). Bounded: when full it starts again, and an item shown again
// is noted again.
public static class ArtworkHints
{
    public const int Capacity = 50_000;

    private static readonly ConcurrentDictionary<string, ArtworkHint> Seen = new(StringComparer.Ordinal);

    // The first sighting with a provider's artwork stays.
    public static void Note(MediaItem item)
    {
        if (item.ArtworkId is not { } id || (Seen.TryGetValue(id, out var known) && known.Candidate is not null))
            return;
        if (Seen.Count >= Capacity)
            Seen.Clear();
        var hint = ArtworkHint.Of(item);
        Seen[id] = known is null || hint.Candidate is not null ? hint : known;
    }

    public static ArtworkHint? Of(string id) => Seen.GetValueOrDefault(id);

    // A provider's artwork found for an id later (ArtworkService).
    internal static void Found(string id, ArtworkRef candidate) =>
        Seen.AddOrUpdate(id, _ => new ArtworkHint(null, null, null, candidate), (_, s) => s with { Candidate = candidate });
}

// The artwork store's front (phase 40): GET artwork/{id} for every item a
// list shows. An id is answered from the store (ArtStore); else from the
// artwork its item's provider gave (ArtworkHints), kept for good unless
// it is the library's own (the local provider serves that from the store
// and the files, and its answer may be an artist's picture standing in);
// else, while it is looked for, an album or a song shows its artist's
// picture, and a request for it is NotFound until something is found.
//
// What is not held is looked for on this service's own task, once per id
// while the server runs: first among the providers by name, the library
// before Tidal (SongResolver), then on the net (ArtworkLookup, as the
// artwork settings say), then the artist's picture. What is found is
// announced by Changed, so a UI asks again.
//
// What the house keeps (likes, favourites, playlist entries) is stored
// without being shown: looked at once at start, and a like or a favourite
// when it is added. A library item's picture is the library's, which the
// artwork add-in keeps. What was not found is asked again when the
// settings change, and at the next start.
public sealed partial class ArtworkService(ArtStore store, ArtworkLookup lookup, ProviderRegistry providers,
    SongResolver resolver, LikeStore likes, PlaylistStore playlists, ILogger<ArtworkService> log) : BackgroundService
{
    private const string LocalProvider = Local.LocalFilesProvider.ProviderId;

    private readonly Channel<(string Id, ArtworkHint Hint)> _wanted =
        Channel.CreateUnbounded<(string, ArtworkHint)>(new UnboundedChannelOptions { SingleReader = true });

    // Asked for once each, until the settings change.
    private readonly ConcurrentDictionary<string, byte> _asked = new(StringComparer.Ordinal);

    // Looked for and not found, until the settings change.
    private readonly ConcurrentDictionary<string, byte> _missing = new(StringComparer.Ordinal);

    private int _pending;

    // An id that now has a picture.
    public event EventHandler<string>? Changed;

    // What the store holds, what is waiting to be looked for, and what was not found.
    public ArtworkStoreStats Stats
    {
        get
        {
            var (images, bytes) = store.Held();
            return new ArtworkStoreStats(images, bytes, Volatile.Read(ref _pending), _missing.Count);
        }
    }

    public async Task<Result<Artwork, ProviderError>> GetAsync(string id, CancellationToken cancellationToken)
    {
        if (Artworks.Parse(id) is not ({ } kind, _))
            return Result.Fail(ProviderError.NotFound, $"no artwork {id}");
        var kept = store.Read(id);
        if (kept.IsSuccess || kept.Failure.Code != ProviderError.NotFound)
            return kept;
        var hint = ArtworkHints.Of(id);
        if (hint?.Candidate is { } candidate)
        {
            var fetched = await FetchAsync(candidate, cancellationToken).ConfigureAwait(false);
            if (fetched.IsSuccess)
            {
                if (candidate.ProviderId != LocalProvider)
                    Keep(id, fetched.Value);
                return fetched;
            }
        }
        if (hint is not null)
            Want(id, hint);
        if (kind is Artworks.AlbumKind or Artworks.SongKind && hint?.Artist is { } artist &&
            store.ReadArtist(artist) is { IsSuccess: true } picture)
        {
            return picture;
        }
        return Result.Fail(ProviderError.NotFound, $"no artwork {id} yet");
    }

    public override Task StartAsync(CancellationToken cancellationToken)
    {
        lookup.Forgotten += OnForgotten;
        likes.Added += OnLiked;
        playlists.FavoriteAdded += OnFavorite;
        return base.StartAsync(cancellationToken);
    }

    public override async Task StopAsync(CancellationToken cancellationToken)
    {
        lookup.Forgotten -= OnForgotten;
        likes.Added -= OnLiked;
        playlists.FavoriteAdded -= OnFavorite;
        await base.StopAsync(cancellationToken).ConfigureAwait(false);
    }

    // Stopping ends it quietly, whether it waits for an id or looks one up.
    protected override async Task ExecuteAsync(CancellationToken stoppingToken)
    {
        try
        {
            Sweep();
            await foreach (var (id, hint) in _wanted.Reader.ReadAllAsync(stoppingToken).ConfigureAwait(false))
                await LookAsync(id, hint, stoppingToken).ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (stoppingToken.IsCancellationRequested)
        {
        }
    }

    private async Task LookAsync(string id, ArtworkHint hint, CancellationToken stoppingToken)
    {
        try
        {
            if (store.Has(id))
                return;
            if (await FindAsync(id, hint, stoppingToken).ConfigureAwait(false))
            {
                _missing.TryRemove(id, out _);
                LogFound(log, id);
                Changed?.Invoke(this, id);
            }
            else
            {
                _missing.TryAdd(id, 0);
            }
        }
        catch (OperationCanceledException) when (stoppingToken.IsCancellationRequested)
        {
            throw;
        }
#pragma warning disable CA1031 // One id's failure must not end the loop for every other; it is logged and asked again later.
        catch (Exception e)
#pragma warning restore CA1031
        {
            LogFailed(log, e, id);
            _asked.TryRemove(id, out _);
        }
        finally
        {
            Interlocked.Decrement(ref _pending);
        }
    }

    // Once per id until the settings change.
    private void Want(string id, ArtworkHint hint)
    {
        if (!_asked.TryAdd(id, 0))
            return;
        Interlocked.Increment(ref _pending);
        _wanted.Writer.TryWrite((id, hint));
    }

    // A kept item without its picture in the store is looked for.
    private void Ensure(MediaItem item)
    {
        if (item.ArtworkId is not { } id || store.Has(id))
            return;
        ArtworkHints.Note(item);
        if (ArtworkHints.Of(id) is { } hint)
            Want(id, hint);
    }

    // Every like, favourite and playlist entry, whoever's.
    private void Sweep()
    {
        if (likes.Likes(null, UserStore.Everyone) is { IsSuccess: true } liked)
        {
            foreach (var like in liked.Value)
                Ensure(like.Item);
        }
        if (playlists.Favorites(UserStore.Everyone) is { IsSuccess: true } favorites)
        {
            foreach (var favorite in favorites.Value)
                Ensure(favorite.Item);
        }
        if (playlists.Playlists() is { IsSuccess: true } lists)
        {
            foreach (var list in lists.Value)
            {
                if (playlists.Entries(list.Id) is { IsSuccess: true } entries)
                {
                    foreach (var entry in entries.Value)
                        Ensure(entry.Item);
                }
            }
        }
    }

    private void OnLiked(object? sender, LikedItem like) => Ensure(like.Item);

    private void OnFavorite(object? sender, Favorite favorite) => Ensure(favorite.Item);

    // Whether the id has a picture now.
    private async Task<bool> FindAsync(string id, ArtworkHint hint, CancellationToken cancellationToken)
    {
        // A kept item's own provider first (the library's is the library's to keep).
        if (hint.Candidate is { } candidate && candidate.ProviderId != LocalProvider &&
            await FetchAsync(candidate, cancellationToken).ConfigureAwait(false) is { IsSuccess: true } own)
        {
            Keep(id, own.Value);
            return true;
        }
        if (await MatchAsync(id, hint, cancellationToken).ConfigureAwait(false))
            return true;

        var (title, artist, album) = (hint.Title, hint.Artist, hint.Album);
        var kind = Artworks.Parse(id)!.Value.Kind;
        if (kind == Artworks.AlbumKind && album is not null && artist is not null)
        {
            var cover = await lookup.CoverAsync(new AlbumQuery(artist, album), cancellationToken).ConfigureAwait(false);
            if (Done(id, cover.IsSuccess ? cover.Value.Cover : null, cover))
                return true;
        }
        else if (kind == Artworks.SongKind && title is not null && artist is not null)
        {
            var song = await lookup.SongAsync(title, artist, cancellationToken).ConfigureAwait(false);
            if (Done(id, song.IsSuccess ? song.Value : null, song))
                return true;
        }
        if (kind is Artworks.AlbumKind or Artworks.SongKind or Artworks.ArtistKind && artist is not null &&
            ArtStore.Named(artist))
        {
            var picture = await lookup.PictureAsync(artist, null, cancellationToken).ConfigureAwait(false);
            if (Done(id, picture.IsSuccess ? picture.Value : null, picture))
            {
                // The artist's own id has it too.
                if (Artworks.ArtistId(artist) is { } artistId && artistId != id)
                    Changed?.Invoke(this, artistId);
                return true;
            }
        }
        return false;
    }

    // The same album, song or artist by name among the providers, the
    // library first: its artwork is fetched and kept under the id (or, the
    // library's own, noted as the id's candidate).
    private async Task<bool> MatchAsync(string id, ArtworkHint hint, CancellationToken cancellationToken)
    {
        var (title, artist, album) = (hint.Title, hint.Artist, hint.Album);
        ArtworkRef? found = null;
        switch (Artworks.Parse(id)!.Value.Kind)
        {
            case Artworks.AlbumKind when album is not null && artist is not null:
                var albums = await resolver.ResolveAlbumsAsync([(album, artist)], new HashSet<(string, string)>(),
                    new HashSet<string>(), cancellationToken).ConfigureAwait(false);
                found = albums.IsSuccess ? albums.Value.Albums[0].Album?.Artwork : null;
                break;
            case Artworks.SongKind when title is not null && artist is not null:
                var songs = await resolver.ResolveAsync([new SongSuggestion(title, artist)], [], cancellationToken)
                    .ConfigureAwait(false);
                found = songs.IsSuccess ? songs.Value.Songs[0].Track?.Artwork : null;
                break;
            case Artworks.ArtistKind when artist is not null:
                var artists = await resolver.ResolveArtistsAsync([artist], new HashSet<(string, string)>(),
                    new HashSet<string>(), cancellationToken).ConfigureAwait(false);
                found = artists.IsSuccess ? artists.Value.Artists[0].Artist?.Artwork : null;
                break;
        }
        if (found is null || found == hint.Candidate)
            return false;
        var fetched = await FetchAsync(found, cancellationToken).ConfigureAwait(false);
        if (!fetched.IsSuccess)
            return false;
        if (found.ProviderId == LocalProvider)
            ArtworkHints.Found(id, found);
        else
            Keep(id, fetched.Value);
        LogMatched(log, id, found.ProviderId);
        return true;
    }

    private async Task<Result<Artwork, ProviderError>> FetchAsync(ArtworkRef reference,
        CancellationToken cancellationToken)
    {
        var provider = providers.Get<IArtworkProvider>(reference.ProviderId);
        return provider.IsSuccess
            ? await provider.Value.GetArtworkAsync(reference, cancellationToken).ConfigureAwait(false)
            : provider.Failure;
    }

    private void Keep(string id, Artwork artwork)
    {
        if (!ArtStore.Keepable(artwork))
            return;
        var kept = ArtworkLookup.Keep(() => store.Keep(id, artwork), $"artwork {id}");
        if (!kept.IsSuccess)
            LogNotKept(log, id, kept.Failure.Detail);
    }

    // Whether the lookup holds a picture for it now; a service that could not
    // answer is asked again the next time the id is.
    private bool Done<T>(string id, string? status, Result<T, AddInError> answer)
    {
        if (!answer.IsSuccess)
        {
            LogNotFound(log, id, answer.Failure.Detail);
            if (answer.Failure.Code == AddInError.Unavailable)
                _asked.TryRemove(id, out _);
            return false;
        }
        return status is "added" or "present";
    }

    // The settings changed: what was not found is looked for again.
    private void OnForgotten(object? sender, EventArgs e)
    {
        _asked.Clear();
        _missing.Clear();
        Sweep();
    }

    [LoggerMessage(Level = LogLevel.Information, Message = "artwork {Id}: found")]
    private static partial void LogFound(ILogger log, string id);

    [LoggerMessage(Level = LogLevel.Debug, Message = "artwork {Id}: matched on {Provider}")]
    private static partial void LogMatched(ILogger log, string id, string provider);

    [LoggerMessage(Level = LogLevel.Information, Message = "artwork {Id}: {Reason}")]
    private static partial void LogNotFound(ILogger log, string id, string reason);

    [LoggerMessage(Level = LogLevel.Warning, Message = "artwork {Id}: {Reason}")]
    private static partial void LogNotKept(ILogger log, string id, string reason);

    [LoggerMessage(Level = LogLevel.Warning, Message = "artwork {Id}: the lookup failed")]
    private static partial void LogFailed(ILogger log, Exception exception, string id);
}
