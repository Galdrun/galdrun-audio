using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using Multiroom.App.Api;

namespace Multiroom.App;

public sealed partial class BrowserViewModel : ObservableObject
{
    private readonly Func<Func<MultiroomClient, CancellationToken, Task>, Task> _execute;
    private readonly Func<string?> _zone;
    private readonly Func<string, string> _zoneName;
    private readonly Stack<(string Provider, string? Node, string Title)> _history = [];
    private string? _provider;
    private string? _node;
    private int _offset;
    private string? _letter;
    private bool _search;
    private CancellationTokenSource? _request;
    private int _columns = 3;
    private bool _searchVisible;
    private CancellationTokenSource? _debounce;
    public event Action? QueueChanged;
    public event Action<string, MediaRow>? CollectionRequested;
    // The play choices (the shell's item sheet) for the album or one of its tracks.
    public event Action<HomeItem>? SheetRequested;
    // The album was liked, unliked, saved or removed as a favourite.
    public event Action? MarksChanged;
    [RelayCommand] private void Collection(string page)
    {
        if (SelectedItem is { } media && (media.Track is not null || (page == "Favourites" && media.Station is not null)))
            CollectionRequested?.Invoke(page, media);
    }
    public BrowserViewModel(Func<Func<MultiroomClient, CancellationToken, Task>, Task> execute, Func<string?> zone,
        Func<string, string>? zoneName = null)
    { _execute = execute; _zone = zone; _zoneName = zoneName ?? (z => z); }
    public ObservableCollection<MediaRow> Items { get; } = [];
    public ObservableCollection<MediaBand> Bands { get; } = [];
    [ObservableProperty] private MediaRow? _selectedItem;
    [ObservableProperty] private string _title = "Your music";
    [ObservableProperty] private string _query = "";
    [ObservableProperty] private string _filter = "";
    [ObservableProperty] private string? _sort;
    [ObservableProperty] private IReadOnlyList<string> _sorts = [];
    [ObservableProperty] private IReadOnlyList<BrowseLetter> _letters = [];
    [ObservableProperty] private BrowseLetter? _selectedLetter;
    [ObservableProperty] private bool _filterable;
    [ObservableProperty] private bool _isGrid;
    [ObservableProperty] private bool _hasMore;
    [ObservableProperty] private string _summary = "";
    [ObservableProperty] private string _notice = "";
    [ObservableProperty] private ArtworkCache? _artwork;
    [ObservableProperty] private MediaRow? _header;
    [ObservableProperty] private string _breadcrumb = "";
    [ObservableProperty] private bool _isLoading;
    [ObservableProperty] private bool _isCompact;
    [ObservableProperty] private bool _optionsExpanded = true;
    public bool HasHeader => Header is not null;
    public bool ShowHeader => HasHeader && !IsCompact && !IsDetail;
    public bool ShowCompactHeader => HasHeader && IsCompact && !IsDetail;

    // An album's page: its cover, its marks and play choices, then its tracks.
    [ObservableProperty, NotifyPropertyChangedFor(nameof(CanMarkAlbum), nameof(AlbumLiked), nameof(AlbumFavorite))]
    private ItemStatus? _albumMarks;
    [ObservableProperty] private IReadOnlyList<AlbumTrack> _albumTracks = [];
    public Album? Album => Header?.Node is { Kind: BrowseKind.Album } node && AlbumTracks.Count > 0
        ? new Album(node.ProviderId, node.NodeId, node.Title, ArtistOf(node.Subtitle)) { ArtworkId = node.ArtworkId } : null;
    // An album header's subtitle is "artist · year · tracks · length", each part optional: its artist is
    // the first part, unless that is already the year or the track count.
    private static string? ArtistOf(string? subtitle) => subtitle?.Split(" · ")[0] is { Length: > 0 } first
        && !int.TryParse(first, out _) && !first.EndsWith(" track", StringComparison.Ordinal)
        && !first.EndsWith(" tracks", StringComparison.Ordinal) ? first : null;
    public bool IsAlbum => Album is not null;
    // An artist's page, shown as an album's: its picture as the cover, then its sections, the albums first
    // (Tidal lists the top tracks first).
    [ObservableProperty] private IReadOnlyList<DetailSection> _artistSections = [];
    public bool IsArtist => ArtistSections.Count > 0;
    public bool IsDetail => IsAlbum || IsArtist;
    public bool IsBrowsing => !IsDetail;
    public int Columns => _columns;
    public string AlbumSummary
    {
        get
        {
            var length = TimeSpan.FromTicks(AlbumTracks.Sum(t => t.Row.Track!.Duration?.Ticks ?? 0));
            var count = AlbumTracks.Count == 1 ? "1 track" : $"{AlbumTracks.Count} tracks";
            return length <= TimeSpan.Zero ? count
                : $"{count} · {(length.TotalHours >= 1 ? $"{(int)length.TotalHours} h {length.Minutes} min" : $"{Math.Max(1, (int)Math.Round(length.TotalMinutes))} min")}";
        }
    }
    public bool CanMarkAlbum => AlbumMarks is not null;
    public bool AlbumLiked => AlbumMarks?.Liked == true;
    public bool AlbumFavorite => AlbumMarks?.Favorite == true;
    private ItemRef? AlbumRef => Album is { } album ? new(LikeKind.Album, album.ProviderId, album.ProviderItemId) : null;
    partial void OnAlbumTracksChanged(IReadOnlyList<AlbumTrack> value)
    {
        foreach (var name in new[] { nameof(Album), nameof(IsAlbum), nameof(IsDetail), nameof(IsBrowsing), nameof(AlbumSummary), nameof(ShowHeader), nameof(ShowCompactHeader), nameof(ShowOptions) })
            OnPropertyChanged(name);
    }
    partial void OnArtistSectionsChanged(IReadOnlyList<DetailSection> value)
    {
        foreach (var name in new[] { nameof(IsArtist), nameof(IsDetail), nameof(IsBrowsing), nameof(ShowHeader), nameof(ShowCompactHeader), nameof(ShowOptions) })
            OnPropertyChanged(name);
    }
    public bool HasSorts => Sorts.Count > 0;
    public bool HasLetters => Letters.Count > 0;
    public bool HasOptions => Filterable || HasSorts || HasLetters;
    public bool ShowOptions => HasOptions && !IsDetail;
    public bool HasNotice => Notice.Length > 0;
    public bool CanBack => _history.Count > 0;
    public bool IsEmpty => Items.Count == 0;
    public bool ShowEmpty => IsEmpty && !IsLoading;
    public bool CanOpen => !IsLoading && SelectedItem?.Node is not null && !ReferenceEquals(SelectedItem, Header);
    public bool CanQueue => !IsLoading && SelectedItem?.Playable == true;
    public bool HasSelectionOrNotice => SelectedItem is not null || HasNotice;
    partial void OnSelectedItemChanged(MediaRow? value)
    { OnPropertyChanged(nameof(CanOpen)); OnPropertyChanged(nameof(CanQueue)); OnPropertyChanged(nameof(HasSelectionOrNotice)); }
    partial void OnIsLoadingChanged(bool value) { OnPropertyChanged(nameof(CanOpen)); OnPropertyChanged(nameof(CanQueue)); OnPropertyChanged(nameof(ShowEmpty)); }
    partial void OnHeaderChanged(MediaRow? value)
    {
        foreach (var name in new[] { nameof(HasHeader), nameof(ShowHeader), nameof(ShowCompactHeader), nameof(Album), nameof(IsAlbum), nameof(IsDetail), nameof(IsBrowsing), nameof(ShowOptions) })
            OnPropertyChanged(name);
    }
    partial void OnIsCompactChanged(bool value)
    { OptionsExpanded = !value; OnPropertyChanged(nameof(ShowHeader)); OnPropertyChanged(nameof(ShowCompactHeader)); }
    partial void OnSortsChanged(IReadOnlyList<string> value) { OnPropertyChanged(nameof(HasSorts)); OnPropertyChanged(nameof(HasOptions)); OnPropertyChanged(nameof(ShowOptions)); }
    partial void OnLettersChanged(IReadOnlyList<BrowseLetter> value) { OnPropertyChanged(nameof(HasLetters)); OnPropertyChanged(nameof(HasOptions)); OnPropertyChanged(nameof(ShowOptions)); }
    partial void OnFilterableChanged(bool value) { OnPropertyChanged(nameof(HasOptions)); OnPropertyChanged(nameof(ShowOptions)); }
    partial void OnNoticeChanged(string value) { OnPropertyChanged(nameof(HasNotice)); OnPropertyChanged(nameof(HasSelectionOrNotice)); }
    partial void OnQueryChanged(string value)
    {
        if (!_searchVisible) return;
        _debounce?.Cancel(); _debounce?.Dispose(); _debounce = new();
        CancelRequest();
        _ = DebouncedSearchAsync(_debounce.Token);
    }
    private async Task DebouncedSearchAsync(CancellationToken ct)
    {
        try { await Task.Delay(350, ct); await SearchAsync(); }
        catch (OperationCanceledException) { }
    }
    public void SetSearchVisible(bool visible) { _searchVisible = visible; if (!visible) _debounce?.Cancel(); }
    public void SetWidth(double width)
    {
        IsCompact = width < 550;
        var columns = Math.Clamp((int)(width / 190), 1, 5);
        if (_columns != columns) { _columns = columns; Regroup(); OnPropertyChanged(nameof(Columns)); }
    }
    private void Regroup()
    {
        Bands.Clear();
        var group = new List<MediaRow>();
        void Flush() { if (group.Count != 0) { Bands.Add(new MediaBand(group.ToArray(), group[0].Grid ? _columns : 1)); group.Clear(); } }
        foreach (var row in Items)
        {
            if (!row.Grid || row.IsHeading) { Flush(); Bands.Add(new MediaBand([row], 1)); }
            else { group.Add(row); if (group.Count == _columns) Flush(); }
        }
        Flush();
    }

    public Task OpenProviderAsync(ProviderView provider)
    {
        _history.Clear(); _provider = provider.Id; _node = null; Title = provider.DisplayName ?? provider.Id;
        _search = false; ResetOptions(); UpdateBreadcrumb(); ClearPage();
        return LoadAsync(false);
    }
    // Opens a node from elsewhere (Home) as a fresh page, without history.
    public Task OpenNodeAsync(NodeReference node, string title)
    {
        _history.Clear(); _provider = node.Provider; _node = node.Id; Title = title;
        _search = false; ResetOptions(); UpdateBreadcrumb(); ClearPage();
        return LoadAsync(false);
    }
    private void CancelRequest() => _request?.Cancel();
    public void Cancel() { CancelRequest(); _debounce?.Cancel(); IsLoading = false; }
    public void Reset()
    {
        Cancel(); _provider = null; _node = null; _history.Clear(); _offset = 0; ClearPage();
        Title = "Your music"; Notice = ""; ResetOptions(); UpdateBreadcrumb();
    }
    // Another page opens empty, never showing the last one (an album, say) while it loads; a refresh keeps it.
    private void ClearPage()
    {
        Items.Clear(); Bands.Clear(); Header = null; SelectedItem = null; HasMore = false; AlbumTracks = []; ArtistSections = []; AlbumMarks = null;
        Summary = ""; OnPropertyChanged(nameof(IsEmpty)); OnPropertyChanged(nameof(ShowEmpty));
    }
    private void UpdateBreadcrumb()
    {
        Breadcrumb = string.Join(" / ", _history.Reverse().Select(p => p.Title).Append(Title));
        OnPropertyChanged(nameof(CanBack));
    }
    private void ResetOptions() { Sort = null; Filter = ""; SelectedLetter = null; _letter = null; _offset = 0; }

    [RelayCommand] private Task OpenAsync() => SelectedItem?.Node is { } node ? Open(node) : Task.CompletedTask;
    // A cover or link of an artist's page opens at a tap.
    [RelayCommand] private Task OpenRow(MediaRow? row) => row?.Node is { } node ? Open(node) : Task.CompletedTask;
    private Task Open(BrowseNode node)
    {
        _history.Push((_provider ?? node.ProviderId, _node, Title));
        _provider = node.ProviderId; _node = node.NodeId; Title = node.Title; _search = false; ResetOptions();
        UpdateBreadcrumb(); ClearPage(); return LoadAsync(false);
    }
    [RelayCommand] private Task BackAsync()
    {
        if (!_history.TryPop(out var page)) return Task.CompletedTask;
        (_provider, _node, Title) = page; _search = false; ResetOptions(); UpdateBreadcrumb(); ClearPage(); return LoadAsync(false);
    }
    [RelayCommand] private Task SearchAsync()
    {
        _debounce?.Cancel(); _history.Clear(); _search = true; _offset = 0; Title = "Search results"; UpdateBreadcrumb(); ClearPage();
        return LoadAsync(false);
    }
    [RelayCommand] private Task ApplyAsync() { _offset = 0; _letter = SelectedLetter?.Letter; return LoadAsync(false); }
    [RelayCommand] private Task MoreAsync() => HasMore && !IsLoading ? LoadAsync(true) : Task.CompletedTask;
    public Task RefreshAsync() { _offset = 0; return LoadAsync(false); }

    private Task LoadAsync(bool append)
    {
        CancelRequest(); _request?.Dispose(); _request = new(); var requestToken = _request.Token;
        return _execute(async (api, pageToken) =>
        {
            using var linked = CancellationTokenSource.CreateLinkedTokenSource(requestToken, pageToken);
            var ct = linked.Token;
            IsLoading = true;
            try
            {
            IReadOnlyList<MediaRow> rows;
            if (_search)
            {
                if (string.IsNullOrWhiteSpace(Query)) { Items.Clear(); Bands.Clear(); Header = null; HasMore = false; Summary = ""; Notice = "Enter an artist, album, track or station."; OnPropertyChanged(nameof(IsEmpty)); OnPropertyChanged(nameof(ShowEmpty)); return; }
                var result = await api.SearchAsync(Query, append ? _offset : 0, ct);
                ct.ThrowIfCancellationRequested();
                rows = MediaRow.FromSearch(result.Items);
                Summary = string.Join(" · ", result.Sources.Select(s => $"{s.Provider}: {s.Tracks + s.Albums + s.Artists + s.Stations + s.Playlists}"));
                Notice = string.Join("\n", result.Failures.Select(f => $"{f.Provider}: {f.Error}"));
                HasMore = result.Sources.Any(s => new[] { s.Tracks, s.Albums, s.Artists, s.Stations, s.Playlists }.Max() >= 60);
                _offset += 60; IsGrid = false; Sorts = []; Letters = []; Filterable = false; Header = null;
            }
            else
            {
                if (_provider is null) return;
                var page = await api.BrowseAsync(_provider, _node, append ? _offset : 0, Sort, Filter, append ? null : _letter, ct);
                ct.ThrowIfCancellationRequested();
                var list = new List<MediaRow>();
                list.AddRange(page.Nodes.Select(n => MediaRow.From(n) with { Grid = page.Layout == BrowseLayout.Grid })); list.AddRange(MediaRow.From(page.Items));
                foreach (var section in page.Sections ?? [])
                {
                    list.Add(new MediaRow(section.Title, section.Total is { } n ? $"{n} items" : "", IsHeading: true));
                    list.AddRange(section.Nodes.Select(n => MediaRow.From(n) with { Grid = section.Layout == BrowseLayout.Grid })); list.AddRange(MediaRow.From(section.Items));
                    if (section.More is { } more) list.Add(MediaRow.From(more) with { Title = $"More {section.Title}" });
                }
                rows = list;
                Sorts = page.Sorts ?? []; Sort = page.Sort; Letters = page.Letters ?? []; Filterable = page.Filterable;
                IsGrid = page.Layout == BrowseLayout.Grid;
                var count = page.Nodes.Count + page.Items.Tracks.Count + page.Items.Stations.Count;
                if (!append) _offset = _letter is null ? 0 : Letters.FirstOrDefault(l => l.Letter == _letter)?.Offset ?? 0;
                _offset += count;
                HasMore = page.Sections is not { Count: > 0 } && count > 0 && (page.Total is { } total ? _offset < total : count >= 60);
                Summary = page.Total is { } known ? $"{known} items" : "";
                Notice = page.Node?.Unplayable ?? "";
                Header = page.Node is { } node ? MediaRow.From(node) with { Cache = Artwork } : null;
            }
            if (!append) { Items.Clear(); SelectedItem = null; }
            foreach (var row in rows) Items.Add(row with { Cache = Artwork });
            Regroup();
            OnPropertyChanged(nameof(IsEmpty)); OnPropertyChanged(nameof(ShowEmpty));
            var tracks = Items.Select(r => r.Track).OfType<Track>().ToArray();
            AlbumTracks = Header?.Node?.Kind == BrowseKind.Album && Items.Count > 0 && tracks.Length == Items.Count
                ? Items.Select((row, i) => new AlbumTrack(i + 1, row, tracks)).ToArray() : [];
            ArtistSections = !_search && Header?.Node is { Kind: BrowseKind.Artist } artist ? DetailSection.Of(Items, artist.Title) : [];
            AlbumMarks = null;
            if (AlbumRef is { } mark)
            {
                // The marks are optional: an album without them still plays.
                try { AlbumMarks = await ReadMarkAsync(api, mark, ct); }
                catch (ApiException) { }
            }
            }
            catch (OperationCanceledException) when (ct.IsCancellationRequested) { }
            finally { if (!requestToken.IsCancellationRequested) IsLoading = false; }
        });
    }

    private static async Task<ItemStatus?> ReadMarkAsync(MultiroomClient api, ItemRef mark, CancellationToken ct)
    {
        var status = (await api.ItemStatusesAsync([mark], ct)).SingleOrDefault();
        return status is not null && status.Kind == mark.Kind && status.ProviderId == mark.ProviderId
            && status.ProviderItemId == mark.ProviderItemId ? status : null;
    }

    [RelayCommand] private Task ToggleAlbumLikeAsync() => ChangeAlbumMarkAsync(favorite: false);
    [RelayCommand] private Task ToggleAlbumFavoriteAsync() => ChangeAlbumMarkAsync(favorite: true);
    private Task ChangeAlbumMarkAsync(bool favorite)
    {
        if (Album is not { } album || AlbumMarks is not { } marks || AlbumRef is not { } mark) return Task.CompletedTask;
        return _execute(async (api, ct) =>
        {
            if (favorite && marks.Favorite) await api.RemoveFavoriteAsync(marks.FavoriteId!.Value, ct);
            else if (favorite) await api.AddFavoriteAsync(album, ct);
            else if (marks.Liked) await api.RemoveLikeAsync(marks.LikeId!.Value, ct);
            else await api.AddLikeAsync(album, ct);
            var status = await ReadMarkAsync(api, mark, ct);
            if (AlbumRef == mark) AlbumMarks = status;
            MarksChanged?.Invoke();
        });
    }

    // The play choices for the whole album or artist, or for one track of a list: that list played from it.
    [RelayCommand] private void PlayAlbum() { if (Album is { } album && HomeItem.Of(album) is { } item) SheetRequested?.Invoke(item); }
    [RelayCommand] private void PlayArtist()
    {
        if (Header?.Node is { Kind: BrowseKind.Artist } node && HomeItem.Of(new Artist(node.ProviderId, node.NodeId, node.Title) { ArtworkId = node.ArtworkId }) is { } item)
            SheetRequested?.Invoke(item);
    }
    [RelayCommand] private void PlayTrack(AlbumTrack? track) { if (track?.Row.Track is { } t) SheetRequested?.Invoke(HomeItem.InList(t, track.List)); }

    [RelayCommand] private Task QueueAsync(string placement) => _execute(async (api, ct) =>
    {
        if (_zone() is not { } zone) throw new ArgumentException("Choose or create a zone before adding music.");
        if (SelectedItem is not { Playable: true } row) return;
        var request = new AddItems(Tracks: row.Track is { } track ? [track] : null, Station: row.Station,
            Node: row.Node is { } node ? new(node.ProviderId, node.NodeId) : null,
            Next: placement == "next", Replace: placement == "now");
        var answer = await api.QueueAsync(zone, request, ct);
        Notice = "Added to " + _zoneName(zone);
        if (answer is { } json && json.TryGetProperty("refused", out var refused) && refused.GetArrayLength() > 0)
            Notice += $" · {refused.GetArrayLength()} items refused by the server";
        if (answer is { } response && response.TryGetProperty("truncated", out var truncated) && truncated.GetBoolean())
            Notice += " · container exceeded the server's limit";
        if (placement == "now") await api.TransportAsync(zone, "play", null, ct);
        QueueChanged?.Invoke();
    });
}

public sealed record MediaRow(string Title, string Subtitle, BrowseNode? Node = null, Track? Track = null,
    RadioStation? Station = null, bool IsHeading = false, bool Grid = false, ArtworkCache? Cache = null)
{
    public bool IsList => !Grid;
    public bool IsContent => !IsHeading;
    public string? Artwork => Node?.ArtworkId ?? Track?.ArtworkId ?? Station?.ArtworkId;
    public bool Playable => Track is not null || Station is not null || Node?.Playable == true;
    public static IReadOnlyList<MediaRow> FromSearch(CatalogItems items)
    {
        var result = new List<MediaRow>();
        void Add(string title, CatalogItems category)
        {
            var rows = From(category);
            if (rows.Count == 0) return;
            result.Add(new MediaRow(title, "", IsHeading: true)); result.AddRange(rows);
        }
        Add("Tracks", new(items.Tracks, [], [], []));
        Add("Albums", new([], items.Albums, [], []));
        Add("Artists", new([], [], items.Artists, []));
        Add("Stations", new([], [], [], items.Stations));
        Add("Playlists", new([], [], [], [], items.Playlists));
        return result;
    }
    public static MediaRow From(BrowseNode n) => new(n.Title, string.Join(" · ", new[] { n.ProviderId, n.Subtitle, n.Unplayable }.Where(s => !string.IsNullOrEmpty(s))), Node: n);
    public static IReadOnlyList<MediaRow> From(CatalogItems items) => [
        ..items.Tracks.Select(t => new MediaRow(t.Title, $"{t.Artist} · {t.ProviderId}", Track: t)),
        ..items.Stations.Select(s => new MediaRow(s.Name, $"{s.Genre} · {s.ProviderId}", Station: s)),
        // An album or artist is its provider's node, queued whole as the server GUI queues it.
        ..items.Albums.Select(a => From(new BrowseNode(a.ProviderId, a.ProviderItemId, a.Title, a.ArtworkId, BrowseKind.Album, a.Artist, Playable: true))),
        ..items.Artists.Select(a => From(new BrowseNode(a.ProviderId, a.ProviderItemId, a.Name, a.ArtworkId, BrowseKind.Artist, Playable: true))),
        ..(items.Playlists ?? []).Select(From)];
}

public sealed record MediaBand(IReadOnlyList<MediaRow> Cards, int Columns);

// One track of an album's (or an artist's) list: its place, title, artist and length, and the whole list it plays in.
public sealed record AlbumTrack(int Number, MediaRow Row, IReadOnlyList<Track> List)
{
    public string Title => Row.Title;
    public string Artist => Row.Track?.Artist ?? "";
    public string Length => Row.Track?.Duration is { } length
        ? length.ToString(length.TotalHours >= 1 ? @"h\:mm\:ss" : @"m\:ss") : "";
}

// One section of an artist's page, its rows as an album's tracks: the artist's tracks as one row opening
// them (or, without that, the tracks themselves), then its albums (newest first), singles and EPs, similar
// artists, each with its title.
public sealed record DetailSection(string Title, IReadOnlyList<MediaRow> Rows, IReadOnlyList<AlbumTrack> Tracks)
{
    public bool HasTitle => Title.Length > 0;

    // The page's rows, split at their headings: the tracks first, then the sections with albums, then the rest.
    public static IReadOnlyList<DetailSection> Of(IEnumerable<MediaRow> rows, string artist)
    {
        var parts = new List<(string Title, List<MediaRow> Rows)>();
        foreach (var row in rows)
        {
            if (row.IsHeading) parts.Add((row.Title, []));
            else { if (parts.Count == 0) parts.Add(("", [])); parts[^1].Rows.Add(row); }
        }
        var sections = parts.Where(p => p.Rows.Count > 0).Select(p => Section(p.Title, p.Rows, artist)).ToArray();
        static int Rank(DetailSection s) => s.Tracks.Count > 0 || s.Rows.Any(r => r.Node?.Kind == BrowseKind.Section && !s.HasTitle) ? 0
            : s.Rows.Any(r => r.Node?.Kind == BrowseKind.Album) ? 1 : 2;
        return [.. sections.OrderBy(Rank)];
    }

    private static DetailSection Section(string title, List<MediaRow> rows, string artist)
    {
        var tracks = rows.Select(r => r.Track).OfType<Track>().ToArray();
        if (tracks.Length > 0)
        {
            // The section's own node ("More Top tracks") opens all of them: one row in their place.
            if (rows.FirstOrDefault(r => r.Node is not null) is { } all)
                return new("", [all with { Title = title, Node = all.Node! with { ArtworkId = all.Node.ArtworkId ?? tracks[0].ArtworkId },
                    Subtitle = string.Join(", ", tracks.Take(3).Select(t => t.Title)) + (tracks.Length > 3 ? ", …" : "") }], []);
            return new(title, [], [.. rows.Where(r => r.Track is not null).Select((row, i) => new AlbumTrack(i + 1, row, tracks))]);
        }
        var nodes = rows.Where(r => r.Node is not null).Select(r => r with { Subtitle = Without(r.Node!.Subtitle, artist) });
        // Albums newest first, by the year in their subtitle; the rest (and "More ...") after them as they came.
        return new(title, [.. nodes.OrderBy(r => r.Node!.Kind == BrowseKind.Section ? 1 : 0).ThenByDescending(r => Year(r.Node!.Subtitle) ?? int.MinValue)], []);
    }
    private static string Without(string? subtitle, string artist) =>
        string.Join(" · ", (subtitle ?? "").Split(" · ").Where(p => p.Length > 0 && p != artist));
    private static int? Year(string? subtitle) =>
        (subtitle ?? "").Split(" · ").Select(p => p.Length == 4 && int.TryParse(p, out var y) ? y : (int?)null).FirstOrDefault(y => y is not null);
}
