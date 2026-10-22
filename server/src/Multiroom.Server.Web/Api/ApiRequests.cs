using Multiroom.Server.Abstractions;
using Multiroom.Server.Library;
using Multiroom.Server.Playback;
using Multiroom.Server.Playback.Core;

namespace Multiroom.Server.Web.Api;

public sealed record EthernetSpeedRequest(string SpeedMbps);
public sealed record LedModeRequest(bool Off);
public sealed record PowerSettingsRequest(uint? StandbyAfterSeconds = null, uint? DeepSleepAfterSeconds = null);
public sealed record PowerActionRequest(string Action);

// The request bodies of /api/v1, one record each. A parameter without a
// default is a required field; any field not named here is refused.

// Exactly one of tracks, station, playlist, favourite or a browse node by
// reference. `Index` puts what is added before the item there; `Next` puts what is added
// right after the current item; `Replace` puts it in place of the whole
// queue, stopped. Otherwise it goes at the end. A node of audiobook tracks
// goes in as one group (phase 31) when `Group` is true; null is the house's
// groupBooks setting. `FromStart` queues a book to start at its beginning
// even where the house resumes books (phase 33).
public sealed record AddItemsRequest(
    IReadOnlyList<Track>? Tracks = null,
    RadioStation? Station = null,
    long? Playlist = null,
    long? Favorite = null,
    NodeReference? Node = null,
    int? Index = null,
    bool Next = false,
    bool Replace = false,
    bool? Group = null,
    bool FromStart = false);

// A playable browse node: its provider and its id.
public sealed record NodeReference(string Provider, string Id);

public sealed record QueuePatch(RepeatMode? Repeat = null, bool? Shuffle = null);

public sealed record ItemMove(int Index);

// No item: resume a paused zone, or play the current item, or the first. An
// item is played from its start, or a book from where it was queued to
// resume (phase 33) unless `FromStart`.
// POST clients/{client}/attach: Server ("host:port") overrides the core's own
// address, for a core the client reaches by another one.
public sealed record AttachRequest(string? Server = null);

public sealed record PlayRequest(long? Item = null, bool FromStart = false);

public sealed record SeekRequest(long Item, long PositionMs);

// The zone's full membership, never a delta.
public sealed record ClientsRequest(IReadOnlyList<string> Clients);

// Any of the room's settings, sent in this order; at least one. Name is the
// room's (phase 39), which its client keeps. Equalizer is the whole of it
// (phase 41). MaxVolumeDb is the level at full volume, 0 for no limit.
public sealed record RoomPatch(
    AudioControl? AudioControl = null,
    ulong? OutputLatencyMs = null,
    double? Volume = null,
    bool? Muted = null,
    string? AudioDeviceId = null,
    uint? BufferMs = null,
    VolumeControl? VolumeControl = null,
    string? Name = null,
    CoreEqualizer? Equalizer = null,
    double? MaxVolumeDb = null,
    DriftCorrection? DriftCorrection = null);

// POST rooms/{client}/equalizer/profile: the profile copied into the room.
public sealed record EqualizerProfileApply(string Profile);

// PUT equalizer/profiles/{name}: a user profile, created or replaced.
public sealed record EqualizerProfileRequest(double PreampDb, IReadOnlyList<CoreEqBand> Bands);

public sealed record ResamplerRequest(string Resampler);

// Any of the core's live audio settings: the timings in milliseconds and the
// memory limit in MB; at least one.
public sealed record CoreAudioPatch(
    uint? MinimumStartBufferMs = null,
    uint? ClientReadyTimeoutMs = null,
    uint? StartDelayMs = null,
    uint? SeekFlushLeadMs = null,
    uint? SeekFillLeadMs = null,
    uint? MemoryLimitMb = null);

// The server core to use by its beacon id, or null for core.host/core.port.
public sealed record CoreSelectionRequest(string? CoreId);

// A zone's own volume and mute (phase 15), over its rooms' own.
// Phase 15: the zone's level; phase 26: its AutoFill, a patch of its own.
// User (phase 36): who is listening in the zone, "" for the household.
public sealed record ZonePatch(double? Volume = null, bool? Muted = null, AutoFillPatch? AutoFill = null,
    string? User = null);

// Folder: the folder it goes in; null or 0 is the top.
public sealed record PlaylistCreate(string Name, long? Folder = null);

// Parent: the folder it goes in; null or 0 is the top. A patch's Parent 0
// moves it to the top, null leaves it where it is.
public sealed record PlaylistFolderCreate(string Name, long? Parent = null);
public sealed record PlaylistFolderPatch(string? Name = null, long? Parent = null);

// A queue saved as a playlist; overwrite replaces one of that name.
public sealed record QueueSaveRequest(string Name, bool Overwrite = false, long? Folder = null);

// The playlist, and how many of the queue's items (stations) it could not hold.
public sealed record QueueSaved(Playlist Playlist, int Skipped);

// Folder 0 moves it to the top, null leaves it where it is.
public sealed record PlaylistPatch(string? Name = null, int? Position = null, long? Folder = null);

// Exactly one of tracks or a node; a node of audiobook tracks is one group
// entry (phase 31) when `Group` is true; null is the house's groupBooks.
public sealed record EntriesRequest(IReadOnlyList<Track>? Tracks = null, int? Index = null, NodeReference? Node = null,
    bool? Group = null);

// The house's playback settings: null keeps.
public sealed record PlaybackSettingsPatch(bool? GroupBooks = null, bool? ResumeBooks = null, bool? FindRadioLikes = null);

// Exactly one of a track, a station or a node; a node must be a book
// (phase 31: audiobook tracks), kept as one favourite with its parts.
// User (phase 36): whose favourite, absent or "" for the household.
public sealed record FavoriteRequest(Track? Track = null, RadioStation? Station = null, NodeReference? Node = null,
    string? User = null);

// "More like this": exactly one of a track (as search and browse answer it)
// or a stored playlist, whose entries are the seed. At most `Limit` tracks,
// 1 to 100.
public sealed record RecommendRequest(Track? Track = null, long? Playlist = null, int Limit = 20);

// The text of an M3U or PLS file.
public sealed record StationImport(string Playlist);

// A library folder: a local or network one by its full path; an smb one by
// host, share, the directory inside it (path) and the login. No user is
// guest. The records that carry a password print without it (rule 7).
public sealed record LibraryFolderCreate(
    LibraryFolderType Type,
    string? Path = null,
    string? Host = null,
    int? Port = null,
    string? Share = null,
    string? User = null,
    string? Password = null,
    string? Domain = null,
    bool Enabled = true,
    MediaType? MediaType = null)
{
    public override string ToString() => $"{nameof(LibraryFolderCreate)} {{ Type = {Type}, Path = {Path}, Host = {Host} }}";
}

// Null keeps what is there. An empty user is guest, an empty password none,
// and an empty mediaType declares none (phase 30: music or audiobook).
public sealed record LibraryFolderPatch(bool? Enabled = null, string? User = null, string? Password = null,
    string? Domain = null, string? MediaType = null)
{
    public override string ToString() => $"{nameof(LibraryFolderPatch)} {{ Enabled = {Enabled} }}";
}

public sealed record SmbSharesRequest(string Host, int? Port = null, string? User = null, string? Password = null,
    string? Domain = null)
{
    public override string ToString() => $"{nameof(SmbSharesRequest)} {{ Host = {Host} }}";
}

public sealed record SmbDirectoriesRequest(string Host, string Share, string? Path = null, int? Port = null,
    string? User = null, string? Password = null, string? Domain = null)
{
    public override string ToString() => $"{nameof(SmbDirectoriesRequest)} {{ Host = {Host}, Share = {Share} }}";
}

// Exactly one of the five (a node liked as a book, phase 37), or metadata.
public sealed record LikeRequest(Track? Track = null, Album? Album = null, Artist? Artist = null,
    RadioStation? Station = null, TrackMetadata? Metadata = null, string? User = null, NodeReference? Node = null);

// 1 to LikeStore.MaxLookup items, as ItemRef: {kind, providerId, providerItemId}.
// User is a scope, as GET likes takes it (phase 36).
public sealed record ItemStatusRequest(IReadOnlyList<ItemRef> Items, string? User = null);

// A user of the household (phase 36): 1 to 40 characters, trimmed.
public sealed record UserCreate(string Name);

// A proposals refresh (phase 38): whose, absent or "" the household.
public sealed record ProposalRefresh(string? User = null);
