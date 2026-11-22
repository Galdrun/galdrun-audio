using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using Multiroom.App.Api;

namespace Multiroom.App;

// What the Search page looks for: the server's search types and the media type it filters the tracks by.
public sealed record SearchKind(string Label, string Types, string? MediaType, string Placeholder);

// Where the Search page looks: every source (Id null) or one provider.
public sealed record SearchScope(string? Id, string Label)
{
    // Its icon in a 24 box, drawn in the text colour: lines, or a filled shape (IsFilled). Tidal and TuneIn have their
    // own marks (Tidal's four diamonds, TuneIn's "in" label), the others generic shapes.
    public string Icon => Id switch
    {
        null => "M 4,4 H 10 V 10 H 4 Z M 14,4 H 20 V 10 H 14 Z M 4,14 H 10 V 20 H 4 Z M 14,14 H 20 V 20 H 14 Z", // all: a grid
        "local" => "M 3,6 L 10,6 L 12,8.5 L 21,8.5 L 21,19 L 3,19 Z", // files: a folder
        "webradio" => "M 3,9 L 21,9 L 21,20 L 3,20 Z M 6,9 L 17,3.5 M 12,14.5 A 2.5,2.5 0 1 0 7,14.5 A 2.5,2.5 0 1 0 12,14.5 M 15,13 L 18,13 M 15,16.5 L 18,16.5", // a radio set
        "tunein" => "M 3,4.5 L 21,4.5 L 21,18 L 8,18 L 4.5,21 L 4.5,18 L 3,18 Z M 8.5,8 L 8.5,8.4 M 8.5,10.5 L 8.5,15 M 12,10.5 L 12,15 M 12,12.5 C 12,9.8 16.5,9.8 16.5,12.5 L 16.5,15", // "in" in a label box
        "tidal" => "M 4.5,6 L 8,9.5 L 4.5,13 L 1,9.5 Z M 12,6 L 15.5,9.5 L 12,13 L 8.5,9.5 Z M 19.5,6 L 23,9.5 L 19.5,13 L 16,9.5 Z M 12,13.5 L 15.5,17 L 12,20.5 L 8.5,17 Z", // four diamonds
        _ => "M 7,18 A 4,4 0 0 1 6.5,10 A 6,6 0 0 1 17.5,9.5 A 4.3,4.3 0 0 1 17,18 Z", // another service: a cloud
    };
    public bool IsFilled => Id == "tidal";
    public Avalonia.Media.Geometry IconGeometry => Avalonia.Media.StreamGeometry.Parse(Icon);
    public Avalonia.Media.IBrush? IconFill => IsFilled ? Avalonia.Media.Brush.Parse("#EEF1ED") : null;
    public double IconStroke => IsFilled ? 0 : 1.7;
}

// One result: a track, station, album or artist as Home shows it (tap: a track or station's sheet, an album or
// artist opened in Browse), or a playlist, opened in Browse.
public sealed record SearchRow(string Title, string Subtitle, string? Artwork, HomeItem? Item, BrowseNode? Node, ArtworkCache? Cache,
    string Source = "")
{
    public bool HasSource => Source.Length > 0;
    public bool HasSubtitle => Subtitle.Length > 0;
    public NodeReference? Opens => Item?.Items.Node ?? (Node is { } n ? new(n.ProviderId, n.NodeId) : null);
}

// The Search page: one kind at a time (the tabs), searched as the query is typed.
public sealed partial class SearchViewModel(Func<Func<MultiroomClient, CancellationToken, Task>, Task> execute) : ObservableObject
{
    private const int Page = 60;
    // Recent searches kept per kind.
    private const int RecentCount = 8;
    public static SearchKind Tracks { get; } = new("Tracks", "tracks", "music", "Search tracks");
    public static SearchKind Artists { get; } = new("Artists", "artists", null, "Search artists");
    public static SearchKind Albums { get; } = new("Albums", "albums", null, "Search albums");
    // The kinds behind the "…" tab.
    public static SearchKind Radio { get; } = new("Radio", "stations", null, "Search radio stations");
    // Whole books (the server answers them as albums whose id is the book's node), not their parts.
    public static SearchKind Audiobooks { get; } = new("Audiobooks", "books", null, "Search audiobooks");
    public static SearchKind Playlists { get; } = new("Playlists", "playlists", null, "Search playlists");
    public static IReadOnlyList<SearchKind> MoreKinds { get; } = [Radio, Audiobooks, Playlists];

    private CancellationTokenSource? _request;
    private CancellationTokenSource? _debounce;
    private bool _visible;
    private int _offset;
    // What the shown results were searched for; a refresh or "more" searches it again.
    private (string Query, SearchKind Kind, string? Source)? _shown;
    public static SearchScope AllSources { get; } = new(null, "All sources");
    // The sources to pick from: all of them, then each provider that searches.
    [ObservableProperty] private IReadOnlyList<SearchScope> _scopes = [AllSources];
    [ObservableProperty, NotifyPropertyChangedFor(nameof(IsOneSource))] private SearchScope _scope = AllSources;
    public bool IsOneSource => Scope.Id is not null;
    partial void OnScopesChanged(IReadOnlyList<SearchScope> value) { if (!value.Any(s => s.Id == Scope.Id)) Scope = AllSources; }
    partial void OnScopeChanged(SearchScope value) { if (HasQuery) _ = SearchAsync(); }
    [RelayCommand] private void ChooseScope(SearchScope? scope) { if (scope is not null) Scope = scope; }

    public event Action<NodeReference, string>? OpenRequested;
    public event Action<HomeItem>? SheetRequested;
    // The recent searches changed (to be saved).
    public event Action? RecentChanged;
    // A provider id as the user knows it ("local" -> "Local", "tidal" -> "Tidal").
    public Func<string, string> SourceName { get; set; } = id => id;
    // The last searches of each kind (by its label), the newest first.
    private Dictionary<string, IReadOnlyList<string>> _recent = new(StringComparer.Ordinal);
    public IReadOnlyDictionary<string, IReadOnlyList<string>> Recent
    {
        get => _recent;
        set { _recent = new(value, StringComparer.Ordinal); OnPropertyChanged(nameof(RecentOfKind)); OnPropertyChanged(nameof(HasRecent)); }
    }
    public IReadOnlyList<string> RecentOfKind => _recent.TryGetValue(Kind.Label, out var list) ? list : [];
    public bool HasRecent => RecentOfKind.Count > 0;
    // The recent searches show under the box while it is tapped into, until a pick, typing or leaving it.
    [ObservableProperty] private bool _isRecentOpen;
    private bool _picking;
    public ObservableCollection<SearchRow> Results { get; } = [];
    [ObservableProperty] private ArtworkCache? _artwork;
    [ObservableProperty, NotifyPropertyChangedFor(nameof(HasQuery), nameof(ShowClear))] private string _query = "";
    [ObservableProperty, NotifyPropertyChangedFor(nameof(IsTracks), nameof(IsArtists), nameof(IsAlbums), nameof(IsMore), nameof(MoreLabel), nameof(RecentOfKind), nameof(HasRecent))]
    private SearchKind _kind = Tracks;
    [ObservableProperty, NotifyPropertyChangedFor(nameof(ShowClear))] private bool _isSearching;
    [ObservableProperty] private bool _hasMore;
    // What was searched and how much was found, over the results.
    [ObservableProperty, NotifyPropertyChangedFor(nameof(HasStatus))] private string _status = "";
    // The providers that failed to answer.
    [ObservableProperty, NotifyPropertyChangedFor(nameof(HasNotice))] private string _notice = "";

    public bool IsTracks => Kind == Tracks;
    public bool IsArtists => Kind == Artists;
    public bool IsAlbums => Kind == Albums;
    public bool IsMore => MoreKinds.Contains(Kind);
    public string MoreLabel => IsMore ? Kind.Label : "…";
    public bool HasQuery => !string.IsNullOrWhiteSpace(Query);
    public bool ShowClear => HasQuery && !IsSearching;
    public bool HasStatus => Status.Length > 0;
    public bool HasNotice => Notice.Length > 0;

    partial void OnQueryChanged(string value) { if (!_picking) IsRecentOpen = false; Debounce(); }
    public void ShowRecent() => IsRecentOpen = HasRecent;
    [RelayCommand] private Task PickRecent(string? query)
    {
        if (query is null) return Task.CompletedTask;
        IsRecentOpen = false;
        _picking = true; try { Query = query; } finally { _picking = false; }
        Remember(query);
        return SearchAsync();
    }
    // A search counts as made once a result of it is opened or Enter pressed, not at each letter typed.
    private void Remember(string query)
    {
        query = query.Trim();
        if (query.Length == 0) return;
        _recent[Kind.Label] = [query, .. RecentOfKind.Where(q => !string.Equals(q, query, StringComparison.OrdinalIgnoreCase)).Take(RecentCount - 1)];
        OnPropertyChanged(nameof(RecentOfKind)); OnPropertyChanged(nameof(HasRecent));
        RecentChanged?.Invoke();
    }
    [RelayCommand] private Task Submit() { IsRecentOpen = false; Remember(Query); return SearchAsync(); }
    partial void OnKindChanged(SearchKind value) { if (HasQuery) _ = SearchAsync(); }
    [RelayCommand] private void ChooseKind(SearchKind? kind) { if (kind is not null) Kind = kind; }
    [RelayCommand] private void Clear() => Query = "";

    private void Debounce()
    {
        _debounce?.Cancel(); _debounce?.Dispose(); _debounce = null;
        if (!_visible) return;
        _request?.Cancel();
        if (!HasQuery) { Show(null); return; }
        _debounce = new();
        _ = DebouncedSearchAsync(_debounce.Token);
    }
    private async Task DebouncedSearchAsync(CancellationToken ct)
    {
        try { await Task.Delay(350, ct); await SearchAsync(); }
        catch (OperationCanceledException) { }
    }

    // Shown again (back from an album opened here): the results stay; a search cut off by leaving runs again.
    public void SetVisible(bool visible)
    {
        _visible = visible;
        if (!visible) { _debounce?.Cancel(); Cancel(); IsRecentOpen = false; }
        else if (HasQuery && _shown != (Query.Trim(), Kind, Scope.Id)) _ = SearchAsync();
    }
    public void Cancel() { _request?.Cancel(); IsSearching = false; }
    public void Reset() { Cancel(); Query = ""; _debounce?.Cancel(); Show(null); }

    private void Show((string Query, SearchKind Kind, string? Source)? shown)
    {
        _shown = shown; _offset = 0; Results.Clear(); HasMore = false; Notice = ""; Status = "";
    }

    [RelayCommand] private Task SearchAsync()
    {
        _debounce?.Cancel();
        if (!HasQuery) { Show(null); return Task.CompletedTask; }
        return LoadAsync((Query.Trim(), Kind, Scope.Id), append: false);
    }
    [RelayCommand] private Task MoreAsync() => HasMore && !IsSearching && _shown is { } shown ? LoadAsync(shown, append: true) : Task.CompletedTask;
    public Task RefreshAsync() => _shown is { } shown ? LoadAsync(shown, append: false) : Task.CompletedTask;

    private Task LoadAsync((string Query, SearchKind Kind, string? Source) search, bool append)
    {
        _request?.Cancel(); _request?.Dispose(); _request = new(); var requestToken = _request.Token;
        return execute(async (api, pageToken) =>
        {
            using var linked = CancellationTokenSource.CreateLinkedTokenSource(requestToken, pageToken);
            var ct = linked.Token;
            IsSearching = true;
            try
            {
                var (query, kind, source) = search;
                var result = await api.SearchAsync(query, append ? _offset : 0, kind.Types, kind.MediaType, source, ct);
                ct.ThrowIfCancellationRequested();
                if (!append) Show(search);
                foreach (var row in Rows(result.Items, kind)) Results.Add(row);
                _offset += Page;
                HasMore = result.Sources.Any(s => new[] { s.Tracks, s.Albums, s.Artists, s.Stations, s.Playlists }.Max() >= Page);
                Status = Results.Count == 0 ? $"Nothing found for “{query}”"
                    : $"{Results.Count}{(HasMore ? "+" : "")} {(Results.Count == 1 ? "result" : "results")} for “{query}”";
                if (source is not null) Status += $" in {SourceName(source)}";
                Notice = string.Join("\n", result.Failures.Select(f => $"{f.Provider}: {f.Error}"));
            }
            catch (OperationCanceledException) when (ct.IsCancellationRequested) { }
            finally { if (!requestToken.IsCancellationRequested) IsSearching = false; }
        });
    }

    private IEnumerable<SearchRow> Rows(CatalogItems items, SearchKind kind)
    {
        // A book is answered as an album whose id is its node: kept as a book (its sheet, its like).
        IEnumerable<MediaItem> albums = kind == Audiobooks
            ? items.Albums.Select(a => new Audiobook($"{a.ProviderId}:{a.ProviderItemId}", a.Title, a.Artist) { ArtworkId = a.ArtworkId })
            : items.Albums;
        IEnumerable<MediaItem> media = [.. items.Tracks, .. items.Stations, .. albums, .. items.Artists];
        foreach (var item in media.Select(m => HomeItem.Of(m)).OfType<HomeItem>())
            yield return new(item.Title, item.Subtitle, item.Artwork, item, null, Artwork, Source(ProviderOf(item.Media)));
        foreach (var node in items.Playlists ?? [])
            yield return new(node.Title, node.Subtitle ?? "", node.ArtworkId, null, node, Artwork, Source(node.ProviderId));
    }
    private static string? ProviderOf(MediaItem media) => media switch
    {
        Track t => t.ProviderId, RadioStation s => s.ProviderId, Album a => a.ProviderId, Artist a => a.ProviderId,
        Audiobook b when b.Reference.IndexOf(':') is var at and > 0 => b.Reference[..at], _ => null
    };
    private string Source(string? provider) => provider is null ? "" : SourceName(provider);
    private void RememberShown() { if (_shown is { } shown && shown.Kind == Kind) Remember(shown.Query); }

    // A tap opens an album, artist or playlist in Browse and gives a track or station's play choices;
    // a long press gives the play choices of all but a playlist.
    [RelayCommand] private void Open(SearchRow? row)
    {
        RememberShown();
        if (row?.Opens is { } node) OpenRequested?.Invoke(node, row.Title);
        else if (row?.Item is { } item) SheetRequested?.Invoke(item);
    }
    public void RequestActions(SearchRow row) { RememberShown(); if (row.Item is { } item) SheetRequested?.Invoke(item); }
}
