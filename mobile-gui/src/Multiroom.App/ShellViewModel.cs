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
    private readonly Func<TimeSpan, CancellationToken, IAsyncEnumerable<DiscoveredServer>> _discovery;
    private readonly Func<string, string?, MultiroomClient> _newClient;
    private string _beforeVolume = "Sources";
    public IReadOnlyList<string> Pages { get; } = ["Home", "Sources", "Search", "Browse", "Playlists", "Favourites", "Recommendations", "Queue", "Zones", "Rooms", "Diagnostics", "Now Playing", "Volume", "Servers", "Server"];
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
        Browser = new(ReadAsync, () => Playback.SelectedZone);
        Collections = new(WriteAsync, () => Playback.SelectedZone);
        Rooms = new(WriteAsync);
        Servers = new(ConnectDiscoveredAsync, _discovery);
        Home = new(ReadAsync, () => Playback.SelectedZone);
        Home.QueueChanged += () => _session?.Refresh();
        Collections.QueueChanged += () => _session?.Refresh();
        Collections.RecommendationsRequested += () => SelectedPage = "Recommendations";
        Browser.CollectionRequested += (page, media) =>
        {
            Collections.PendingMedia = media;
            SelectedPage = page;
            if (page == "Recommendations") _ = Collections.MoreLikeCommand.ExecuteAsync("pending");
        };
        Browser.QueueChanged += () => _session?.Refresh();
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
    [ObservableProperty, NotifyPropertyChangedFor(nameof(ShowTopProgress)), NotifyPropertyChangedFor(nameof(ShowArtworkProgress))] private bool _isBusy;
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
    public bool ShowContentHeader => !IsHome && !IsNowPlaying;
    public bool ShowTopProgress => IsBusy && !IsNowPlaying;
    public bool ShowArtworkProgress => IsBusy && IsNowPlaying;
    public bool IsVolume => SelectedPage == "Volume";
    // The chooser pages are pure pickers/forms: the bottom bar never shows
    // there, connected or not.
    public bool ShowZoneBar => Playback.HasZone && !IsZones && !IsVolume && !IsSetup;
    public bool IsSearch => SelectedPage == "Search";
    public bool IsBrowser => SelectedPage is "Browse" or "Search";
    public bool IsCollections => SelectedPage is "Playlists" or "Favourites" or "Recommendations";
    public bool IsRooms => SelectedPage == "Rooms";
    public bool IsDiagnostics => SelectedPage == "Diagnostics";
    public bool ShowSideQueue => IsWide && !IsQueue;
    public bool HasError => Error.Length != 0;
    public bool CanBrowseProvider => SelectedProvider is { State: ProviderState.Ready, Capabilities: { } caps } && caps.Contains("browse");
    public bool NoSources => Providers.Count == 0;
    partial void OnErrorChanged(string value) => OnPropertyChanged(nameof(HasError));
    partial void OnSelectedProviderChanged(ProviderView? value) => OnPropertyChanged(nameof(CanBrowseProvider));
    partial void OnProvidersChanged(IReadOnlyList<ProviderView> value) => OnPropertyChanged(nameof(NoSources));
    partial void OnIsWideChanged(bool value) => OnPropertyChanged(nameof(ShowSideQueue));
    partial void OnSelectedPageChanged(string value)
    {
        _page.Cancel(); _page.Dispose(); _page = new(); Browser.Cancel(); Browser.SetSearchVisible(IsSearch); Playback.CancelMarks();
        foreach (var property in new[] { nameof(Heading), nameof(IsHome), nameof(IsSettings), nameof(IsSources), nameof(IsQueue),
            nameof(IsZones), nameof(IsBrowser), nameof(IsSearch), nameof(IsNowPlaying), nameof(ShowSideQueue),
            nameof(IsCollections), nameof(IsRooms), nameof(IsDiagnostics), nameof(IsVolume), nameof(IsServers),
            nameof(IsSetup), nameof(ShowZoneBar), nameof(ShowContentHeader), nameof(ShowTopProgress), nameof(ShowArtworkProgress) }) OnPropertyChanged(property);
        if (IsCollections) Collections.Page = value;
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

    [RelayCommand]
    private Task ConnectAsync() => ConnectCoreAsync(null);

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
            Browser.Artwork = new ArtworkCache(client); Playback.Artwork = Browser.Artwork;
            Connection = "Connecting…"; Connected = false;
            var status = await client.StatusAsync(ct);
            Providers = await client.ProvidersAsync(ct);
            ct.ThrowIfCancellationRequested();
            Connection = status.Core.Connected ? "Connected" : "Server online · audio core offline"; Connected = true;
            _connectedAddress = client.BaseUri.AbsoluteUri;
            session.ConnectionChanged += online => Dispatch(session, () =>
            { Connected = online; Connection = online ? "Connected" : "Reconnecting… · showing last state"; });
            session.RefreshFailed += message => Dispatch(session, () => Error = message);
            session.PlaybackChanged += state => Dispatch(session, () => { Playback.Apply(state); Rooms.Apply(state); });
            session.ProvidersChanged += providers => Dispatch(session, () => Providers = providers);
            var artwork = Browser.Artwork;
            session.ArtworkFound += id => _ = artwork.AnnounceAsync(id);
            session.ContentInvalidated += type =>
            {
                if (type is "resync" or "events.lost" or "core.eventsLost" or "core.synchronised")
                    Dispatch(session, () =>
                    {
                        if (IsBrowser && !Browser.IsLoading) _ = Browser.RefreshAsync();
                        if (IsNowPlaying) _ = RefreshCurrentMarksAsync();
                    });
                else if (type == "like.changed")
                    Dispatch(session, () => { if (IsNowPlaying) _ = RefreshCurrentMarksAsync(); });
                else if (type is "indexer.progress" or "library.folderChanged")
                    Dispatch(session, () => Browser.Notice = "Your library is updating. Refresh for the latest music.");
            };
            session.Start();
            await SavePreferencesAsync();
            SelectedPage = "Home";
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
        Connection = "Not connected"; Connected = false;
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
                Address = MultiroomClient.NormalizeAddress(preferences.Address).AbsoluteUri;
                Playback.PreferredZone = preferences.ActiveZone;
            }
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or System.Text.Json.JsonException or ArgumentException)
        { Error = "Saved preferences could not be read. Enter the server address to reconnect."; }
        // A remembered server is connected by its id; a first start opens the picker.
        if (preferences?.ServerId is { } id) { await ConnectStoredAsync(id); return; }
        await Servers.ScanAsync();
    }

    // The stored id is resolved through the same mDNS browse the picker uses;
    // when no announcement answers in time, the last address that worked is
    // tried under the same id so a missing announcement neither blocks startup
    // nor forgets the chosen server. The picker stays the first page and says
    // which server did not answer.
    private async Task ConnectStoredAsync(string serverId)
    {
        Connection = "Looking for your Galdrun server…";
        if (await Servers.FindAsync(serverId, TimeSpan.FromSeconds(10)) is { } server)
        {
            await ConnectDiscoveredAsync(server);
            return;
        }
        Servers.Warning = $"Server {DiscoveredServer.DisplayName(serverId)} was not found.";
        await ConnectCoreAsync(serverId);
    }
    private async Task SavePreferencesAsync()
    {
        if (_connectedAddress is not { } address) return;
        var settings = new Preferences(address, Playback.SelectedZone ?? Playback.PreferredZone, _connectedServerId);
        await _settings.WaitAsync();
        try { await settings.SaveAsync(_preferencesPath, CancellationToken.None); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        { Error = "Connected, but server preferences could not be saved."; }
        finally { _settings.Release(); }
    }
}
