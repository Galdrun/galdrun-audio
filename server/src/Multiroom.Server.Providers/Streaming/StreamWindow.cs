using System.Net;
using System.Net.Http.Headers;
using Microsoft.Extensions.Logging;
using Multiroom.Server.Abstractions;

namespace Multiroom.Server.Providers.Streaming;

// Streamed tracks served with their length, so the media endpoint serves each
// one seekable (the contract's SHOULD) and every GET of it gets the same
// bytes, while only a window of each track is held. Every on-demand streaming
// provider resolves its tracks through one of these (one per provider, on
// the provider's HttpClient); none keeps a cache of its own. Live radio and
// local files do not come through here.
//
// Resolving a track measures its pieces (a Range probe each) and fetches its
// first and last piece before it answers: the staged promise, first bytes
// ready, holds for the demuxer's probe at either end. Those two stay held.
// Each open reader then wants a window of pieces: one behind its position,
// for small seeks back, up to `WindowBytes` ahead. Two workers per track
// fetch wanted pieces, the one a reader waits for first, and wait when
// nothing is wanted, so the download keeps pace with playback. A piece no
// reader wants any more is dropped, and fetched again if a reader seeks back
// to it. A window of 0, or one at least as long as the track, holds it whole.
//
// A manifest of one URL is fetched in 1 MiB ranges when its server takes
// ranges; a segment list (DASH) segment by segment, each segment one piece.
// Signed URLs that expire while a track plays (401, 403 or 410) are renewed
// through the provider's manifest function; renewed URLs must give pieces of
// the same lengths, or the track is Gone.
//
// Two zones on one track share it, each reader with its own window. A track
// is released when its last stream is: its downloads stop and its pieces are
// freed, in memory or on disk (<directory>/<key>.<tag>-<n>/<index>), once
// the last reader closes. Nothing is kept for a replay.
public sealed partial class StreamWindow : IDisposable
{
    public const int ChunkBytes = 1024 * 1024;
    private const long MaxSegmentBytes = 64L * 1024 * 1024;
    private const int Workers = 2;
    private const int Attempts = 3;
    private const int ProbeConcurrency = 8;

    private readonly HttpClient _http;
    private readonly StreamingSettings _settings;
    private readonly string _directory;
    private readonly ILogger _log;
    private readonly Lock _lock = new();
    private readonly Dictionary<string, Entry> _entries = new(StringComparer.Ordinal);
    private readonly CancellationTokenSource _stop = new();
    private long _serial;

    // `directory` holds the provider's pieces in disk mode:
    // <dataDirectory>/streams/<providerId>.
    public StreamWindow(HttpClient http, StreamingSettings settings, string directory, ILogger log)
    {
        _http = http;
        _settings = settings;
        _directory = directory;
        _log = log;
        DeleteLeftovers();
    }

    // Tracks resolved and not released, their open readers, and the bytes
    // of the pieces they hold.
    public (int Tracks, int Readers, long Bytes) Held
    {
        get
        {
            List<Entry> entries;
            lock (_lock)
                entries = [.. _entries.Values];
            var held = entries.Select(e => e.Held).ToList();
            return (entries.Count, held.Sum(h => h.Readers), held.Sum(h => h.Bytes));
        }
    }

    // For a provider's status.
    public string Describe()
    {
        var (tracks, readers, bytes) = Held;
        var window = _settings.WindowBytes == 0 ? "whole track" : $"{_settings.WindowBytes} bytes";
        return $"tracks {tracks}, readers {readers}, held {bytes} bytes " +
            $"({(_settings.Buffer == TrackBuffer.Disk ? "disk" : "memory")}, window {window})";
    }

    // The track under `key`, shared while it is in use, or fetched through
    // `manifest`, which is called again when its URLs expire. `name` names
    // it in failures.
    public async Task<Result<ResolvedStream, ProviderError>> GetAsync(string key, string name,
        Func<CancellationToken, Task<Result<StreamManifest, ProviderError>>> manifest,
        CancellationToken cancellationToken)
    {
        Entry entry;
        bool created;
        lock (_lock)
        {
            created = !_entries.TryGetValue(key, out var found);
            entry = found ?? new Entry(key, name, this);
            entry.Users++;
            if (created)
                _entries[key] = entry;
        }

        Failure<ProviderError>? failure;
        if (created)
        {
            failure = await PrepareAsync(entry, manifest, cancellationToken).ConfigureAwait(false);
            if (failure is not null)
                Drop(entry);
            entry.Prepared.TrySetResult(failure);
        }
        else
        {
            try
            {
                failure = await entry.Prepared.Task.WaitAsync(cancellationToken).ConfigureAwait(false);
            }
            catch (OperationCanceledException)
            {
                Unpin(entry);
                throw;
            }
        }
        if (failure is { } failed)
        {
            Unpin(entry);
            return failed;
        }

        var released = 0;
        return ResolvedStream.KnownLength(entry.Length, entry.ContentType, entry.Open, () =>
        {
            if (Interlocked.Exchange(ref released, 1) == 0)
                Unpin(entry);
        }, entry.Format);
    }

    // Every track released.
    public void Dispose()
    {
        _stop.Cancel();
        List<Entry> held;
        lock (_lock)
        {
            held = [.. _entries.Values];
            _entries.Clear();
        }
        foreach (var entry in held)
            entry.Release();
        _stop.Dispose();
    }

    private async Task<Failure<ProviderError>?> PrepareAsync(Entry entry,
        Func<CancellationToken, Task<Result<StreamManifest, ProviderError>>> manifest, CancellationToken cancellationToken)
    {
        try
        {
            var resolved = await manifest(cancellationToken).ConfigureAwait(false);
            if (!resolved.IsSuccess)
                return resolved.Failure;
            var measured = await MeasureAsync(resolved.Value.Pieces, entry.Name, cancellationToken).ConfigureAwait(false);
            if (!measured.IsSuccess)
                return measured.Failure;
            var pieces = measured.Value;
            var length = pieces.Sum(s => s.Length);
            if (length == 0)
                return Result.Fail(ProviderError.Failed, $"{entry.Name}: the stream is empty");

            IPieceStore store;
            try
            {
                store = _settings.Buffer == TrackBuffer.Disk
                    ? new DiskPieceStore(Path.Combine(_directory,
                        $"{FileSafe(entry.Key)}.{FileSafe(resolved.Value.Tag)}-{Interlocked.Increment(ref _serial)}"))
                    : new MemoryPieceStore(pieces.Count);
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException)
            {
                return Result.Fail(ProviderError.Failed, $"{entry.Name}: the stream's directory cannot be written: {e.Message}");
            }

            entry.Begin(resolved.Value, pieces, length, store, manifest, _stop.Token);
            await entry.WaitAsync(0, cancellationToken).ConfigureAwait(false);
            await entry.WaitAsync(pieces.Count - 1, cancellationToken).ConfigureAwait(false);
            return null;
        }
        catch (IOException e)
        {
            return Result.Fail(ProviderError.Unavailable, e.Message);
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            return Result.Fail(ProviderError.Unavailable, $"{entry.Name}: resolving was cancelled");
        }
    }

    // The pieces of the stream, with their offsets and lengths.
    private async Task<Result<IReadOnlyList<Piece>, ProviderError>> MeasureAsync(IReadOnlyList<Uri> urls, string name,
        CancellationToken cancellationToken)
    {
        if (urls.Count == 0)
            return Result.Fail(ProviderError.Failed, $"{name}: the manifest names no media");
        if (urls.Count == 1)
        {
            var probed = await ProbeAsync(urls[0], name, cancellationToken).ConfigureAwait(false);
            if (!probed.IsSuccess)
                return probed.Failure;
            var (length, ranges) = probed.Value;
            if (!ranges && length > MaxSegmentBytes)
                return Result.Fail(ProviderError.Failed, $"{name}: the stream takes no ranges and is {length} bytes");
            if (!ranges)
                return new[] { new Piece(0, 0, length, Ranged: false) };
            var chunks = new List<Piece>();
            for (long offset = 0; offset < length; offset += ChunkBytes)
                chunks.Add(new Piece(0, offset, Math.Min(ChunkBytes, length - offset), Ranged: true));
            return chunks;
        }

        using var gate = new SemaphoreSlim(ProbeConcurrency);
        var probes = await Task.WhenAll(urls.Select(async url =>
        {
            await gate.WaitAsync(cancellationToken).ConfigureAwait(false);
            try
            {
                return await ProbeAsync(url, name, cancellationToken).ConfigureAwait(false);
            }
            finally
            {
                gate.Release();
            }
        })).ConfigureAwait(false);

        var pieces = new List<Piece>(urls.Count);
        long at = 0;
        for (var i = 0; i < probes.Length; i++)
        {
            if (!probes[i].IsSuccess)
                return probes[i].Failure;
            var length = probes[i].Value.Length;
            if (length > MaxSegmentBytes)
                return Result.Fail(ProviderError.Failed, $"{name}: segment {i} is {length} bytes");
            pieces.Add(new Piece(i, at, length, Ranged: false));
            at += length;
        }
        return pieces;
    }

    // A GET of the first byte: 206 with the total in Content-Range means the
    // server takes ranges; 200 with a Content-Length means it does not.
    private async Task<Result<(long Length, bool Ranges), ProviderError>> ProbeAsync(Uri url, string name,
        CancellationToken cancellationToken)
    {
        try
        {
            using var request = new HttpRequestMessage(HttpMethod.Get, url);
            request.Headers.Range = new RangeHeaderValue(0, 0);
            using var response = await _http.SendAsync(request, HttpCompletionOption.ResponseHeadersRead,
                cancellationToken).ConfigureAwait(false);
            if (response.StatusCode == HttpStatusCode.PartialContent &&
                response.Content.Headers.ContentRange is { Length: { } total })
            {
                return (total, true);
            }
            if (response.StatusCode == HttpStatusCode.OK && response.Content.Headers.ContentLength is { } length)
                return (length, false);
            return Result.Fail(ProviderError.Unavailable,
                $"{name}: the stream's server answered HTTP {(int)response.StatusCode} without a length");
        }
        catch (OperationCanceledException) when (!cancellationToken.IsCancellationRequested)
        {
            return Result.Fail(ProviderError.Unavailable, $"{name}: the stream's server did not answer in time");
        }
        catch (HttpRequestException e)
        {
            return Result.Fail(ProviderError.Unavailable, $"{name}: the stream's server cannot be reached ({e.HttpRequestError})");
        }
    }

    // A stream released: the track goes once no stream holds it.
    private void Unpin(Entry entry)
    {
        bool last;
        lock (_lock)
        {
            last = --entry.Users == 0;
            if (last && _entries.TryGetValue(entry.Key, out var held) && held == entry)
                _entries.Remove(entry.Key);
        }
        if (last)
            entry.Release();
    }

    // A failed track: out at once, so the next resolve starts it afresh.
    private void Drop(Entry entry)
    {
        lock (_lock)
        {
            if (_entries.TryGetValue(entry.Key, out var held) && held == entry)
                _entries.Remove(entry.Key);
        }
        entry.Release();
    }

    // What an earlier run did not delete, after a crash or a kill, or in
    // disk mode before a switch to memory.
    private void DeleteLeftovers()
    {
        var directory = new DirectoryInfo(_directory);
        if (!directory.Exists)
            return;
        foreach (var leftover in directory.EnumerateFileSystemInfos())
        {
            try
            {
                if (leftover is DirectoryInfo track)
                    track.Delete(recursive: true);
                else
                    leftover.Delete();
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException)
            {
                LogLeftoverFailed(_log, leftover.FullName, e.Message);
            }
        }
    }

    private static string FileSafe(string text) =>
        string.Concat(text.Select(c => char.IsAsciiLetterOrDigit(c) || c is '-' or '_' ? c : '_'));

    [LoggerMessage(Level = LogLevel.Warning, Message = "streaming: track {Key} failed: {Reason}")]
    private static partial void LogTrackFailed(ILogger log, string key, string reason);

    [LoggerMessage(Level = LogLevel.Debug, Message = "streaming: track {Key} renewed its expired URLs")]
    private static partial void LogRenewed(ILogger log, string key);

    [LoggerMessage(Level = LogLevel.Warning, Message = "streaming: leftover {Path}: {Reason}")]
    private static partial void LogLeftoverFailed(ILogger log, string path, string reason);

    // One piece: the URL at index `Url` of the manifest, whole or a range of it.
    private sealed record Piece(int Url, long Offset, long Length, bool Ranged);

    // One track. Its own lock guards the pieces, the readers and the
    // download state; the window's lock guards Users.
    private sealed class Entry(string key, string name, StreamWindow window) : IDisposable
    {
        private readonly Lock _gate = new();
        private readonly List<Reader> _readers = [];
        private readonly SemaphoreSlim _renewing = new(1, 1);
        private IReadOnlyList<Piece> _pieces = [];
        private IReadOnlyList<Uri> _urls = [];
        private int _urlVersion;
        private Func<CancellationToken, Task<Result<StreamManifest, ProviderError>>>? _manifest;
        private bool[] _held = [];
        private bool[] _taken = [];
        private bool _whole;
        private IPieceStore? _store;
        private TaskCompletionSource _changed = new(TaskCreationOptions.RunContinuationsAsynchronously);
        private IOException? _failure;
        private CancellationTokenSource? _cancel;
        // Workers running and readers open: the store is freed after them.
        private int _holds;
        private bool _released;
        private bool _freed;

        public string Key { get; } = key;

        public string Name { get; } = name;

        public string ContentType { get; private set; } = "application/octet-stream";

        public AudioFormat? Format { get; private set; }

        public long Length { get; private set; }

        public int Users { get; set; }

        // Null on success; set once, by whoever created the entry.
        public TaskCompletionSource<Failure<ProviderError>?> Prepared { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public (int Readers, long Bytes) Held
        {
            get
            {
                lock (_gate)
                {
                    long bytes = 0;
                    for (var i = 0; i < _held.Length; i++)
                    {
                        if (_held[i])
                            bytes += _pieces[i].Length;
                    }
                    return (_readers.Count, bytes);
                }
            }
        }

        public void Begin(StreamManifest manifest, IReadOnlyList<Piece> pieces, long length, IPieceStore store,
            Func<CancellationToken, Task<Result<StreamManifest, ProviderError>>> renew, CancellationToken stop)
        {
            lock (_gate)
            {
                ContentType = manifest.ContentType;
                Format = manifest.Format;
                Length = length;
                _pieces = pieces;
                _urls = manifest.Pieces;
                _manifest = renew;
                _held = new bool[pieces.Count];
                _taken = new bool[pieces.Count];
                _whole = window._settings.Whole(length);
                _store = store;
                _cancel = CancellationTokenSource.CreateLinkedTokenSource(stop);
                _holds += Workers;
            }
            for (var i = 0; i < Workers; i++)
                _ = WorkAsync(_cancel.Token);
        }

        public Result<Stream, ProviderError> Open()
        {
            lock (_gate)
            {
                if (_failure is not null)
                    return Result.Fail(ProviderError.Gone, $"{Name}: {_failure.Message}");
                var reader = new Reader(this);
                _readers.Add(reader);
                _holds++;
                return reader;
            }
        }

        public int PieceAt(long position)
        {
            var pieces = _pieces;
            int low = 0, high = pieces.Count - 1;
            while (low < high)
            {
                var middle = (low + high + 1) / 2;
                if (pieces[middle].Offset <= position)
                    low = middle;
                else
                    high = middle - 1;
            }
            return low;
        }

        public long EndOf(int piece) => _pieces[piece].Offset + _pieces[piece].Length;

        // The reader is at `position` now: its window follows it, and what
        // no window wants any more is dropped. The piece it is in.
        public int Move(Reader reader, long position)
        {
            var at = PieceAt(Math.Min(position, Length - 1));
            lock (_gate)
            {
                if (at == reader.Piece || _released)
                    return at;
                reader.Piece = at;
                reader.Last = at;
                if (_whole)
                {
                    reader.Last = _pieces.Count - 1;
                }
                else
                {
                    while (reader.Last + 1 < _pieces.Count && EndOf(reader.Last + 1) - _pieces[at].Offset <= window._settings.WindowBytes)
                        reader.Last++;
                }
                DropUnwanted();
                Pulse();
                return at;
            }
        }

        public void Leave(Reader reader)
        {
            lock (_gate)
            {
                _readers.Remove(reader);
                _holds--;
                if (!_released)
                    DropUnwanted();
                Pulse();
            }
            Free();
        }

        // Until the piece is held; a reader's own is fetched first. Throws
        // IOException when the track failed or was released before it came.
        public async ValueTask WaitAsync(int piece, CancellationToken cancellationToken)
        {
            while (true)
            {
                Task changed;
                lock (_gate)
                {
                    if (_held[piece])
                        return;
                    if (_failure is not null)
                        throw new IOException(_failure.Message, _failure);
                    changed = _changed.Task;
                }
                await changed.WaitAsync(cancellationToken).ConfigureAwait(false);
            }
        }

        // From a held piece, which the reader's window keeps held.
        public ValueTask<int> ReadAsync(int piece, long position, Memory<byte> buffer, CancellationToken cancellationToken) =>
            _store!.ReadAsync(piece, position - _pieces[piece].Offset, buffer, cancellationToken);

        // Downloads stop and waiting readers fail; the pieces are freed once
        // the last open reader closes. Idempotent.
        public void Release()
        {
            CancellationTokenSource? cancel;
            lock (_gate)
            {
                if (_released)
                    return;
                _released = true;
                _failure ??= new IOException($"{Name}: released");
                cancel = _cancel;
                Pulse();
            }
            try
            {
                cancel?.Cancel();
            }
            catch (ObjectDisposedException)
            {
                // Already freed: its workers had stopped.
            }
            Free();
        }

        private void Free()
        {
            IPieceStore? store;
            lock (_gate)
            {
                if (!_released || _holds > 0 || _freed)
                    return;
                _freed = true;
                store = _store;
            }
            store?.Dispose();
            Dispose();
        }

        // Once freed: no worker runs any more.
        public void Dispose()
        {
            _cancel?.Dispose();
            _renewing.Dispose();
        }

        private async Task WorkAsync(CancellationToken stop)
        {
            try
            {
                while (true)
                {
                    int? index;
                    Task changed;
                    lock (_gate)
                    {
                        if (_failure is not null)
                            return;
                        index = Take();
                        changed = _changed.Task;
                    }
                    if (index is { } taken)
                    {
                        await FetchAsync(taken, stop).ConfigureAwait(false);
                        Finish(taken);
                    }
                    else
                    {
                        await changed.WaitAsync(stop).ConfigureAwait(false);
                    }
                }
            }
            catch (OperationCanceledException) when (stop.IsCancellationRequested)
            {
                // Released, or the provider stopped.
            }
            catch (Exception e) when (e is IOException or HttpRequestException or ObjectDisposedException)
            {
                Fail(e.Message);
            }
            finally
            {
                lock (_gate)
                    _holds--;
                Free();
            }
        }

        // Under _gate: the next piece to fetch, or null when nothing wanted
        // is missing. Each reader's own piece, then the first and the last,
        // then the window's, nearest a reader first, then those behind.
        private int? Take()
        {
            int? Missing(int index) => index >= 0 && index < _held.Length && !_held[index] && !_taken[index] ? index : null;

            var next = _readers.Where(r => r.Piece >= 0).Select(r => Missing(r.Piece)).FirstOrDefault(i => i is not null) ??
                Missing(0) ?? Missing(_pieces.Count - 1);
            if (next is null && _whole)
                next = Enumerable.Range(0, _pieces.Count).Select(Missing).FirstOrDefault(i => i is not null);
            if (next is null)
            {
                var nearest = int.MaxValue;
                foreach (var reader in _readers.Where(r => r.Piece >= 0))
                {
                    for (var i = reader.Piece + 1; i <= reader.Last && i - reader.Piece < nearest; i++)
                    {
                        if (Missing(i) is { } ahead)
                        {
                            (next, nearest) = (ahead, i - reader.Piece);
                            break;
                        }
                    }
                }
                next ??= _readers.Where(r => r.Piece >= 0).Select(r => Missing(r.Piece - 1)).FirstOrDefault(i => i is not null);
            }
            if (next is { } index)
                _taken[index] = true;
            return next;
        }

        // Under _gate.
        private bool Wanted(int index) =>
            _whole || index == 0 || index == _pieces.Count - 1 ||
            _readers.Any(r => r.Piece >= 0 && index >= r.Piece - 1 && index <= r.Last);

        // Under _gate.
        private void DropUnwanted()
        {
            if (_whole)
                return;
            for (var i = 0; i < _held.Length; i++)
            {
                if (_held[i] && !Wanted(i))
                {
                    _held[i] = false;
                    _store!.Drop(i);
                }
            }
        }

        // One piece into the store. A piece whose length is not what was
        // measured is fetched again, then fails the track: one URL must keep
        // its bytes. An expired URL is renewed once per fetch, which is not
        // an attempt.
        private async Task FetchAsync(int index, CancellationToken stop)
        {
            var piece = _pieces[index];
            var renewed = false;
            string? reason = null;
            for (var attempt = 1; attempt <= Attempts;)
            {
                int version;
                Uri url;
                lock (_gate)
                {
                    version = _urlVersion;
                    url = _urls[piece.Url];
                }
                (reason, var expired) = await TryFetchAsync(index, piece, url, stop).ConfigureAwait(false);
                if (reason is null)
                    return;
                if (expired && !renewed)
                {
                    renewed = true;
                    await RenewAsync(version, stop).ConfigureAwait(false);
                    continue;
                }
                attempt++;
            }
            throw new IOException($"{Name}: piece {index + 1} of {_pieces.Count} failed: {reason}");
        }

        private async Task<(string? Reason, bool Expired)> TryFetchAsync(int index, Piece piece, Uri url,
            CancellationToken stop)
        {
            try
            {
                using var request = new HttpRequestMessage(HttpMethod.Get, url);
                if (piece.Ranged)
                    request.Headers.Range = new RangeHeaderValue(piece.Offset, piece.Offset + piece.Length - 1);
                using var response = await window._http.SendAsync(request, HttpCompletionOption.ResponseHeadersRead, stop)
                    .ConfigureAwait(false);
                if (response.StatusCode is HttpStatusCode.Unauthorized or HttpStatusCode.Forbidden or HttpStatusCode.Gone)
                    return ($"HTTP {(int)response.StatusCode}", true);
                var expected = piece.Ranged ? HttpStatusCode.PartialContent : HttpStatusCode.OK;
                if (response.StatusCode != expected)
                    return ($"HTTP {(int)response.StatusCode}", false);
                if (piece.Ranged && response.Content.Headers.ContentRange?.From != piece.Offset)
                    return ("the answer is not the range asked for", false);

                await using var body = await response.Content.ReadAsStreamAsync(stop).ConfigureAwait(false);
                return (await _store!.WriteAsync(index, piece.Length, body, stop).ConfigureAwait(false), false);
            }
            catch (OperationCanceledException) when (!stop.IsCancellationRequested)
            {
                return ("no answer in time", false);
            }
            catch (HttpRequestException e)
            {
                return ($"cannot be reached ({e.HttpRequestError})", false);
            }
            catch (IOException e)
            {
                return (e.Message, false);
            }
        }

        // New URLs from the provider, once for all pieces that found theirs
        // expired since `seen`. Throws IOException when there are none, or
        // when they measure differently: the core resumes by offset, so
        // other bytes under the same media URL must be Gone, not played.
        private async Task RenewAsync(int seen, CancellationToken stop)
        {
            await _renewing.WaitAsync(stop).ConfigureAwait(false);
            try
            {
                lock (_gate)
                {
                    if (_urlVersion != seen)
                        return;
                }
                var renewed = await _manifest!(stop).ConfigureAwait(false);
                if (!renewed.IsSuccess)
                    throw new IOException($"{Name}: the expired stream cannot be renewed: {renewed.Failure.Detail}");
                var measured = await window.MeasureAsync(renewed.Value.Pieces, Name, stop).ConfigureAwait(false);
                if (!measured.IsSuccess)
                    throw new IOException($"{Name}: the expired stream cannot be renewed: {measured.Failure.Detail}");
                if (!measured.Value.SequenceEqual(_pieces))
                    throw new IOException($"{Name}: the renewed stream is not the same bytes");
                lock (_gate)
                {
                    _urls = renewed.Value.Pieces;
                    _urlVersion++;
                }
                LogRenewed(window._log, Key);
            }
            finally
            {
                _renewing.Release();
            }
        }

        private void Finish(int index)
        {
            lock (_gate)
            {
                _taken[index] = false;
                _held[index] = true;
                if (!Wanted(index))
                {
                    _held[index] = false;
                    _store!.Drop(index);
                }
                Pulse();
            }
        }

        private void Fail(string reason)
        {
            lock (_gate)
            {
                if (_failure is not null)
                    return;
                _failure = new IOException(reason);
                Pulse();
            }
            LogTrackFailed(window._log, Key, reason);
            window.Drop(this);
        }

        // Wakes every waiter; called under _gate.
        private void Pulse()
        {
            var changed = _changed;
            _changed = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
            changed.TrySetResult();
        }
    }

    // One GET's reader: seekable, of the track's length. Its position, from
    // its first read or seek on, is where its window is. A read waits for
    // the piece it starts in, and answers no further than that piece's end.
    private sealed class Reader(Entry entry) : Stream
    {
        private long _position;
        private int _disposed;

        // Its piece (-1 before its first read or seek) and the last piece
        // of its window; set under the entry's lock.
        public int Piece { get; set; } = -1;

        public int Last { get; set; } = -1;

        public override bool CanRead => true;

        public override bool CanSeek => true;

        public override bool CanWrite => false;

        public override long Length => entry.Length;

        public override long Position
        {
            get => _position;
            set
            {
                ArgumentOutOfRangeException.ThrowIfNegative(value);
                _position = value;
                if (value < entry.Length)
                    entry.Move(this, value);
            }
        }

        public override async ValueTask<int> ReadAsync(Memory<byte> buffer, CancellationToken cancellationToken = default)
        {
            if (buffer.IsEmpty || _position >= entry.Length)
                return 0;
            var piece = entry.Move(this, _position);
            await entry.WaitAsync(piece, cancellationToken).ConfigureAwait(false);
            var count = (int)Math.Min(buffer.Length, entry.EndOf(piece) - _position);
            var read = await entry.ReadAsync(piece, _position, buffer[..count], cancellationToken).ConfigureAwait(false);
            if (read == 0)
                throw new IOException($"{entry.Name}: a held piece is shorter than measured");
            _position += read;
            return read;
        }

        public override Task<int> ReadAsync(byte[] buffer, int offset, int count, CancellationToken cancellationToken) =>
            ReadAsync(buffer.AsMemory(offset, count), cancellationToken).AsTask();

        public override int Read(byte[] buffer, int offset, int count) =>
            ReadAsync(buffer.AsMemory(offset, count)).AsTask().GetAwaiter().GetResult();

        public override long Seek(long offset, SeekOrigin origin) => Position = origin switch
        {
            SeekOrigin.Begin => offset,
            SeekOrigin.Current => _position + offset,
            SeekOrigin.End => entry.Length + offset,
            _ => throw new ArgumentOutOfRangeException(nameof(origin)),
        };

        public override void Flush()
        {
        }

        public override void SetLength(long value) => throw new NotSupportedException();

        public override void Write(byte[] buffer, int offset, int count) => throw new NotSupportedException();

        protected override void Dispose(bool disposing)
        {
            if (disposing && Interlocked.Exchange(ref _disposed, 1) == 0)
                entry.Leave(this);
            base.Dispose(disposing);
        }
    }
}
