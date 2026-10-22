using Microsoft.Extensions.DependencyInjection;
using Multiroom.Server.Abstractions;
using Multiroom.Server.Library;
using Multiroom.Server.Providers.Covers;

namespace Multiroom.Server.Providers.Local;

public static class LocalFilesServiceCollectionExtensions
{
    // The library (AddLibrary) must be registered too: search and browse read
    // its database, and its folders are this provider's. `directory` holds
    // the picture cache and the kept art: <dataDirectory>/local.
    public static IServiceCollection AddLocalFiles(this IServiceCollection services, string directory) =>
        services
            .AddSingleton(new ArtStore(Path.Combine(directory, "art")))
            .AddProvider(LocalFilesProvider.ProviderId, s => new LocalFilesProvider(
                s.GetRequiredService<LibraryFolders>(),
                new LibraryCatalog(s.GetRequiredService<LibraryDatabase>(), LocalFilesProvider.ProviderId),
                s.GetRequiredService<LibraryIndexer>(), Path.Combine(directory, "artwork"),
                s.GetRequiredService<ArtStore>()));
}

// Files on this machine's disks, on the shares it mounts, and on the NAS
// shares it reads itself over SMB: the library's folders, from the config and
// from the API. Search and browse are the library's (phases 5 and 14), and a
// library track's ProviderItemId is "track:<id>", which survives the file
// moving. Only a file inside one of the enabled folders resolves, through
// that folder's own file access. A playable node (an album, an artist, a
// folder, ...) expands to its tracks. Artwork is what the artwork add-in
// keeps on this server (ArtStore: an album's cover, an artist's picture),
// else a file's embedded picture or the cover beside it, kept in a bounded
// cache; an album or track without either shows its artist's picture.
public sealed class LocalFilesProvider(LibraryFolders folders, LibraryCatalog catalog, LibraryIndexer indexer,
    string artworkDirectory, ArtStore? store = null)
    : IProvider, ISearchProvider, IBrowseProvider, IStreamResolver, IArtworkProvider, IContainerExpander
{
    public const string ProviderId = LibraryCatalog.LocalProvider;

    // Covers are small; a few hundred albums' fit.
    public const long ArtworkCacheBytes = 64 * 1024 * 1024;

    private readonly ArtworkCache _artwork = new(artworkDirectory, ArtworkCacheBytes);

    public string Id => ProviderId;

    public string DisplayName => "Local files";

    public ProviderCapabilities Capabilities => ProviderCapabilities.Search | ProviderCapabilities.Browse |
        ProviderCapabilities.Stream | ProviderCapabilities.Artwork | ProviderCapabilities.Expand;

    // A disk folder is looked at now; an smb folder is as the indexer last
    // found it, since asking a NAS is not a status call's to wait on.
    public Task<ProviderStatus> GetStatusAsync(CancellationToken cancellationToken)
    {
        var enabled = indexer.Folders.Where(f => f.Enabled).ToList();
        var missing = enabled.FirstOrDefault(f => f.Type == LibraryFolderType.Smb
            ? f.State is LibraryFolderState.Unreachable or LibraryFolderState.AuthFailed
            : !Directory.Exists(f.Path));
        var progress = indexer.Status;
        var sessions = folders.Sessions.Status;
        var (pictures, bytes) = _artwork.Held;
        var detail = $"{enabled.Count} folder(s); scanned {progress.Scanned}, indexed {progress.Indexed}, " +
            $"failed {progress.Failed}, queue {progress.QueueDepth}; " +
            $"artwork {pictures} image(s), {bytes} of {_artwork.Capacity} bytes" +
            (sessions.Count == 0 ? "" : "; smb " + string.Join(", ", sessions.Select(s =>
                $"{s.Server} {s.Sessions} of {Library.Smb.SmbSessions.SessionsPerServer} session(s), {s.InUse} in use")));
        return Task.FromResult(missing is null
            ? new ProviderStatus(ProviderState.Ready, detail)
            : new ProviderStatus(ProviderState.Unavailable, $"{missing.Path} is not reachable; {detail}"));
    }

    public Task<Result<CatalogItems, ProviderError>> SearchAsync(string text, SearchKinds kinds, int offset, int limit,
        CancellationToken cancellationToken) => catalog.SearchAsync(text, kinds, offset, limit, cancellationToken);

    public Task<Result<BrowsePage, ProviderError>> BrowseAsync(BrowseRequest request,
        CancellationToken cancellationToken) => catalog.BrowseAsync(request, cancellationToken);

    public Task<Result<ExpandedContainer, ProviderError>> ExpandAsync(string nodeId, int limit,
        CancellationToken cancellationToken) => catalog.ExpandAsync(nodeId, limit, cancellationToken);

    // A library track ("track:<id>") resolves to wherever its file is now. A
    // full path (or an smb URI) inside a folder resolves as itself, so a file
    // can be queued before the indexer reaches it.
    public Task<Result<ResolvedStream, ProviderError>> ResolveAsync(Track track, CancellationToken cancellationToken)
    {
        var path = track.ProviderItemId;
        if (path.StartsWith(LibraryCatalog.TrackPrefix, StringComparison.Ordinal))
        {
            var found = catalog.PathOf(path);
            if (!found.IsSuccess)
                return Task.FromResult<Result<ResolvedStream, ProviderError>>(found.Failure);
            path = found.Value;
        }

        return folders.FilesFor(path) is { } files
            ? files.ResolveAsync(path, cancellationToken)
            : Task.FromResult<Result<ResolvedStream, ProviderError>>(
                Result.Fail(ProviderError.NotFound, $"{path} is in no local folder"));
    }

    // "album:<id>": the cover the store keeps, else the picture of the
    // album's first track; "track:<id>" (a loose track): the track's picture;
    // either, without one, the picture of its artist; "artist:<id>": the
    // artist's picture.
    public async Task<Result<Artwork, ProviderError>> GetArtworkAsync(ArtworkRef reference,
        CancellationToken cancellationToken)
    {
        if (reference.ProviderId != ProviderId)
            return Result.Fail(ProviderError.NotFound, $"no artwork {reference.Reference}");
        ArtworkNames? names = null;
        if (store is not null && catalog.ArtworkNamesOf(reference.Reference) is { IsSuccess: true } named)
            names = named.Value;
        if (names is { Album: { } album, AlbumArtist: { } albumArtist } &&
            store!.ReadAlbum(album, albumArtist) is { IsSuccess: true } kept)
        {
            return kept;
        }
        var own = await OwnPictureAsync(reference.Reference, cancellationToken).ConfigureAwait(false);
        if (own.IsSuccess || own.Failure.Code != ProviderError.NotFound ||
            (names?.Artist ?? names?.AlbumArtist) is not { } artist)
        {
            return own;
        }
        return store!.ReadArtist(artist) is { IsSuccess: true } picture ? picture : own;
    }

    // Read off the request's thread: a share is synchronous.
    private async Task<Result<Artwork, ProviderError>> OwnPictureAsync(string reference,
        CancellationToken cancellationToken)
    {
        var file = catalog.ArtworkFileOf(reference);
        if (!file.IsSuccess)
            return file.Failure;
        var (path, modified) = file.Value;
        if (folders.FilesFor(path) is not { } files)
            return Result.Fail(ProviderError.NotFound, $"{path} is in no local folder");
        return await _artwork.GetAsync($"{reference} {path} {modified}",
            ct => Task.Run(() => files.ReadPicture(path), ct), cancellationToken).ConfigureAwait(false);
    }
}
