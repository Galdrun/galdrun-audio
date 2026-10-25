namespace Multiroom.Server.Playback.Core;

// The answers of 4713, as the core's ControlApi.cpp writes them. A parameter
// with no default is a field the core always writes, and a missing one is a
// malformed answer. A parameter defaulting to null is one the core leaves out
// when it has nothing to say -- unknown is absent, never zero, on this side
// of the wire as on that one. The `ok`, `command` and `id` of the envelope
// are CoreLink's and are not repeated here.

public sealed record StatusReply(
    CoreServerInfo Server,
    IReadOnlyList<CoreClientInfo> Clients,
    IReadOnlyList<CoreZone> Zones);

public sealed record ClientsReply(IReadOnlyList<CoreClientInfo> Clients);

public sealed record ZonesReply(IReadOnlyList<CoreZone> Zones);

public sealed record ZoneCreated(string Zone, bool Created);

public sealed record ZoneDeleted(string Zone, bool Deleted);

public sealed record ZoneClientsSet(string Zone, IReadOnlyList<string> Added, IReadOnlyList<string> Removed);

// Staged is false for a sourceId the zone already holds: the replay no-op.
public sealed record SourceStaged(string Zone, string SourceId, bool Staged);

public sealed record StagedCleared(string Zone, ulong Cleared);

public sealed record ZonePlayed(string Zone, ZoneState State, TransportState Transport);

public sealed record ZonePaused(string Zone, bool Paused, ulong TimelineNs);

public sealed record ZoneStopped(string Zone, bool Stopped, ulong? TimelineNs = null);

public sealed record ZoneSought(string Zone, string SourceId, ulong PositionNs, ulong CutTimelineNs, ulong TimelineNs);


public sealed record GroupedSet(string Client, bool Grouped, bool Changed);

public sealed record AudioControlSet(string Client, AudioControl AudioControl, bool Changed);

// Changed is false when the room already reported this setting, and nothing
// was relayed.
public sealed record VolumeControlSet(string Client, VolumeControl VolumeControl, bool Changed);

// Changed is false when the room already reported this setting.
public sealed record DriftCorrectionSet(string Client, DriftCorrection DriftCorrection, bool Changed);

public sealed record VolumeSet(string Client, bool Changed, double? Value = null, bool? Muted = null);

public sealed record ClientRejoined(string Client, string Zone, ulong TimelineNs);

public sealed record ClientWoken(string Client, string WakeOnLan);

public sealed record OutputLatencySet(string Client, ulong LatencyMs, bool Changed, bool Rejoined);

public sealed record EqualizerSet(string Client, CoreEqualizer Equalizer, bool Changed);

public sealed record AudioDeviceSet(string Client, string DeviceId, bool Changed);

public sealed record AudioRescanRequested(string Client);

// Server is where the client says it is pointed, "" for nowhere, null for a
// client from before it said; Connected is whether it is a room of this core.
// So: Connected is attached here, Server "" is unattached, anything else is
// attached to another core -- unless AttachedHere: pointed at this core, but
// not (yet) a room of it.
public sealed record DiscoveredClient(
    string Client,
    string Address,
    int ControlPort,
    uint ProtocolVersion,
    bool Present,
    ulong LastSeenMsAgo,
    ulong FirstSeenMsAgo,
    ulong Beacons,
    bool Connected,
    string? ClientVersion = null,
    string? Platform = null,
    string? Server = null,
    bool AttachedHere = false,
    string? Name = null,
    string? Host = null);

public sealed record DiscoveredClientsReply(IReadOnlyList<DiscoveredClient> Clients, ulong Forgotten);

// Server is "" after client.detach. Changed is false when the client was
// already pointed there.
public sealed record ClientPointed(string Client, string Server, bool Changed);

public sealed record ClientNetworkSettings(string Client, bool Supported,
    System.Text.Json.JsonElement? Network = null, int? ApplyDelaySeconds = null);
public sealed record ClientLedSettings(string Client, bool Supported, bool? Off = null);

// DeepSleep says whether the client can deep sleep at all.
public sealed record ClientPowerSettings(string Client, bool Supported, string? State = null,
    uint? StandbyAfterSeconds = null, uint? DeepSleepAfterSeconds = null, bool? DeepSleep = null);

// The core config's `audio` section. Live names the keys audio.configure takes;
// the others are read at the core's start. Resamplers names what
// resampler.configure takes. Changed only answers audio.configure.
public sealed record AudioSettingsReply(
    CoreAudioSettings Audio,
    IReadOnlyList<string> Live,
    CoreMemoryLimit MemoryLimit,
    bool? Changed = null,
    IReadOnlyList<string>? Resamplers = null);

// The core's phase 42: one ceiling over every zone's audio memory
// (audio.memoryLimitMb), with the range audio.configure accepts for it. MaxMb
// is absent where the machine does not say; the core checks the value itself.
public sealed record CoreMemoryLimit(uint Mb, uint MinMb, uint UsedMb, uint? MaxMb = null);

public sealed record CoreAudioSettings(
    uint MaxSampleRate,
    uint MaxChannels,
    uint PacketDurationMs,
    uint MemoryLimitMb,
    uint TargetBufferMs,
    uint MinimumStartBufferMs,
    uint ClientReadyTimeoutMs,
    uint StartDelayMs,
    uint SeekFlushLeadMs,
    uint SeekFillLeadMs,
    uint DriftWindowMs,
    uint CorrectionLimitPpm,
    uint ConvergenceLimitPpm,
    string Resampler,
    string AudioControl,
    string Dither);

// Changed is false when the room already reported this buffer size, and nothing
// was relayed.
public sealed record BufferSet(string Client, uint BufferMs, bool Changed);

// Changed is false when the room already had this name. The name the client
// takes comes back as room.name (MATP section 60).
public sealed record NameSet(string Client, string Name, bool Changed);

// Zone is the one the room was asked into, when it was in one.
public sealed record ClientForgotten(string Client, string? Zone = null);

// Bindings is the input's every gesture as they will be once the client has
// applied the change; Changed is false when they already were.
public sealed record InputBindingsSet(string Client, string InputId,
    IReadOnlyDictionary<string, CoreInputBinding> Bindings, bool Changed);

// The core keeps the resampler for its next start and switches live: Renegotiated
// names the zoned rooms it asked to re-negotiate onto the new filter. A
// fixed-ratio resampler (VariableRatio false, r8brain) corrects no drift.
public sealed record ResamplerConfigured(string Resampler, bool VariableRatio, bool Changed,
    IReadOnlyList<string>? Renegotiated = null);

public sealed record Subscription(bool Subscribed, bool Changed);

// --- the parts of status, clients and zones --------------------------------

public enum ZoneState
{
    Idle,
    Playing,
    Ended,
    Failed,
}

public enum TransportState
{
    Stopped,
    Starting,
    Playing,
    Paused,
}

public sealed record AudioFormatInfo(uint SampleRate, uint Channels, string SampleType);

public sealed record CoreServerInfo(
    string Version,
    ulong UptimeMs,
    ulong Zones,
    ulong Clients,
    ulong Connected,
    ulong Members,
    string? Resampler = null,
    CoreSystemMetrics? System = null,
    // The core's phase 42: every zone's audio memory together, and its ceiling.
    ulong? MemoryUsedBytes = null,
    ulong? MemoryLimitBytes = null);

// A process's own figures as the core and the client sample them (the core's
// phase 34, MATP section 55). Each is absent when that process cannot say; a
// core that predates phase 34 sends none of them.
public sealed record CoreSystemMetrics(
    double? CpuPercent = null,
    double? SystemCpuPercent = null,
    ulong? MemoryBytes = null,
    ulong? SystemMemoryTotalBytes = null,
    ulong? SystemMemoryAvailableBytes = null,
    ulong? UptimeMs = null,
    double? TemperatureC = null,
    ulong? PrivateMemoryBytes = null,
    ulong? Threads = null,
    ulong? Handles = null,
    ulong? SystemUptimeMs = null,
    int? WifiRssiDbm = null,
    ulong? WifiChannel = null,
    string? WifiBssid = null,
    ulong? WifiRxRateKbps = null,
    string? CpuArchitecture = null,
    string? CpuModel = null,
    string? MachineModel = null,
    ulong? CpuCores = null,
    ulong? CpuMhz = null,
    // An ESP32's system memory split into its own RAM and its PSRAM.
    ulong? InternalMemoryTotalBytes = null,
    ulong? InternalMemoryAvailableBytes = null,
    ulong? PsramTotalBytes = null,
    ulong? PsramAvailableBytes = null,
    // The interface a client reaches the core by: "ethernet" or "wifi".
    string? NetworkLink = null);

// One room as the core's RoomDirectory and its zone see it.
public sealed record CoreClientInfo(
    string ClientId,
    bool Connected,
    CoreRoomSettings Settings,
    bool Member,
    CoreDriftCorrection DriftCorrection,
    string? Session = null,
    string? Device = null,
    CoreCapabilities? Capabilities = null,
    CoreStreamAnswer? Stream = null,
    string? Zone = null,
    CoreOutput? Output = null,
    string? Start = null,
    string? StartDetail = null,
    CoreSync? Sync = null,
    CorePlayback? Playback = null,
    ulong? Rejoins = null,
    CoreSystemMetrics? System = null,
    ulong? SystemAgeMs = null,
    CoreAudioDevices? AudioDevices = null,
    IReadOnlyList<CoreInput>? Inputs = null,
    uint? BufferMs = null,
    CoreVolumeControl? VolumeControl = null,
    string? Address = null,
    bool? Unstable = null,
    ulong? BackoffMs = null,
    ulong? EpisodeRejoins = null,
    string? Name = null,
    bool? DeepSleep = null,
    // A disconnected room's announced deep sleep (the stream's powerState while connected).
    string? PowerState = null,
    // With powerState "sleep": the MAC its Wake-on-LAN magic packet names.
    string? WakeOnLan = null,
    bool Active = true);

// The room's Volume Control as its client reported it (MATP section 59), and
// the path it resolved: never Auto. Reason says why Effective is not what
// Configured asked for; absent where it is.
public sealed record CoreVolumeControl(VolumeControl Configured, VolumeControl Effective, string? Reason = null);

// A room's buttons and encoders as its client reported them (MATP section
// 57): every gesture of the input's kind -- click, doubleClick and longPress
// for a button, clockwise and counterClockwise for an encoder -- and what it
// is bound to.
public sealed record CoreInput(string Id, InputKind Kind, IReadOnlyDictionary<string, CoreInputBinding> Bindings);

// Step is for VolumeUp and VolumeDown, Value for SetVolume, Scope for those
// three and ToggleMute; each is absent where the action takes none.
public sealed record CoreInputBinding(InputAction Action, double? Step = null, double? Value = null,
    VolumeScope? Scope = null);

public enum InputKind
{
    Button,
    Encoder,
}

public enum InputAction
{
    None,
    Play,
    Pause,
    TogglePause,
    Stop,
    Next,
    Previous,
    VolumeUp,
    VolumeDown,
    SetVolume,
    ToggleMute,
    StandbyOff,
    StandbyOn,
    ToggleStandby,
    // User actions: InputActions runs nothing for them; the event stream's
    // room.input is all they do, for a tinkerer's own logic.
    Action1,
    Action2,
    Action3,
    Action4,
    Action5,
    Action6,
    Action7,
    Action8,
    Action9,
    Action10,
    DeepSleep,
}

// Which volume an action changes: the room's own, or its zone's factor over
// every member.
public enum VolumeScope
{
    Room,
    Zone,
}

// The room's sound cards as its client reported them (MATP section 56).
// Configured is empty for the client's default; Selected is absent while the
// room has no usable device.
public sealed record CoreAudioDevices(string Configured, IReadOnlyList<CoreAudioDevice> Devices,
    string? Selected = null);

public sealed record CoreAudioDevice(string Id, string Name, bool IsDefault);

// HardwareVolume is absent for a client that did not say (one from before
// MATP section 12 carried it), which is not the same as Available false.
// AccessMode is "exclusive" or "shared", and absent the same way.
public sealed record CoreCapabilities(IReadOnlyList<AudioFormatInfo> Formats, uint? PreferredSampleRate = null,
    CoreHardwareVolume? HardwareVolume = null, string? AccessMode = null, CoreHardwareClock? HardwareClock = null);

// The device's verified clock trim, in whole ppm around its nominal rate;
// absent for a client without a drift-correction setting.
public sealed record CoreHardwareClock(bool Available, string? Control = null, int? MinPpm = null, int? MaxPpm = null);

// The room's device has its own volume control, and the client plays the
// room's volume on it rather than scaling the samples (MATP sections 12 and
// 39). Informational: SET_VOLUME is the same either way. MinDb and MaxDb are
// absent where the device reports no decibels.
public sealed record CoreHardwareVolume(bool Available, string? Control = null, uint? Steps = null,
    double? MinDb = null, double? MaxDb = null, bool? HasMute = null);

public sealed record CoreStreamAnswer(bool Ready, AudioFormatInfo? Format = null, string? Reason = null,
    string? PowerState = null);

// The settings as stored in the core. AudioControlIsDefault says the room has
// no value of its own and follows the core's default; volume, mute, latency
// and equalizer are absent where nothing was ever asked. Grouped is this
// server's, sent with the memberships.
public sealed record CoreRoomSettings(
    AudioControl AudioControl,
    bool AudioControlIsDefault,
    double? Volume = null,
    bool? Muted = null,
    ulong? OutputLatencyMs = null,
    CoreEqualizer? Equalizer = null,
    bool Grouped = false);

// Actuator is the plan's: none, resampling or hardwareClock. Mode is the
// client's own setting and HardwareClockAvailable its device's trim, both
// absent for a client without the setting; HardwareClock is what the client
// last programmed while its clock is the actuator.
public sealed record CoreDriftCorrection(bool Requested, bool? Active = null, string? DisabledBy = null,
    string? Actuator = null, DriftCorrection? Mode = null, bool? HardwareClockAvailable = null,
    CoreHardwareClockState? HardwareClock = null);

public sealed record CoreHardwareClockState(int AppliedPpm, bool Saturated, string? Error = null);

// Present for a room that is a member of a zone: its plan and its stream.
// ReplanPending: left alone while its zone plays, it keeps this plan until its
// next track.
public sealed record CoreOutput(
    string State,
    AudioFormatInfo SourceFormat,
    AudioFormatInfo Format,
    bool BitPerfect,
    string Reason,
    string Plan,
    double Ratio,
    ulong Packets,
    ulong QueuedPackets,
    ulong BufferedMs,
    ulong ConnectionDrops,
    string? Resampler = null,
    string? LastError = null,
    CoreOutputEqualizer? Equalizer = null,
    bool? ReplanPending = null);

// Active: on and changing samples, which a bitperfect stream never is.
// ClippedSamples: samples past full scale after it, since the room joined.
public sealed record CoreOutputEqualizer(bool Active, ulong ClippedSamples);

// What has not been measured is absent.
public sealed record CoreSync(
    double CorrectionPpm,
    double AppliedPpm,
    double? DriftPpm = null,
    double? StandardErrorPpm = null,
    double? Confidence = null,
    double? OffsetUs = null,
    double? TargetOffsetUs = null,
    string? ClockQuality = null,
    double? HardwareTargetPpm = null);

// The room's own last PLAYBACK_REPORT, and how long ago it came.
public sealed record CorePlayback(ulong BufferedMs, ulong Underruns, ulong AgeMs);

public sealed record CoreZone(
    string Zone,
    ZoneState State,
    TransportState Transport,
    IReadOnlyList<CoreSegment> Staged,
    IReadOnlyList<CoreFailedSource> FailedSources,
    IReadOnlyList<string> Members,
    IReadOnlyList<string> Clients,
    ulong StartsSent,
    ulong Rejoins,
    string? LastError = null,
    ulong? TimelineNs = null,
    AudioFormatInfo? SourceFormat = null,
    CoreSource? Source = null,
    // Not the core's: House fills it from the model's rooms (phase 34), each
    // member that has a health by its client id.
    IReadOnlyDictionary<string, CoreRoomHealth>? Health = null,
    // The core's phase 42: what the zone holds (nothing unless it plays), and
    // the blocks it asked for and did not get.
    ulong? Blocks = null,
    ulong? MemoryBytes = null,
    ulong? BlocksRefused = null);

public sealed record CoreSegment(
    string SourceId,
    string MediaUri,
    string? TrackId = null,
    string? Title = null,
    ulong? StartOffsetNs = null);

// The audible source. MediaUri and Title are absent when the model learnt of
// it from zone.sourceStarted for a staging it never saw.
public sealed record CoreSource(
    string SourceId,
    bool Ended,
    string? MediaUri = null,
    string? TrackId = null,
    string? Title = null,
    ulong? StartOffsetNs = null,
    ulong? TimelineStartNs = null);

public sealed record CoreFailedSource(string SourceId, string Reason);
