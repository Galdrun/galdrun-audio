using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using Multiroom.App.Api;

namespace Multiroom.App;

public sealed partial class CollectionsViewModel(
    Func<Func<MultiroomClient, CancellationToken, Task>, Task> execute,
    Func<string?> zone,
    Func<string, string>? zoneName = null) : ObservableObject
{
    private bool _refreshing;
    private int _entriesVersion;
    private Track? _seedTrack;
    private long? _seedPlaylist;
    public event Action? QueueChanged;
    public event Action? RecommendationsRequested;
    // A warning for the shell's toast, such as items the zone refused.
    public event Action<string>? Warned;
    [ObservableProperty] private string _page = "Playlists";
    [ObservableProperty] private IReadOnlyList<Playlist> _playlists = [];
    [ObservableProperty] private Playlist? _selectedPlaylist;
    [ObservableProperty, NotifyPropertyChangedFor(nameof(NoEntries))] private IReadOnlyList<PlaylistEntry> _entries = [];
    [ObservableProperty] private PlaylistEntry? _selectedEntry;
    [ObservableProperty, NotifyPropertyChangedFor(nameof(NoFavorites))] private IReadOnlyList<Favorite> _favorites = [];
    [ObservableProperty] private Favorite? _selectedFavorite;
    [ObservableProperty] private IReadOnlyList<RecommendedTrack> _recommendations = [];
    [ObservableProperty] private RecommendedTrack? _selectedRecommendation;
    [ObservableProperty] private string _name = "";
    [ObservableProperty] private string _recommendationStatus = "Choose a track or playlist and select More like this.";
    [ObservableProperty] private MediaRow? _pendingMedia;
    public bool IsPlaylists => Page == "Playlists";
    public bool IsFavorites => Page == "Favourites";
    public bool IsRecommendations => Page == "Recommendations";
    public bool CanAddTrack => PendingMedia?.Track is not null && SelectedPlaylist is not null;
    public bool CanFavorite => PendingMedia?.Track is not null || PendingMedia?.Station is not null;
    public bool HasPlaylist => SelectedPlaylist is not null;
    public bool NoEntries => Entries.Count == 0;
    public bool NoFavorites => Favorites.Count == 0;
    partial void OnPageChanged(string value)
    { foreach (var p in new[] { nameof(IsPlaylists), nameof(IsFavorites), nameof(IsRecommendations) }) OnPropertyChanged(p); }
    partial void OnPendingMediaChanged(MediaRow? value) { OnPropertyChanged(nameof(CanAddTrack)); OnPropertyChanged(nameof(CanFavorite)); }
    partial void OnSelectedPlaylistChanged(Playlist? value)
    {
        OnPropertyChanged(nameof(CanAddTrack)); OnPropertyChanged(nameof(HasPlaylist));
        Name = value?.Name ?? "";
        if (!_refreshing) { Entries = []; SelectedEntry = null; _ = LoadEntriesAsync(); }
    }
    private Task LoadEntriesAsync() => execute(LoadEntriesAsync);
    private async Task LoadEntriesAsync(MultiroomClient api, CancellationToken ct)
    {
        var version = ++_entriesVersion;
        var id = SelectedPlaylist?.Id;
        var entries = id is { } playlist ? await api.EntriesAsync(playlist, ct) : [];
        ct.ThrowIfCancellationRequested();
        if (version != _entriesVersion || SelectedPlaylist?.Id != id) return;
        var selected = SelectedEntry?.Id;
        Entries = entries; SelectedEntry = entries.FirstOrDefault(e => e.Id == selected);
    }
    public Task RefreshAsync() => execute(async (api, ct) =>
    {
        if (IsRecommendations) { if (_seedTrack is not null || _seedPlaylist is not null) await RecommendAsync(api, ct); return; }
        await ReloadAsync(api, ct);
    });
    private async Task ReloadAsync(MultiroomClient api, CancellationToken ct)
    {
        var playlists = await api.PlaylistsAsync(ct);
        var favorites = await api.FavoritesAsync(ct);
        ct.ThrowIfCancellationRequested();
        var playlistId = SelectedPlaylist?.Id; var favoriteId = SelectedFavorite?.Id;
        _refreshing = true;
        try { Playlists = playlists; SelectedPlaylist = playlists.FirstOrDefault(p => p.Id == playlistId); }
        finally { _refreshing = false; }
        Favorites = favorites; SelectedFavorite = favorites.FirstOrDefault(f => f.Id == favoriteId);
        await LoadEntriesAsync(api, ct);
    }
    public void Reset()
    {
        _entriesVersion++; _refreshing = true;
        Playlists = []; SelectedPlaylist = null; Entries = []; SelectedEntry = null;
        Favorites = []; SelectedFavorite = null; Recommendations = []; SelectedRecommendation = null;
        PendingMedia = null; _seedTrack = null; _seedPlaylist = null;
        RecommendationStatus = "Choose a track or playlist and select More like this."; _refreshing = false;
    }
    [RelayCommand] private Task CreateAsync() => execute(async (api, ct) =>
    {
        if (string.IsNullOrWhiteSpace(Name)) throw new ArgumentException("Enter a playlist name.");
        var created = await api.CreatePlaylistAsync(Name.Trim(), ct);
        await ReloadAsync(api, ct); _refreshing = true;
        try { SelectedPlaylist = Playlists.FirstOrDefault(p => p.Id == created.Id); }
        finally { _refreshing = false; }
        await LoadEntriesAsync(api, ct);
    });
    [RelayCommand] private Task RenameAsync() => execute(async (api, ct) =>
    {
        if (SelectedPlaylist is not { } p) return;
        if (string.IsNullOrWhiteSpace(Name)) throw new ArgumentException("Enter a playlist name.");
        await api.PatchPlaylistAsync(p.Id, Name.Trim(), null, ct); await ReloadAsync(api, ct);
    });
    [RelayCommand] private Task DeleteAsync() => execute(async (api, ct) =>
    { if (SelectedPlaylist is { } p) { await api.DeletePlaylistAsync(p.Id, ct); await ReloadAsync(api, ct); } });
    [RelayCommand] private Task MovePlaylistAsync(string direction) => execute(async (api, ct) =>
    {
        if (SelectedPlaylist is not { } p) return;
        var index = Playlists.ToList().FindIndex(x => x.Id == p.Id) + (direction == "up" ? -1 : 1);
        if (index < 0 || index >= Playlists.Count) return;
        await api.PatchPlaylistAsync(p.Id, null, index, ct); await ReloadAsync(api, ct);
    });
    [RelayCommand] private Task AddTrackAsync() => execute(async (api, ct) =>
    {
        if (SelectedPlaylist is not { } p || PendingMedia?.Track is not { } track) return;
        await api.AddEntriesAsync(p.Id, [track], ct); await ReloadAsync(api, ct);
    });
    [RelayCommand] private Task RemoveEntryAsync() => execute(async (api, ct) =>
    { if (SelectedPlaylist is { } p && SelectedEntry is { } e) { await api.RemoveEntryAsync(p.Id, e.Id, ct); await ReloadAsync(api, ct); } });
    [RelayCommand] private Task MoveEntryAsync(string direction) => execute(async (api, ct) =>
    {
        if (SelectedPlaylist is not { } p || SelectedEntry is not { } e) return;
        var index = Entries.ToList().FindIndex(x => x.Id == e.Id) + (direction == "up" ? -1 : 1);
        if (index < 0 || index >= Entries.Count) return;
        await api.MoveEntryAsync(p.Id, e.Id, index, ct); await ReloadAsync(api, ct);
    });
    [RelayCommand] private Task AddFavoriteAsync() => execute(async (api, ct) =>
    {
        if (!CanFavorite) return;
        await api.AddFavoriteAsync(PendingMedia!.Track, PendingMedia.Station, ct);
        await ReloadAsync(api, ct);
    });
    [RelayCommand] private Task RemoveFavoriteAsync() => execute(async (api, ct) =>
    { if (SelectedFavorite is { } f) { await api.RemoveFavoriteAsync(f.Id, ct); await ReloadAsync(api, ct); } });
    [RelayCommand] private async Task MoreLikeAsync(string source)
    {
        var track = source switch { "entry" => SelectedEntry?.Track, "favorite" => SelectedFavorite?.Track,
            "recommendation" => SelectedRecommendation?.Track, "pending" => PendingMedia?.Track, _ => null };
        var playlist = source == "playlist" ? SelectedPlaylist?.Id : null;
        if (track is null && playlist is null) return;
        _seedTrack = track; _seedPlaylist = playlist;
        Recommendations = []; SelectedRecommendation = null;
        RecommendationsRequested?.Invoke();
        await execute(RecommendAsync);
    }
    private async Task RecommendAsync(MultiroomClient api, CancellationToken ct)
    {
        var result = await api.RecommendAsync(_seedTrack, _seedPlaylist, ct);
        ct.ThrowIfCancellationRequested();
        Recommendations = result.Items; SelectedRecommendation = null;
        RecommendationStatus = result.Items.Count == 0 ? "No recommendations returned." : $"{result.Items.Count} recommendations";
        RecommendationStatus += "\n" + string.Join(" · ", result.Sources.Select(s => $"{s.AddIn}: {s.Tracks}"));
        // Failure messages can echo submitted values; name the add-in with safe mapped text.
        foreach (var failure in result.Failures)
            RecommendationStatus += $"\n{failure.AddIn}: " + (failure.Error == "timeout" ? "Timed out. Refresh to retry." : "Unavailable. Refresh to retry.");
    }
    [RelayCommand] private Task QueueAsync(string placement) => execute(async (api, ct) =>
    {
        AddItems? items = IsPlaylists && SelectedPlaylist is { } p ? new(Playlist: p.Id) :
            IsFavorites && SelectedFavorite is { } f ? new(Favorite: f.Id) :
            IsRecommendations && SelectedRecommendation is { } r ? new(Tracks: [r.Track]) : null;
        if (items is null) return;
        if (zone() is not { } target) throw new ArgumentException("Choose or create a zone before adding music.");
        var answer = await api.QueueAsync(target, items with { Next = placement == "next", Replace = placement == "now" }, ct);
        if (answer is { } json && json.TryGetProperty("refused", out var refused) && refused.GetArrayLength() is var count and > 0)
            Warned?.Invoke($"{count} items refused by {zoneName?.Invoke(target) ?? target}");
        if (placement == "now") await api.TransportAsync(target, "play", null, ct);
        QueueChanged?.Invoke();
    });
}
