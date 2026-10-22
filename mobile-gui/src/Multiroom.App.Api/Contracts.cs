using System.Text.Json;
using System.Text.Json.Serialization;

namespace Multiroom.App.Api;

// Owned wire contracts. No reference to server implementation assemblies.

// Every item a list shows (server phase 40), its picture named by ArtworkId
// (GET artwork/{id}). As a list entry's `item` it carries its kind.
[JsonPolymorphic(TypeDiscriminatorPropertyName = "kind")]
[JsonDerivedType(typeof(Track), "track")]
[JsonDerivedType(typeof(Album), "album")]
[JsonDerivedType(typeof(Artist), "artist")]
[JsonDerivedType(typeof(RadioStation), "station")]
[JsonDerivedType(typeof(MediaGroup), "group")]
[JsonDerivedType(typeof(Audiobook), "audiobook")]
public abstract record MediaItem
{
    public string? ArtworkId { get; init; }
}
public sealed record Track(string ProviderId, string ProviderItemId, string Title, string? Artist = null,
    string? Album = null, TimeSpan? Duration = null) : MediaItem;
public sealed record Album(string ProviderId, string ProviderItemId, string Title, string? Artist = null,
    int? Year = null) : MediaItem;
public sealed record Artist(string ProviderId, string ProviderItemId, string Name) : MediaItem;
public sealed record RadioStation(string ProviderId, string ProviderItemId, string Name, string? Genre = null,
    Uri? Homepage = null) : MediaItem;
// A book queued or kept as one item; its parts are the server's.
public sealed record MediaGroup(string Title, string? Artist = null) : MediaItem;
public sealed record Audiobook(string Reference, string Title, string? Author = null) : MediaItem;
public sealed record CatalogItems(IReadOnlyList<Track> Tracks, IReadOnlyList<Album> Albums,
    IReadOnlyList<Artist> Artists, IReadOnlyList<RadioStation> Stations);
public enum ProviderState { Ready, Unavailable, AuthRequired, Disabled }
public enum BrowseKind { Folder, Root, Section, Artist, Album, Playlist, Genre, Composer, Year, Stations }
public enum BrowseLayout { List, Grid }
public sealed record BrowseNode(string ProviderId, string NodeId, string Title, string? ArtworkId = null,
    BrowseKind Kind = BrowseKind.Folder, string? Subtitle = null, bool Playable = false, string? Unplayable = null);
public sealed record BrowseLetter(string Letter, int Offset);
public sealed record BrowseSection(string Title, IReadOnlyList<BrowseNode> Nodes, CatalogItems Items,
    BrowseNode? More = null, int? Total = null, BrowseLayout Layout = BrowseLayout.List);
public sealed record BrowsePage(IReadOnlyList<BrowseNode> Nodes, CatalogItems Items, BrowseNode? Node = null,
    int? Total = null, IReadOnlyList<BrowseSection>? Sections = null, IReadOnlyList<string>? Sorts = null,
    string? Sort = null, bool Filterable = false, IReadOnlyList<BrowseLetter>? Letters = null,
    BrowseLayout Layout = BrowseLayout.List);
public sealed record ProviderView(string Id, ProviderState State, string Detail, string? DisplayName = null,
    IReadOnlyList<string>? Capabilities = null, IReadOnlyList<BrowseNode>? Root = null);
public sealed record SearchSource(string Provider, int Tracks, int Albums, int Artists, int Stations);
public sealed record SearchFailure(string Provider, string Error, string Message);
public sealed record SearchResult(string Query, CatalogItems Items, IReadOnlyList<SearchSource> Sources,
    IReadOnlyList<SearchFailure> Failures);
public sealed record ServerStatus(string Version, CoreStatus Core, JsonElement Media, JsonElement Library,
    IReadOnlyList<ProviderView> Providers, JsonElement AddIns, EventStatus Events);
public sealed record CoreStatus(bool Connected, long Connections, int Zones, int Rooms,
    int QueuedEvents, long DroppedEvents, string? Version = null);
public sealed record EventStatus(int Subscribers, int Held, long Dropped);
public enum RepeatMode { Off, One, All }
public enum QueueTransport { Stopped, Starting, Playing, Paused }
public sealed record QueueItem(long Id, MediaItem Item, string? Failure = null)
{
    [JsonIgnore] public Track? Track => Item as Track;
    [JsonIgnore] public RadioStation? Station => Item as RadioStation;
}
public sealed record NowPlaying(long Item, string SourceId, ulong? TimelineNs = null, ulong? ServerTimeNs = null,
    ulong StartOffsetNs = 0);
public sealed record QueueState(string Zone, IReadOnlyList<QueueItem> Items, RepeatMode Repeat, bool Shuffle,
    QueueTransport Transport, long? Current = null, NowPlaying? NowPlaying = null, long? PositionMs = null);
public sealed record ZoneDesired(IReadOnlyList<string>? Clients = null, double? Volume = null, bool? Muted = null);
public sealed record ZoneView(string Zone, ZoneDesired? Desired = null, JsonElement? Actual = null,
    QueueState? Queue = null, string? StreamTitle = null, Track? RadioTrack = null);
public sealed record RoomDesired(string? Zone = null, string? AudioControl = null,
    double? Volume = null, bool? Muted = null, ulong? OutputLatencyMs = null,
    double? EffectiveVolume = null, bool? EffectiveMuted = null, bool Grouped = false);
public sealed record RoomActual(string Client, bool Connected, bool Member, string? Device = null,
    JsonElement? Capabilities = null, string? Zone = null, JsonElement? Settings = null, RoomPlan? Plan = null,
    string? NotReadyReason = null, string? StartDetail = null, JsonElement? Sync = null,
    ulong? BufferedMs = null, JsonElement? Playback = null, ulong? Rejoins = null,
    JsonElement? System = null, ulong? SystemAgeMs = null, JsonElement? AudioDevices = null,
    JsonElement? DriftCorrection = null);
// DriftActuator: none, resampling or hardwareClock; a bitperfect plan corrected
// through the client's own clock is still bitperfect.
public sealed record RoomPlan(bool BitPerfect, string Reason, JsonElement OutputFormat, bool DriftCorrected,
    JsonElement? SourceFormat = null, string? Description = null, string? DriftActuator = null);
// Name is what people call the room (server phase 39); Client stays the key
// of every request.
public sealed record RoomView(string Client, RoomDesired Desired, RoomActual? Actual = null, string? Name = null)
{
    public string DisplayName => string.IsNullOrEmpty(Name) ? Client : Name;
}
public sealed record NodeReference(string Provider, string Id);
public sealed record AddItems(IReadOnlyList<Track>? Tracks = null, RadioStation? Station = null,
    long? Playlist = null, long? Favorite = null, NodeReference? Node = null, int? Index = null,
    bool Next = false, bool Replace = false);
public sealed record ApiError(string Error, string Message);
public sealed record Playlist(long Id, string Name, int Position, DateTimeOffset Created, int Count);
public sealed record PlaylistEntry(long Id, MediaItem Item)
{
    [JsonIgnore] public Track? Track => Item as Track;
}
public sealed record Favorite(long Id, DateTimeOffset Added, MediaItem Item)
{
    [JsonIgnore] public Track? Track => Item as Track;
    [JsonIgnore] public RadioStation? Station => Item as RadioStation;
    public string Title => Item switch { Track t => t.Title, RadioStation s => s.Name, MediaGroup g => g.Title, _ => "Unknown media" };
}
public enum LikeKind { Track, Album, Artist, Station }
public sealed record ItemRef(LikeKind Kind, string ProviderId, string ProviderItemId);
public sealed record ItemStatus(LikeKind Kind, string ProviderId, string ProviderItemId, bool Liked, bool Favorite,
    long? LikeId = null, long? FavoriteId = null);
public sealed record LikedItem(long Id, DateTimeOffset Added, MediaItem Item);
public sealed record RecommendationSource(string AddIn, double Score, string Reason);
public sealed record RecommendedTrack(Track Track, double Score, IReadOnlyList<RecommendationSource> Sources)
{
    public string Explanation => string.Join("\n", Sources.Select(s => $"{s.AddIn}: {s.Reason}"));
}
public sealed record RecommendationCount(string AddIn, int Tracks);
public sealed record AddInFailure(string AddIn, string Error, string Message);
public sealed record RecommendationAnswer(IReadOnlyList<RecommendedTrack> Items,
    IReadOnlyList<RecommendationCount> Sources, IReadOnlyList<AddInFailure> Failures);
public enum AudioControl { BitPerfect, Auto }
// How a corrected room is kept in step; the room's client keeps the setting.
public enum DriftCorrection { Resampling, HardwareClock, Auto }
public sealed record RoomPatch(AudioControl? AudioControl = null,
    ulong? OutputLatencyMs = null, double? Volume = null, bool? Muted = null, string? Name = null,
    DriftCorrection? DriftCorrection = null);
