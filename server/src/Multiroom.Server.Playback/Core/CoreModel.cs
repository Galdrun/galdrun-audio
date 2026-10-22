using System.Collections.Immutable;
using System.Text.Json.Serialization;

namespace Multiroom.Server.Playback.Core;

// One room as this server knows it from the core: the core's `status` entry,
// flattened, and kept current by the room.* events. Absent is unknown.
public sealed record CoreRoom(
    string Client,
    bool Connected,
    bool Member,
    string? Device = null,
    CoreCapabilities? Capabilities = null,
    string? Zone = null,
    CoreRoomSettings? Settings = null,
    CorePlan? Plan = null,
    string? NotReadyReason = null,
    string? StartDetail = null,
    CoreSync? Sync = null,
    ulong? BufferedMs = null,
    CorePlayback? Playback = null,
    ulong? Rejoins = null,
    CoreSystemMetrics? System = null,
    ulong? SystemAgeMs = null,
    CoreAudioDevices? AudioDevices = null,
    IReadOnlyList<CoreInput>? Inputs = null,
    uint? BufferMs = null,
    CoreVolumeControl? VolumeControl = null,
    string? Address = null,
    CoreRoomHealth? Health = null,
    string? Name = null,
    string? PowerState = null,
    CoreDriftCorrection? DriftCorrection = null,
    // Section 57: whether the client can run the deepSleep input action.
    bool? DeepSleep = null,
    // With PowerState "sleep": the MAC its Wake-on-LAN magic packet names.
    string? WakeOnLan = null);

// Whether a room in a zone keeps falling behind (the core's phase 41, this
// server's phase 34): absent for a room in no zone. Rejoins is the episode's
// while Unstable, else 0 (the room's lifetime count is CoreRoom.Rejoins).
// Since is when this server heard of the episode: the core's own instant is on
// its monotonic clock, so an episode first seen in a status has none. Trigger
// and Detail are the room.unstable event's. WifiRssiDbm is the room's last
// report, so a UI can say "weak Wi-Fi" rather than "unstable".
//
// Degraded is this server's own (2026-10-08, the owner's ask): the core counts
// only rejoins, so a room that underruns without being moved yet looks fine
// to it. A room with DegradedUnderruns or more underruns in its playback
// reports within DegradedWindow is degraded, Since when it became so, until
// the window holds fewer. Unstable, the core's, wins over it. RecentUnderruns
// is how many the window holds; UnderrunTimes, kept for the window, is not
// answered.
public sealed record CoreRoomHealth(
    RoomHealthState State,
    ulong Rejoins,
    DateTimeOffset? Since = null,
    ulong? BackoffMs = null,
    string? Trigger = null,
    string? Detail = null,
    int? WifiRssiDbm = null,
    int RecentUnderruns = 0,
    [property: JsonIgnore] IReadOnlyList<DateTimeOffset>? UnderrunTimes = null)
{
    public const int DegradedUnderruns = 3;

    public static readonly TimeSpan DegradedWindow = TimeSpan.FromSeconds(30);
}

public enum RoomHealthState
{
    Ok,
    Degraded,
    Unstable,
}

// The room's actual plan, from room.joined or from status. SourceFormat,
// Description and Equalizer only come with status.
public sealed record CorePlan(
    bool BitPerfect,
    string Reason,
    AudioFormatInfo OutputFormat,
    bool DriftCorrected,
    AudioFormatInfo? SourceFormat = null,
    string? Description = null,
    CoreOutputEqualizer? Equalizer = null,
    string? DriftActuator = null);

public sealed record CoreSnapshot(
    ImmutableDictionary<string, CoreZone> Zones,
    ImmutableDictionary<string, CoreRoom> Rooms)
{
    public static CoreSnapshot Empty { get; } = new(
        ImmutableDictionary.Create<string, CoreZone>(StringComparer.Ordinal),
        ImmutableDictionary.Create<string, CoreRoom>(StringComparer.Ordinal));
}

// What the core is doing, as far as this server knows: built from `status`,
// then kept current by events and by the answers to this server's own
// commands (a zone.create or a source.stage has no event of its own). Readers
// take an immutable snapshot and never lock; CoreLink's reader is the only
// writer, so updates land in wire order.
public sealed class CoreModel(TimeProvider? time = null)
{
    private readonly Lock _lock = new();
    private readonly TimeProvider _time = time ?? TimeProvider.System;
    private volatile CoreSnapshot _current = CoreSnapshot.Empty;

    public CoreSnapshot Current => _current;

    public void Apply(CoreEvent coreEvent)
    {
        lock (_lock)
            _current = Applied(_current, coreEvent, _time.GetUtcNow());
    }

    // The answer to one of this server's commands, where it says something no
    // event will. A `status` answer replaces the whole model.
    public void Apply(CoreCommand command, object reply)
    {
        lock (_lock)
            _current = Applied(_current, command, reply, _time.GetUtcNow());
    }

    // Before is the room as the model held it, for what status cannot say:
    // when an episode it already knew of began, its trigger and detail, and
    // the underruns its earlier reports counted.
    private static CoreRoom FromStatus(CoreClientInfo c, CoreRoom? before, DateTimeOffset now) => new(
        c.ClientId,
        c.Connected,
        c.Member,
        c.Device,
        c.Capabilities,
        c.Zone,
        c.Settings,
        c.Output is { } o
            ? new CorePlan(o.BitPerfect, o.Reason, o.Format, c.DriftCorrection.Active ?? false, o.SourceFormat, o.Plan,
                o.Equalizer, c.DriftCorrection.Actuator)
            : null,
        c.Stream is { Ready: false } s ? s.Reason : null,
        c.StartDetail,
        c.Sync,
        c.Output?.BufferedMs,
        c.Playback,
        c.Rejoins,
        c.System,
        c.SystemAgeMs,
        c.AudioDevices,
        c.Inputs,
        c.BufferMs,
        c.VolumeControl,
        c.Address,
        HealthOf(c, before, now),
        c.Name,
        c.Stream?.PowerState ?? c.PowerState,
        c.DriftCorrection,
        c.DeepSleep,
        c.WakeOnLan);

    // A core before its phase 41, or a room in no zone, says nothing.
    private static CoreRoomHealth? HealthOf(CoreClientInfo c, CoreRoom? room, DateTimeOffset now)
    {
        if (c.Unstable is not { } unstable)
            return null;
        var before = room?.Health;
        var backoff = c.BackoffMs is 0 ? null : c.BackoffMs;
        var rssi = c.System?.WifiRssiDbm;
        var counted = UnderrunsIn(before?.UnderrunTimes, room?.Playback?.Underruns, c.Playback?.Underruns, now);
        List<DateTimeOffset>? times = counted.Count == 0 ? null : counted;
        var recent = counted.Count;
        if (unstable)
        {
            var known = before is { State: RoomHealthState.Unstable } ? before : null;
            return new CoreRoomHealth(RoomHealthState.Unstable, c.EpisodeRejoins ?? 0, known?.Since, backoff,
                known?.Trigger, known?.Detail, rssi, recent, times);
        }
        if (recent >= CoreRoomHealth.DegradedUnderruns)
        {
            var since = before is { State: RoomHealthState.Degraded, Since: { } was } ? was : now;
            return new CoreRoomHealth(RoomHealthState.Degraded, 0, since, backoff, WifiRssiDbm: rssi,
                RecentUnderruns: recent, UnderrunTimes: times);
        }
        return new CoreRoomHealth(RoomHealthState.Ok, 0, BackoffMs: backoff, WifiRssiDbm: rssi,
            RecentUnderruns: recent, UnderrunTimes: times);
    }

    // The underruns within the window: the earlier ones still in it, and the
    // ones the room's count rose by since the last report, at `now`. A count
    // that fell is a client that started again; nothing is added for it.
    private static List<DateTimeOffset> UnderrunsIn(IReadOnlyList<DateTimeOffset>? earlier, ulong? was, ulong? reported,
        DateTimeOffset now)
    {
        var times = (earlier ?? []).Where(t => now - t < CoreRoomHealth.DegradedWindow).ToList();
        if (was is { } a && reported is { } b && b > a)
            times.AddRange(Enumerable.Repeat(now, (int)Math.Min(b - a, 1000)));
        return times;
    }

    // An event keeps the underruns counted so far.
    private static CoreRoomHealth Keep(CoreRoomHealth health, CoreRoom r) => health with
    {
        RecentUnderruns = r.Health?.RecentUnderruns ?? 0,
        UnderrunTimes = r.Health?.UnderrunTimes,
    };

    private static CoreSnapshot Applied(CoreSnapshot m, CoreEvent coreEvent, DateTimeOffset now) => coreEvent switch
    {
        SourceNeeded e => Zone(m, e.Zone, z => z with { Staged = [] }),
        SourceStarted e => Zone(m, e.Zone, z =>
        {
            var staged = z.Staged.FirstOrDefault(s => s.SourceId == e.SourceId);
            return z with
            {
                Source = new CoreSource(e.SourceId, false, staged?.MediaUri, e.TrackId ?? staged?.TrackId,
                    staged?.Title, e.StartOffsetNs == 0 ? null : e.StartOffsetNs, e.TimelineNs),
                Staged = [.. z.Staged.Where(s => s.SourceId != e.SourceId)],
            };
        }),
        SourceFailed e => Zone(m, e.Zone, z => z with
        {
            FailedSources = [.. z.FailedSources, new CoreFailedSource(e.SourceId, e.Reason)],
            Staged = [.. z.Staged.Where(s => s.SourceId != e.SourceId)],
        }),
        ZoneEnded e => Zone(m, e.Zone, z => z with
        {
            State = ZoneState.Ended,
            Source = z.Source?.SourceId == e.AfterSourceId ? z.Source with { Ended = true } : z.Source,
        }),
        ZoneTransport e => Zone(m, e.Zone, z => z with
        {
            Transport = e.State,
            TimelineNs = e.State == TransportState.Stopped ? null : e.TimelineNs,
        }),
        RoomConnected e => Room(m, e.Client, r => r with
        {
            Connected = true,
            PowerState = null,
            WakeOnLan = null,
            Device = e.DeviceName,
            Address = e.Address ?? r.Address,
            Zone = e.Zone ?? r.Zone,
            NotReadyReason = null,
        }),
        // The core forgets a room's system figures and devices with its
        // connection.
        RoomDisconnected e => Leave(Room(m, e.Client, r => Unplanned(r) with
        {
            Connected = false,
            // An announced deep sleep is why it went away.
            PowerState = r.PowerState is "sleep" or "sleepButtonOnly" ? r.PowerState : null,
            WakeOnLan = r.PowerState == "sleep" ? r.WakeOnLan : null,
            System = null,
            SystemAgeMs = null,
            AudioDevices = null,
            Inputs = null,
            DeepSleep = null,
            BufferMs = null,
            VolumeControl = null,
        }), e.Client, e.Zone),
        RoomJoined e => Join(Room(m, e.Client, r => r with
        {
            Member = true,
            Zone = e.Zone,
            Plan = new CorePlan(e.BitPerfect, e.Reason, e.OutputFormat, e.DriftCorrected, DriftActuator: e.DriftActuator),
            Health = r.Health ?? new CoreRoomHealth(RoomHealthState.Ok, 0, WifiRssiDbm: r.System?.WifiRssiDbm),
            NotReadyReason = null,
            StartDetail = null,
        }), e.Client, e.Zone),
        // Removed from the zone or the zone deleted: it no longer belongs
        // there. Moved or replanned: it answers again and rejoins. Leaving
        // never adds a room: one forgotten while this event was on its way
        // would come back, and the zone keeper would give it a new zone.
        RoomLeft e when !m.Rooms.ContainsKey(e.Client) => Leave(m, e.Client, e.Zone),
        RoomLeft e => Leave(Room(m, e.Client, r => Unplanned(r) with
        {
            Zone = e.Cause is "removed" or "deleted" ? null : r.Zone,
        }), e.Client, e.Zone),
        RoomNotReady e => Room(m, e.Client, r => r with { NotReadyReason = e.Reason }),
        // A room's lifetime rejoins come from status alone (phase 34): while it
        // is unstable the core sends no room.fellBehind, so counting them here
        // would fall behind.
        RoomLeftBehind e => Room(m, e.Client, r => r with { StartDetail = e.Detail }),
        RoomUnstable e => Room(m, e.Client, r => r with
        {
            Health = Keep(new CoreRoomHealth(RoomHealthState.Unstable, e.Rejoins, now, e.BackoffMs is 0 ? null : e.BackoffMs,
                e.Trigger, e.Detail, r.System?.WifiRssiDbm), r),
        }),
        RoomStable e => Room(m, e.Client, r => r with
        {
            Health = Keep(new CoreRoomHealth(RoomHealthState.Ok, 0, WifiRssiDbm: r.System?.WifiRssiDbm), r),
        }),
        RoomCapabilities e => Room(m, e.Client, r => r with { Device = e.Device, Capabilities = e.Capabilities }),
        RoomAudioDevices e => Room(m, e.Client, r => r with { AudioDevices = e.AudioDevices }),
        RoomBufferConfig e => Room(m, e.Client, r => r with { BufferMs = e.BufferMs }),
        RoomVolumeControl e => Room(m, e.Client, r => r with { VolumeControl = e.VolumeControl }),
        RoomPowerState e => Room(m, e.Client, r => r with
        {
            PowerState = e.PowerState,
            WakeOnLan = e.WakeOnLan,
            NotReadyReason = e.PowerState == "on" ? null : r.NotReadyReason,
        }),
        // Kept through a disconnect, like the address: a room that is away is
        // still shown by its name (MATP section 60).
        RoomName e => Room(m, e.Client, r => r with { Name = e.Name }),
        RoomForgotten e => m with { Rooms = m.Rooms.Remove(e.Client) },
        _ => m,
    };

    private static CoreSnapshot Applied(CoreSnapshot m, CoreCommand command, object reply, DateTimeOffset now) => (command, reply) switch
    {
        (GetStatus, StatusReply r) => new CoreSnapshot(
            r.Zones.ToImmutableDictionary(z => z.Zone, StringComparer.Ordinal),
            r.Clients.ToImmutableDictionary(c => c.ClientId, c => FromStatus(c, m.Rooms.GetValueOrDefault(c.ClientId), now),
                StringComparer.Ordinal)),
        (GetClients, ClientsReply r) => m with
        {
            Rooms = r.Clients.ToImmutableDictionary(c => c.ClientId, c => FromStatus(c, m.Rooms.GetValueOrDefault(c.ClientId), now),
                StringComparer.Ordinal),
        },
        (GetZones, ZonesReply r) => m with
        {
            Zones = r.Zones.ToImmutableDictionary(z => z.Zone, StringComparer.Ordinal),
        },
        (ClientForget c, ClientForgotten) => m with { Rooms = m.Rooms.Remove(c.Client) },
        (ZoneCreate c, ZoneCreated) when !m.Zones.ContainsKey(c.Zone) => m with
        {
            Zones = m.Zones.Add(c.Zone, new CoreZone(c.Zone, ZoneState.Idle, TransportState.Stopped,
                [], [], [], [], 0, 0)),
        },
        (ZoneDelete c, ZoneDeleted) => m with
        {
            Zones = m.Zones.Remove(c.Zone),
            Rooms = m.Rooms.SetItems(m.Rooms.Values.Where(r => r.Zone == c.Zone)
                .Select(r => KeyValuePair.Create(r.Client, Unplanned(r) with { Zone = null }))),
        },
        (ZoneSetClients c, ZoneClientsSet r) => SetClients(m, c.Zone, c.Clients, r.Removed),
        (SourceStage c, SourceStaged { Staged: true }) => Zone(m, c.Zone, z => z with
        {
            Staged = [.. z.Staged, new CoreSegment(c.SourceId, c.MediaUri, c.TrackId, c.Title, c.StartOffsetNs)],
        }),
        (SourceClearStaged c, StagedCleared) => Zone(m, c.Zone, z => z with { Staged = [] }),
        (ZonePlay c, ZonePlayed r) => Zone(m, c.Zone, z => z with { State = r.State, Transport = r.Transport }),
        (ZonePause c, ZonePaused r) => Zone(m, c.Zone, z => z with
        {
            Transport = TransportState.Paused,
            TimelineNs = r.TimelineNs,
        }),
        (ZoneStop c, ZoneStopped { Stopped: true }) => Zone(m, c.Zone, z => z with
        {
            State = ZoneState.Idle,
            Transport = TransportState.Stopped,
            TimelineNs = null,
        }),
        (ClientSetAudioControl c, AudioControlSet) => Settings(m, c.Client, s => s with
        {
            AudioControl = c.AudioControl,
            AudioControlIsDefault = false,
        }),
        (ClientSetGrouped c, GroupedSet) => Settings(m, c.Client, s => s with { Grouped = c.Grouped }),
        (ClientSetVolume c, VolumeSet r) => Settings(m, c.Client, s => s with { Volume = r.Value, Muted = r.Muted }),
        (ClientSetOutputLatency c, OutputLatencySet r) => Settings(m, c.Client, s => s with
        {
            OutputLatencyMs = r.LatencyMs,
        }),
        (ClientSetEqualizer c, EqualizerSet r) => Settings(m, c.Client, s => s with { Equalizer = r.Equalizer }),
        // Asked, not yet done: the client re-reports and the next `clients`
        // shows the device it plays on.
        (ClientSetAudioDevice c, AudioDeviceSet) => m.Rooms.TryGetValue(c.Client, out var room) &&
            room.AudioDevices is { } devices
                ? m with { Rooms = m.Rooms.SetItem(c.Client, room with { AudioDevices = devices with { Configured = c.DeviceId } }) }
                : m,
        // The same: the core answers with the bindings as they will be, and
        // the client's INPUT_CONTROLS confirms them in the next `clients`.
        (ClientSetInputBindings c, InputBindingsSet r) => m.Rooms.TryGetValue(c.Client, out var room) &&
            room.Inputs is { } inputs
                ? m with
                {
                    Rooms = m.Rooms.SetItem(c.Client, room with
                    {
                        Inputs = [.. inputs.Select(i => i.Id == r.InputId ? i with { Bindings = r.Bindings } : i)],
                    }),
                }
                : m,
        _ => m,
    };

    // An event about a zone the model does not hold changes nothing: the
    // model is behind, and the next status puts it right.
    private static CoreSnapshot Zone(CoreSnapshot m, string zone, Func<CoreZone, CoreZone> change) =>
        m.Zones.TryGetValue(zone, out var z) ? m with { Zones = m.Zones.SetItem(zone, change(z)) } : m;

    // A room the core has just told us about is created: the core keeps a
    // record for it too.
    private static CoreSnapshot Room(CoreSnapshot m, string client, Func<CoreRoom, CoreRoom> change)
    {
        var room = m.Rooms.GetValueOrDefault(client) ?? new CoreRoom(client, Connected: false, Member: false);
        return m with { Rooms = m.Rooms.SetItem(client, change(room)) };
    }

    // Settings the model does not know yet stay unknown until status.
    private static CoreSnapshot Settings(CoreSnapshot m, string client, Func<CoreRoomSettings, CoreRoomSettings> change) =>
        m.Rooms.TryGetValue(client, out var r) && r.Settings is { } s
            ? m with { Rooms = m.Rooms.SetItem(client, r with { Settings = change(s) }) }
            : m;

    private static CoreRoom Unplanned(CoreRoom r) =>
        r with { Member = false, Plan = null, Sync = null, BufferedMs = null, Health = null };

    private static CoreSnapshot Join(CoreSnapshot m, string client, string zone) =>
        Zone(m, zone, z => z.Members.Contains(client) ? z : z with { Members = [.. z.Members, client] });

    private static CoreSnapshot Leave(CoreSnapshot m, string client, string? zone) =>
        zone is null ? m : Zone(m, zone, z => z with { Members = [.. z.Members.Where(c => c != client)] });

    private static CoreSnapshot SetClients(CoreSnapshot m, string zone, IReadOnlyList<string> clients,
        IReadOnlyList<string> removed)
    {
        m = Zone(m, zone, z => z with { Clients = clients });
        foreach (var client in removed)
            m = Room(m, client, r => r with { Zone = null });
        foreach (var client in clients)
            m = Room(m, client, r => r with { Zone = zone });
        // A room asked into this zone leaves any other zone's wanted list.
        foreach (var other in m.Zones.Values.Where(z => z.Zone != zone && z.Clients.Any(clients.Contains)))
            m = Zone(m, other.Zone, z => z with { Clients = [.. z.Clients.Where(c => !clients.Contains(c))] });
        return m;
    }
}
