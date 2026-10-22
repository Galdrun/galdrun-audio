using System.Collections.ObjectModel;
using System.Net.Sockets;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using Multiroom.App.Api;

namespace Multiroom.App;

// The mDNS picker behind the Servers page: one browse fills the list live
// while it listens, a stored server id is looked for by the same browse just
// ending at its match, and picking a server hands it to the shell to connect
// to and remember.
public sealed partial class ServersViewModel(
    Func<DiscoveredServer, Task> connect,
    Func<TimeSpan, CancellationToken, IAsyncEnumerable<DiscoveredServer>>? browse = null) : ObservableObject
{
    private readonly Func<TimeSpan, CancellationToken, IAsyncEnumerable<DiscoveredServer>> _browse =
        browse ?? ServerDiscovery.BrowseAsync;
    private CancellationTokenSource _scan = new();

    public ObservableCollection<DiscoveredServer> Servers { get; } = [];
    [ObservableProperty] private bool _isScanning;
    [ObservableProperty] private string _scanState = "";
    [ObservableProperty] private string _warning = "";
    public bool HasServers => Servers.Count > 0;
    public bool ShowEmpty => !IsScanning && Servers.Count == 0;
    public bool HasWarning => Warning.Length > 0;
    partial void OnWarningChanged(string value) => OnPropertyChanged(nameof(HasWarning));
    partial void OnIsScanningChanged(bool value) => OnPropertyChanged(nameof(ShowEmpty));

    [RelayCommand]
    public Task ScanAsync() => BrowseAsync(TimeSpan.FromSeconds(6));

    [RelayCommand]
    private async Task ConnectServerAsync(DiscoveredServer? server)
    {
        if (server is { } chosen) await connect(chosen);
    }

    // The stored-server startup path: the same browse, stopping at the match.
    public async Task<DiscoveredServer?> FindAsync(string serverId, TimeSpan window)
    {
        await BrowseAsync(window, server => !server.Instance.Equals(serverId, StringComparison.OrdinalIgnoreCase));
        return Servers.FirstOrDefault(server => server.Instance.Equals(serverId, StringComparison.OrdinalIgnoreCase));
    }

    public void Cancel() => _scan.Cancel();

    private async Task BrowseAsync(TimeSpan window, Func<DiscoveredServer, bool>? keepBrowsing = null)
    {
        _scan.Cancel(); _scan.Dispose(); _scan = new();
        var token = _scan.Token;
        // A new scan starts from an empty list and a clean warning; the current
        // connection, if any, is the shell's and is not touched here.
        Servers.Clear();
        Warning = "";
        OnPropertyChanged(nameof(HasServers));
        OnPropertyChanged(nameof(ShowEmpty));
        IsScanning = true;
        ScanState = "Searching for Galdrun servers on this network…";
        try
        {
            await foreach (var server in _browse(window, token))
            {
                Apply(server);
                if (keepBrowsing is not null && !keepBrowsing(server)) break;
            }
            ScanState = Servers.Count == 0
                ? "No servers answered. Check that a Galdrun server is running on this network, then scan again."
                : $"{Servers.Count} server{(Servers.Count == 1 ? "" : "s")} found.";
        }
        catch (Exception e) when (e is OperationCanceledException or SocketException or ObjectDisposedException) { }
        finally { IsScanning = false; }
    }

    // A server that answers again keeps its place in the list with its newest
    // address and port.
    private void Apply(DiscoveredServer server)
    {
        for (var i = 0; i < Servers.Count; i++)
            if (Servers[i].Instance.Equals(server.Instance, StringComparison.OrdinalIgnoreCase))
            {
                Servers[i] = server;
                return;
            }
        Servers.Add(server);
        OnPropertyChanged(nameof(HasServers));
        OnPropertyChanged(nameof(ShowEmpty));
    }
}
