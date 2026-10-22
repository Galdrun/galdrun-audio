using System.Globalization;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json.Serialization;

namespace Multiroom.Server.Abstractions;

// Phase 40: every item a list shows is a MediaItem, and every MediaItem
// names its picture in the one artwork store by ArtworkId (Artworks). The id
// is derived from the item's metadata, never looked up: the same album from
// the library, from Tidal or heard on the radio is one id and one image, and
// an item kept from before has one without being touched.
//
// As a list's `item` it is written with its kind ("track", "album", ...).
[JsonPolymorphic(TypeDiscriminatorPropertyName = "kind")]
[JsonDerivedType(typeof(Track), "track")]
[JsonDerivedType(typeof(Album), "album")]
[JsonDerivedType(typeof(Artist), "artist")]
[JsonDerivedType(typeof(RadioStation), "station")]
[JsonDerivedType(typeof(QueueGroup), "group")]
[JsonDerivedType(typeof(Audiobook), "audiobook")]
public abstract record MediaItem
{
    // Null when nothing names a picture: no album, artist or provider artwork.
    public abstract string? ArtworkId { get; }
}

// A list's entry (a like, a favourite, a playlist entry, a queue item): its
// one item, whichever field holds it. The API writes it as `item`.
public interface IMediaEntry
{
    MediaItem Item { get; }
}

// The artwork store's ids, `<kind>:<hash>`: an album by its title and artist,
// an artist by name, a song without an album by its title and artist, and
// anything else by its provider's artwork reference. Names are folded
// (Names.Key), so spelling and case do not split one album in two.
public static class Artworks
{
    public const string AlbumKind = "album";
    public const string ArtistKind = "artist";
    public const string SongKind = "song";
    public const string RefKind = "ref";

    public static string? AlbumId(string? album, string? artist) =>
        Named(album) && Named(artist) ? $"{AlbumKind}:{Hash(Names.Key(album!) + "|" + Names.Key(artist!))}" : null;

    public static string? ArtistId(string? artist) =>
        Named(artist) ? $"{ArtistKind}:{Hash(Names.Key(artist!))}" : null;

    public static string? SongId(string? title, string? artist) =>
        Named(title) && Named(artist) ? $"{SongKind}:{Hash(Names.Key(title!) + "|" + Names.Key(artist!))}" : null;

    public static string? RefId(ArtworkRef? reference) =>
        reference is null ? null : $"{RefKind}:{Hash(reference.ProviderId + "|" + reference.Reference)}";

    // A track's picture: its album's, else its own song's (a radio song names
    // no album), else whatever its provider gave it.
    public static string? Of(Track track) =>
        AlbumId(track.Album, track.Artist) ?? SongId(track.Title, track.Artist) ?? RefId(track.Artwork);

    // An id's kind and hash, or null when it is not one of ours.
    public static (string Kind, string Hash)? Parse(string id)
    {
        var colon = id.IndexOf(':', StringComparison.Ordinal);
        if (colon < 0)
            return null;
        var (kind, hash) = (id[..colon], id[(colon + 1)..]);
        return kind is AlbumKind or ArtistKind or SongKind or RefKind && hash.Length == 32 &&
            hash.All(c => char.IsAsciiDigit(c) || c is >= 'a' and <= 'f')
                ? (kind, hash)
                : null;
    }

    // The first 16 bytes of the key's SHA-256, lower-case hex: as ArtStore
    // names its files.
    public static string Hash(string key) =>
        Convert.ToHexStringLower(SHA256.HashData(Encoding.UTF8.GetBytes(key)), 0, 16);

    private static bool Named(string? name) => name is not null && Names.Key(name).Length > 0;
}

// Names compared as one whatever their spelling: lower case, no accents,
// every run of anything but letters and digits one space. A name's Key also
// reads "&" as "and" and drops a leading "The".
public static class Names
{
    public static string Key(string name)
    {
        var folded = Fold(name.Replace("&", " and ", StringComparison.Ordinal));
        return folded.StartsWith("the ", StringComparison.Ordinal) ? folded[4..] : folded;
    }

    public static string Fold(string text)
    {
        var folded = new StringBuilder(text.Length);
        foreach (var c in text.Normalize(NormalizationForm.FormD))
        {
            if (CharUnicodeInfo.GetUnicodeCategory(c) == UnicodeCategory.NonSpacingMark)
                continue;
            folded.Append(char.IsLetterOrDigit(c) ? char.ToLowerInvariant(c) : ' ');
        }
        return string.Join(' ', folded.ToString().Split(' ', StringSplitOptions.RemoveEmptyEntries));
    }
}
