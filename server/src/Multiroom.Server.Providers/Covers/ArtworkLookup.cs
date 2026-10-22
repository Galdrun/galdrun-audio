using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Logging.Abstractions;
using Multiroom.Server.Abstractions;
using Multiroom.Server.Providers.AddIns;

namespace Multiroom.Server.Providers.Covers;

// Finds a picture on the net by name and keeps it in the artwork store
// (ArtStore): the part of the artwork add-in that is not the library's files,
// shared since phase 40 with ArtworkService, which asks it for what a list
// shows. One of these per server, so a setting changed through the API, and
// what was not found, count for both.
//
// An album is identified by the MusicBrainz ids it was tagged with, else by
// the resolver (MusicBrainz, by name); then its sources are asked in order
// (IArtworkSource) and the first cover that can be fetched is kept. An
// artist's picture the same way, by the artist's id. A song without an album
// is resolved to the album it is on (MusicBrainzSource.ResolveSongAsync).
// Without the resolver, only tagged ids are used. The resolver and the
// sources are `current`'s for each call.
//
// What was not found is remembered while the server runs (until
// ForgetMissing), so the tracks of one album ask once. A service that cannot
// answer now is Unavailable: the caller asks again later.
public sealed partial class ArtworkLookup(ArtStore store, Func<ArtworkSourceSet> current, HttpClient? http = null,
    ILogger? log = null) : IDisposable
{
    private readonly ILogger _log = log ?? NullLogger.Instance;
    private readonly Lock _lock = new();
    private readonly HashSet<string> _missing = new(StringComparer.Ordinal);

    // Raised after what was not found is forgotten: the settings changed.
    public event EventHandler? Forgotten;

    public ArtworkSourceSet Current => current();

    public void ForgetMissing()
    {
        lock (_lock)
            _missing.Clear();
        Forgotten?.Invoke(this, EventArgs.Empty);
    }

    // The album's cover kept under its album id: "present" (kept already),
    // "added" (found now) or "notFound"; and the album artist's id as it is
    // known now.
    public async Task<Result<(string Cover, string? ArtistId), AddInError>> CoverAsync(AlbumQuery query,
        CancellationToken cancellationToken)
    {
        var (album, albumArtist) = (query.Album, query.Artist);
        if (store.HasAlbum(album, albumArtist))
            return ("present", query.ArtistId);
        var key = Artworks.AlbumId(album, albumArtist)!;
        if (Missing(key))
            return ("notFound", query.ArtistId);
        var set = current();
        var how = query.Identified ? "tagged" : "by name";
        if (set.Resolver is { } resolver)
        {
            var resolved = await resolver.ResolveAlbumAsync(query, cancellationToken).ConfigureAwait(false);
            if (!resolved.IsSuccess)
                return Refused(resolver.Id, resolved.Failure);
            query = resolved.Value;
        }
        if (!query.Identified)
        {
            LogAlbumUnknown(_log, album, albumArtist,
                set.Resolver is null ? "MusicBrainz is off and its tags carry no ids" : "no single match on MusicBrainz");
        }
        else
        {
            LogAlbumIdentified(_log, album, albumArtist, how, query.ReleaseGroupId ?? "-", query.ReleaseId ?? "-",
                query.ArtistId ?? "-");
            var found = await KeepCoverAsync(set, query, key, $"the cover of {album}", cancellationToken)
                .ConfigureAwait(false);
            if (!found.IsSuccess)
                return found.Failure;
            if (found.Value is ({ } source, var bytes))
            {
                LogCoverAdded(_log, album, albumArtist, source, bytes);
                return ("added", query.ArtistId);
            }
            var tried = Names(set);
            LogCoverMissing(_log, album, albumArtist, tried);
        }
        Remember(key);
        return ("notFound", query.ArtistId);
    }

    // The song's cover kept under its song id: "present", "added" or
    // "notFound". Only with the resolver: a song carries no tagged ids.
    public async Task<Result<string, AddInError>> SongAsync(string title, string artist,
        CancellationToken cancellationToken)
    {
        if (Artworks.SongId(title, artist) is not { } id)
            return "notFound";
        if (store.Has(id))
            return "present";
        if (Missing(id))
            return "notFound";
        var set = current();
        if (set.Resolver is { } resolver)
        {
            var resolved = await resolver.ResolveSongAsync(title, artist, cancellationToken).ConfigureAwait(false);
            if (!resolved.IsSuccess)
                return Refused(resolver.Id, resolved.Failure);
            if (resolved.Value is { } query)
            {
                // The album it is on may be kept already.
                if (store.ReadAlbum(query.Album, artist) is { IsSuccess: true } kept)
                {
                    var copied = Keep(() => store.Keep(id, kept.Value), $"the cover of {title}");
                    return copied.IsSuccess ? "added" : copied.Failure;
                }
                var found = await KeepCoverAsync(set, query, id, $"the cover of {title}", cancellationToken)
                    .ConfigureAwait(false);
                if (!found.IsSuccess)
                    return found.Failure;
                if (found.Value is ({ } source, _))
                {
                    LogSongAdded(_log, title, artist, query.Album, source);
                    return "added";
                }
            }
        }
        LogSongMissing(_log, title, artist);
        Remember(id);
        return "notFound";
    }

    // The artist's picture kept under its artist id: "present", "added" or
    // "notFound".
    public async Task<Result<string, AddInError>> PictureAsync(string artist, string? artistId,
        CancellationToken cancellationToken)
    {
        if (store.HasArtist(artist))
            return "present";
        var key = Artworks.ArtistId(artist)!;
        if (Missing(key))
            return "notFound";
        var set = current();
        artistId = MusicBrainzSource.Mbid(artistId);
        if (set.Resolver is { } resolver)
        {
            var resolved = await resolver.ResolveArtistAsync(artist, artistId, cancellationToken).ConfigureAwait(false);
            if (!resolved.IsSuccess)
                return Refused(resolver.Id, resolved.Failure);
            artistId = resolved.Value;
        }
        if (artistId is null)
        {
            LogArtistUnknown(_log, artist,
                set.Resolver is null ? "MusicBrainz is off and its tags carry no id" : "no single match on MusicBrainz");
        }
        else
        {
            foreach (var source in set.Sources)
            {
                var pictures = await source.PicturesAsync(artistId, cancellationToken).ConfigureAwait(false);
                if (!pictures.IsSuccess)
                    return Refused(source.Id, pictures.Failure);
                var fetched = await FirstAsync(pictures.Value, $"the picture of {artist}", cancellationToken)
                    .ConfigureAwait(false);
                if (!fetched.IsSuccess)
                    return Refused(source.Id, fetched.Failure);
                if (fetched.Value is not { } picture)
                    continue;
                var kept = Keep(() => store.KeepArtist(artist, picture), $"the picture of {artist}");
                if (!kept.IsSuccess)
                    return kept.Failure;
                LogPictureAdded(_log, artist, artistId, source.Id, picture.Bytes.Length);
                return "added";
            }
            var tried = Names(set);
            LogPictureMissing(_log, artist, artistId, tried);
        }
        Remember(key);
        return "notFound";
    }

    public void Dispose() => http?.Dispose();

    // Keeps a cover kept already under its own id, or fetched now.
    internal static Result<bool, AddInError> Keep(Action keep, string what)
    {
        try
        {
            keep();
            return true;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            return Result.Fail(AddInError.Failed, $"{what} cannot be kept: {e.Message}");
        }
    }

    // The identified album's first cover the sources hold, kept under `id`:
    // the source's id and the bytes kept, or null when none holds one.
    private async Task<Result<(string Source, int Bytes)?, AddInError>> KeepCoverAsync(ArtworkSourceSet set,
        AlbumQuery query, string id,
        string what, CancellationToken cancellationToken)
    {
        foreach (var source in set.Sources)
        {
            var covers = await source.CoversAsync(query, cancellationToken).ConfigureAwait(false);
            if (!covers.IsSuccess)
                return Refused(source.Id, covers.Failure);
            var fetched = await FirstAsync(covers.Value, what, cancellationToken).ConfigureAwait(false);
            if (!fetched.IsSuccess)
                return Refused(source.Id, fetched.Failure);
            if (fetched.Value is not { } cover)
                continue;
            var kept = Keep(() => store.Keep(id, cover), what);
            if (!kept.IsSuccess)
                return kept.Failure;
            return (source.Id, cover.Bytes.Length);
        }
        return ((string, int)?)null;
    }

    private bool Missing(string key)
    {
        lock (_lock)
            return _missing.Contains(key);
    }

    private void Remember(string key)
    {
        lock (_lock)
            _missing.Add(key);
    }

    private static string Names(ArtworkSourceSet set) =>
        set.Sources.Count == 0 ? "no source" : string.Join(", ", set.Sources.Select(s => s.Id));

    [LoggerMessage(Level = LogLevel.Information,
        Message = "artwork: {Album} by {Artist} ({How}): release group {ReleaseGroup}, release {Release}, artist {ArtistId}")]
    private static partial void LogAlbumIdentified(ILogger log, string album, string artist, string how,
        string releaseGroup, string release, string artistId);

    [LoggerMessage(Level = LogLevel.Information, Message = "artwork: {Album} by {Artist} not identified: {Reason}")]
    private static partial void LogAlbumUnknown(ILogger log, string album, string artist, string reason);

    [LoggerMessage(Level = LogLevel.Information, Message = "artwork: the cover of {Album} by {Artist} added from {Source} ({Bytes} bytes)")]
    private static partial void LogCoverAdded(ILogger log, string album, string artist, string source, int bytes);

    [LoggerMessage(Level = LogLevel.Information, Message = "artwork: no cover of {Album} by {Artist} at {Sources}")]
    private static partial void LogCoverMissing(ILogger log, string album, string artist, string sources);

    [LoggerMessage(Level = LogLevel.Information,
        Message = "artwork: the cover of {Title} by {Artist} (on {Album}) added from {Source}")]
    private static partial void LogSongAdded(ILogger log, string title, string artist, string album, string source);

    [LoggerMessage(Level = LogLevel.Information, Message = "artwork: no cover of the song {Title} by {Artist}")]
    private static partial void LogSongMissing(ILogger log, string title, string artist);

    [LoggerMessage(Level = LogLevel.Information, Message = "artwork: {Artist} not identified: {Reason}")]
    private static partial void LogArtistUnknown(ILogger log, string artist, string reason);

    [LoggerMessage(Level = LogLevel.Information,
        Message = "artwork: the picture of {Artist} ({ArtistId}) added from {Source} ({Bytes} bytes)")]
    private static partial void LogPictureAdded(ILogger log, string artist, string artistId, string source, int bytes);

    [LoggerMessage(Level = LogLevel.Information, Message = "artwork: no picture of {Artist} ({ArtistId}) at {Sources}")]
    private static partial void LogPictureMissing(ILogger log, string artist, string artistId, string sources);

    // The first of the URLs that holds an image, or null; a failure only
    // when its service cannot answer now.
    private async Task<Result<Artwork?, ProviderError>> FirstAsync(IReadOnlyList<Uri> uris, string name,
        CancellationToken cancellationToken)
    {
        foreach (var uri in uris)
        {
            if (http is null)
                return Result.Fail(ProviderError.Unavailable, $"{name}: no client to fetch it with");
            var fetched = await ArtworkCache.FetchAsync(http, uri, name, ArtworkCache.MaxImageBytes, cancellationToken)
                .ConfigureAwait(false);
            if (fetched.IsSuccess)
                return fetched.Value;
            if (fetched.Failure.Code == ProviderError.Unavailable)
                return fetched.Failure;
        }
        return (Artwork?)null;
    }

    private static Failure<AddInError> Refused(string source, Failure<ProviderError> failure) =>
        Result.Fail(failure.Code == ProviderError.Unavailable ? AddInError.Unavailable : AddInError.Failed,
            $"{source}: {failure.Detail}");
}
