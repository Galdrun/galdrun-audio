namespace Multiroom.Server.Playback.Core;

// One room's settings as this server asked for them; null is never asked.
// There is no drift setting: the core corrects a room from its Audio Control,
// its grouping and its device. Name is not
// asked for: it is the last name the room said (phase 39), kept so a room that
// is away is still shown by it. The client owns it, so it is never sent.
// MaxVolumeDb is the room's level at full volume, for a line input that
// clips at full scale; it is not a core setting either, only an offset in the
// volume Effective() sends. WakeOnLan is the MAC the room said before it went
// into a deep sleep the network can end, kept until it connects again: the
// core forgets it when it restarts, and Diff() wakes the room with it.
public sealed record DesiredRoom(
    AudioControl? AudioControl = null,
    double? Volume = null,
    bool? Muted = null,
    ulong? OutputLatencyMs = null,
    string? Name = null,
    CoreEqualizer? Equalizer = null,
    double? MaxVolumeDb = null,
    string? WakeOnLan = null);

// A zone's own volume and mute (phase 15): a level over each member's volume
// and a mute over each member's mute. Neither is a core setting.
public sealed record DesiredZoneLevel(double Volume = 1.0, bool Muted = false);

// What is sent for a room: its own settings with its zone's applied. Volume
// is the room's lowered by the zone's and its maximum (in decibels), Muted the room's or the zone's. Grouped is
// whether it shares its zone, which has the core correct it unless it is
// BitPerfect: while its zone's wanted list names another room, and while it
// was left alone in a zone that plays, until that zone stops (phase 16). Null
// is not asked.
public sealed record EffectiveRoom(
    double? Volume,
    bool? Muted,
    bool Grouped = false);

// What this server has asked the core for and wants to stay true: the zones
// that exist, their memberships, and each room's settings (audio control,
// output latency, equalizer) and volume. The core is restartable and
// amnesiac, so after every (re)connect CoreLink diffs this table against the
// model read from `status` and sends exactly the commands the difference
// needs -- never a replay of what it remembers sending.
//
// Zones the table does not name are left alone: it says what must exist, not
// all that may.
//
// Recorded from the commands callers send (CoreLink.SendAsync), when the core
// accepted them or when there was no core to ask. The table records intent,
// not the wire (phases 15 and 16): a room's own volume, mute and drift
// correction, and each zone's volume and mute. What is sent for a room is
// Effective(), and the diff compares the core with that.
public sealed class DesiredState
{
    private readonly Lock _lock = new();

    // Insertion order is the order zones are re-created in. A null membership
    // is a zone that must exist with no membership asked for.
    private readonly List<string> _zoneOrder = [];
    private readonly Dictionary<string, IReadOnlyList<string>?> _zones = new(StringComparer.Ordinal);
    private readonly Dictionary<string, DesiredRoom> _rooms = new(StringComparer.Ordinal);
    private readonly Dictionary<string, DesiredZoneLevel> _levels = new(StringComparer.Ordinal);

    // Rooms left alone in a zone that was playing: their correction stays on
    // until that zone stops, pauses or plays out. Not kept across a restart.
    private readonly HashSet<string> _pendingStop = new(StringComparer.Ordinal);

    // Every zone that must exist, in creation order, with the membership
    // asked for (null: none asked).
    public IReadOnlyList<(string Zone, IReadOnlyList<string>? Clients)> Zones
    {
        get
        {
            lock (_lock)
                return [.. _zoneOrder.Select(z => (z, _zones[z]))];
        }
    }

    public IReadOnlyDictionary<string, DesiredRoom> Rooms
    {
        get
        {
            lock (_lock)
                return new Dictionary<string, DesiredRoom>(_rooms, StringComparer.Ordinal);
        }
    }

    // The zones given a volume or a mute of their own; any other is at 1.0,
    // not muted.
    public IReadOnlyDictionary<string, DesiredZoneLevel> ZoneLevels
    {
        get
        {
            lock (_lock)
                return new Dictionary<string, DesiredZoneLevel>(_levels, StringComparer.Ordinal);
        }
    }

    // Every room the table names, as a room or as a member.
    public IReadOnlyList<string> Clients
    {
        get
        {
            lock (_lock)
                return ClientsLocked();
        }
    }

    // Raised after a command changed the table, outside its lock; a handler
    // must not wait on anything.
    public event EventHandler? Changed;

    // At startup, before CoreLink first connects: the table as a previous run
    // of this server left it, so a core that restarts meanwhile is rebuilt.
    public void Restore(IEnumerable<(string Zone, IReadOnlyList<string>? Clients)> zones,
        IReadOnlyDictionary<string, DesiredRoom> rooms,
        IReadOnlyDictionary<string, DesiredZoneLevel>? levels = null)
    {
        lock (_lock)
        {
            foreach (var (zone, clients) in zones)
            {
                AddZone(zone);
                _zones[zone] = clients;
            }
            foreach (var (client, room) in rooms)
                _rooms[client] = room;
            foreach (var (zone, level) in levels ?? new Dictionary<string, DesiredZoneLevel>())
                _levels[zone] = level;
        }
    }

    // `model` is the core as it stands when the command was sent: a room left
    // alone in a zone that plays keeps its correction until the zone stops.
    public void Record(CoreCommand command, CoreSnapshot? model = null)
    {
        if (command is not (ZoneCreate or ZoneDelete or ZoneSetClients or ClientSetAudioControl
            or ClientSetVolume or ClientSetOutputLatency or ClientSetEqualizer))
        {
            return;
        }
        lock (_lock)
        {
            switch (command)
            {
                case ZoneCreate c:
                    AddZone(c.Zone);
                    break;
                case ZoneDelete c:
                    _zones.Remove(c.Zone);
                    _zoneOrder.Remove(c.Zone);
                    _levels.Remove(c.Zone);
                    _pendingStop.RemoveWhere(client => ZoneOfLocked(client) is null);
                    break;
                case ZoneSetClients c:
                    var shared = _zoneOrder.Where(z => _zones[z] is { Count: > 1 }).ToList();
                    AddZone(c.Zone);
                    _zones[c.Zone] = [.. c.Clients];
                    // A room is in one zone at most: asked in here, it is out
                    // of wherever it was asked before.
                    foreach (var other in _zoneOrder.Where(z => z != c.Zone && _zones[z] is not null).ToList())
                        _zones[other] = [.. _zones[other]!.Where(client => !c.Clients.Contains(client))];
                    _pendingStop.RemoveWhere(client => ZoneOfLocked(client) is not { } zone || _zones[zone]!.Count > 1);
                    foreach (var zone in shared.Where(z => _zones.GetValueOrDefault(z) is { Count: 1 }))
                    {
                        if (model?.Zones.GetValueOrDefault(zone)?.Transport is TransportState.Playing or TransportState.Starting)
                            _pendingStop.Add(_zones[zone]![0]);
                    }
                    break;
                case ClientSetAudioControl c:
                    SetRoom(c.Client, r => r with { AudioControl = c.AudioControl });
                    break;
                case ClientSetVolume c:
                    SetRoom(c.Client, r => r with { Volume = c.Value ?? r.Volume, Muted = c.Muted ?? r.Muted });
                    break;
                case ClientSetOutputLatency c:
                    SetRoom(c.Client, r => r with { OutputLatencyMs = c.LatencyMs });
                    break;
                case ClientSetEqualizer c:
                    SetRoom(c.Client, r => r with { Equalizer = c.Equalizer });
                    break;
            }
        }
        Changed?.Invoke(this, EventArgs.Empty);
    }

    // The name the room said last; unchanged is not a change.
    public void RememberName(string client, string name)
    {
        lock (_lock)
        {
            if (_rooms.GetValueOrDefault(client)?.Name == name)
                return;
            SetRoom(client, r => r with { Name = name });
        }
        Changed?.Invoke(this, EventArgs.Empty);
    }

    // The MAC a sleeping room is woken at, null once it is back; unchanged is
    // not a change.
    public void RememberWake(string client, string? wakeOnLan)
    {
        lock (_lock)
        {
            if (_rooms.GetValueOrDefault(client)?.WakeOnLan == wakeOnLan)
                return;
            SetRoom(client, r => r with { WakeOnLan = wakeOnLan });
        }
        Changed?.Invoke(this, EventArgs.Empty);
    }

    // A room gone for good (phase 39): its settings, its kept name and its
    // place in any membership. The zones stay; one left empty is ZoneKeeper's.
    public void Forget(string client)
    {
        lock (_lock)
        {
            _rooms.Remove(client);
            foreach (var zone in _zoneOrder.Where(z => _zones[z]?.Contains(client) == true).ToList())
                _zones[zone] = [.. _zones[zone]!.Where(c => c != client)];
            _pendingStop.Remove(client);
        }
        Changed?.Invoke(this, EventArgs.Empty);
    }

    // A zone's own volume and mute; null leaves one as it is.
    public void SetZoneLevel(string zone, double? volume, bool? muted)
    {
        lock (_lock)
        {
            var level = _levels.GetValueOrDefault(zone) ?? new DesiredZoneLevel();
            _levels[zone] = level with { Volume = volume ?? level.Volume, Muted = muted ?? level.Muted };
        }
        Changed?.Invoke(this, EventArgs.Empty);
    }

    // The room's level at full volume, 0 dB for none.
    public void SetMaxVolumeDb(string client, double maxVolumeDb)
    {
        lock (_lock)
            SetRoom(client, r => r with { MaxVolumeDb = maxVolumeDb < 0.0 ? maxVolumeDb : null });
        Changed?.Invoke(this, EventArgs.Empty);
    }

    // The zone stopped, paused or played out: the correction its lone room
    // kept may go. Answers the rooms released.
    public IReadOnlyList<string> ReleasePendingStop(string zone)
    {
        lock (_lock)
        {
            var released = _pendingStop.Where(client => ZoneOfLocked(client) == zone).ToList();
            _pendingStop.ExceptWith(released);
            return released;
        }
    }

    // The zone whose wanted list names the room, or null.
    public string? ZoneOf(string client)
    {
        lock (_lock)
            return ZoneOfLocked(client);
    }

    // The zone's wanted list; empty when none is asked.
    public IReadOnlyList<string> MembersOf(string zone)
    {
        lock (_lock)
            return _zones.GetValueOrDefault(zone) ?? [];
    }

    public EffectiveRoom Effective(string client)
    {
        lock (_lock)
            return EffectiveLocked(client);
    }

    // A copy with `command` recorded, to compare the effective settings before
    // and after it; this table is left as it is.
    public DesiredState With(CoreCommand command, CoreSnapshot model)
    {
        var copy = new DesiredState();
        lock (_lock)
        {
            copy._zoneOrder.AddRange(_zoneOrder);
            foreach (var (zone, clients) in _zones)
                copy._zones[zone] = clients;
            foreach (var (client, room) in _rooms)
                copy._rooms[client] = room;
            foreach (var (zone, level) in _levels)
                copy._levels[zone] = level;
            copy._pendingStop.UnionWith(_pendingStop);
        }
        copy.Record(command, model);
        return copy;
    }

    // The commands that bring the core from `model` to this table, in the
    // order they must go: zones, then memberships, then room settings. Room
    // settings are compared as they are sent: effective.
    public IReadOnlyList<CoreCommand> Diff(CoreSnapshot model)
    {
        var commands = new List<CoreCommand>();
        lock (_lock)
        {
            foreach (var zone in _zoneOrder)
            {
                if (!model.Zones.ContainsKey(zone))
                    commands.Add(new ZoneCreate(zone));
            }

            foreach (var zone in _zoneOrder)
            {
                if (_zones[zone] is not { } wanted)
                    continue;
                IReadOnlyList<string> actual = model.Zones.TryGetValue(zone, out var z) ? z.Clients : [];
                if (!actual.ToHashSet(StringComparer.Ordinal).SetEquals(wanted))
                    commands.Add(new ZoneSetClients(zone, wanted));
            }

            foreach (var client in ClientsLocked())
            {
                var wanted = _rooms.GetValueOrDefault(client) ?? new DesiredRoom();
                var effective = EffectiveLocked(client);
                var actual = model.Rooms.GetValueOrDefault(client)?.Settings;
                // A setting the core holds only as its default is not the
                // room's own, and would follow the default if that changed.
                if (wanted.AudioControl is { } audioControl &&
                    (actual is null || actual.AudioControlIsDefault || actual.AudioControl != audioControl))
                {
                    commands.Add(new ClientSetAudioControl(client, audioControl));
                }
                if ((actual?.Grouped ?? false) != effective.Grouped)
                    commands.Add(new ClientSetGrouped(client, effective.Grouped));
                var volume = effective.Volume is { } v && actual?.Volume != v ? v : (double?)null;
                var muted = effective.Muted is { } mute && actual?.Muted != mute ? mute : (bool?)null;
                if (volume is not null || muted is not null)
                    commands.Add(new ClientSetVolume(client, volume, muted));
                // The core reports no latency for a room that was never given one,
                // which is 0.
                if (wanted.OutputLatencyMs is { } latency && (actual?.OutputLatencyMs ?? 0) != latency)
                    commands.Add(new ClientSetOutputLatency(client, latency));
                if (wanted.Equalizer is { } equalizer && actual?.Equalizer != equalizer)
                    commands.Add(ClientSetEqualizer.For(client, equalizer));
                // Asleep, and a core that no longer holds it so has restarted
                // since: nothing else would wake it.
                if (wanted.WakeOnLan is { } mac &&
                    model.Rooms.GetValueOrDefault(client) is not ({ Connected: true } or { PowerState: "sleep" }))
                {
                    commands.Add(new ClientWake(client, mac));
                }
            }
        }
        return commands;
    }

    private string? ZoneOfLocked(string client) =>
        _zoneOrder.FirstOrDefault(z => _zones[z]?.Contains(client) == true);

    private List<string> ClientsLocked() =>
        [.. _rooms.Keys.Concat(_zones.Values.SelectMany(c => c ?? [])).Distinct(StringComparer.Ordinal)];

    // A room never given a volume counts as 1.0 once its zone has a level of
    // its own, so the zone can turn it down and back; so does one with a
    // maximum. Volume is a slider position linear in decibels over
    // VolumeRangeDb (the core's gainForPosition), so levels combine by adding
    // their decibels: a zone at 0.5 takes 30 dB off its room, and MaxVolumeDb
    // takes exactly itself. A position at 0 stays silence; a sum below the
    // scale plays its quietest step rather than turning into a mute. Rounded,
    // so that 0.8 lowered by 0.5 is sent as 0.3 and not as its binary residue.
    private const double VolumeRangeDb = 60.0;
    private const double QuietestPosition = 0.001;

    private static double Lower(double position, double byDb) =>
        position <= 0.0 ? 0.0 : Math.Max(Math.Round(position + byDb / VolumeRangeDb, 9), QuietestPosition);

    private EffectiveRoom EffectiveLocked(string client)
    {
        var room = _rooms.GetValueOrDefault(client) ?? new DesiredRoom();
        var zone = ZoneOfLocked(client);
        var level = zone is null ? null : _levels.GetValueOrDefault(zone);
        var volume = level is null ? room.Volume
            : level.Volume <= 0.0 ? 0.0
            : Lower(room.Volume ?? 1.0, (level.Volume - 1.0) * VolumeRangeDb);
        if (room.MaxVolumeDb is { } maxDb)
            volume = Lower(volume ?? 1.0, maxDb);
        var muted = level is null ? room.Muted : (room.Muted ?? false) || level.Muted;

        var grouped = (zone is not null && _zones[zone]!.Count > 1) || _pendingStop.Contains(client);
        return new EffectiveRoom(volume, muted, grouped);
    }

    private void AddZone(string zone)
    {
        if (_zones.TryAdd(zone, null))
            _zoneOrder.Add(zone);
    }

    private void SetRoom(string client, Func<DesiredRoom, DesiredRoom> change) =>
        _rooms[client] = change(_rooms.GetValueOrDefault(client) ?? new DesiredRoom());
}
