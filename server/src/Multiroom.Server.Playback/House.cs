using System.Globalization;
using Multiroom.Server.Abstractions;
using Multiroom.Server.Library;
using Multiroom.Server.Playback.Core;
using Multiroom.Server.Providers;

namespace Multiroom.Server.Playback;

// A zone this server asked the core to hold, the membership it asked for
// (null: none asked), and its own volume and mute over its rooms' (phase 15).
public sealed record ZoneDesired(IReadOnlyList<string>? Clients, double Volume, bool Muted);

// One zone, both halves: Desired is this server's table, Actual the core's
// model, and Queue the queue engine's. Any may be absent. Zone is the id and
// never changes; Name is its rooms' client ids (the room's name), in
// membership order, joined with " + " (the id while it has none). StreamTitle is what a playing station
// last announced, for the UI only: the rooms keep the name.
public sealed record ZoneView(
    string Zone,
    string Name,
    ZoneDesired? Desired,
    CoreZone? Actual,
    QueueState? Queue,
    string? StreamTitle,
    Track? RadioTrack = null,
    AutoFillView? AutoFill = null,
    string User = "");

// What this server asked for a room: the zone whose membership names it, and
// its own settings and volume. Null is never asked, except Volume: in a zone
// with a level a room that never asked plays at 1.0 (DesiredState.Effective),
// so it answers 1.0 -- a UI that showed the core's effective value instead
// set the room's own from it, and raising it made the room quieter. The
// effective values are what is sent for it, its zone's volume and mute
// applied (phase 15).
// Grouped: it shares its zone, so the core drift corrects it unless its Audio
// Control is BitPerfect.
public sealed record RoomDesired(
    string? Zone,
    AudioControl? AudioControl,
    double? Volume,
    bool? Muted,
    ulong? OutputLatencyMs,
    double? EffectiveVolume,
    bool? EffectiveMuted,
    CoreEqualizer? Equalizer = null,
    bool Grouped = false,
    double? MaxVolumeDb = null);

// A room, both halves. Actual is the core's own account, unchanged: the
// connection, the device and what it can play, the settings as the core
// stores them, the plan with bitPerfect and the core's reason for it, drift
// and offset, the buffer, underruns, rejoins and the age of the room's last
// report. Absent when the core has never reported the room.
// Name is what people call the room (phase 39): the core's, else the last one
// this server kept, else the client id. User is who listens in it, "" the
// household (ZoneUsers).
public sealed record RoomView(string Client, string Name, RoomDesired Desired, CoreRoom? Actual, string User = "");

// The house as a caller sees it: zones and rooms
// with both halves, and a zone's own level. The REST API and the MCP tools
// both read and set them here, so they answer alike.
public sealed class House(CoreLink link, QueueEngine queues, ProviderRegistry providers, AutoFill autoFill,
    ZoneUsers users, UserStore userStore, EqualizerProfiles equalizerProfiles)
{
    // A zone's volume or mute was set here; recorded whether the core took it.
    public event EventHandler<string>? ZoneLevelChanged;

    // Every zone any half knows of, by name.
    public SortedSet<string> ZoneNames() =>
        new(link.Model.Current.Zones.Keys.Concat(link.Desired.Zones.Select(z => z.Zone))
            .Concat(queues.Queues.Select(q => q.Zone)).Where(zone =>
            {
                var clients = link.Desired.Zones.FirstOrDefault(z => z.Zone == zone).Clients
                    ?? link.Model.Current.Zones.GetValueOrDefault(zone)?.Clients ?? [];
                return clients.Count == 0 || clients.Any(IsActive);
            }), StringComparer.Ordinal);

    public IReadOnlyList<ZoneView> Zones() => [.. ZoneNames().Select(ZoneOf)];

    // A zone as a person names it: its id, its name, or one of its rooms (a
    // room's client id is its name), each in any case. Anything else is
    // answered as it is, for the caller's own notFound.
    public string ResolveZone(string text)
    {
        var names = ZoneNames();
        if (names.Contains(text))
            return text;
        var wanted = text.Trim();
        if (Zones().FirstOrDefault(z => string.Equals(z.Name, wanted, StringComparison.OrdinalIgnoreCase)) is { } named)
            return named.Zone;
        var room = RoomNames().FirstOrDefault(r => string.Equals(r, wanted, StringComparison.OrdinalIgnoreCase))
            ?? RoomNames().FirstOrDefault(r => string.Equals(NameOf(r), wanted, StringComparison.OrdinalIgnoreCase));
        return room is null
            ? text
            : link.Desired.ZoneOf(room) ?? link.Model.Current.Rooms.GetValueOrDefault(room)?.Zone ?? text;
    }

    public Result<ZoneView, QueueError> Zone(string zone) =>
        ZoneNames().Contains(zone) ? ZoneOf(zone) : Result.Fail(QueueError.NotFound, $"no zone {zone}");

    // The core's settings, drift, buffer and report age come only with its
    // `clients` answer; no event carries them. Asked on every read, so a room
    // that connected after the last status is not left with events alone. With
    // no core the model is answered as it stands.
    public async Task<IReadOnlyList<RoomView>> RoomsAsync(CancellationToken cancellationToken)
    {
        await link.SendAsync(new GetClients(), cancellationToken).ConfigureAwait(false);
        return [.. RoomNames().Select(Room)];
    }

    public async Task<Result<RoomView, CoreError>> RoomAsync(string client, CancellationToken cancellationToken)
    {
        await link.SendAsync(new GetClients(), cancellationToken).ConfigureAwait(false);
        return RoomNames().Contains(client) ? Room(client) : Result.Fail(CoreError.NotFound, $"no room {client}");
    }

    public RoomView Room(string client)
    {
        var settings = link.Desired.Rooms.GetValueOrDefault(client) ?? new DesiredRoom();
        var effective = link.Desired.Effective(client);
        var zone = link.Desired.ZoneOf(client);
        var volume = settings.Volume ?? (zone is not null && link.Desired.ZoneLevels.ContainsKey(zone) ? 1.0 : null);
        return new RoomView(client, NameOf(client),
            new RoomDesired(zone, settings.AudioControl, volume,
                settings.Muted, settings.OutputLatencyMs, effective.Volume, effective.Muted,
                settings.Equalizer, effective.Grouped, settings.MaxVolumeDb),
            link.Model.Current.Rooms.GetValueOrDefault(client), users.OfRoom(client));
    }

    // Phase 15: the zone's own volume and mute, recorded and sent to every
    // member as its effective pair. The zone must be one this server holds.
    // With no core connected it is recorded all the same and answered
    // notConnected, as a room's patch is.
    public async Task<Result<ZoneView, CoreError>> SetZoneLevelAsync(string zone, double? volume, bool? muted,
        CancellationToken cancellationToken)
    {
        if (volume is null && muted is null)
            return Result.Fail(CoreError.InvalidArgument, "a zone patch needs volume, muted, or both");
        if (volume is < 0.0 or > 1.0)
            return Result.Fail(CoreError.InvalidArgument,
                string.Create(CultureInfo.InvariantCulture, $"volume {volume} is outside 0.0..1.0"));
        if (!link.Desired.Zones.Any(z => z.Zone == zone))
            return Result.Fail(CoreError.NotFound, $"no zone {zone}");
        var set = await link.SetZoneLevelAsync(zone, volume, muted, cancellationToken).ConfigureAwait(false);
        ZoneLevelChanged?.Invoke(this, zone);
        return set.IsSuccess ? ZoneOf(zone) : set.Failure;
    }

    // Phase 26: a zone's AutoFill, for a zone any half knows of.
    public Result<AutoFillView, QueueError> AutoFill(string zone) =>
        ZoneNames().Contains(zone) ? autoFill.View(zone) : Result.Fail(QueueError.NotFound, $"no zone {zone}");

    public Result<AutoFillView, QueueError> SetAutoFill(string zone, AutoFillPatch patch)
    {
        if (!ZoneNames().Contains(zone))
            return Result.Fail(QueueError.NotFound, $"no zone {zone}");
        return autoFill.Set(zone, patch);
    }

    // Phase 36: who listens in a room, "" for the household; a name
    // UserStore does not know is refused by name. The room's zone follows
    // (ZoneUsers.Of).
    public Result<RoomView, CoreError> SetUser(string client, string user)
    {
        if (!RoomNames().Contains(client))
            return Result.Fail(CoreError.NotFound, $"no room {client}");
        var resolved = userStore.Resolve(user);
        if (!resolved.IsSuccess)
            return Result.Fail(resolved.Failure.Code == LibraryError.InvalidArgument
                ? CoreError.InvalidArgument
                : CoreError.Unavailable, resolved.Failure.Detail);
        users.Set(client, resolved.Value);
        return Room(client);
    }

    // A room's own volume and mute (the room patch's pair), answered as the room.
    public async Task<Result<RoomView, CoreError>> SetRoomLevelAsync(string client, double? volume, bool? muted,
        CancellationToken cancellationToken)
    {
        if (volume is null && muted is null)
            return Result.Fail(CoreError.InvalidArgument, "a room volume needs volume, muted, or both");
        if (volume is < 0.0 or > 1.0)
            return Result.Fail(CoreError.InvalidArgument,
                string.Create(CultureInfo.InvariantCulture, $"volume {volume} is outside 0.0..1.0"));
        var set = await link.SendAsync(new ClientSetVolume(client, volume, muted), cancellationToken).ConfigureAwait(false);
        return set.IsSuccess ? Room(client) : set.Failure;
    }

    // The room's level at full volume: -60..0 dB, 0 for no limit.
    public async Task<Result<RoomView, CoreError>> SetMaxVolumeDbAsync(string client, double maxVolumeDb,
        CancellationToken cancellationToken)
    {
        if (maxVolumeDb is < -60.0 or > 0.0)
            return Result.Fail(CoreError.InvalidArgument,
                string.Create(CultureInfo.InvariantCulture, $"maxVolumeDb {maxVolumeDb} is outside -60..0"));
        var set = await link.SetMaxVolumeDbAsync(client, maxVolumeDb, cancellationToken).ConfigureAwait(false);
        return set.IsSuccess ? Room(client) : set.Failure;
    }

    // Phase 39: renames the room, checked by MATP section 60's rule. The
    // client keeps the name, so the room must be connected; what it takes
    // comes back as room.name.
    public async Task<Result<RoomView, CoreError>> RenameAsync(string client, string name,
        CancellationToken cancellationToken)
    {
        if (!IsRoomName(name))
            return Result.Fail(CoreError.InvalidArgument,
                $"name \"{name}\" must be 1 to 64 bytes without control characters or surrounding blanks");
        var set = await link.SendAsync(new ClientSetName(client, name), cancellationToken).ConfigureAwait(false);
        return set.IsSuccess ? Room(client) : set.Failure;
    }

    // The room's whole equalizer, checked against the core's ranges first so
    // that one recorded while no core is connected is one the core will take.
    // Answered notConnected then, and asserted when the core is back.
    public async Task<Result<RoomView, CoreError>> SetEqualizerAsync(string client, CoreEqualizer equalizer,
        CancellationToken cancellationToken)
    {
        if (equalizer.Invalid() is { } invalid)
            return Result.Fail(CoreError.InvalidArgument, invalid);
        var set = await link.SendAsync(ClientSetEqualizer.For(client, equalizer), cancellationToken).ConfigureAwait(false);
        return set.IsSuccess ? Room(client) : set.Failure;
    }

    // A profile copied into the room and switched on: the room keeps the
    // copy, whatever happens to the profile afterwards.
    public Task<Result<RoomView, CoreError>> ApplyEqualizerProfileAsync(string client, string profile,
        CancellationToken cancellationToken) =>
        equalizerProfiles.Find(profile) is { } found
            ? SetEqualizerAsync(client, new CoreEqualizer(true, found.PreampDb, found.Bands), cancellationToken)
            : Task.FromResult<Result<RoomView, CoreError>>(
                Result.Fail(CoreError.NotFound, $"no equalizer profile {profile}"));

    // MATP section 60: 1 to 64 bytes of UTF-8, no control characters, no blank
    // at either end.
    public static bool IsRoomName(string name) =>
        name.Length > 0 && System.Text.Encoding.UTF8.GetByteCount(name) <= 64 &&
        name[0] != ' ' && name[^1] != ' ' && !name.Any(char.IsControl);

    // What people call the room: the core's name, the one kept, or its id.
    public string NameOf(string client) =>
        link.Model.Current.Rooms.GetValueOrDefault(client)?.Name
        ?? link.Desired.Rooms.GetValueOrDefault(client)?.Name
        ?? client;

    // Every room either half knows of, by client id.
    private bool IsActive(string client) => link.Model.Current.Rooms.GetValueOrDefault(client)?.Active != false;

    private SortedSet<string> RoomNames() =>
        new(link.Model.Current.Rooms.Keys.Concat(link.Desired.Rooms.Keys)
            .Concat(link.Desired.Zones.SelectMany(z => z.Clients ?? [])).Where(IsActive), StringComparer.Ordinal);

    private ZoneView ZoneOf(string zone)
    {
        var desired = link.Desired.Zones.FirstOrDefault(z => z.Zone == zone);
        var level = link.Desired.ZoneLevels.GetValueOrDefault(zone) ?? new DesiredZoneLevel();
        var queue = queues.GetQueue(zone);
        var state = queue.IsSuccess ? queue.Value : null;
        var title = StreamTitleOf(state);
        var model = link.Model.Current;
        var actual = model.Zones.GetValueOrDefault(zone);
        if (actual is not null)
            actual = actual with
            {
                Clients = [.. actual.Clients.Where(IsActive)],
                Members = [.. actual.Members.Where(IsActive)],
                Health = actual.Members.Where(IsActive)
                    .Select(c => (Client: c, model.Rooms.GetValueOrDefault(c)?.Health))
                    .Where(h => h.Health is not null)
                    .ToDictionary(h => h.Client, h => h.Health!, StringComparer.Ordinal),
            };
        var members = (desired.Clients ?? actual?.Clients ?? []).Where(IsActive).ToArray();
        var name = members.Length == 0 ? zone : string.Join(" + ", members.Select(NameOf));
        return new ZoneView(zone, name, desired.Zone is null ? null : new ZoneDesired(
            desired.Clients is null ? null : members, level.Volume, level.Muted),
            actual, state, title, RadioTrackMetadata.FromTitle(title),
            autoFill.View(zone), users.Of(zone));
    }

    private string? StreamTitleOf(QueueState? queue)
    {
        var station = queue?.Items.FirstOrDefault(i => i.Id == queue.Current)?.Station;
        if (station is null || queue!.Transport == QueueTransport.Stopped)
            return null;
        var radio = providers.Get<IStationTitles>(station.ProviderId);
        return radio.IsSuccess ? radio.Value.CurrentTitle(station.ProviderItemId) : null;
    }
}
