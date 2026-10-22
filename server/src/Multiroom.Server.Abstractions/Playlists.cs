using System.Text.Json.Serialization;

namespace Multiroom.Server.Abstractions;

// Favourites and playlists: references to
// provider-scoped tracks and stations, not copies. The Track or RadioStation
// in an entry is the snapshot taken when it was added: its provider-scoped
// identity plays it, and its metadata still names it when the provider has
// lost it.
//
// Each entry is an IMediaEntry (phase 40): the outward API answers its one
// item as `item`, the store keeps it in its own field.

// Position is the playlist's place among the playlists; Count its entries;
// Folder the folder it lies in, null at the top.
public sealed record Playlist(long Id, string Name, int Position, DateTimeOffset Created, int Count, long? Folder = null);

// A folder of playlists and other folders; Parent is the folder it lies in,
// null at the top.
public sealed record PlaylistFolder(long Id, string Name, long? Parent);

// One entry of one playlist, in playlist order: a track, or a group (phase
// 31: a book kept as one entry, its parts stored as they were). Id is never
// reused, so the same track added twice is two entries.
public sealed record PlaylistEntry(long Id, Track? Track, QueueGroup? Group = null) : IMediaEntry
{
    public string Title => Track?.Title ?? Group!.Title;

    [JsonIgnore]
    public MediaItem Item => (MediaItem?)Track ?? Group!;
}

// A liked track, album, artist, station or audiobook (phase 37), exactly one
// of the five, as it was when liked. User is whose like it is (phase 36), ""
// for the household. Found: for a song liked as a radio played it, the
// provider track it was found as (RadioLikeFinder), null until then.
public sealed record LikedItem(long Id, Track? Track, Album? Album, Artist? Artist, RadioStation? Station,
    DateTimeOffset Added, string User = "", Audiobook? Audiobook = null, FoundTrack? Found = null) : IMediaEntry
{
    [JsonIgnore]
    public MediaItem Item =>
        (MediaItem?)Track ?? (MediaItem?)Album ?? (MediaItem?)Artist ?? (MediaItem?)Audiobook ?? Station!;
}

// A radio song's like as a playable track: Close when only the title's core
// or the primary artist matched (SongMatchKind.Close), not the whole name.
public sealed record FoundTrack(Track Track, bool Close);

// A book as a like keeps it (phase 37): the node it is queued by, as a
// group's Source (`local:book:<id>`, `local:folder:<path>`, a Tidal album),
// and what names it; never its parts.
public sealed record Audiobook(string Reference, string Title, string? Author, ArtworkRef? Artwork, long? DurationMs)
    : MediaItem
{
    public override string? ArtworkId => Artworks.AlbumId(Title, Author) ?? Artworks.RefId(Artwork);

    public static Audiobook Of(QueueGroup group) => new(group.Source ?? "", group.Title, group.Artist, group.Artwork,
        (long?)group.Duration?.TotalMilliseconds);
}

// A track, a station or a group (a book, with its parts as they were when it
// was added), exactly one of the three. User as a like's.
public sealed record Favorite(long Id, Track? Track, RadioStation? Station, DateTimeOffset Added,
    QueueGroup? Group = null, string User = "") : IMediaEntry
{
    public string Title => Track?.Title ?? Group?.Title ?? Station!.Name;

    [JsonIgnore]
    public MediaItem Item => (MediaItem?)Track ?? (MediaItem?)Group ?? Station!;
}
