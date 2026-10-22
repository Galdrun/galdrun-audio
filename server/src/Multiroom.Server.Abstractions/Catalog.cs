using System.Text.Json.Serialization;

namespace Multiroom.Server.Abstractions;

// The domain model: what providers answer and what the rest of the server reads.
//
// Every item is provider-scoped: ProviderId names the provider ("local",
// "tidal", ...) and ProviderItemId is that provider's own id, opaque to
// everyone else. Two providers holding the same song are two items, and
// saying they are one is nobody's job until phase 12. The metadata is the
// normalized part both the UI and the queue engine read; an unknown value is
// null, never an empty string or zero.
//
// MediaType is what the track is (phase 30), null when nothing says; read as
// music wherever a decision is needed (MediaTypeClassifier.IsMusic).
// MediaTypeSource names the rule that decided it, where the provider knows.
public sealed record Track(
    string ProviderId,
    string ProviderItemId,
    string Title,
    string? Artist = null,
    string? Album = null,
    TimeSpan? Duration = null,
    ArtworkRef? Artwork = null,
    AudioFormat? Format = null,
    MediaType? MediaType = null,
    MediaTypeSource? MediaTypeSource = null) : MediaItem
{
    public override string? ArtworkId => Artworks.Of(this);
}

// What a track's audio is, as far as its provider knows: the codec ("FLAC",
// "MP3", "AAC", ...), the sample rate in Hz, the bits per sample (lossless
// only), the channels and the bitrate in kbit/s. A part not known is null.
public sealed record AudioFormat(
    string? Codec = null,
    int? SampleRate = null,
    int? BitDepth = null,
    int? Channels = null,
    int? Bitrate = null);

public sealed record Album(
    string ProviderId,
    string ProviderItemId,
    string Title,
    string? Artist = null,
    int? Year = null,
    ArtworkRef? Artwork = null) : MediaItem
{
    public override string? ArtworkId => Artworks.AlbumId(Title, Artist) ?? Artworks.RefId(Artwork);
}

public sealed record Artist(
    string ProviderId,
    string ProviderItemId,
    string Name,
    ArtworkRef? Artwork = null) : MediaItem
{
    public override string? ArtworkId => Artworks.ArtistId(Name) ?? Artworks.RefId(Artwork);
}

// A station never ends and is never seekable; it resolves through
// ILiveStreamResolver, not IStreamResolver. It is Radio by its kind, and
// answers so, so a UI reads one field for every item.
public sealed record RadioStation(
    string ProviderId,
    string ProviderItemId,
    string Name,
    string? Genre = null,
    Uri? Homepage = null,
    ArtworkRef? Artwork = null) : MediaItem
{
    // Its logo: a station is never named by its songs. The API never writes
    // Artwork, so a station sent back or kept (a queue item, a favourite)
    // has none; its providers name the logo by the station's own id.
    public override string? ArtworkId => Artworks.RefId(Logo);

    [JsonIgnore]
    public ArtworkRef Logo => Artwork ?? new(ProviderId, ProviderItemId);

#pragma warning disable CA1822 // An instance property, so the station's JSON carries it beside its other fields.
    public MediaType MediaType => MediaType.Radio;
#pragma warning restore CA1822
}

// Artwork by reference: the provider named fetches it (IArtworkProvider). The
// reference is the provider's own, and like a media URL it carries no token.
// Since phase 40 it is where the artwork store fetches an item's picture
// from; the outward API answers ArtworkId (MediaItem) instead.
public sealed record ArtworkRef(string ProviderId, string Reference);

public sealed record Artwork(string ContentType, ReadOnlyMemory<byte> Bytes);

// A page of items of each kind, as a search or a browse node answers it.
// Playlists are a search's only: playable nodes of kind Playlist, opened and
// queued as nodes. A browse page answers its containers as its Nodes.
public sealed record CatalogItems(
    IReadOnlyList<Track> Tracks,
    IReadOnlyList<Album> Albums,
    IReadOnlyList<Artist> Artists,
    IReadOnlyList<RadioStation> Stations,
    IReadOnlyList<BrowseNode> Playlists)
{
    public static CatalogItems Empty { get; } = new([], [], [], [], []);
}

// The kinds a search asks for. A provider answers these and leaves every
// other list empty.
[Flags]
public enum SearchKinds
{
    None = 0,
    Tracks = 1,
    Albums = 2,
    Artists = 4,
    Playlists = 8,
    Stations = 16,
    All = Tracks | Albums | Artists | Playlists | Stations,
}

// What a browse node is (phase 14). The server says what a node is; the UI
// decides how it looks.
public enum BrowseKind
{
    // A container with no more to say: a list such as "Albums".
    Folder,

    // A provider's own entry point, such as "For you".
    Root,

    // A part of a page, paged on its own: "Top tracks" on an artist.
    Section,

    Artist,
    Album,
    Playlist,
    Genre,
    Composer,
    Year,

    // A list of radio stations.
    Stations,
}

// How a page or a section is best shown: a list, or a grid of covers.
public enum BrowseLayout
{
    List,
    Grid,
}

// A container in a provider's tree: a folder, "new releases", an album. It
// opens with IBrowseProvider under its NodeId. Subtitle is what a UI puts
// under the title (an album's artist and year, a playlist's track count).
// Playable says whether it can be queued as a whole (IContainerExpander);
// when it cannot, Unplayable says why.
public sealed record BrowseNode(
    string ProviderId,
    string NodeId,
    string Title,
    ArtworkRef? Artwork = null,
    BrowseKind Kind = BrowseKind.Folder,
    string? Subtitle = null,
    bool Playable = false,
    string? Unplayable = null) : MediaItem
{
    // An artist's node is the artist's picture; any other its provider's artwork.
    public override string? ArtworkId =>
        (Kind == BrowseKind.Artist ? Artworks.ArtistId(Title) : null) ?? Artworks.RefId(Artwork);
}

// The first entry of a letter in a sorted list: a jump target.
public sealed record BrowseLetter(string Letter, int Offset);

// One part of a sectioned page: its own nodes and items (a first page of
// them), and the node that opens the rest, paged on its own.
public sealed record BrowseSection(
    string Title,
    IReadOnlyList<BrowseNode> Nodes,
    CatalogItems Items,
    BrowseNode? More = null,
    int? Total = null,
    BrowseLayout Layout = BrowseLayout.List);

// A page of a node. Nodes are the containers in it, Items the tracks and
// stations. Node is the node itself, for a UI's header. Total counts the
// paged list (null when the provider does not know). A sectioned page has
// Sections instead, each paged on its own. Sorts are the orders the node
// takes (Sort the one answered), Filterable whether it takes a text filter,
// and Letters where each letter starts in the sorted list.
public sealed record BrowsePage(
    IReadOnlyList<BrowseNode> Nodes,
    CatalogItems Items,
    BrowseNode? Node = null,
    int? Total = null,
    IReadOnlyList<BrowseSection>? Sections = null,
    IReadOnlyList<string>? Sorts = null,
    string? Sort = null,
    bool Filterable = false,
    IReadOnlyList<BrowseLetter>? Letters = null,
    BrowseLayout Layout = BrowseLayout.List);

// A browse: the node (null is the provider's root), the page, and the
// options a node may take. An option the node does not take is refused as
// InvalidArgument, never ignored. A letter starts the page at that letter's
// first entry, so it goes without an offset.
public sealed record BrowseRequest(
    string? NodeId,
    int Offset = 0,
    int Limit = 20,
    string? Sort = null,
    string? Filter = null,
    string? Letter = null);

// A container expanded into what it queues: its tracks in order, at most the
// limit asked (Truncated says more were left out), and those left out because
// they cannot play, each with why. Chapters holds the chapter marks of the
// tracks that have them (phase 32), by ProviderItemId.
public sealed record ExpandedContainer(IReadOnlyList<Track> Tracks, IReadOnlyList<RefusedTrack> Refused, bool Truncated,
    IReadOnlyDictionary<string, IReadOnlyList<Chapter>>? Chapters = null);

// A chapter mark inside one track (phase 32): an audiobook bought as one
// file. Its start and end on the track's own timeline; EndMs is the next
// chapter's start, or the track's length (null when unknown) for the last.
public sealed record Chapter(string Title, long StartMs, long? EndMs);

public sealed record RefusedTrack(Track Track, string Reason);
