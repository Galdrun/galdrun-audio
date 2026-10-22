namespace Multiroom.App.Api;

public sealed class ArtworkCache(MultiroomClient client) : IDisposable
{
    private readonly CancellationTokenSource _stop = new();
    private readonly SemaphoreSlim _gate = new(1);
    private readonly Dictionary<string, byte[]> _cache = [];
    private readonly Queue<string> _order = [];
    private long _bytes;
    public const long Capacity = 32 * 1024 * 1024;

    // An id whose picture the server found or changed (artwork.changed): what
    // was kept for it, perhaps its artist standing in, goes, and views ask again.
    public event Action<string>? Found;

    public async Task AnnounceAsync(string id)
    {
        // A fill in progress holds the gate; this waits its turn, never the caller's thread.
        try { await _gate.WaitAsync(_stop.Token).ConfigureAwait(false); }
        catch (OperationCanceledException) { return; }
        try
        {
            if (_cache.Remove(id, out var old))
            {
                _bytes -= old.Length;
                var rest = _order.Where(o => o != id).ToArray();
                _order.Clear();
                foreach (var o in rest) _order.Enqueue(o);
            }
        }
        finally { _gate.Release(); }
        Found?.Invoke(id);
    }

    public async Task<byte[]> GetAsync(string id, CancellationToken ct)
    {
        using var linked = CancellationTokenSource.CreateLinkedTokenSource(ct, _stop.Token);
        // Serial cache fills deduplicate concurrent requests and bound in-flight encoded bytes.
        await _gate.WaitAsync(linked.Token).ConfigureAwait(false);
        try
        {
            if (_cache.TryGetValue(id, out var found)) return found;
            var bytes = await client.ArtworkAsync(id, linked.Token).ConfigureAwait(false);
            while ((_bytes + bytes.Length > Capacity || _cache.Count >= 128) && _order.TryDequeue(out var old))
            { _bytes -= _cache[old].Length; _cache.Remove(old); }
            _cache.Add(id, bytes); _order.Enqueue(id); _bytes += bytes.Length;
            return bytes;
        }
        finally { _gate.Release(); }
    }
    public void Dispose() => _stop.Cancel();
}

public sealed partial class MultiroomClient
{
    // An item's picture by its artworkId, from the server's artwork store.
    public async Task<byte[]> ArtworkAsync(string id, CancellationToken ct)
    {
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(ct);
        timeout.CancelAfter(TimeSpan.FromSeconds(20));
        using var request = Request(HttpMethod.Get, $"artwork/{Escape(id)}");
        using var response = await OpenAsync(request, timeout.Token).ConfigureAwait(false);
        using var stream = await response.Content.ReadAsStreamAsync(timeout.Token).ConfigureAwait(false);
        using var buffer = new MemoryStream();
        var chunk = new byte[16384];
        int read;
        while ((read = await stream.ReadAsync(chunk, timeout.Token).ConfigureAwait(false)) > 0)
        {
            if (buffer.Length + read > 8 * 1024 * 1024) throw new IOException("Artwork is too large.");
            buffer.Write(chunk, 0, read);
        }
        return buffer.ToArray();
    }
}
