using System.Security.Cryptography;
using System.Text;

namespace Multiroom.App.Api;

// Pictures by artworkId. Recent ones stay in memory; with a folder, every
// picture is also kept on disk, so a restart shows it without the server.
// Several downloads run at once; asks for one id share its download, which
// stops once every ask for it was cancelled.
public sealed class ArtworkCache(MultiroomClient client, string? folder = null) : IDisposable
{
    private sealed class Pending(CancellationToken stop)
    {
        public readonly CancellationTokenSource Cancel = CancellationTokenSource.CreateLinkedTokenSource(stop);
        public Task<byte[]> Task = null!;
        public int Waiters;
    }

    private readonly CancellationTokenSource _stop = new();
    private readonly SemaphoreSlim _downloads = new(4);
    private readonly SemaphoreSlim _disk = new(1);
    private readonly Lock _lock = new();
    private readonly Dictionary<string, (byte[] Bytes, LinkedListNode<string> Node)> _cache = [];
    private readonly LinkedList<string> _order = []; // Least recently used first.
    private readonly Dictionary<string, Pending> _pending = [];
    private long _bytes;
    private long _diskBytes = -1; // Unknown until a write counts the folder.
    public const long Capacity = 32 * 1024 * 1024;
    public const long DiskCapacity = 256 * 1024 * 1024;
    // Thumbnails are a few KB each: a long queue fits whole.
    public const int MaxEntries = 512;

    // An id whose picture the server found or changed (artwork.changed): what
    // was kept for it, perhaps its artist standing in, goes, and views ask again.
    public event Action<string>? Found;

    // The sizes the server makes (GET artwork/{id}?size=), longest side in pixels.
    public static readonly IReadOnlyList<(string Name, int Pixels)> Sizes = [("tiny", 96), ("small", 192), ("medium", 384), ("large", 768)];
    // The smallest size at least that many pixels; null (the picture itself) above the largest.
    public static string? SizeFor(double pixels) => Sizes.FirstOrDefault(s => s.Pixels >= pixels).Name;
    private static string Key(string id, string? size) => size is null ? id : $"{id}?size={size}";

    public async Task AnnounceAsync(string id)
    {
        string[] keys = [id, .. Sizes.Select(size => Key(id, size.Name))];
        lock (_lock)
        {
            foreach (var key in keys)
            {
                if (_cache.Remove(key, out var old)) { _bytes -= old.Bytes.Length; _order.Remove(old.Node); }
                _pending.Remove(key); // A download in flight may bring the old picture; it is not kept.
            }
        }
        if (folder is not null)
        {
            try { await _disk.WaitAsync(_stop.Token).ConfigureAwait(false); }
            catch (OperationCanceledException) { return; }
            try { foreach (var key in keys) if (FileOf(key) is { } path) File.Delete(path); _diskBytes = -1; }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
            finally { _disk.Release(); }
        }
        Found?.Invoke(id);
    }

    public Task<byte[]> GetAsync(string id, CancellationToken ct) => GetAsync(id, null, ct);

    // At a named size (Sizes), or the picture itself for null.
    public async Task<byte[]> GetAsync(string id, string? size, CancellationToken ct)
    {
        ct.ThrowIfCancellationRequested(); _stop.Token.ThrowIfCancellationRequested();
        var key = Key(id, size);
        Pending pending;
        lock (_lock)
        {
            if (_cache.TryGetValue(key, out var found))
            { _order.Remove(found.Node); _order.AddLast(found.Node); return found.Bytes; }
            if (!_pending.TryGetValue(key, out pending!))
            {
                var started = pending = new(_stop.Token); _pending[key] = started;
                started.Task = Task.Run(() => LoadAsync(id, size, key, started));
            }
            pending.Waiters++;
        }
        try { return await pending.Task.WaitAsync(ct).ConfigureAwait(false); }
        finally
        {
            lock (_lock)
            {
                if (--pending.Waiters == 0 && !pending.Task.IsCompleted)
                {
                    if (_pending.TryGetValue(key, out var current) && current == pending) _pending.Remove(key);
                    try { pending.Cancel.Cancel(); } catch (ObjectDisposedException) { }
                }
            }
        }
    }

    // Pictures a list will show soon, fetched in the background so scrolling finds them here (at most
    // as many as memory keeps); a failure is left for the view that asks.
    public void Prefetch(IEnumerable<string?> ids, string? size)
    {
        foreach (var id in ids.OfType<string>().Distinct().Take(MaxEntries))
            _ = GetAsync(id, size, _stop.Token).ContinueWith(t => _ = t.Exception, TaskScheduler.Default);
    }

    private async Task<byte[]> LoadAsync(string id, string? size, string key, Pending pending)
    {
        var ct = pending.Cancel.Token;
        try
        {
            var bytes = await ReadDiskAsync(key, ct).ConfigureAwait(false);
            if (bytes is null)
            {
                await _downloads.WaitAsync(ct).ConfigureAwait(false);
                try { bytes = await client.ArtworkAsync(id, size, ct).ConfigureAwait(false); }
                finally { _downloads.Release(); }
                if (IsCurrent(key, pending)) await WriteDiskAsync(key, bytes).ConfigureAwait(false);
            }
            lock (_lock) if (IsCurrent(key, pending)) Remember(key, bytes);
            return bytes;
        }
        finally
        {
            lock (_lock)
            {
                if (IsCurrent(key, pending)) _pending.Remove(key);
                pending.Cancel.Dispose();
            }
        }
    }

    private bool IsCurrent(string id, Pending pending)
    {
        lock (_lock) return _pending.TryGetValue(id, out var current) && current == pending;
    }

    private void Remember(string id, byte[] bytes)
    {
        while ((_bytes + bytes.Length > Capacity || _cache.Count >= MaxEntries) && _order.First is { } oldest)
        { _bytes -= _cache[oldest.Value].Bytes.Length; _cache.Remove(oldest.Value); _order.RemoveFirst(); }
        _cache[id] = (bytes, _order.AddLast(id)); _bytes += bytes.Length;
    }

    private string? FileOf(string id) => folder is null ? null
        : Path.Combine(folder, Convert.ToHexStringLower(SHA256.HashData(Encoding.UTF8.GetBytes(id))));

    private async Task<byte[]?> ReadDiskAsync(string id, CancellationToken ct)
    {
        if (FileOf(id) is not { } path || !File.Exists(path)) return null;
        try
        {
            var bytes = await File.ReadAllBytesAsync(path, ct).ConfigureAwait(false);
            File.SetLastWriteTimeUtc(path, DateTime.UtcNow); // Trimming drops the longest unseen first.
            return bytes.Length > 0 ? bytes : null;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return null; }
    }

    private async Task WriteDiskAsync(string id, byte[] bytes)
    {
        if (FileOf(id) is not { } path) return;
        try { await _disk.WaitAsync(_stop.Token).ConfigureAwait(false); }
        catch (OperationCanceledException) { return; }
        var temporary = path + ".tmp";
        try
        {
            Directory.CreateDirectory(folder!);
            if (_diskBytes < 0) _diskBytes = Files().Sum(file => file.Length);
            await File.WriteAllBytesAsync(temporary, bytes, _stop.Token).ConfigureAwait(false);
            File.Move(temporary, path, overwrite: true);
            _diskBytes += bytes.Length;
            if (_diskBytes > DiskCapacity)
                foreach (var file in Files().OrderBy(file => file.LastWriteTimeUtc))
                {
                    if (_diskBytes <= DiskCapacity * 3 / 4) break;
                    file.Delete(); _diskBytes -= file.Length;
                }
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or OperationCanceledException)
        {
            _diskBytes = -1;
            try { File.Delete(temporary); } catch (Exception f) when (f is IOException or UnauthorizedAccessException) { }
        }
        finally { _disk.Release(); }
    }

    private IEnumerable<FileInfo> Files() => new DirectoryInfo(folder!).EnumerateFiles();

    public Task<byte[]?> RoomImageAsync(string room, CancellationToken ct) => client.RoomImageAsync(room, ct);

    public void Dispose() => _stop.Cancel();
}

public sealed partial class MultiroomClient
{
    // An item's picture by its artworkId, from the server's artwork store.
    public Task<byte[]> ArtworkAsync(string id, CancellationToken ct) => ArtworkAsync(id, null, ct);
    // At a named size (ArtworkCache.Sizes); the picture itself for null.
    public Task<byte[]> ArtworkAsync(string id, string? size, CancellationToken ct) =>
        PictureAsync(size is null ? $"artwork/{Escape(id)}" : $"artwork/{Escape(id)}?size={Escape(size)}", ct);
    // A room's own picture (null: it has none; the room's art stands in, as in the web interface).
    public async Task<byte[]?> RoomImageAsync(string client, CancellationToken ct)
    {
        try { return await PictureAsync($"rooms/{Escape(client)}/image", ct).ConfigureAwait(false); }
        catch (ApiException e) when (e.Status == System.Net.HttpStatusCode.NotFound) { return null; }
    }
    private async Task<byte[]> PictureAsync(string path, CancellationToken ct)
    {
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(ct);
        timeout.CancelAfter(TimeSpan.FromSeconds(20));
        using var request = Request(HttpMethod.Get, path);
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
