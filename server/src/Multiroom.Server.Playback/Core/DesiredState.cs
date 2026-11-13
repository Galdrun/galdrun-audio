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
// InputsEnabled: a Bluetooth output room follows the buttons of its device's
// own room (InputActions); null is no. Not a core setting.
public sealed record DesiredRoom(
    AudioControl? AudioControl = null,
    double? Volume = null,
    bool? Muted = null,
    ulong? OutputLatencyMs = null,
    string? Name = null,
    CoreEqualizer? Equalizer = null,
    double? MaxVolumeDb = null,
    string? WakeOnLan = null,
    bool? InputsEnabled = null);

// A zone's volume and mute (phase 15), as Raumfeld has them. Volume is not
// kept: it is the loudest member's, and setting it moves every member by the
// same amount, so the rooms keep their balance. Muted is the zone's own, a
// mute over each member's. Neither is a core setting.
public sealed record DesiredZoneLevel(double Volume = 1.0, bool Muted = false);

// What is sent for a room: its own settings with its zone's applied. Volume
// is the room's within 0..1, lowered by its maximum (in decibels), Muted the room's or the zone's. Grouped is
// whether it shares its zone, which has the core correct it unless it is
// BitPerfect: while its zone's wanted list names another room. A room left
// alone in a zone that plays is told so at once; the core keeps its plan to
// the end of the track and plans it alone from the next. Null is not asked.
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
    // The zones given a mute of their own, muted or not.
    private readonly Dictionary<string, bool> _zoneMuted = new(StringComparer.Ordinal);

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

    // The zone's volume, its loudest member's (0 with none), and its mute.
    public DesiredZoneLevel ZoneLevel(string zone)
    {
        lock (_lock)
        {
            var members = _zones.GetValueOrDefault(zone) ?? [];
            var volume = members.Count == 0 ? 0.0 : Math.Clamp(members.Max(RawVolumeLocked), 0.0, 1.0);
            return new DesiredZoneLevel(volume, _zoneMuted.GetValueOrDefault(zone));
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

    // Raised after a room's own volume was recorded, with the room; its zone's
    // volume (ZoneLevel) is its loudest member's, so it may have changed too.
    // Outside the lock, like Changed.
    public event EventHandler<string>? RoomVolumeChanged;

    // At startup, before CoreLink first connects: the table as a previous run
    // of this server left it, so a core that restarts meanwhile is rebuilt.
    // A zone volume other than 1.0 is from a file that kept one over its
    // rooms: it is moved into them, so each room plays as loud as it did.
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
            {
                _zoneMuted[zone] = level.Muted;
                if (level.Volume != 1.0)
                    MoveLocked(_zones.GetValueOrDefault(zone) ?? [], level.Volume - 1.0);
            }
        }
    }

    public void Record(CoreCommand command)
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
                    foreach (var client in _zones.GetValueOrDefault(c.Zone) ?? [])
                        SettleLocked(client);
                    _zones.Remove(c.Zone);
                    _zoneOrder.Remove(c.Zone);
                    _zoneMuted.Remove(c.Zone);
                    break;
                case ZoneSetClients c:
                {
                    var moving = c.Clients.Concat(_zones.GetValueOrDefault(c.Zone) ?? []).Distinct(StringComparer.Ordinal)
                        .ToDictionary(client => client, ZoneOfLocked, StringComparer.Ordinal);
                    AddZone(c.Zone);
                    _zones[c.Zone] = [.. c.Clients];
                    // A room is in one zone at most: asked in here, it is out
                    // of wherever it was asked before.
                    foreach (var other in _zoneOrder.Where(z => z != c.Zone && _zones[z] is not null).ToList())
                        _zones[other] = [.. _zones[other]!.Where(client => !c.Clients.Contains(client))];
                    foreach (var (client, was) in moving)
                    {
                        if (ZoneOfLocked(client) != was)
                            SettleLocked(client);
                    }
                    break;
                }
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
        if (command is ClientSetVolume { Value: not null } volume)
            RoomVolumeChanged?.Invoke(this, volume.Client);
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
        }
        Changed?.Invoke(this, EventArgs.Empty);
    }

    // A zone's volume and mute; null leaves one as it is. The volume moves
    // every member by what takes the loudest to it (Raumfeld's ZoneVolume): a
    // member pushed below 0 keeps its distance there and plays silence, so
    // turning the zone up again brings every room back where it was.
    public void SetZoneLevel(string zone, double? volume, bool? muted)
    {
        lock (_lock)
        {
            var members = _zones.GetValueOrDefault(zone) ?? [];
            if (volume is { } v && members.Count > 0)
                MoveLocked(members, v - members.Max(RawVolumeLocked));
            if (muted is { } m)
                _zoneMuted[zone] = m;
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

    // Whether the room's buttons and encoders run their actions.
    public void SetInputsEnabled(string client, bool enabled)
    {
        lock (_lock)
            SetRoom(client, r => r with { InputsEnabled = enabled });
        Changed?.Invoke(this, EventArgs.Empty);
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
    public DesiredState With(CoreCommand command)
    {
        var copy = new DesiredState();
        lock (_lock)
        {
            copy._zoneOrder.AddRange(_zoneOrder);
            foreach (var (zone, clients) in _zones)
                copy._zones[zone] = clients;
            foreach (var (client, room) in _rooms)
                copy._rooms[client] = room;
            foreach (var (zone, muted) in _zoneMuted)
                copy._zoneMuted[zone] = muted;
        }
        copy.Record(command);
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

    // Volume is a slider position linear in decibels over VolumeRangeDb (the
    // core's gainForPosition), as Raumfeld's percent is: moving positions by
    // the same amount moves rooms by the same decibels, and MaxVolumeDb takes
    // exactly itself. A room's own position may lie outside 0..1 after its
    // zone moved it; it is played within. A position at 0 stays silence; a
    // maximum below the scale plays its quietest step rather than a mute.
    // Rounded, so that 0.8 moved by -0.5 is kept as 0.3 and not as its binary
    // residue.
    private const double VolumeRangeDb = 60.0;
    private const double QuietestPosition = 0.001;

    private static double Lower(double position, double byDb) =>
        position <= 0.0 ? 0.0 : Math.Max(Math.Round(position + byDb / VolumeRangeDb, 9), QuietestPosition);

    // A room never given a volume plays at full, as far as its zone knows.
    private double RawVolumeLocked(string client) => _rooms.GetValueOrDefault(client)?.Volume ?? 1.0;

    private void MoveLocked(IEnumerable<string> clients, double by)
    {
        foreach (var client in clients)
            SetRoom(client, r => r with { Volume = Math.Round((r.Volume ?? 1.0) + by, 9) });
    }

    // Out of its zone, a room's distance to the others means nothing: its
    // position is taken back within 0..1, which plays the same.
    private void SettleLocked(string client)
    {
        if (_rooms.GetValueOrDefault(client)?.Volume is { } v and (< 0.0 or > 1.0))
            SetRoom(client, r => r with { Volume = Math.Clamp(v, 0.0, 1.0) });
    }

    private EffectiveRoom EffectiveLocked(string client)
    {
        var room = _rooms.GetValueOrDefault(client) ?? new DesiredRoom();
        var zone = ZoneOfLocked(client);
        double? volume = room.Volume is { } own ? Math.Clamp(own, 0.0, 1.0) : null;
        if (room.MaxVolumeDb is { } maxDb)
            volume = Lower(volume ?? 1.0, maxDb);
        bool? zoneMuted = zone is not null && _zoneMuted.TryGetValue(zone, out var m) ? m : null;
        var muted = zoneMuted is null ? room.Muted : (room.Muted ?? false) || zoneMuted.Value;

        var grouped = zone is not null && _zones[zone]!.Count > 1;
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
