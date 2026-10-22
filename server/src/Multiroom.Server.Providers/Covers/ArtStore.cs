using Multiroom.Server.Abstractions;

namespace Multiroom.Server.Providers.Covers;

// The artwork store (phase 40), kept on this server whole and for good
// (<dataDirectory>/local/art): every picture a list shows, by its artwork id
// (Artworks). The library's covers, from their files or found on the net, an
// artist's picture, a Tidal cover or a station logo once fetched, a radio
// song's cover found by name. Nothing is ever written to the library's
// folders, and nothing is evicted.
//
// An album is kept by its title and its artist, an artist by its name, each
// whole and folded (Names.Key; "Live" stays): the names survive the library
// being indexed again, its ids may not. A file is named by the id's hash and
// by its image type, under albums/, artists/, songs/ or refs/.
public sealed class ArtStore(string directory)
{
    // Whether names can be kept by: a name of only punctuation folds to nothing.
    public static bool Named(string artist) => Artworks.ArtistId(artist) is not null;

    public static bool Named(string album, string artist) => Artworks.AlbumId(album, artist) is not null;

    // The types a picture can be kept as; another is served, never kept.
    public static bool Keepable(Artwork artwork) => ArtworkCache.Extensions.ContainsKey(artwork.ContentType);

    public bool Has(string id) => Find(PathOf(id)) is not null;

    public Result<Artwork, ProviderError> Read(string id) => ReadFile(PathOf(id), $"no artwork {id}");

    public void Keep(string id, Artwork artwork) => KeepFile(PathOf(id), artwork);

    public bool HasAlbum(string album, string artist) => Find(PathOf(Artworks.AlbumId(album, artist))) is not null;

    public Result<Artwork, ProviderError> ReadAlbum(string album, string artist) =>
        ReadFile(PathOf(Artworks.AlbumId(album, artist)), $"no cover of {album}");

    public void KeepAlbum(string album, string artist, Artwork cover) => KeepFile(PathOf(Artworks.AlbumId(album, artist)), cover);

    public bool HasArtist(string artist) => Find(PathOf(Artworks.ArtistId(artist))) is not null;

    public Result<Artwork, ProviderError> ReadArtist(string artist) =>
        ReadFile(PathOf(Artworks.ArtistId(artist)), $"no picture of {artist}");

    public void KeepArtist(string artist, Artwork picture) => KeepFile(PathOf(Artworks.ArtistId(artist)), picture);

    // How many pictures it keeps, and their bytes.
    public (int Files, long Bytes) Held()
    {
        if (!Directory.Exists(directory))
            return (0, 0);
        var files = new DirectoryInfo(directory).EnumerateFiles("*", SearchOption.AllDirectories)
            .Where(f => f.Extension != ".tmp").ToList();
        return (files.Count, files.Sum(f => f.Length));
    }

    // Whether the album's kept cover is these bytes.
    public bool HoldsAlbum(string album, string artist, Artwork cover) =>
        ReadAlbum(album, artist) is { IsSuccess: true } kept && kept.Value.ContentType == cover.ContentType &&
        kept.Value.Bytes.Span.SequenceEqual(cover.Bytes.Span);

    // albums/<hash>, artists/<hash>, songs/<hash>, refs/<hash>: the file's
    // name without its type; null for no id or one not ours.
    private string? PathOf(string? id) =>
        id is not null && Artworks.Parse(id) is ({ } kind, { } hash) ? Path.Combine(directory, kind + "s", hash) : null;

    private static Result<Artwork, ProviderError> ReadFile(string? stem, string none)
    {
        if (Find(stem) is not var (path, type))
            return Result.Fail(ProviderError.NotFound, none);
        try
        {
            return new Artwork(type, File.ReadAllBytes(path));
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            return Result.Fail(ProviderError.Unavailable, $"{none}: {e.Message}");
        }
    }

    // Written whole under a temporary name, then moved in; a kept file of
    // another type goes. Throws IOException or UnauthorizedAccessException,
    // and IOException for a type it cannot be kept as (Keepable).
    private static void KeepFile(string? stem, Artwork artwork)
    {
        if (stem is null)
            throw new IOException("no name to keep the image by");
        if (!ArtworkCache.Extensions.TryGetValue(artwork.ContentType, out var extension))
            throw new IOException($"an image of type {artwork.ContentType} is not kept");
        var path = stem + extension;
        var temporary = path + ".tmp";
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        File.WriteAllBytes(temporary, artwork.Bytes.Span);
        File.Move(temporary, path, overwrite: true);
        foreach (var other in ArtworkCache.Extensions.Values.Where(e => stem + e != path))
            File.Delete(stem + other);
    }

    private static (string Path, string Type)? Find(string? stem)
    {
        if (stem is null)
            return null;
        foreach (var (type, extension) in ArtworkCache.Extensions)
        {
            if (File.Exists(stem + extension))
                return (stem + extension, type);
        }
        return null;
    }
}
