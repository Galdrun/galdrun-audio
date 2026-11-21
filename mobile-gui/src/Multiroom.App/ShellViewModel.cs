using Avalonia.Threading;
using Avalonia.Controls;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using Multiroom.App.Api;

namespace Multiroom.App;

public partial class ShellViewModel : ObservableObject
{
    private ControllerSession? _session;
    private CancellationTokenSource _page = new();
    private readonly SemaphoreSlim _writes = new(1);
    private int _pending;
    private bool _initialized;
    private string? _connectedAddress;
    private string? _connectedServerId;
    private readonly SemaphoreSlim _settings = new(1);
    private readonly string _preferencesPath;
    // The preferences as last read or written.
    private Preferences? _stored;
    private readonly Func<TimeSpan, CancellationToken, IAsyncEnumerable<DiscoveredServer>> _discovery;
    private readonly Func<string, string?, MultiroomClient> _newClient;
    private string _beforeVolume = "Sources";
    // The page an album was opened from, for back once Browse has no history of its own.
    private string? _beforeBrowse;
    // Home's "See all" shelf as it was when Home was last left, opened again when back returns to Home.
    private HomeShelf? _homeShelf;
    // The pages left on the way here, oldest first, for back. Opening one of them again returns to it, so a page
    // is in it once at most. Home is where it starts: back on Home leaves the app.
    private readonly List<string> _history = [];
    public IReadOnlyList<string> Pages { get; } = ["Home", "Sources", "Search", "Browse", "Playlists", "Favourites", "Recommendations", "Queue", "Zones", "Rooms", "Diagnostics", "Now Playing", "Volume", "Zone Setup", "Settings", "Servers", "Server"];
    // The menu's pages: Browse is reached from Sources, Home and Search.
    public IReadOnlyList<string> MenuPages { get; } = ["Home", "Sources", "Search", "Playlists", "Favourites", "Recommendations", "Queue", "Zones", "Rooms", "Diagnostics", "Now Playing", "Volume", "Settings", "Servers", "Server"];
    [ObservableProperty] private bool _isMenuOpen;
    [RelayCommand] private void ToggleMenu() => IsMenuOpen = !IsMenuOpen;
    public PlaybackViewModel Playback { get; }
    public BrowserViewModel Browser { get; }
    public CollectionsViewModel Collections { get; }
    public RoomsViewModel Rooms { get; }
    public ServersViewModel Servers { get; }
    public HomeViewModel Home { get; }
    public ShellViewModel(Func<TimeSpan, CancellationToken, IAsyncEnumerable<DiscoveredServer>>? discovery = null,
        string? preferencesPath = null, Func<string, string?, MultiroomClient>? clientFactory = null)
    {
        _discovery = discovery ?? ServerDiscovery.BrowseAsync;
        _preferencesPath = preferencesPath ?? Preferences.DefaultPath;
        _newClient = clientFactory ?? ((address, token) => new MultiroomClient(address, token));
        Playback = new(WriteAsync);
        Browser = new(ReadAsync, () => Playback.SelectedZone, Playback.ZoneNameOf);
        Collections = new(WriteAsync, () => Playback.SelectedZone, Playback.ZoneNameOf);
        Rooms = new(WriteAsync);
        Servers = new(ConnectDiscoveredAsync, _discovery);
        Home = new(ReadAsync, () => Playback.SelectedZone, Playback.ZoneNameOf);
        Home.KeepQueueOnPlayNow = Browser.KeepQueueOnPlayNow = Collections.KeepQueueOnPlayNow = () => PlayNowKeepsQueue;
        Home.QueueChanged += () => _session?.Refresh();
        Home.PlayedIn += zone => Playback.SelectedZone = zone;
        Home.OpenRequested += (node, title) =>
        {
            _beforeBrowse = SelectedPage; SelectedPage = "Browse";
            _ = Browser.OpenNodeAsync(node, title);
        };
        Collections.QueueChanged += () => _session?.Refresh();
        Collections.Warned += message => Error = message;
        Collections.RecommendationsRequested += () => SelectedPage = "Recommendations";
        Browser.CollectionRequested += (page, media) =>
        {
            Collections.PendingMedia = media;
            SelectedPage = page;
            if (page == "Recommendations") _ = Collections.MoreLikeCommand.ExecuteAsync("pending");
        };
        Browser.QueueChanged += () => _session?.Refresh();
        Browser.PropertyChanged += (_, args) => { if (args.PropertyName is nameof(Browser.IsDetail) or nameof(Browser.IsBrowsing)) OnPropertyChanged(nameof(ShowContentHeader)); };
        Browser.SheetRequested += item => Home.OpenSheetCommand.Execute(item);
        Browser.MarksChanged += () => _ = Home.RefreshAsync();
        // Update zone closes Zone Setup.
        Playback.MembershipSaved += () => { if (IsZoneSetup && !GoBack()) SelectedPage = "Home"; };
        Playback.PropertyChanged += (_, args) =>
        {
            if (args.PropertyName == nameof(Playback.SelectedZone) && _connectedAddress is not null) _ = SavePreferencesAsync();
            if (args.PropertyName == nameof(Playback.HasZone)) OnPropertyChanged(nameof(ShowZoneBar));
            if (args.PropertyName == nameof(Playback.CurrentMarkItem) && IsNowPlaying && _session is not null) _ = RefreshCurrentMarksAsync();
        };
    }
    [ObservableProperty] private string _selectedPage = "Servers";
    [ObservableProperty] private string _address = "http://127.0.0.1:4725";
    [ObservableProperty] private string _token = "";
    [ObservableProperty] private string _connection = "Not connected";
    [ObservableProperty] private string _error = "";
    [ObservableProperty, NotifyPropertyChangedFor(nameof(ShowTopProgress))] private bool _isBusy;
    [ObservableProperty] private bool _isWide;
    [ObservableProperty] private bool _isCompact;
    [ObservableProperty] private bool _connected;
    [ObservableProperty] private IReadOnlyList<ProviderView> _providers = [];
    [ObservableProperty] private ProviderView? _selectedProvider;
    public string Heading => SelectedPage;
    public bool IsHome => SelectedPage == "Home";
    public bool IsSettings => SelectedPage == "Server";
    public bool IsServers => SelectedPage == "Servers";
    // The two chooser pages share the plain fullscreen chrome: GALDRUN only on
    // top, no navigation, no bottom bar.
    public bool IsSetup => IsServers || IsSettings;
    public bool IsSources => SelectedPage == "Sources";
    public bool IsQueue => SelectedPage == "Queue";
    public bool IsZones => SelectedPage == "Zones";
    public bool IsNowPlaying => SelectedPage == "Now Playing";
    // An album page starts with its cover, the volume page with its rooms: no brand, connection, heading or refresh above it.
    public bool ShowContentHeader => !IsHome && !IsNowPlaying && !IsVolume && !IsZoneSetup && !IsQueue && !(IsBrowser && !Browser.IsBrowsing);
    // Not on Now Playing: its transport buttons and the waking pill show what it waits for.
    public bool ShowTopProgress => IsBusy && _busyShown && !IsNowPlaying;
    public bool IsVolume => SelectedPage == "Volume";
    // The selected zone's rooms, opened from the zone's name on the zone bar.
    public bool IsZoneSetup => SelectedPage == "Zone Setup";
    // The chooser pages are pure pickers/forms: the bottom bar never shows
    // there, connected or not.
    // Zone Setup covers it (MainView), rather than hiding it, so Now Playing beneath keeps its place while Zone Setup rises.
    public bool ShowZoneBar => Playback.HasZone && !IsZones && !IsVolume && !IsSetup;
    public bool IsSearch => SelectedPage == "Search";
    public bool IsBrowser => SelectedPage is "Browse" or "Search";
    public bool IsCollections => SelectedPage is "Playlists" or "Favourites" or "Recommendations";
    public bool IsRooms => SelectedPage == "Rooms";
    public bool IsDiagnostics => SelectedPage == "Diagnostics";
    // The app's own settings ("Server" is the manual connection form).
    public bool IsAppSettings => SelectedPage == "Settings";
    // The app's CPU use over the page (PerformanceView), saved with the preferences.
    [ObservableProperty] private bool _showPerformance;
    partial void OnShowPerformanceChanged(bool value) { if (value != (_stored?.ShowPerformance ?? false)) _ = SavePreferencesAsync(); }
    // What "Play now" does (PlayNow): off replaces the zone's queue, on puts the music after what plays and plays it.
    [ObservableProperty] private bool _playNowKeepsQueue;
    partial void OnPlayNowKeepsQueueChanged(bool value) { if (value != (_stored?.PlayNowKeepsQueue ?? false)) _ = SavePreferencesAsync(); }
    public bool ShowSideQueue => IsWide && !IsQueue;
    public bool HasError => Error.Length != 0;
    // The connecting page: over everything while the app searches for its server, at a start and whenever the
    // connection drops (SearchAsync). After SplashTime it offers Manual; the search goes on until a server is
    // connected, the picker opens or Manual is chosen.
    public TimeSpan SplashTime { get; set; } = TimeSpan.FromSeconds(4);
    // One browse of the search, and the least time between two (a browse that fails at once must not spin).
    public TimeSpan SearchRound { get; set; } = TimeSpan.FromSeconds(2);
    public TimeSpan SearchRest { get; set; } = TimeSpan.FromSeconds(1);
    // A remembered server that does not announce itself is tried at its last address this often.
    public TimeSpan StoredRetry { get; set; } = TimeSpan.FromSeconds(10);
    [ObservableProperty] private bool _isConnecting;
    [ObservableProperty] private bool _showManual;
    // What the connecting page says: the server it connects to, or that it searches for one.
    [ObservableProperty] private string _connectingText = "Searching for server…";
    // How long connecting may take before it fails (the HTTP client itself waits as long as the system lets it).
    public TimeSpan ConnectTimeout { get; set; } = TimeSpan.FromSeconds(10);
    private CancellationTokenSource? _search;
    private IDisposable? _manualTimer;
    // The page left for Manual while a lost connection was waited for; it opens again once the connection is back.
    private string? _lostFrom;
    // The search Manual interrupted, resumed when back leaves the Manual page.
    private (string? ServerId, bool Startup) _searching;
    private (string? ServerId, bool Startup)? _resumeSearch;
    private void StopSearch()
    {
        _search?.Cancel(); _search = null;
        _manualTimer?.Dispose(); _manualTimer = null;
        IsConnecting = false; ShowManual = false;
    }
    [RelayCommand] private void OpenManual()
    {
        var from = _session is not null ? SelectedPage : null;
        var resume = _search is not null ? _searching : ((string?, bool)?)null;
        StopSearch();
        SelectedPage = "Server"; _lostFrom = from; _resumeSearch = resume;
    }

    // Searches the network for Galdrun servers until the search ends (StopSearch), behind the connecting page.
    // serverId: the server to find again (remembered, or the one whose connection dropped); it is connected as soon
    // as it answers. At a start, a remembered server that never answers is also tried at its last address; with no
    // server remembered, once SplashTime has passed one server that answered is connected and several open the
    // picker. After a drop with no id (a manual address) nothing is browsed: the session reconnects by itself.
    private async Task SearchAsync(string? serverId, bool startup)
    {
        _search?.Cancel(); _manualTimer?.Dispose();
        var search = _search = new CancellationTokenSource();
        _searching = (serverId, startup);
        var ct = search.Token;
        IsConnecting = true; ShowManual = false;
        ConnectingText = serverId is not null ? $"Connecting to {DiscoveredServer.DisplayName(serverId)}…"
            : startup ? "Searching for server…" : $"Connecting to {HostOf(_connectedAddress ?? Address)}…";
        _manualTimer = DispatcherTimer.RunOnce(() => { if (_search == search) ShowManual = true; }, SplashTime);
        var clock = System.Diagnostics.Stopwatch.StartNew();
        TimeSpan? triedStored = null;
        var found = new List<DiscoveredServer>();
        static bool Same(DiscoveredServer server, string id) => server.Instance.Equals(id, StringComparison.OrdinalIgnoreCase);
        while (!ct.IsCancellationRequested)
        {
            var round = System.Diagnostics.Stopwatch.StartNew();
            DiscoveredServer? match = null;
            if (serverId is not null || startup)
                try
                {
                    await foreach (var server in _discovery(SearchRound, ct))
                    {
                        if (serverId is not null) { if (Same(server, serverId)) { match = server; break; } continue; }
                        found.RemoveAll(other => Same(other, server.Instance)); found.Add(server);
                        if (clock.Elapsed >= SplashTime) break;
                    }
                }
                catch (Exception e) when (e is OperationCanceledException or System.Net.Sockets.SocketException or ObjectDisposedException) { }
            if (ct.IsCancellationRequested) return;
            if (match is not null) await ConnectDiscoveredAsync(match);
            else if (serverId is null && startup && found.Count > 0 && clock.Elapsed >= SplashTime)
            {
                if (found.Count > 1)
                {
                    StopSearch();
                    Servers.Show(found);
                    SelectedPage = "Servers";
                    return;
                }
                await ConnectDiscoveredAsync(found[0]);
                found.Clear();
            }
            else if (serverId is not null && startup && clock.Elapsed >= SplashTime
                && (triedStored is not { } at || clock.Elapsed - at >= StoredRetry))
            { triedStored = clock.Elapsed; await ConnectCoreAsync(serverId); }
            if (_search != search) return; // connected, or the search was replaced
            var rest = SearchRest - round.Elapsed;
            if (rest > TimeSpan.Zero)
                try { await Task.Delay(rest, ct); } catch (OperationCanceledException) { return; }
        }
    }
    public bool CanBrowseProvider => SelectedProvider is { State: ProviderState.Ready, Capabilities: { } caps } && caps.Contains("browse");
    public bool NoSources => Providers.Count == 0;
    // An error shows as a toast over the page for a few seconds; Error itself stays until the next request.
    public TimeSpan ErrorShown { get; set; } = TimeSpan.FromSeconds(5);
    [ObservableProperty] private bool _showError;
    // Pending news from the server, behind the header bar's info icon instead of in a page.
    [ObservableProperty, NotifyPropertyChangedFor(nameof(HasInfo))] private string _info = "";
    public bool HasInfo => Info.Length != 0;
    public const string LibraryUpdating = "Your library is being updated. New music shows up by itself once it is done.";
    // The indexer's count of new or changed files and its passes, as last seen (none before the first
    // indexer.progress), and while an update shows, the pass that must end before the pages are read again.
    private long? _libraryChanges;
    private long _libraryPasses;
    private long? _libraryUntilPass;

    // New or changed music shows the info; once the indexer has finished a pass after that with no folder
    // waiting, the open page is read again by itself and the info goes.
    public void LibraryProgress(IndexerStatus status)
    {
        var changes = status.Indexed + status.Failed;
        if (changes > (_libraryChanges ?? changes)) { _libraryUntilPass ??= _libraryPasses; Info = LibraryUpdating; }
        _libraryChanges = changes;
        if (_libraryUntilPass is { } pass && status.Passes > pass && status.QueueDepth == 0)
        {
            _libraryUntilPass = null; Info = "";
            RefreshLibraryPage();
        }
        _libraryPasses = status.Passes;
    }
    // Home and Browse show the library; other pages read it when opened.
    private void RefreshLibraryPage() { if (IsHome || (IsBrowser && !Browser.IsLoading)) _ = RefreshAsync(); }
    private IDisposable? _hideError;
    partial void OnErrorChanged(string value)
    {
        OnPropertyChanged(nameof(HasError));
        _hideError?.Dispose(); _hideError = null;
        ShowError = value.Length != 0;
        if (ShowError) _hideError = DispatcherTimer.RunOnce(() => ShowError = false, ErrorShown);
    }
    // The busy dot shows once a request takes a moment, so quick ones never flash it.
    public TimeSpan BusyDelay { get; set; } = TimeSpan.FromMilliseconds(300);
    private bool _busyShown;
    private IDisposable? _showBusy;
    partial void OnIsBusyChanged(bool value)
    {
        _showBusy?.Dispose(); _showBusy = null;
        void Show(bool shown) { _busyShown = shown; OnPropertyChanged(nameof(ShowTopProgress)); }
        if (value && BusyDelay > TimeSpan.Zero) _showBusy = DispatcherTimer.RunOnce(() => Show(true), BusyDelay);
        else Show(value);
    }
    partial void OnSelectedProviderChanged(ProviderView? value) => OnPropertyChanged(nameof(CanBrowseProvider));
    partial void OnProvidersChanged(IReadOnlyList<ProviderView> value) => OnPropertyChanged(nameof(NoSources));
    partial void OnIsWideChanged(bool value) => OnPropertyChanged(nameof(ShowSideQueue));
    partial void OnSelectedPageChanging(string? oldValue, string newValue) { if (oldValue == "Home") _homeShelf = Home.OpenShelf; }
    partial void OnSelectedPageChanged(string? oldValue, string newValue)
    {
        var index = _history.LastIndexOf(newValue);
        if (newValue == "Home") _history.Clear();
        else if (index >= 0) _history.RemoveRange(index, _history.Count - index);
        else if (oldValue is not null) _history.Add(oldValue);
    }
    partial void OnSelectedPageChanged(string value)
    {
        if (value != "Browse") _beforeBrowse = null;
        if (value != "Server") { _lostFrom = null; _resumeSearch = null; }
        IsMenuOpen = false;
        _page.Cancel(); _page.Dispose(); _page = new(); Browser.Cancel(); Browser.SetSearchVisible(IsSearch); Playback.CancelMarks();
        foreach (var property in new[] { nameof(Heading), nameof(IsHome), nameof(IsSettings), nameof(IsSources), nameof(IsQueue),
            nameof(IsZones), nameof(IsBrowser), nameof(IsSearch), nameof(IsNowPlaying), nameof(ShowSideQueue),
            nameof(IsCollections), nameof(IsRooms), nameof(IsDiagnostics), nameof(IsAppSettings), nameof(IsVolume), nameof(IsZoneSetup), nameof(IsServers),
            nameof(IsSetup), nameof(ShowZoneBar), nameof(ShowContentHeader), nameof(ShowTopProgress) }) OnPropertyChanged(property);
        if (IsCollections) Collections.Page = value;
        if (!IsHome) { Home.OpenShelf = null; Home.CloseSheet(); }
        if (IsServers && !Servers.IsScanning && Servers.Servers.Count == 0) _ = Servers.ScanAsync();
        if (_session is not null && (IsHome || value is "Playlists" or "Favourites" || IsRooms || IsDiagnostics)) _ = RefreshAsync();
        if (_session is not null && IsNowPlaying) _ = RefreshCurrentMarksAsync();
    }
    public void SetWidth(double width) { IsWide = width >= 1200; IsCompact = width < 700; }
    [RelayCommand] private void Navigate(string page) { if (Pages.Contains(page)) SelectedPage = page; }
    [RelayCommand] private void OpenVolume()
    {
        if (!Playback.HasZone) return;
        _beforeVolume = SelectedPage; SelectedPage = "Volume";
    }
    [RelayCommand] private void CloseVolume() => SelectedPage = _beforeVolume;
    // The system back (Android's, or a right click on the desktop) steps out of what is open, innermost first;
    // false leaves it to the system.
    [RelayCommand] private void Back() => GoBack();
    public bool GoBack()
    {
        // The Manual page opened from the connecting page goes back to it: the search goes on where it stopped.
        if (IsSettings && _resumeSearch is { } resume)
        {
            var page = _lostFrom ?? "Servers";
            SelectedPage = page;
            _ = SearchAsync(resume.ServerId, resume.Startup);
            return true;
        }
        if (IsMenuOpen) { IsMenuOpen = false; return true; }
        if (Home.IsSheetOpen) { if (Home.IsActionsStep) Home.CloseSheet(); else Home.SheetBackCommand.Execute(null); return true; }
        if (IsVolume) { CloseVolume(); return true; }
        if (IsBrowser && Browser.CanBack) { Browser.BackCommand.Execute(null); return true; }
        if (IsBrowser && _beforeBrowse is { } opener) { BackTo(opener); return true; }
        if (IsHome && Home.IsShelfOpen) { Home.CloseShelfCommand.Execute(null); return true; }
        if (_history.Count > 0) { BackTo(_history[^1]); return true; }
        return false;
    }
    private void BackTo(string page)
    {
        var shelf = _homeShelf;
        SelectedPage = page;
        if (page == "Home") Home.OpenShelf = shelf;
    }

    // The Manual page's Connect: the address as typed (a bare host or IP is the server's HTTP address on its usual
    // API port), connected behind the connecting page, which goes once it worked or failed.
    [RelayCommand]
    private async Task ConnectAsync()
    {
        Address = ManualAddress(Address);
        ConnectingText = $"Connecting to {HostOf(Address)}…"; ShowManual = false; IsConnecting = true;
        try { await ConnectCoreAsync(null); }
        finally { IsConnecting = false; }
    }
    public const int DefaultPort = 4725;
    public static string ManualAddress(string text)
    {
        var address = text.Trim();
        if (address.Length == 0 || address.Contains("://", StringComparison.Ordinal)
            || !Uri.TryCreate("http://" + address, UriKind.Absolute, out var uri)) return address;
        var builder = new UriBuilder(uri);
        if (!System.Text.RegularExpressions.Regex.IsMatch(address, @"^(\[[^\]]*\]|[^/:]+):\d+")) builder.Port = DefaultPort;
        return builder.Uri.AbsoluteUri;
    }
    private static string HostOf(string address) => Uri.TryCreate(address, UriKind.Absolute, out var uri) ? uri.Host : address;

    // A picked server is connected by its announced address and remembered by
    // its instance id; a manual address connects the way the Server page
    // always did, without an id to reconnect to.
    private async Task ConnectDiscoveredAsync(DiscoveredServer server)
    {
        Address = server.BaseUri.AbsoluteUri;
        await ConnectCoreAsync(server.Instance);
    }

    private async Task ConnectCoreAsync(string? serverId)
    {
        await RunAsync(async ct =>
        {
            _connectedServerId = serverId;
            var client = _newClient(Address, Token);
            _connectedAddress = null;
            var previous = _session; _session = null;
            if (previous is not null) await previous.DisposeAsync();
            Browser.Artwork?.Dispose(); Browser.Reset(); Playback.Apply(new([], [])); Collections.Reset(); Rooms.Reset(); Home.Reset();
            var session = new ControllerSession(client); _session = session;
            // Each server keeps its pictures in its own folder beside the preferences; ids are unique per server only.
            var artworkFolder = Path.Combine(Path.GetDirectoryName(_preferencesPath)!, "Artwork", Convert.ToHexStringLower(
                System.Security.Cryptography.SHA256.HashData(System.Text.Encoding.UTF8.GetBytes(serverId ?? client.BaseUri.Authority)))[..16]);
            Browser.Artwork = new ArtworkCache(client, artworkFolder); Playback.Artwork = Browser.Artwork;
            Connection = "Connecting…"; Connected = false;
            // A server that does not answer fails after ConnectTimeout, reported as too slow (RunAsync).
            using var timeout = CancellationTokenSource.CreateLinkedTokenSource(ct);
            timeout.CancelAfter(ConnectTimeout);
            var status = await client.StatusAsync(timeout.Token);
            Providers = await client.ProvidersAsync(timeout.Token);
            ct.ThrowIfCancellationRequested();
            Connection = status.Core.Connected ? "Connected" : "Server online · audio core offline"; Connected = true;
            _connectedAddress = client.BaseUri.AbsoluteUri;
            session.ConnectionChanged += online => Dispatch(session, () =>
            {
                // The stream reports every failed retry; the search starts once, when the connection goes. The session
                // keeps reconnecting meanwhile; the search finds the server again if it comes back at another address.
                var lost = !online && Connected;
                Connected = online; Connection = online ? "Connected" : "Reconnecting… · showing last state";
                if (lost) _ = SearchAsync(_connectedServerId, startup: false);
                else if (online)
                {
                    StopSearch();
                    if (_lostFrom is { } page) { _lostFrom = null; SelectedPage = page; }
                }
            });
            session.RefreshFailed += message => Dispatch(session, () => Error = message);
            session.PlaybackChanged += state => Dispatch(session, () => { Playback.Apply(state); Rooms.Apply(state); });
            session.ProvidersChanged += providers => Dispatch(session, () => Providers = providers);
            var artwork = Browser.Artwork;
            session.ArtworkFound += id => _ = artwork.AnnounceAsync(id);
            session.IndexerChanged += status => Dispatch(session, () => LibraryProgress(status));
            session.ContentInvalidated += type =>
            {
                if (type is "resync" or "events.lost" or "core.eventsLost" or "core.synchronised")
                    Dispatch(session, () =>
                    {
                        if (IsBrowser && !Browser.IsLoading) _ = Browser.RefreshAsync();
                        if (IsNowPlaying) _ = RefreshCurrentMarksAsync();
                    });
                else if (type is "like.changed" or "favorite.changed" or "proposals.changed" or "proposals.running")
                    Dispatch(session, () =>
                    {
                        if (IsNowPlaying && type is "like.changed" or "favorite.changed") _ = RefreshCurrentMarksAsync();
                        if (IsHome) _ = Home.RefreshAsync();
                    });
                else if (type == "library.folderChanged")
                    Dispatch(session, RefreshLibraryPage);
            };
            session.Start();
            await SavePreferencesAsync();
            SelectedPage = "Home";
            StopSearch();
        });
    }
    private void Dispatch(ControllerSession session, Action action) => Dispatcher.UIThread.Post(() => { if (_session == session) action(); });

    [RelayCommand] private async Task BrowseProviderAsync()
    {
        if (!CanBrowseProvider || SelectedProvider is not { } provider) return;
        SelectedPage = "Browse";
        await Browser.OpenProviderAsync(provider);
    }
    [RelayCommand] private Task RefreshAsync()
    {
        if (IsHome) return Home.RefreshAsync();
        if (IsBrowser) return Browser.RefreshAsync();
        if (IsCollections) return Collections.RefreshAsync();
        if (IsRooms || IsDiagnostics) return Rooms.RefreshAsync(IsDiagnostics);
        return ReadAsync(async (api, ct) => { Providers = await api.ProvidersAsync(ct); _session?.Refresh(); });
    }

    private async Task RunAsync(Func<CancellationToken, Task> action)
    {
        var ct = _page.Token;
        _pending++; IsBusy = true; Error = "";
        try { await action(ct); }
        catch (OperationCanceledException) { if (!ct.IsCancellationRequested) Error = "The server took too long to respond. Try again."; }
        catch (ApiException e) { if (!ct.IsCancellationRequested) { Error = e.Message; if (e.Status == System.Net.HttpStatusCode.Unauthorized) Connection = "Authorization required"; } }
        catch (ArgumentException e) { if (!ct.IsCancellationRequested) Error = e.Message; }
        catch (HttpRequestException) { if (!ct.IsCancellationRequested) { Error = "Cannot reach the server. Check its address and network connection."; Connection = "Unavailable"; } }
        finally { IsBusy = --_pending != 0; }
    }

    private Task ReadAsync(Func<MultiroomClient, CancellationToken, Task> action) => RunAsync(async ct =>
    {
        var session = _session;
        if (session is null) { SelectedPage = "Servers"; return; }
        await action(session.Client, ct);
    });
    private Task RefreshCurrentMarksAsync() => ReadAsync(Playback.RefreshMarksAsync);
    private Task WriteAsync(Func<MultiroomClient, CancellationToken, Task> action) => ReadAsync(async (api, ct) =>
    {
        await _writes.WaitAsync(ct);
        try { await action(api, ct); }
        finally { _writes.Release(); _session?.Refresh(); }
    });
    public async Task DisconnectAsync()
    {
        _connectedAddress = null;
        Servers.Cancel();
        Playback.CancelMarks();
        _page.Cancel(); _page.Dispose(); _page = new(); Browser.Cancel(); Browser.Artwork?.Dispose();
        var previous = _session; _session = null;
        if (previous is not null) await previous.DisposeAsync();
        StopSearch(); _lostFrom = null;
        Connection = "Not connected"; Connected = false; Info = ""; _libraryChanges = null; _libraryUntilPass = null;
    }
    public async Task InitializeAsync()
    {
        if (_initialized) return;
        _initialized = true;
        Preferences? preferences = null;
        try
        {
            preferences = await Preferences.LoadAsync(_preferencesPath, CancellationToken.None);
            if (preferences is not null)
            {
                _stored = preferences;
                Address = MultiroomClient.NormalizeAddress(preferences.Address).AbsoluteUri;
                Playback.PreferredZone = preferences.ActiveZone;
                ShowPerformance = preferences.ShowPerformance;
                PlayNowKeepsQueue = preferences.PlayNowKeepsQueue;
            }
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or System.Text.Json.JsonException or ArgumentException)
        { Error = "Saved preferences could not be read. Enter the server address to reconnect."; }
        // A remembered server is found again by its id; a first start connects the one server found, or opens the
        // picker for several. The search runs on by itself, behind the connecting page.
        _ = SearchAsync(preferences?.ServerId, startup: true);
    }
    private async Task SavePreferencesAsync()
    {
        // Not connected, a setting changes alone: the server remembered stays.
        var settings = _connectedAddress is { } address
            ? new Preferences(address, Playback.SelectedZone ?? Playback.PreferredZone, _connectedServerId, ShowPerformance, PlayNowKeepsQueue)
            : (_stored ?? new Preferences(Address)) with { ShowPerformance = ShowPerformance, PlayNowKeepsQueue = PlayNowKeepsQueue };
        await _settings.WaitAsync();
        try { await settings.SaveAsync(_preferencesPath, CancellationToken.None); _stored = settings; }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        { Error = _connectedAddress is null ? "Settings could not be saved." : "Connected, but server preferences could not be saved."; }
        finally { _settings.Release(); }
    }
}
