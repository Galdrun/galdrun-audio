using System.Globalization;
using System.Text.Json;
using System.Text.Json.Serialization;
using Multiroom.Server.Abstractions;

namespace Multiroom.Server.Playback.Core;

// Every request this server sends on 4713, one record per command. A record's
// properties are the command's `params`, serialized in camel case with absent
// optionals left out -- the exact field names the core's ControlApi accepts,
// because it refuses an unknown key by name. There is no dictionary anywhere
// near the socket.
//
// TReply is the answer's shape (CoreReplies.cs), so a caller gets it typed.
public abstract record CoreCommand
{
    private protected CoreCommand(string name) => Name = name;

    // The `command` of the request line, not one of its params.
    [JsonIgnore]
    public string Name { get; }

    internal abstract Type ReplyType { get; }
}

public abstract record CoreCommand<TReply> : CoreCommand
{
    private protected CoreCommand(string name)
        : base(name)
    {
    }

    internal override Type ReplyType => typeof(TReply);
}

public sealed record GetStatus() : CoreCommand<StatusReply>("status");

public sealed record GetClients() : CoreCommand<ClientsReply>("clients");

public sealed record GetZones() : CoreCommand<ZonesReply>("zones");

public sealed record ZoneCreate(string Zone) : CoreCommand<ZoneCreated>("zone.create");

public sealed record ZoneDelete(string Zone) : CoreCommand<ZoneDeleted>("zone.delete");

// The full membership, never a delta: the core replaces the list.
public sealed record ZoneSetClients(string Zone, IReadOnlyList<string> Clients)
    : CoreCommand<ZoneClientsSet>("zone.setClients");

// A sourceId names one staging of one zone, not a track: stage a track twice
// and it is two ids.
public sealed record SourceStage(
    string Zone,
    string SourceId,
    string MediaUri,
    string? TrackId = null,
    string? Title = null,
    ulong? StartOffsetNs = null) : CoreCommand<SourceStaged>("source.stage");

public sealed record SourceClearStaged(string Zone) : CoreCommand<StagedCleared>("source.clearStaged");

// Synchronous on the core's API thread: it answers once the first source is
// open, which takes GETs from this server's media endpoint.
public sealed record ZonePlay(string Zone) : CoreCommand<ZonePlayed>("zone.play");

public sealed record ZonePause(string Zone) : CoreCommand<ZonePaused>("zone.pause");

public sealed record ZoneStop(string Zone) : CoreCommand<ZoneStopped>("zone.stop");

public sealed record ZoneSeek(string Zone, ulong PositionNs) : CoreCommand<ZoneSought>("zone.seek");

// Whether the room shares its zone with another room. The core then corrects
// it whatever its own settings, unless it is bitperfect Force. Not desired state: it follows from the
// memberships, and is sent with them.
public sealed record ClientSetGrouped(string Client, bool Grouped)
    : CoreCommand<GroupedSet>("client.setGrouped");

public sealed record ClientSetAudioControl(string Client, AudioControl AudioControl)
    : CoreCommand<AudioControlSet>("client.setAudioControl");

// Not desired state: the client keeps its Volume Control in its own config
// (MATP section 59), and the core refuses Software while the room's Audio
// Control is BitPerfect. The client's VOLUME_CONFIG shows in the next
// `clients` as the room's volumeControl, with the path it resolved.
public sealed record ClientSetVolumeControl(string Client, VolumeControl VolumeControl)
    : CoreCommand<VolumeControlSet>("client.setVolumeControl");

// Not desired state either: the client keeps the actuator its drift is
// corrected through, and the core
// refuses HardwareClock for a device without a verified clock trim. The
// client answers with new capabilities, which renegotiate its stream.
public sealed record ClientSetDriftCorrection(string Client, DriftCorrection DriftCorrection)
    : CoreCommand<DriftCorrectionSet>("client.setDriftCorrection");

// Value and mute are independent; at least one of the two is required.
public sealed record ClientSetVolume(
    string Client,
    double? Value = null,
    bool? Muted = null,
    ulong? ApplyAtTimelineNs = null) : CoreCommand<VolumeSet>("client.setVolume");

public sealed record ClientRejoin(string Client) : CoreCommand<ClientRejoined>("client.rejoin");

// A room in a deep sleep the network can end (powerState "sleep"): one
// Wake-on-LAN magic packet to the address it reported before it went away.
// WakeOnLan is this server's copy of it, for a core that restarted since.
public sealed record ClientWake(string Client, string? WakeOnLan = null) : CoreCommand<ClientWoken>("client.wake");

public sealed record ClientSetOutputLatency(string Client, ulong LatencyMs)
    : CoreCommand<OutputLatencySet>("client.setOutputLatency");

// The room's whole equalizer every time (the core's phase 44). The core runs
// it on a resampled room only; a bitperfect stream is never touched.
public sealed record ClientSetEqualizer(string Client, bool Enabled, double PreampDb, IReadOnlyList<CoreEqBand> Bands)
    : CoreCommand<EqualizerSet>("client.setEqualizer")
{
    public static ClientSetEqualizer For(string client, CoreEqualizer equalizer) =>
        new(client, equalizer.Enabled, equalizer.PreampDb, equalizer.Bands);

    [JsonIgnore]
    public CoreEqualizer Equalizer => new(Enabled, PreampDb, Bands);
}

// Not desired state: the client keeps the choice in its own config. An empty
// DeviceId is the client's system default.
public sealed record ClientSetAudioDevice(string Client, string DeviceId)
    : CoreCommand<AudioDeviceSet>("client.setAudioDevice");

// Not desired state either: the client keeps its buffer size in its own config
// (MATP section 58) and refuses, never clamps, a value it cannot use; its
// BUFFER_CONFIG shows in the next `clients` as the room's bufferMs.
public sealed record ClientSetBuffer(string Client, uint BufferMs)
    : CoreCommand<BufferSet>("client.setBuffer");

// Not desired state: the client keeps its name (MATP section 60). The core
// refuses it for a room that is not connected or has never said a name. The
// param is `name`; Name here is the command's own.
public sealed record ClientSetName(string Client, [property: JsonPropertyName("name")] string RoomName)
    : CoreCommand<NameSet>("client.setName");

// A room that is not connected, gone from the core: out of its zone, its
// settings and its record dropped (the core's phase 43).
public sealed record ClientForget(string Client) : CoreCommand<ClientForgotten>("client.forget");

// The core's audio section as its config holds it, and the keys of it that
// audio.configure changes on a running core.
public sealed record GetAudioSettings() : CoreCommand<AudioSettingsReply>("audio.settings");

// The timings and the memory limit: the barrier, the ready timeout, the lead
// of every START_AT, PAUSE_AT and STOP_AT, a seek's two leads, and the one
// ceiling over every zone's audio memory (phase 42). The core checks them with
// its config's own rules, writes them to its config file and uses them from
// the next message on; at least one is required.
public sealed record AudioConfigure(
    uint? MinimumStartBufferMs = null,
    uint? ClientReadyTimeoutMs = null,
    uint? StartDelayMs = null,
    uint? SeekFlushLeadMs = null,
    uint? SeekFillLeadMs = null,
    uint? MemoryLimitMb = null) : CoreCommand<AudioSettingsReply>("audio.configure");

// Answered before the client's scan; its new list arrives with the next `clients`.
public sealed record ClientRescanAudio(string Client) : CoreCommand<AudioRescanRequested>("client.rescanAudio");

// Not desired state either: the client keeps its bindings in its own config.
// A gesture Bindings leaves out keeps what it had.
public sealed record ClientSetInputBindings(string Client, string InputId,
    IReadOnlyDictionary<string, CoreInputBinding> Bindings)
    : CoreCommand<InputBindingsSet>("client.setInputBindings");

// The clients the core heard announce themselves on its network (their UDP
// beacon), attached to it or not. This server never addresses a client: the
// core points one at itself with client.attach, or at nothing with
// client.detach, through the client's own control API.
public sealed record GetDiscoveredClients() : CoreCommand<DiscoveredClientsReply>("discovery.clients");

// Server is the MATP address ("host:port") the client is to use; without it
// the core's own address on the client's network.
public sealed record ClientAttach(string Client, string? Server = null)
    : CoreCommand<ClientPointed>("client.attach");

public sealed record ClientDetach(string Client) : CoreCommand<ClientPointed>("client.detach");

public sealed record ClientGetNetwork(string Client) : CoreCommand<ClientNetworkSettings>("client.getNetwork");
public sealed record ClientSetEthernetSpeed(string Client, string SpeedMbps)
    : CoreCommand<ClientNetworkSettings>("client.setEthernetSpeed");
public sealed record ClientGetLeds(string Client) : CoreCommand<ClientLedSettings>("client.getLeds");
public sealed record ClientSetLeds(string Client, bool Off) : CoreCommand<ClientLedSettings>("client.setLeds");

// The client's idle delays (0 is off; deep sleep 60 s at least) and its own
// power commands: standbyOn, standbyOff, toggleStandby or deepSleep.
public sealed record ClientGetPower(string Client) : CoreCommand<ClientPowerSettings>("client.getPower");
public sealed record ClientSetPower(string Client, uint? StandbyAfterSeconds = null, uint? DeepSleepAfterSeconds = null)
    : CoreCommand<ClientPowerSettings>("client.setPower");
public sealed record ClientPower(string Client, string Action) : CoreCommand<ClientPowerSettings>("client.power");

public sealed record ResamplerConfigure(string Resampler)
    : CoreCommand<ResamplerConfigured>("resampler.configure");

public sealed record EventsSubscribe() : CoreCommand<Subscription>("events.subscribe");

public sealed record EventsUnsubscribe() : CoreCommand<Subscription>("events.unsubscribe");

// A room's Audio Control (MATP section 59): BitPerfect is strict -- never
// resampled, not even in a zone shared with other rooms, and no software
// volume either; Auto is bitperfect where possible, and resampled and drift
// corrected where the device cannot play the source or the zone is shared.
public enum AudioControl
{
    BitPerfect,
    Auto,
}

// AudioControl as its camel-case name, reading "bitPerfectFallback", the old
// name for auto, from a playback.json or a core that predates the merge. Put
// before the general enum converter in every options that reads one.
public sealed class AudioControlJsonConverter : JsonConverter<AudioControl>
{
    public override AudioControl Read(ref Utf8JsonReader reader, Type typeToConvert, JsonSerializerOptions options) =>
        reader.TokenType == JsonTokenType.String ? reader.GetString() switch
        {
            "bitPerfect" => AudioControl.BitPerfect,
            "auto" or "bitPerfectFallback" => AudioControl.Auto,
            var other => throw new JsonException($"audioControl {other} is not bitPerfect or auto"),
        } : throw new JsonException("audioControl must be a string");

    public override void Write(Utf8JsonWriter writer, AudioControl value, JsonSerializerOptions options) =>
        writer.WriteStringValue(value == AudioControl.BitPerfect ? "bitPerfect" : "auto");
}

// How a room's client plays its volume (MATP section 59). Auto resolves from
// the Audio Control and the device: hardware where there is a control, else
// software -- or Disabled under BitPerfect. Disabled plays the mute alone.
public enum VolumeControl
{
    Auto,
    Hardware,
    Software,
    Disabled,
}

// How a room's drift is corrected when it is: the core resamples its stream,
// or the client trims its DAC clock on the core's command, which leaves the
// samples -- and so a bitperfect room -- untouched. It chooses the actuator,
// not whether the room is synchronized. Auto is the hardware clock where the
// device has a verified trim, resampling otherwise; the client's default.
public enum DriftCorrection
{
    Resampling,
    HardwareClock,
    Auto,
}

// One filter of a room's equalizer; GainDb is not used by HighPass and LowPass.
public enum EqFilterType
{
    Peak,
    LowShelf,
    HighShelf,
    HighPass,
    LowPass,
}

public sealed record CoreEqBand(EqFilterType Type, double FrequencyHz, double Q, double GainDb = 0.0);

// A room's equalizer as the core takes and reports it, and as this server
// keeps it. Compared by value, bands in order.
// The ranges are the core's (processing/Equalizer.h), checked here too so
// that a setting recorded while no core is connected is one it will take.
public sealed record CoreEqualizer(bool Enabled, double PreampDb, IReadOnlyList<CoreEqBand> Bands)
{
    public const int MaxBands = 20;

    public bool Equals(CoreEqualizer? other) =>
        other is not null && Enabled == other.Enabled && PreampDb.Equals(other.PreampDb) &&
        Bands.SequenceEqual(other.Bands);

    public override int GetHashCode() => HashCode.Combine(Enabled, PreampDb, Bands.Count);

    // Null when every value is in range, otherwise what is not.
    public string? Invalid()
    {
        if (!(PreampDb is >= -30.0 and <= 12.0))
            return string.Create(CultureInfo.InvariantCulture, $"preampDb {PreampDb} is outside -30..12");
        if (Bands.Count > MaxBands)
            return $"{Bands.Count} bands, at most {MaxBands}";
        for (var i = 0; i < Bands.Count; i++)
        {
            var band = Bands[i];
            if (!(band.FrequencyHz is >= 10.0 and <= 24000.0))
                return string.Create(CultureInfo.InvariantCulture, $"band {i}: frequencyHz {band.FrequencyHz} is outside 10..24000");
            if (!(band.GainDb is >= -24.0 and <= 24.0))
                return string.Create(CultureInfo.InvariantCulture, $"band {i}: gainDb {band.GainDb} is outside -24..24");
            if (!(band.Q is >= 0.1 and <= 10.0))
                return string.Create(CultureInfo.InvariantCulture, $"band {i}: q {band.Q} is outside 0.1..10");
        }
        return null;
    }
}
