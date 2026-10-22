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
    private bool _zoneVolumeDirty;
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
    public string? PreferredZone { get; set; }
    public PlaybackViewModel(Func<Func<MultiroomClient, CancellationToken, Task>, Task> execute, TimeProvider? time = null)
    { _execute = execute; _time = time ?? TimeProvider.System; }
    public ObservableCollection<QueueRow> Items { get; } = [];
    [ObservableProperty] private IReadOnlyList<string> _zones = [];
    [ObservableProperty] private string? _selectedZone;
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
    [ObservableProperty] private RoomView? _selectedRoom;
    [ObservableProperty] private decimal _requestedVolume = 100;
    [ObservableProperty] private decimal? _requestedZoneVolume;
    [ObservableProperty, NotifyPropertyChangedFor(nameof(PositionText))] private double _progressSeconds;
    public double TrackLengthSeconds => Math.Max(0, Current?.Track?.Duration?.TotalSeconds ?? 0);
    public double ProgressMaximum => Math.Max(1, TrackLengthSeconds);
    public bool CanScrub => CanSeekCurrent && TrackLengthSeconds > 0;
    public bool HasPosition => Queue?.PositionMs is >= 0;
    public string PositionText => _scrubbing || HasPosition ? FormatPosition(ProgressSeconds) : "—:—";
    public string LengthText => Current?.Station is not null ? "LIVE" : TrackLengthSeconds > 0 ? FormatPosition(TrackLengthSeconds) : "—:—";
    public string QueuePosition => Queue?.Current is { } id && Queue.Items.ToList().FindIndex(item => item.Id == id) is var index && index >= 0
        ? $"{index + 1} / {Queue.Items.Count}" : "";
    private static string FormatPosition(double seconds) => TimeSpan.FromSeconds(Math.Max(0, seconds)).ToString(seconds >= 3600 ? @"h\:mm\:ss" : @"m\:ss");
    public bool IsPlaying => Queue?.Transport is QueueTransport.Playing or QueueTransport.Starting;
    public string PlayPauseLabel => IsPlaying ? "Pause" : "Play";
    public string ZonePosition => $"Zone {Zones.ToList().IndexOf(SelectedZone ?? "") + 1} of {Zones.Count}: {SelectedZone}";
    public IReadOnlyList<bool> ZoneIndicators => Zones.Select(zone => zone == SelectedZone).ToArray();
    public ZoneDesired? ZoneSettings => _snapshot.Zones.FirstOrDefault(z => z.Zone == SelectedZone)?.Desired;
    public string ZoneVolumeStatus => ZoneSettings?.Volume is { } volume ? $"Zone volume: {volume:P0}" : "Zone volume: unknown";
    public string ZoneMuteLabel => ZoneSettings?.Muted == true ? "Unmute zone" : "Mute zone";
    public bool CanSetZoneVolume => HasZone && RequestedZoneVolume is >= 0 and <= 100;
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
    public string ActualVolume => SelectedRoom?.Actual?.Settings is { } settings && settings.TryGetProperty("volume", out var v)
        ? $"Reported volume: {v.GetDouble():P0}" : "Reported volume: unknown";
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
        _snapshot = snapshot;
        var names = snapshot.Zones.Select(z => z.Zone).ToArray();
        if (!Zones.SequenceEqual(names)) Zones = names;
        if (_selectWhenAvailable is { } desired && Zones.Contains(desired)) { SelectedZone = desired; _selectWhenAvailable = null; }
        else if (SelectedZone is null || !Zones.Contains(SelectedZone))
            SelectedZone = PreferredZone is { } preferred && Zones.Contains(preferred) ? preferred : Zones.FirstOrDefault();
        if (SelectedZone is not null) PreferredZone = SelectedZone;
        Update();
    }

    partial void OnSelectedZoneChanged(string? value)
    { _zoneVolumeDirty = false; SelectedItem = null; Membership.Clear(); Update(); OnPropertyChanged(nameof(HasZone)); }
    partial void OnRequestedZoneVolumeChanged(decimal? value)
    { _zoneVolumeDirty = true; OnPropertyChanged(nameof(CanSetZoneVolume)); }
    partial void OnSelectedItemChanged(QueueRow? value) => CanSeek = IsSeekCandidate(value?.Item ?? Current);
    partial void OnSelectedRoomChanged(RoomView? value) => OnPropertyChanged(nameof(ActualVolume));

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
        if (!_zoneVolumeDirty)
        {
            RequestedZoneVolume = zone?.Desired?.Volume is { } volume ? (decimal)volume * 100 : null;
            _zoneVolumeDirty = false;
        }
        Duration = item?.Track?.Duration is { } length ? length.ToString(@"h\:mm\:ss") : item?.Station is not null ? "LIVE" : "";
        var selectedId = SelectedItem?.Item.Id;
        var desired = (queue?.Items ?? []).Select(q => new QueueRow(q, q.Id == queue?.Current)).ToArray();
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
        SelectedRoom = Rooms.FirstOrDefault(r => r.Client == SelectedRoom?.Client) ?? Rooms.FirstOrDefault();
        foreach (var room in _snapshot.Rooms)
        {
            var choice = Membership.FirstOrDefault(c => c.Client == room.Client);
            if (choice is null) Membership.Add(new RoomChoice(room.Client, room.Desired.Zone == SelectedZone));
            else if (!choice.Dirty) choice.Reset(room.Desired.Zone == SelectedZone);
        }
        for (var i = Membership.Count - 1; i >= 0; i--)
            if (!_snapshot.Rooms.Any(r => r.Client == Membership[i].Client)) Membership.RemoveAt(i);
        OnPropertyChanged(nameof(IsEmpty)); OnPropertyChanged(nameof(HasZone));
        OnPropertyChanged(nameof(CurrentArtwork));
        OnPropertyChanged(nameof(CanSeekCurrent)); OnPropertyChanged(nameof(ActualVolume));
        foreach (var property in new[] { nameof(IsPlaying), nameof(PlayPauseLabel), nameof(ZonePosition), nameof(ZoneIndicators),
            nameof(ZoneVolumeStatus), nameof(ZoneMuteLabel), nameof(CanSetZoneVolume), nameof(CanMuteZone),
            nameof(TrackLengthSeconds), nameof(ProgressMaximum), nameof(CanScrub), nameof(HasPosition),
            nameof(PositionText), nameof(LengthText), nameof(QueuePosition), nameof(IsRepeatEnabled), nameof(IsRepeatOne) }) OnPropertyChanged(property);
        RefreshProgress();
    }

    public void RefreshProgress()
    {
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
        OnPropertyChanged(nameof(PositionText));
    }

    public void CancelScrub()
    {
        _scrubbing = false; _scrubZone = null; _scrubItem = null;
        RefreshProgress(); OnPropertyChanged(nameof(PositionText));
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
        return _execute(async (api, ct) => { if (zone is not null) await api.TransportAsync(zone, action, null, ct); });
    }
    [RelayCommand] private Task SetZoneVolumeAsync()
    {
        var zone = SelectedZone; var volume = RequestedZoneVolume;
        return _execute(async (api, ct) =>
        {
            if (zone is null || volume is not (>= 0 and <= 100)) return;
            var result = await api.SetZoneVolumeAsync(zone, (double)(volume.Value / 100), null, ct);
            ct.ThrowIfCancellationRequested();
            if (SelectedZone == zone && RequestedZoneVolume == volume) _zoneVolumeDirty = false;
            Apply(_snapshot with { Zones = _snapshot.Zones.Select(z => z.Zone == zone ? result : z).ToArray() });
        });
    }
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

    [RelayCommand] private Task TransportAsync(string action) => _execute(async (api, ct) =>
    { if (SelectedZone is { } zone) await api.TransportAsync(zone, action, null, ct); });
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
        if (from >= 0 && from != to) await api.MoveQueueAsync(zone, row.Item.Id, to, ct);
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
    [RelayCommand] private Task SetVolumeAsync() => _execute(async (api, ct) =>
    { if (SelectedRoom is { } room) await api.SetRoomVolumeAsync(room.Client, (double)(RequestedVolume / 100), ct); });
}

public sealed partial class RoomChoice(string client, bool assigned) : ObservableObject
{
    public string Client { get; } = client;
    public bool Dirty { get; set; }
    [ObservableProperty] private bool _assigned = assigned;
    partial void OnAssignedChanged(bool value) => Dirty = true;
    public void Reset(bool assigned) { Assigned = assigned; Dirty = false; }
}

public sealed record QueueRow(QueueItem Item, bool IsCurrent)
{
    public string Title => (IsCurrent ? "▶  " : "") + (Item.Track?.Title ?? Item.Station?.Name);
    public string Subtitle => Item.Track?.Artist ?? Item.Station?.Genre ?? "";
    public bool HasFailure => Item.Failure is not null;
}

public sealed record QueueRowMove(QueueRow Row, int Index);
