namespace Multiroom.Server.Playback.Core;

// The core's event lines (its phase 24, 25 and 27), one record per event, plus
// CoreSynchronised, which is CoreLink's own. Handlers receive them in wire
// order, after the model has taken them in.
public abstract record CoreEvent;

public sealed record SourceNeeded(string Zone, string AfterSourceId, ulong? RemainingNs = null) : CoreEvent;

// The audible instant: "now playing".
public sealed record SourceStarted(
    string Zone,
    string SourceId,
    ulong TimelineNs,
    ulong ServerTimeNs,
    ulong StartOffsetNs,
    string? TrackId = null) : CoreEvent;

public sealed record SourceFailed(string Zone, string SourceId, string Reason) : CoreEvent;

public sealed record ZoneEnded(string Zone, string AfterSourceId) : CoreEvent;

public sealed record ZoneTransport(string Zone, TransportState State, ulong TimelineNs) : CoreEvent;

public sealed record RoomConnected(string Client, string DeviceName, bool Reconnect, string? Zone = null, string? Address = null) : CoreEvent;

public sealed record RoomDisconnected(string Client, string Reason, string? Zone = null) : CoreEvent;
public sealed record RoomActive(string Client, bool Active) : CoreEvent;

public sealed record RoomJoined(
    string Client,
    string Zone,
    bool BitPerfect,
    string Reason,
    AudioFormatInfo OutputFormat,
    bool DriftCorrected,
    string? DriftActuator = null) : CoreEvent;

// Cause: removed, moved, deleted or replanned.
public sealed record RoomLeft(string Client, string Zone, string Cause) : CoreEvent;

public sealed record RoomNotReady(string Client, string Zone, string Reason) : CoreEvent;

public sealed record RoomFellBehind(string Client, string Zone, string Trigger, ulong TimelineNs) : CoreEvent;

public sealed record RoomLeftBehind(string Client, string Zone, string Detail) : CoreEvent;

// The core's phase 41: a room whose third rejoin landed within 30 s, and the
// end of that episode after 30 s of streaming. Rejoins counts the episode's
// moves, not the room's lifetime ones; while a room is unstable the core sends
// no room.fellBehind for it. Nothing on this server acts on either (phase 34).
public sealed record RoomUnstable(string Client, string Zone, string Trigger, ulong Rejoins, ulong BackoffMs,
    string? Detail = null) : CoreEvent;

public sealed record RoomStable(string Client, string Zone, ulong Rejoins, ulong UnstableForMs) : CoreEvent;

// What a room's client keeps and reports, announced by the core as it arrives
// so that nothing has to be polled for it: its capabilities re-sent on a live
// session (MATP section 12), its sound cards (56), its buffer (58), and its
// Volume Control with the path it resolved (59).
public sealed record RoomCapabilities(string Client, string Device, CoreCapabilities Capabilities) : CoreEvent;

public sealed record RoomAudioDevices(string Client, CoreAudioDevices AudioDevices) : CoreEvent;

public sealed record RoomBufferConfig(string Client, uint BufferMs) : CoreEvent;

public sealed record RoomVolumeControl(string Client, CoreVolumeControl VolumeControl) : CoreEvent;

public sealed record RoomPowerState(string Client, string PowerState, string? WakeOnLan = null) : CoreEvent;

// The room's name, as its client said it (MATP section 60), and a room the
// core forgot (client.forget).
public sealed record RoomName(string Client, string Name) : CoreEvent;

public sealed record RoomForgotten(string Client) : CoreEvent;

// Events were dropped: by the core's queue for this connection, or, when
// Local, by CoreLink's own queue to the handlers because one was slow. Either
// way the model is still right (CoreLink resyncs it on the core's) and a
// handler reads it instead of the events it missed.
public sealed record EventsLost(ulong Count, bool Local = false) : CoreEvent;

// CoreLink has read `status` into the model and re-asserted the desired state:
// after a (re)connect, or after the core's events.lost.
public sealed record CoreSynchronised(bool Reconnected) : CoreEvent;

public interface ICoreEventSubscriber
{
    // Called on CoreLink's dispatch task, one event at a time. A slow handler
    // delays the handlers, never the reader; past the queue's bound, events
    // are dropped and the handler is told with EventsLost.
    ValueTask HandleAsync(CoreEvent coreEvent, CancellationToken cancellationToken);
}

// A button or encoder on a room was used (the core's phase 37, MATP section
// 57), with what the gesture is bound to. The core only announces it;
// InputActions does it. Count is the encoder's detents, 1 for a button; a
// volume step is Step * Count. Zone is absent for a room in none.
public sealed record RoomInput(
    string Client,
    string InputId,
    string Gesture,
    InputAction Action,
    uint Count,
    double? Step = null,
    double? Value = null,
    VolumeScope? Scope = null,
    string? Zone = null) : CoreEvent;
