using System.Text.Json;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using Multiroom.App.Api;

namespace Multiroom.App;

public sealed partial class RoomsViewModel(Func<Func<MultiroomClient, CancellationToken, Task>, Task> execute) : ObservableObject
{
    [ObservableProperty] private IReadOnlyList<RoomView> _rooms = [];
    [ObservableProperty] private RoomView? _selectedRoom;
    [ObservableProperty] private IReadOnlyList<string> _zones = [];
    [ObservableProperty] private string? _targetZone;
    [ObservableProperty] private AudioControl? _audioControl;
    [ObservableProperty] private DriftCorrection? _driftCorrection;
    [ObservableProperty] private bool? _muted;
    [ObservableProperty] private decimal? _volume;
    [ObservableProperty] private decimal? _latency;
    [ObservableProperty] private string? _roomName;
    // Forget is two clicks: the first arms it, the second sends it.
    [ObservableProperty] private bool _confirmForget;
    [ObservableProperty] private string _diagnostics = "Refresh to read server diagnostics.";
    public IReadOnlyList<AudioControl> AudioControls { get; } = Enum.GetValues<AudioControl>();
    public IReadOnlyList<DriftCorrection> DriftCorrections { get; } = Enum.GetValues<DriftCorrection>();
    // Only a client that reports the setting has one; the server refuses the
    // hardware clock for a device without a verified trim.
    public bool HasDriftCorrection => ModeOf(SelectedRoom) is not null;
    public bool HasRoom => SelectedRoom is not null;
    // The client keeps its name, so only a connected room is renamed; only one
    // that is away can be forgotten.
    public bool CanRename => SelectedRoom?.Actual?.Connected == true;
    public bool CanForget => SelectedRoom is not null && SelectedRoom.Actual?.Connected != true;
    public string RoomId => SelectedRoom is { } r && r.DisplayName != r.Client ? $"Client id: {r.Client}" : "";
    public string ForgetLabel => ConfirmForget ? "Confirm: forget this room" : "Forget room";
    public string Desired => SelectedRoom is not { } r ? "Select a room." :
        $"Zone: {r.Desired.Zone ?? "Unassigned"}\nAudio control: {Show(r.Desired.AudioControl)}\n" +
        $"Volume: {Show(r.Desired.Volume)}\nMute: {Show(r.Desired.Muted)}\n" +
        $"Output latency (ms): {Show(r.Desired.OutputLatencyMs)}\n" +
        $"Effective volume: {Show(r.Desired.EffectiveVolume)}\nEffective mute: {Show(r.Desired.EffectiveMuted)}" +
        // A room sharing its zone is corrected unless it is Bit-Perfect.
        (r.Desired.Grouped
            ? "\n\nIn a zone with other rooms this room is drift corrected; " +
              "Bit-Perfect keeps it bitperfect and lets it drift."
            : "");
    public string Actual => SelectedRoom?.Actual is not { } a ? "No actual room state reported." :
        $"Connected: {a.Connected}\nZone: {Show(a.Zone)}\nMember: {a.Member}\nDevice: {Show(a.Device)}\n" +
        $"Bit-perfect: {Show(a.Plan?.BitPerfect)}{(a.Plan is { BitPerfect: true, DriftActuator: "hardwareClock" } ? " · hardware clock synchronized" : "")}\n" +
        $"Reason: {Show(a.Plan?.Reason)}\nDrift corrected: {Show(a.Plan?.DriftCorrected)}\n" +
        $"Drift correction: {Show(a.DriftCorrection)}\n" +
        $"Output format: {Show(a.Plan?.OutputFormat)}\nSource format: {Show(a.Plan?.SourceFormat)}\n" +
        $"Reported settings: {Show(a.Settings)}\nCapabilities: {Show(a.Capabilities)}\nAudio devices: {Show(a.AudioDevices)}\n" +
        $"Sync: {Show(a.Sync)}\nBuffered (ms): {Show(a.BufferedMs)}\nPlayback / underruns / report age: {Show(a.Playback)}\n" +
        $"Rejoins: {Show(a.Rejoins)}\nNot ready: {Show(a.NotReadyReason)}\nStart detail: {Show(a.StartDetail)}\n" +
        $"System: {Show(a.System)}\nSystem report age (ms): {Show(a.SystemAgeMs)}";
    private static string Show(object? value) => value is JsonElement json ? json.ValueKind switch
    {
        JsonValueKind.Null or JsonValueKind.Undefined => "Unknown",
        JsonValueKind.Object => string.Join(", ", json.EnumerateObject().Select(p => $"{p.Name}: {Show(p.Value)}")),
        JsonValueKind.Array => string.Join("; ", json.EnumerateArray().Select(v => Show(v))),
        _ => json.ToString()
    } : value?.ToString() ?? "Unknown";
    private static DriftCorrection? ModeOf(RoomView? room) =>
        room?.Actual?.DriftCorrection is { ValueKind: JsonValueKind.Object } dc && dc.TryGetProperty("mode", out var mode) &&
        Enum.TryParse<DriftCorrection>(mode.GetString(), true, out var parsed) ? parsed : null;
    partial void OnSelectedRoomChanged(RoomView? oldValue, RoomView? newValue)
    {
        OnPropertyChanged(nameof(HasRoom)); OnPropertyChanged(nameof(Desired)); OnPropertyChanged(nameof(Actual));
        OnPropertyChanged(nameof(HasDriftCorrection));
        OnPropertyChanged(nameof(CanRename)); OnPropertyChanged(nameof(CanForget)); OnPropertyChanged(nameof(RoomId));
        // Keep pending edits through unrelated SSE snapshots of the same room.
        if (oldValue?.Client == newValue?.Client) return;
        RoomName = newValue?.DisplayName; ConfirmForget = false;
        TargetZone = newValue?.Desired.Zone;
        AudioControl = Enum.TryParse<AudioControl>(newValue?.Desired.AudioControl, true, out var mode) ? mode : null;
        DriftCorrection = ModeOf(newValue);
        Muted = newValue?.Desired.Muted;
        Volume = newValue?.Desired.Volume is { } volume ? (decimal)volume * 100 : null;
        Latency = newValue?.Desired.OutputLatencyMs;
    }
    public void Apply(PlaybackSnapshot snapshot)
    {
        var client = SelectedRoom?.Client;
        Rooms = snapshot.Rooms; Zones = snapshot.Zones.Select(z => z.Zone).ToArray();
        SelectedRoom = Rooms.FirstOrDefault(r => r.Client == client) ?? Rooms.FirstOrDefault();
    }
    public void Reset() { Apply(new([], [])); Diagnostics = "Refresh to read server diagnostics."; }
    public Task RefreshAsync(bool diagnostics = false) => execute(async (api, ct) =>
    {
        var zones = await api.ZonesAsync(ct); var rooms = await api.RoomsAsync(ct);
        ct.ThrowIfCancellationRequested(); Apply(new(zones, rooms));
        if (!diagnostics) return;
        var status = await api.StatusAsync(ct); ct.ThrowIfCancellationRequested();
        Diagnostics = $"Server version: {status.Version}\nCore connected: {status.Core.Connected}\nCore version: {Show(status.Core.Version)}\n" +
            $"Core connections: {status.Core.Connections}\nRooms: {status.Core.Rooms} · Zones: {status.Core.Zones}\n" +
            $"Core queued events: {status.Core.QueuedEvents} · Dropped: {status.Core.DroppedEvents}\n\n" +
            "Providers\n" + string.Join("\n", status.Providers.Select(p => $"{p.DisplayName ?? p.Id} ({p.Id}): {p.State}")) +
            $"\n\nLibrary / indexer\n{Show(status.Library)}\n\nAdd-ins\n{Show(status.AddIns)}\n\n" +
            $"Events: {status.Events.Subscribers} subscribers · {status.Events.Held} held · {status.Events.Dropped} dropped\n\n" +
            $"Media reachability\n{Show(status.Media)}\n\n" +
            "Rooms\n" + string.Join("\n", rooms.Select(r => $"{r.DisplayName} ({r.Client}): desired zone {r.Desired.Zone ?? "unassigned"}, actual zone {r.Actual?.Zone ?? "unknown"}, connected {Show(r.Actual?.Connected)}")) +
            "\n\nZones\n" + string.Join("\n", zones.Select(z => $"{z.Zone}: {Show(z.Actual)}"));
    });
    [RelayCommand] private Task SaveSettingAsync(string setting) => execute(async (api, ct) =>
    {
        if (SelectedRoom is not { } room) return;
        RoomPatch patch = setting switch
        {
            "audioControl" when AudioControl is { } mode => new(AudioControl: mode),
            "driftCorrection" when DriftCorrection is { } drift => new(DriftCorrection: drift),
            "mute" when Muted is { } muted => new(Muted: muted),
            "volume" when Volume is >= 0 and <= 100 => new(Volume: (double)(Volume.Value / 100)),
            "latency" when Latency is >= 0 and <= ulong.MaxValue && decimal.Truncate(Latency.Value) == Latency.Value => new(OutputLatencyMs: (ulong)Latency.Value),
            "name" when !string.IsNullOrEmpty(RoomName) => new(Name: RoomName),
            _ => throw new ArgumentException("Choose a setting value. Volume must be 0–100, latency a whole non-negative number and the name not empty.")
        };
        // A refused multi-setting request may still change desired state; always re-read it.
        try { await api.PatchRoomAsync(room.Client, patch, ct); }
        finally { var rooms = await api.RoomsAsync(ct); ct.ThrowIfCancellationRequested(); Apply(new(Zones.Select(z => new ZoneView(z)).ToArray(), rooms)); }
    });
    partial void OnConfirmForgetChanged(bool value) => OnPropertyChanged(nameof(ForgetLabel));
    [RelayCommand] private Task ForgetAsync() => execute(async (api, ct) =>
    {
        if (SelectedRoom is not { } room || !CanForget) return;
        if (!ConfirmForget) { ConfirmForget = true; return; }
        ConfirmForget = false;
        await api.ForgetRoomAsync(room.Client, ct);
        var zones = await api.ZonesAsync(ct); var rooms = await api.RoomsAsync(ct);
        ct.ThrowIfCancellationRequested(); Apply(new(zones, rooms));
    });
    [RelayCommand] private Task RejoinAsync() => execute(async (api, ct) =>
    { if (SelectedRoom is { } r) { await api.RejoinRoomAsync(r.Client, ct); var rooms = await api.RoomsAsync(ct); ct.ThrowIfCancellationRequested(); Apply(new(Zones.Select(z => new ZoneView(z)).ToArray(), rooms)); } });
    [RelayCommand] private Task AssignAsync(string action) => execute(async (api, ct) =>
    {
        if (SelectedRoom is not { } room) return;
        var target = action == "remove" ? room.Desired.Zone : TargetZone;
        if (target is null) return;
        // Fetch a fresh full membership immediately before editing it.
        var zones = await api.ZonesAsync(ct);
        var zone = zones.FirstOrDefault(z => z.Zone == target) ?? throw new ArgumentException("The zone no longer exists. Refresh the page.");
        var clients = (zone.Desired?.Clients ?? []).Where(c => c != room.Client).ToList();
        if (action != "remove") clients.Add(room.Client);
        await api.SetClientsAsync(target, clients, ct);
        zones = await api.ZonesAsync(ct); var rooms = await api.RoomsAsync(ct);
        ct.ThrowIfCancellationRequested(); Apply(new(zones, rooms));
    });
}
