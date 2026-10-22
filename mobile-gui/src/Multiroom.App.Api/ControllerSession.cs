using System.Threading.Channels;

namespace Multiroom.App.Api;

public sealed record PlaybackSnapshot(IReadOnlyList<ZoneView> Zones, IReadOnlyList<RoomView> Rooms);

// One owned session per controller; invalidations coalesce while REST is in flight.
public sealed class ControllerSession : IAsyncDisposable
{
    private readonly CancellationTokenSource _stop = new();
    private readonly Channel<bool> _refresh = Channel.CreateBounded<bool>(new BoundedChannelOptions(1)
        { FullMode = BoundedChannelFullMode.DropOldest, SingleReader = true });
    private Task? _running;
    private int _fullRefresh;
    public MultiroomClient Client { get; }
    public PlaybackSnapshot Playback { get; private set; } = new([], []);
    public event Action<PlaybackSnapshot>? PlaybackChanged;
    public event Action<IReadOnlyList<ProviderView>>? ProvidersChanged;
    public event Action<bool>? ConnectionChanged;
    public event Action<string>? ContentInvalidated;
    // An artworkId that has a picture now (server phase 40: artwork.changed).
    public event Action<string>? ArtworkFound;
    public event Action<string>? RefreshFailed;

    public ControllerSession(MultiroomClient client) => Client = client;
    public void Start() => _running ??= Task.WhenAll(WatchAsync(), RefreshLoopAsync());
    public void Refresh(bool full = false)
    {
        if (full) Interlocked.Exchange(ref _fullRefresh, 1);
        _refresh.Writer.TryWrite(true);
    }

    private static string? ArtworkId(string data)
    {
        try
        {
            using var json = System.Text.Json.JsonDocument.Parse(data);
            return json.RootElement.TryGetProperty("id", out var id) && id.ValueKind == System.Text.Json.JsonValueKind.String
                ? id.GetString() : null;
        }
        catch (System.Text.Json.JsonException) { return null; }
    }

    private async Task WatchAsync()
    {
        try
        {
            await new EventStream(Client).RunAsync(e =>
            {
                if (e.Type.StartsWith("zone.", StringComparison.Ordinal) || e.Type.StartsWith("room.", StringComparison.Ordinal) ||
                    e.Type.StartsWith("queue.", StringComparison.Ordinal) || e.Type is "events.lost" or "core.eventsLost" or "core.synchronised")
                    Refresh(e.Type is "events.lost" or "core.eventsLost" or "core.synchronised");
                if (e.Type == "artwork.changed" && ArtworkId(e.Data) is { } artwork) ArtworkFound?.Invoke(artwork);
                ContentInvalidated?.Invoke(e.Type);
                return Task.CompletedTask;
            }, connected =>
            {
                ConnectionChanged?.Invoke(connected);
                if (connected) { Refresh(full: true); ContentInvalidated?.Invoke("resync"); }
                return Task.CompletedTask;
            }, _stop.Token).ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (_stop.IsCancellationRequested) { }
    }

    private async Task RefreshLoopAsync()
    {
        try
        {
            await foreach (var _ in _refresh.Reader.ReadAllAsync(_stop.Token).ConfigureAwait(false))
            {
                try
                {
                    var zones = Client.ZonesAsync(_stop.Token);
                    var rooms = Client.RoomsAsync(_stop.Token);
                    await Task.WhenAll(zones, rooms).ConfigureAwait(false);
                    Playback = new(await zones.ConfigureAwait(false), await rooms.ConfigureAwait(false));
                    PlaybackChanged?.Invoke(Playback);
                    if (Interlocked.Exchange(ref _fullRefresh, 0) != 0)
                        ProvidersChanged?.Invoke(await Client.ProvidersAsync(_stop.Token).ConfigureAwait(false));
                }
                catch (OperationCanceledException) when (_stop.IsCancellationRequested) { break; }
                catch (Exception e) when (e is ApiException or HttpRequestException or OperationCanceledException)
                { RefreshFailed?.Invoke(e is ApiException api ? api.Message : "Unable to refresh server state."); }
            }
        }
        catch (OperationCanceledException) when (_stop.IsCancellationRequested) { }
    }

    public async ValueTask DisposeAsync()
    {
        await _stop.CancelAsync().ConfigureAwait(false);
        if (_running is not null) await _running.ConfigureAwait(false);
        _stop.Dispose();
        Client.Dispose();
    }
}
