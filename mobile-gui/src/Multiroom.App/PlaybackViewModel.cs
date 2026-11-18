using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;
using System.Text.Json;
using CommunityToolkit.Mvvm.Input;
using Multiroom.App.Api;

namespace Multiroom.App;

public sealed partial class PlaybackViewModel : ObservableObject
{
    private readonly Func<Func<MultiroomClient, CancellationToken, Task>, Task> _execute;
    private PlaybackSnapshot _snapshot = new([], []);
    private string? _selectWhenAvailable;
    private readonly LatestValue _zoneVolumeSend;
    private string? _zoneVolumeTarget;
    private bool _applyingZoneVolume;
    private readonly TimeProvider _time;
    private long _positionAt;
    private readonly Dictionary<string, long> _positionSamples = [];
    private bool _scrubbing;
    private string? _scrubZone;
    private long? _scrubItem;
    private ItemRef? _markItem;
    private ItemStatus? _marks;
    private CancellationTokenSource? _markRead;
    private int _markVersion;
    private readonly Dictionary<string, (DateTimeOffset End, long Sampled, int Seconds)> _sleepDeadlines = [];
    public string? PreferredZone { get; set; }
    public PlaybackViewModel(Func<Func<MultiroomClient, CancellationToken, Task>, Task> execute, TimeProvider? time = null)
    { _execute = execute; _time = time ?? TimeProvider.System; _zoneVolumeSend = new(execute, () => { if (_zoneVolumeTarget == SelectedZone) Update(); }); }
    public ObservableCollection<QueueRow> Items { get; } = [];
    [ObservableProperty] private IReadOnlyList<string> _zones = [];
    [ObservableProperty] private string? _selectedZone;
    // The zones as people call them, in Zones' order; a picker selects by Zone (the id).
    [ObservableProperty] private IReadOnlyList<ZoneChoice> _zoneChoices = [];
    [ObservableProperty] private QueueRow? _selectedItem;
    [ObservableProperty] private string _newZone = "";
    [ObservableProperty] private string _title = "Nothing playing";
    [ObservableProperty] private string _subtitle = "Choose music to fill the room";
    [ObservableProperty] private string _transport = "Stopped";
    [ObservableProperty] private string _repeat = "Off";
    [ObservableProperty] private bool _shuffle;
    [ObservableProperty] private bool _canSeek;
    [ObservableProperty] private decimal _seekSeconds;
    [ObservableProperty] private string _duration = "";
    [ObservableProperty] private IReadOnlyList<RoomView> _rooms = [];
    [ObservableProperty] private ArtworkCache? _artwork;
    // The volume page: the selected zone's volume in percent, and each of its rooms'.
    [ObservableProperty] private double _zoneVolume;
    public ObservableCollection<RoomVolume> RoomVolumes { get; } = [];
    [ObservableProperty, NotifyPropertyChangedFor(nameof(PositionText))] private double _progressSeconds;
    public double TrackLengthSeconds => Math.Max(0, Current?.Track?.Duration?.TotalSeconds ?? 0);
    public double ProgressMaximum => Math.Max(1, TrackLengthSeconds);
    public bool CanScrub => CanSeekCurrent && TrackLengthSeconds > 0;
    public bool HasPosition => Queue?.PositionMs is >= 0;
    public bool IsScrubbing => _scrubbing;
    public string PositionText => _scrubbing || HasPosition ? FormatPosition(ProgressSeconds) : "—:—";
    public string LengthText => Current?.Station is not null ? "LIVE" : TrackLengthSeconds > 0 ? FormatPosition(TrackLengthSeconds) : "—:—";
    public string QueueCount => Items.Count == 1 ? "1 track" : $"{Items.Count} tracks";
    public string QueuePosition => Queue?.Current is { } id && Queue.Items.ToList().FindIndex(item => item.Id == id) is var index && index >= 0
        ? $"{index + 1} / {Queue.Items.Count}" : "";
    private static string FormatPosition(double seconds) => TimeSpan.FromSeconds(Math.Max(0, seconds)).ToString(seconds >= 3600 ? @"h\:mm\:ss" : @"m\:ss");
    public bool IsPlaying => Queue?.Transport is QueueTransport.Playing or QueueTransport.Starting;
    public string PlayPauseLabel => IsPlaying ? "Pause" : "Play";
    public string? ZoneName => _snapshot.Zones.FirstOrDefault(z => z.Zone == SelectedZone)?.DisplayName ?? SelectedZone;
    public string ZoneNameOf(string zone) => _snapshot.Zones.FirstOrDefault(z => z.Zone == zone)?.DisplayName ?? zone;
    public string ZonePosition => $"Zone {Zones.ToList().IndexOf(SelectedZone ?? "") + 1} of {Zones.Count}: {ZoneName}";
    public IReadOnlyList<bool> ZoneIndicators => Zones.Select(zone => zone == SelectedZone).ToArray();
    public ZoneDesired? ZoneSettings => _snapshot.Zones.FirstOrDefault(z => z.Zone == SelectedZone)?.Desired;
    public bool ZoneMuted => ZoneSettings?.Muted == true;
    public string ZoneMuteLabel => ZoneMuted ? "Unmute zone" : "Mute zone";
    public bool CanSetZoneVolume => HasZone && ZoneSettings?.Volume is not null;
    public bool CanMuteZone => HasZone && ZoneSettings?.Muted is not null;
    public ObservableCollection<RoomChoice> Membership { get; } = [];
    public string? CurrentArtwork => Current?.Item.ArtworkId;
    public bool CanSeekCurrent => IsSeekCandidate(Current);
    public ItemRef? CurrentMarkItem => Current?.Track is { } track ? new(LikeKind.Track, track.ProviderId, track.ProviderItemId)
        : Current?.Station is { } station ? new(LikeKind.Station, station.ProviderId, station.ProviderItemId) : null;
    public bool CanFavoriteCurrent => CurrentMarkItem is not null && _marks is not null;
    public bool FavoriteSaved => _marks?.Favorite == true;
    public string FavoriteLabel => FavoriteSaved ? "Remove from favourites" : "Add to favourites";
    private ItemStatus? _likeMarks;
    private ItemRef? _likeItem;
    public Track? CurrentLikeTrack => Current?.Track ?? (Current?.Station is not null
        ? _snapshot.Zones.FirstOrDefault(z => z.Zone == SelectedZone)?.RadioTrack : null);
    public ItemRef? CurrentLikeItem => CurrentLikeTrack is { } track ? new(LikeKind.Track, track.ProviderId, track.ProviderItemId) : null;
    public bool CanLikeCurrent => CurrentLikeItem is not null && _likeMarks is not null;
    public bool Liked => _likeMarks?.Liked == true;
    public string LikeLabel => Liked ? "Unlike" : "I like it";
    public bool IsRepeatEnabled => Queue?.Repeat is RepeatMode.All or RepeatMode.One;
    public bool IsRepeatOne => Queue?.Repeat == RepeatMode.One;
    public bool HasZone => SelectedZone is not null;
    public bool IsEmpty => Items.Count == 0;
    public QueueState? Queue => _snapshot.Zones.FirstOrDefault(z => z.Zone == SelectedZone)?.Queue;
    public QueueItem? Current => Queue?.Items.FirstOrDefault(i => i.Id == Queue.Current);
    public static bool IsSeekCandidate(QueueItem? item) => item?.Track is not null && item.Station is null;

    public void Apply(PlaybackSnapshot snapshot)
    {
        foreach (var zone in snapshot.Zones)
            if (!ReferenceEquals(zone.Queue, _snapshot.Zones.FirstOrDefault(previous => previous.Zone == zone.Zone)?.Queue))
                _positionSamples[zone.Zone] = _time.GetTimestamp();
        foreach (var removed in _positionSamples.Keys.Where(name => !snapshot.Zones.Any(zone => zone.Zone == name)).ToArray())
            _positionSamples.Remove(removed);
        // Each running sleep timer's deadline on this device's clock, taken when its end is first seen.
        foreach (var zone in snapshot.Zones)
            if (zone.SleepTimer is { Active: true, EndsAt: { } end })
            { if (_sleepDeadlines.GetValueOrDefault(zone.Zone).End != end) _sleepDeadlines[zone.Zone] = (end, _time.GetTimestamp(), zone.SleepTimer.RemainingSeconds); }
            else _sleepDeadlines.Remove(zone.Zone);
        foreach (var removed in _sleepDeadlines.Keys.Where(name => !snapshot.Zones.Any(zone => zone.Zone == name)).ToArray())
            _sleepDeadlines.Remove(removed);
        _snapshot = snapshot;
        var names =snapshot.Zones.Select(z => z.Zone).ToArray();
        if (!Zones.SequenceEqual(names)) Zones = names;
        var choices = snapshot.Zones.Select(z => new ZoneChoice(z.Zone, z.DisplayName)).ToArray();
        if (!ZoneChoices.SequenceEqual(choices)) ZoneChoices = choices;
        if (_selectWhenAvailable is { } desired && Zones.Contains(desired)) { SelectedZone = desired; _selectWhenAvailable = null; }
        else if (SelectedZone is null || !Zones.Contains(SelectedZone))
            SelectedZone = PreferredZone is { } preferred && Zones.Contains(preferred) ? preferred : Zones.FirstOrDefault();
        if (SelectedZone is not null) PreferredZone = SelectedZone;
        Update();
    }

    partial void OnSelectedZoneChanged(string? value)
    { SelectedItem = null; Membership.Clear(); Update(); OnPropertyChanged(nameof(HasZone)); }
    // Moved by hand: sent as it moves, the newest value once the one before is answered.
    partial void OnZoneVolumeChanged(double value)
    {
        if (_applyingZoneVolume || SelectedZone is null) return;
        _zoneVolumeTarget = SelectedZone;
        _ = SetZoneVolumeAsync();
    }
    partial void OnSelectedItemChanged(QueueRow? value) => CanSeek = IsSeekCandidate(value?.Item ?? Current);

    private void Update()
    {
        _positionAt = _positionSamples.GetValueOrDefault(SelectedZone ?? "");
        if (_scrubbing && (_scrubZone != SelectedZone || _scrubItem != Current?.Id)) CancelScrub();
        var zone = _snapshot.Zones.FirstOrDefault(z => z.Zone == SelectedZone);
        var queue = zone?.Queue;
        var item = Current;
        if (_markItem != CurrentMarkItem || _likeItem != CurrentLikeItem)
        {
            _markItem = CurrentMarkItem; _likeItem = CurrentLikeItem; CancelMarks();
            OnPropertyChanged(nameof(CurrentMarkItem));
        }
        Title = item?.Track?.Title ?? item?.Station?.Name ?? "Nothing playing";
        Subtitle = zone?.StreamTitle ?? (item?.Track is { } track ? string.Join(" · ", new[] { track.Artist, track.Album }.Where(s => !string.IsNullOrEmpty(s))) : "Choose music to fill the room");
        Transport = queue?.Transport.ToString() ?? "Stopped";
        Repeat = queue?.Repeat.ToString() ?? "Off";
        Shuffle = queue?.Shuffle ?? false;
        // Not while a moved volume is still being sent: the dial would jump back to the value before.
        if (!_zoneVolumeSend.Running)
        {
            _applyingZoneVolume = true;
            ZoneVolume = Math.Round((zone?.Desired?.Volume ?? 0) * 100);
            _applyingZoneVolume = false;
        }
        Duration = item?.Track?.Duration is { } length ? length.ToString(@"h\:mm\:ss") : item?.Station is not null ? "LIVE" : "";
        var selectedId = SelectedItem?.Item.Id;
        var desired = (queue?.Items ?? []).Select((q, i) => new QueueRow(q, q.Id == queue?.Current, i + 1)).ToArray();
        // Only replace rows which actually changed, preserving scrolling and selection on unrelated events.
        for (var i = 0; i < desired.Length; i++)
        {
            if (i >= Items.Count) Items.Add(desired[i]);
            else if (Items[i] != desired[i]) Items[i] = desired[i];
        }
        while (Items.Count > desired.Length) Items.RemoveAt(Items.Count - 1);
        SelectedItem = Items.FirstOrDefault(r => r.Item.Id == selectedId);
        CanSeek = IsSeekCandidate(SelectedItem?.Item ?? Current);
        Rooms = _snapshot.Rooms.Where(r => r.Desired.Zone == SelectedZone || r.Actual?.Zone == SelectedZone).ToArray();
        var volumes = Rooms.Select(r => RoomVolumes.FirstOrDefault(v => v.Client == r.Client) ?? NewRoomVolume(r.Client)).ToArray();
        for (var i = 0; i < volumes.Length; i++) volumes[i].Apply(Rooms[i]);
        if (!RoomVolumes.SequenceEqual(volumes)) { RoomVolumes.Clear(); foreach (var row in volumes) RoomVolumes.Add(row); }
        foreach (var room in _snapshot.Rooms)
        {
            var choice = Membership.FirstOrDefault(c => c.Client == room.Client);
            if (choice is null) Membership.Add(NewRoomChoice(room));
            else if (choice.Name != room.DisplayName) choice.Name = room.DisplayName;
            else if (!choice.Dirty) choice.Reset(room.Desired.Zone == SelectedZone);
        }
        for (var i = Membership.Count - 1; i >= 0; i--)
            if (!_snapshot.Rooms.Any(r => r.Client == Membership[i].Client)) Membership.RemoveAt(i);
        OnPropertyChanged(nameof(IsEmpty)); OnPropertyChanged(nameof(HasZone));
        OnPropertyChanged(nameof(CurrentArtwork));
        OnPropertyChanged(nameof(CanSeekCurrent));
        foreach (var property in new[] { nameof(IsPlaying), nameof(PlayPauseLabel), nameof(ZoneName), nameof(ZonePosition), nameof(ZoneIndicators),
            nameof(ZoneMuted), nameof(ZoneMuteLabel), nameof(CanSetZoneVolume), nameof(CanMuteZone),
            nameof(TrackLengthSeconds), nameof(ProgressMaximum), nameof(CanScrub), nameof(HasPosition),
            nameof(PositionText), nameof(LengthText), nameof(QueuePosition), nameof(QueueCount), nameof(IsRepeatEnabled), nameof(IsRepeatOne) }) OnPropertyChanged(property);
        RefreshProgress();
    }

    private RoomChoice NewRoomChoice(RoomView room)
    {
        var choice = new RoomChoice(room.Client, room.Desired.Zone == SelectedZone) { Name = room.DisplayName };
        if (Artwork is { } pictures) _ = choice.LoadPictureAsync(pictures);
        return choice;
    }
    private RoomVolume NewRoomVolume(string client)
    {
        var row = new RoomVolume(client, _execute);
        if (Artwork is { } pictures) _ = row.LoadPictureAsync(pictures);
        return row;
    }

    // The selected zone's sleep timer: minutes left (0: off), counted down here from the seconds the server
    // answered, so the clocks need not agree.
    public int SleepMinutesLeft { get; private set; }
    public bool IsSleepActive => SleepMinutesLeft > 0;
    public string SleepLabel => IsSleepActive ? $"Sleep timer: stops in {SleepMinutesLeft} min (tap: 10 minutes less, hold: off)"
        : "Sleep timer off (tap: 60 minutes)";
    private void RefreshSleep()
    {
        var left = SelectedZone is { } zone && _sleepDeadlines.TryGetValue(zone, out var timer)
            ? (int)Math.Max(0, Math.Ceiling((timer.Seconds - _time.GetElapsedTime(timer.Sampled).TotalSeconds) / 60)) : 0;
        if (left == SleepMinutesLeft) return;
        SleepMinutesLeft = left;
        foreach (var property in new[] { nameof(SleepMinutesLeft), nameof(IsSleepActive), nameof(SleepLabel) }) OnPropertyChanged(property);
    }
    // A tap starts it at 60 minutes; each further tap takes 10 off what is left (57 -> 50), down to off.
    [RelayCommand] private Task Sleep() => SetSleepAsync(IsSleepActive ? (SleepMinutesLeft + 9) / 10 * 10 - 10 : 60);
    [RelayCommand] private Task SleepOff() => IsSleepActive ? SetSleepAsync(0) : Task.CompletedTask;
    private Task SetSleepAsync(int minutes)
    {
        var zone = SelectedZone;
        return _execute(async (api, ct) =>
        {
            if (zone is null) return;
            var timer = await api.SetSleepTimerAsync(zone, minutes, ct);
            ct.ThrowIfCancellationRequested();
            Apply(_snapshot with { Zones = _snapshot.Zones.Select(z => z.Zone == zone ? z with { SleepTimer = timer } : z).ToArray() });
        });
    }

    // The zone plays while none of its devices is connected and one sleeps with Wake-on-LAN: the core woke it on
    // play and it rejoins on its own, but nothing is heard until then. Shown at most WakeTimeout, in case it
    // never comes back.
    public static readonly TimeSpan WakeTimeout = TimeSpan.FromSeconds(90);
    private string? _wakingZone;
    private long _wakingSince;
    // The last text stays while the pill fades out.
    public string? WakingText { get; private set; }
    public bool IsWaking { get; private set; }
    private void RefreshWaking()
    {
        var rooms = _snapshot.Rooms.Where(r => r.Desired.Zone == SelectedZone).ToArray();
        var asleep = rooms.Where(r => r.Actual is { Connected: false, PowerState: "sleep" }).ToArray();
        var waiting = SelectedZone is not null && IsPlaying && asleep.Length > 0 && !rooms.Any(r => r.Actual?.Connected == true);
        if (!waiting) _wakingZone = null;
        else if (_wakingZone != SelectedZone) { _wakingZone = SelectedZone; _wakingSince = _time.GetTimestamp(); }
        var text = waiting && _time.GetElapsedTime(_wakingSince) < WakeTimeout
            ? $"Waking up {string.Join(", ", asleep.Select(r => r.DisplayName))}…" : null;
        if (text is not null && text != WakingText) { WakingText = text; OnPropertyChanged(nameof(WakingText)); }
        if ((text is not null) == IsWaking) return;
        IsWaking = text is not null; OnPropertyChanged(nameof(IsWaking));
    }

    public void RefreshProgress()
    {
        RefreshSleep(); RefreshWaking();
        if (_scrubbing) return;
        var seconds = HasPosition ? Queue!.PositionMs!.Value / 1000d : 0;
        if (HasPosition && Queue?.Transport == QueueTransport.Playing)
            seconds += _time.GetElapsedTime(_positionAt).TotalSeconds;
        ProgressSeconds = TrackLengthSeconds > 0 ? Math.Clamp(seconds, 0, TrackLengthSeconds) : Math.Max(0, seconds);
    }

    public void BeginScrub()
    {
        if (!CanScrub || CommitScrubCommand.IsRunning) return;
        _scrubbing = true; _scrubZone = SelectedZone; _scrubItem = Current?.Id;
        OnPropertyChanged(nameof(PositionText)); OnPropertyChanged(nameof(IsScrubbing));
    }

    public void CancelScrub()
    {
        _scrubbing = false; _scrubZone = null; _scrubItem = null;
        RefreshProgress(); OnPropertyChanged(nameof(PositionText)); OnPropertyChanged(nameof(IsScrubbing));
    }

    [RelayCommand]
    private async Task CommitScrubAsync()
    {
        if (!_scrubbing || _scrubZone != SelectedZone || _scrubItem != Current?.Id || !CanScrub) { CancelScrub(); return; }
        var zone = _scrubZone!; var item = _scrubItem!.Value;
        var positionMs = checked((long)(Math.Clamp(ProgressSeconds, 0, TrackLengthSeconds) * 1000));
        try
        {
            await _execute(async (api, ct) =>
            {
                var response = await api.SeekAsync(zone, item, positionMs, ct);
                QueueState queue;
                try { queue = response?.Deserialize<QueueState>(MultiroomClient.Json) ?? throw new JsonException(); }
                catch (JsonException) { throw new ApiException(System.Net.HttpStatusCode.OK, "invalidResponse", "The server returned an unexpected response."); }
                ct.ThrowIfCancellationRequested();
                if (SelectedZone == zone && Current?.Id == item && queue.Zone == zone)
                    Apply(_snapshot with { Zones = _snapshot.Zones.Select(z => z.Zone == zone ? z with { Queue = queue } : z).ToArray() });
            });
        }
        finally { CancelScrub(); }
    }

    private void SetMarks(ItemStatus? marks, ItemStatus? likeMarks = null)
    {
        _marks = marks;
        _likeMarks = likeMarks ?? (CurrentLikeItem == CurrentMarkItem ? marks : null);
        foreach (var property in new[] { nameof(CanFavoriteCurrent), nameof(FavoriteSaved), nameof(FavoriteLabel),
            nameof(CanLikeCurrent), nameof(Liked), nameof(LikeLabel) }) OnPropertyChanged(property);
        FavoriteCurrentCommand.NotifyCanExecuteChanged(); LikeCurrentCommand.NotifyCanExecuteChanged();
    }

    public void CancelMarks()
    {
        _markVersion++; _markRead?.Cancel(); _markRead?.Dispose(); _markRead = null;
        SetMarks(null);
    }

    public async Task RefreshMarksAsync(MultiroomClient api, CancellationToken ct)
    {
        _markRead?.Cancel(); _markRead?.Dispose(); _markRead = null;
        var version = ++_markVersion; var item = CurrentMarkItem; var likeItem = CurrentLikeItem;
        if (item is null) return;
        using var read = CancellationTokenSource.CreateLinkedTokenSource(ct); _markRead = read;
        var token = read.Token;
        try
        {
            var items = likeItem is not null && likeItem != item ? new[] { item, likeItem } : new[] { item };
            var statuses = await api.ItemStatusesAsync(items, token);
            token.ThrowIfCancellationRequested();
            if (version != _markVersion || item != CurrentMarkItem || likeItem != CurrentLikeItem) return;
            if (statuses.Count != items.Length || statuses.Where((status, index) => status is null || status.Kind != items[index].Kind ||
                status.ProviderId != items[index].ProviderId || status.ProviderItemId != items[index].ProviderItemId ||
                (status.Liked && status.LikeId is not > 0) || (status.Favorite && status.FavoriteId is not > 0)).Any())
                throw new ApiException(System.Net.HttpStatusCode.OK, "invalidResponse", "The server returned an unexpected response.");
            SetMarks(statuses[0], likeItem is null ? null : statuses[^1]);
        }
        catch (OperationCanceledException) when (token.IsCancellationRequested && !ct.IsCancellationRequested) { }
        finally { if (ReferenceEquals(_markRead, read)) _markRead = null; }
    }

    [RelayCommand(CanExecute = nameof(CanFavoriteCurrent))]
    private Task FavoriteCurrentAsync() => ChangeMarkAsync(favorite: true);

    [RelayCommand(CanExecute = nameof(CanLikeCurrent))]
    private Task LikeCurrentAsync() => ChangeMarkAsync(favorite: false);

    private Task ChangeMarkAsync(bool favorite)
    {
        var track = Current?.Track; var station = Current?.Station; var likeTrack = CurrentLikeTrack;
        var item = CurrentMarkItem; var likeItem = CurrentLikeItem; var marks = favorite ? _marks : _likeMarks;
        if (item is null || marks is null) return Task.CompletedTask;
        return _execute(async (api, ct) =>
        {
            if (favorite)
            {
                if (marks.Favorite) await api.RemoveFavoriteAsync(marks.FavoriteId!.Value, ct);
                else await api.AddFavoriteAsync(track, station, ct);
            }
            else
            {
                if (marks.Liked) await api.RemoveLikeAsync(marks.LikeId!.Value, ct);
                else if (track is not null) await api.AddLikeAsync(track, ct);
                else if (likeTrack is not null) await api.AddRadioLikeAsync(likeTrack, ct);
            }
            if (item == CurrentMarkItem && likeItem == CurrentLikeItem) await RefreshMarksAsync(api, ct);
        });
    }

    [RelayCommand] private void ChangeZone(string direction)
    {
        var index = Zones.ToList().IndexOf(SelectedZone ?? "");
        if (index < 0) return;
        var next = (index + (direction == "next" ? 1 : -1) + Zones.Count) % Zones.Count;
        SelectedZone = Zones[next];
    }
    [RelayCommand] private Task PlayPauseAsync()
    {
        var zone = SelectedZone; var action = IsPlaying ? "pause" : "play";
        // The server accepts play with every device off; nothing would be heard, so say why instead. A device in
        // deep sleep is not off: the core wakes it.
        var silent = action == "play" && !_snapshot.Rooms.Any(r => r.Desired.Zone == zone && r.Actual?.CanPlay == true);
        return _execute(async (api, ct) =>
        {
            if (zone is null) return;
            if (silent) throw new ApiException(System.Net.HttpStatusCode.Conflict, "notConnected",
                $"No device in {ZoneNameOf(zone)} is connected. Turn a device on and try again.");
            await api.TransportAsync(zone, action, null, ct);
        });
    }
    public Task SetZoneVolumeAsync() => _zoneVolumeSend.SendAsync(() => ZoneVolume, async (volume, api, ct) =>
    {
        if (_zoneVolumeTarget is not { } zone) return;
        var result = await api.SetZoneVolumeAsync(zone, Math.Clamp(volume, 0, 100) / 100, null, ct);
        ct.ThrowIfCancellationRequested();
        Apply(_snapshot with { Zones = _snapshot.Zones.Select(z => z.Zone == zone ? result : z).ToArray() });
    });
    [RelayCommand] private Task MuteZoneAsync()
    {
        var zone = SelectedZone; var muted = ZoneSettings?.Muted;
        return _execute(async (api, ct) =>
        {
            if (zone is null || muted is null) return;
            var result = await api.SetZoneVolumeAsync(zone, null, !muted.Value, ct);
            ct.ThrowIfCancellationRequested();
            Apply(_snapshot with { Zones = _snapshot.Zones.Select(z => z.Zone == zone ? result : z).ToArray() });
        });
    }

    // Previous and next share TransportCommand; this tells their spinners apart.
    [ObservableProperty, NotifyPropertyChangedFor(nameof(IsPreviousPending)), NotifyPropertyChangedFor(nameof(IsNextPending))] private string? _pendingTransport;
    public bool IsPreviousPending => PendingTransport == "previous";
    public bool IsNextPending => PendingTransport == "next";
    // Previous goes back to the start of the track once more than this of it has played, else to the one before.
    public static readonly TimeSpan RestartAfter = TimeSpan.FromSeconds(5);
    [RelayCommand] private async Task TransportAsync(string action)
    {
        PendingTransport = action;
        var restart = action == "previous" && CanSeekCurrent && ProgressSeconds > RestartAfter.TotalSeconds ? Current?.Id : null;
        try
        {
            await _execute(async (api, ct) =>
            {
                if (SelectedZone is not { } zone) return;
                if (restart is not { } item) { await api.TransportAsync(zone, action, null, ct); return; }
                var response = await api.SeekAsync(zone, item, 0, ct);
                ct.ThrowIfCancellationRequested();
                // At once, not at the next event: the position jumps back as the button is pressed.
                QueueState? queue = null;
                try { queue = response?.Deserialize<QueueState>(MultiroomClient.Json); } catch (JsonException) { }
                if (queue is not null && queue.Zone == zone && SelectedZone == zone)
                    Apply(_snapshot with { Zones = _snapshot.Zones.Select(z => z.Zone == zone ? z with { Queue = queue } : z).ToArray() });
            });
        }
        finally { PendingTransport = null; }
    }
    [RelayCommand] private Task StartItemAsync() => _execute(async (api, ct) =>
    { if (SelectedZone is { } zone && SelectedItem is { } row) await api.TransportAsync(zone, "play", row.Item.Id, ct); });
    [RelayCommand] private Task SeekAsync() => _execute(async (api, ct) =>
    {
        var item = SelectedItem?.Item ?? Current;
        if (SelectedZone is { } zone && IsSeekCandidate(item) && SeekSeconds >= 0)
            await api.SeekAsync(zone, item!.Id, checked((long)(SeekSeconds * 1000)), ct);
    });
    [RelayCommand] private Task SeekCurrentAsync() => _execute(async (api, ct) =>
    {
        if (SelectedZone is { } zone && IsSeekCandidate(Current) && SeekSeconds >= 0)
            await api.SeekAsync(zone, Current!.Id, checked((long)(SeekSeconds * 1000)), ct);
    });
    [RelayCommand] private Task RepeatAsync() => _execute(async (api, ct) =>
    { if (SelectedZone is { } zone) await api.QueueModeAsync(zone, Queue?.Repeat switch { RepeatMode.Off => RepeatMode.All, RepeatMode.All => RepeatMode.One, _ => RepeatMode.Off }, null, ct); });
    [RelayCommand] private Task ShuffleAsync() => _execute(async (api, ct) =>
    { if (SelectedZone is { } zone) await api.QueueModeAsync(zone, null, !Shuffle, ct); });
    [RelayCommand] private Task RemoveAsync() => _execute(async (api, ct) =>
    { if (SelectedZone is { } zone && SelectedItem is { } row) await api.RemoveQueueAsync(zone, row.Item.Id, ct); });
    [RelayCommand] private Task MoveAsync(string direction) => _execute(async (api, ct) =>
    {
        if (SelectedZone is not { } zone || SelectedItem is not { } row) return;
        var index = Items.IndexOf(row) + (direction == "up" ? -1 : 1);
        if (index >= 0 && index < Items.Count) await api.MoveQueueAsync(zone, row.Item.Id, index, ct);
    });
    [RelayCommand] private Task RemoveItemAsync(QueueRow? row) => _execute(async (api, ct) =>
    { if (SelectedZone is { } zone && row is not null) await api.RemoveQueueAsync(zone, row.Item.Id, ct); });
    [RelayCommand] private Task MoveToAsync(QueueRowMove? move) => _execute(async (api, ct) =>
    {
        if (SelectedZone is not { } zone || move is not { Row: { } row }) return;
        var to = Math.Clamp(move.Index, 0, Math.Max(0, Items.Count - 1));
        var from = Items.IndexOf(row);
        if (from < 0 || from == to) return;
        // Shown where it was dropped at once; the queue event that follows renumbers the rows.
        Items.Move(from, to);
        try { await api.MoveQueueAsync(zone, row.Item.Id, to, ct); }
        catch { if (Items.IndexOf(row) is var at && at >= 0) Items.Move(at, from); throw; }
    });
    [RelayCommand] private Task CreateZoneAsync() => _execute(async (api, ct) =>
    {
        if (string.IsNullOrWhiteSpace(NewZone)) return;
        var name = NewZone.Trim();
        if (Zones.Contains(name)) { SelectedZone = name; return; }
        await api.SetClientsAsync(name, [], ct); _selectWhenAvailable = name;
    });
    [RelayCommand] private Task DeleteZoneAsync() => _execute(async (api, ct) =>
    { if (SelectedZone is { } zone) await api.DeleteZoneAsync(zone, ct); });
    [RelayCommand] private Task SaveMembershipAsync() => _execute(async (api, ct) =>
    {
        if (SelectedZone is not { } zone) return;
        await api.SetClientsAsync(zone, Membership.Where(r => r.Assigned).Select(r => r.Client).ToArray(), ct);
        foreach (var row in Membership) row.Dirty = false;
    });
}

// A room's own volume and mute on the volume page.
public sealed partial class RoomVolume : ObservableObject
{
    private readonly Func<Func<MultiroomClient, CancellationToken, Task>, Task> _execute;
    private readonly LatestValue _volumeSend;
    private RoomView? _room;
    private bool _applying;
    private int _muteSends;
    public RoomVolume(string client, Func<Func<MultiroomClient, CancellationToken, Task>, Task> execute)
    {
        Client = client; _name = client; _execute = execute;
        _volumeSend = new(execute, () => { if (_room is { } room) Apply(room); });
    }
    public string Client { get; }
    [ObservableProperty, NotifyPropertyChangedFor(nameof(MuteLabel))] private string _name;
    [ObservableProperty, NotifyPropertyChangedFor(nameof(VolumeText))] private double _volume;
    [ObservableProperty, NotifyPropertyChangedFor(nameof(MuteLabel))] private bool _muted;
    [ObservableProperty] private bool _connected;
    public string VolumeText => $"{Volume:0}%";
    public string MuteLabel => Muted ? $"Unmute {Name}" : $"Mute {Name}";
    // The room's own picture, else its art as the web interface shows it.
    private Avalonia.Media.Imaging.Bitmap? _picture;
    public Avalonia.Media.IImage Picture => _picture ?? RoomArt.For(Client);
    public async Task LoadPictureAsync(ArtworkCache pictures)
    {
        if (await RoomArt.PictureAsync(pictures, Client) is not { } picture) return;
        _picture = picture; OnPropertyChanged(nameof(Picture));
    }
    // The server's state, except what is still being sent from here.
    internal void Apply(RoomView room)
    {
        _room = room; _applying = true;
        Name = room.DisplayName; Connected = room.Actual?.Connected == true;
        if (!_volumeSend.Running) Volume = Math.Round((room.Desired.Volume ?? 0) * 100);
        if (_muteSends == 0) Muted = room.Desired.Muted == true;
        _applying = false;
    }
    partial void OnVolumeChanged(double value) { if (!_applying) _ = SetVolumeAsync(); }
    // What the server accepted counts as its state until its next snapshot, so nothing jumps back meanwhile; a
    // refused change returns to the state before.
    public Task SetVolumeAsync() => _volumeSend.SendAsync(() => Volume, async (volume, api, ct) =>
    {
        volume = Math.Clamp(volume, 0, 100) / 100;
        await api.SetRoomVolumeAsync(Client, volume, ct);
        if (_room is { } room) _room = room with { Desired = room.Desired with { Volume = volume } };
    });
    [RelayCommand] private async Task ToggleMuteAsync()
    {
        var muted = !Muted;
        _applying = true; Muted = muted; _applying = false;
        _muteSends++;
        try
        {
            await _execute(async (api, ct) =>
            {
                await api.PatchRoomAsync(Client, new(Muted: muted), ct);
                if (_room is { } room) _room = room with { Desired = room.Desired with { Muted = muted } };
            });
        }
        finally { if (--_muteSends == 0 && _room is { } room) Apply(room); }
    }
}

// Sends a value being dragged: one request at a time, and once it is answered the newest value if it moved on
// meanwhile, so a slider neither floods the server nor ends on a value it has left. done runs after the last one.
internal sealed class LatestValue(Func<Func<MultiroomClient, CancellationToken, Task>, Task> execute, Action done)
{
    private bool _pending;
    public bool Running { get; private set; }
    public async Task SendAsync(Func<double> current, Func<double, MultiroomClient, CancellationToken, Task> send)
    {
        _pending = true;
        if (Running) return;
        Running = true;
        try
        {
            while (_pending)
            {
                _pending = false;
                var value = current();
                await execute((api, ct) => send(value, api, ct));
            }
        }
        finally { Running = false; done(); }
    }
}

public sealed partial class RoomChoice(string client, bool assigned) : ObservableObject
{
    public string Client { get; } = client;
    [ObservableProperty] private string _name = client;
    public bool Dirty { get; set; }
    [ObservableProperty] private bool _assigned = assigned;
    partial void OnAssignedChanged(bool value) => Dirty = true;
    public void Reset(bool assigned) { Assigned = assigned; Dirty = false; }
    private Avalonia.Media.Imaging.Bitmap? _picture;
    public Avalonia.Media.IImage Picture => _picture ?? RoomArt.For(Client);
    public async Task LoadPictureAsync(ArtworkCache pictures)
    {
        if (await RoomArt.PictureAsync(pictures, Client) is not { } picture) return;
        _picture = picture; OnPropertyChanged(nameof(Picture));
    }
}

// A queue row as the web interface shows it: its number (bars while it plays), artwork, two lines and its length.
public sealed record QueueRow(QueueItem Item, bool IsCurrent, int Number)
{
    public string Title => Item.Track?.Title ?? Item.Station?.Name ?? (Item.Item as MediaGroup)?.Title ?? "";
    public string? Artwork => Item.Item.ArtworkId;
    public string Length => Item.Station is not null ? "LIVE" : Item.Track?.Duration is { } length
        ? length.ToString(length.TotalHours >= 1 ? @"h\:mm\:ss" : @"m\:ss") : "";
    public string Subtitle => Item.Track is { } track ? string.Join(" · ", new[] { track.Artist, track.Album }.Where(t => !string.IsNullOrEmpty(t)))
        : Item.Station is { } station ? string.Join(" · ", new[] { station.Genre, "Radio" }.Where(t => !string.IsNullOrEmpty(t)))
        : (Item.Item as MediaGroup)?.Artist ?? "";
    public bool HasFailure => Item.Failure is not null;
}

public sealed record QueueRowMove(QueueRow Row, int Index);

public sealed record ZoneChoice(string Zone, string Name);
