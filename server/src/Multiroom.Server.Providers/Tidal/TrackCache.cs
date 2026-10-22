using System.Net;
using System.Net.Http.Headers;
using Microsoft.Extensions.Logging;
using Microsoft.Win32.SafeHandles;
using Multiroom.Server.Abstractions;

namespace Multiroom.Server.Providers.Tidal;

// Tidal tracks reassembled into files of a known length, so the media
// endpoint serves each one seekable (the contract's SHOULD) and every GET of
// it gets the same bytes, whatever order its pieces arrived in.
//
// Resolving a track measures its pieces (a Range probe each), reserves its
// length in the cache, and fetches its first and last piece before it
// answers: the staged promise, first bytes ready, holds for the demuxer's
// probe at either end. The rest downloads behind it, as fast as the CDN
// gives it, two pieces at a time. A reader that reaches a piece not there
// yet waits for it, and that piece is fetched next.
//
// A manifest of one URL is fetched in 1 MiB ranges when its server takes
// ranges; a DASH manifest is fetched segment by segment, and its init and
// media segments concatenated are one fragmented MP4.
//
// The cache holds at most `capacity` bytes (rule 6), counting tracks still
// downloading. A track is in use from its resolve until its stream is
// released (retired from the media endpoint). What is not in use is evicted
// least recently played first; when all of it is in use, a resolve that does
// not fit is refused. Nothing survives a restart: the files are deleted when
// the cache is disposed, and whatever a crash left behind at startup.
//
// A file is named <key>.<grant><extension>, the tier Tidal granted.
internal sealed partial class TrackCache : IDisposable
{
    public const int ChunkBytes = 1024 * 1024;
    private const long MaxSegmentBytes = 64L * 1024 * 1024;
    private const int Workers = 2;
    private const int Attempts = 3;
    private const int ProbeConcurrency = 8;
    private const string PartSuffix = ".part";

    private static readonly Dictionary<string, string> Extensions = new(StringComparer.OrdinalIgnoreCase)
    {
        ["audio/flac"] = ".flac",
        ["audio/mp4"] = ".mp4",
        ["audio/mpeg"] = ".mp3",
        ["audio/aac"] = ".aac",
    };

    private readonly string _directory;
    private readonly long _capacity;
    private readonly HttpClient _http;
    private readonly ILogger _log;
    private readonly Lock _lock = new();
    private readonly Dictionary<string, Entry> _entries = new(StringComparer.Ordinal);
    private readonly CancellationTokenSource _stop = new();

    public TrackCache(string directory, long capacity, HttpClient http, ILogger log)
    {
        _directory = directory;
        _capacity = capacity;
        _http = http;
        _log = log;
        DeleteLeftovers();
    }

    public long Capacity => _capacity;

    // Rule 6's status field: tracks held, their bytes, and how many are in use.
    public (int Tracks, long Bytes, int InUse) Held
    {
        get
        {
            lock (_lock)
                return (_entries.Count, _entries.Values.Sum(e => e.Length), _entries.Values.Count(e => e.Users > 0));
        }
    }

    // The track under `key`, from the cache or fetched through `manifest`.
    // `name` names it in failures.
    public async Task<Result<ResolvedStream, ProviderError>> GetAsync(string key, string name,
        Func<CancellationToken, Task<Result<TidalManifest, ProviderError>>> manifest,
        CancellationToken cancellationToken)
    {
        Entry entry;
        bool created;
        lock (_lock)
        {
            created = !_entries.TryGetValue(key, out var found);
            entry = found ?? new Entry(key, name, this);
            entry.Users++;
            entry.LastPlayed = DateTime.UtcNow;
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
        }, TidalProvider.FormatOfGrant(entry.Grant));
    }

    // Every track out, its file deleted.
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
            entry.Evict();
        _stop.Dispose();
    }

    private async Task<Failure<ProviderError>?> PrepareAsync(Entry entry,
        Func<CancellationToken, Task<Result<TidalManifest, ProviderError>>> manifest, CancellationToken cancellationToken)
    {
        try
        {
            var resolved = await manifest(cancellationToken).ConfigureAwait(false);
            if (!resolved.IsSuccess)
                return resolved.Failure;
            var measured = await MeasureAsync(resolved.Value, entry.Name, cancellationToken).ConfigureAwait(false);
            if (!measured.IsSuccess)
                return measured.Failure;
            var segments = measured.Value;
            var length = segments.Sum(s => s.Length);
            if (length == 0)
                return Result.Fail(ProviderError.Failed, $"{entry.Name}: the stream is empty");

            if (Reserve(entry, length) is { } refused)
                return refused;

            var contentType = resolved.Value.ContentType;
            var grant = resolved.Value.Grant;
            var path = Path.Combine(_directory, $"{entry.Key}.{grant}{ExtensionOf(contentType)}{PartSuffix}");
            SafeFileHandle writer;
            try
            {
                Directory.CreateDirectory(_directory);
                writer = File.OpenHandle(path, FileMode.Create, FileAccess.ReadWrite,
                    FileShare.ReadWrite | FileShare.Delete, FileOptions.Asynchronous);
                RandomAccess.SetLength(writer, length);
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException)
            {
                return Result.Fail(ProviderError.Failed, $"{entry.Name}: the cache file cannot be written: {e.Message}");
            }

            entry.Begin(contentType, grant, path, segments, writer, _stop.Token);
            await entry.WaitAsync(0, cancellationToken).ConfigureAwait(false);
            await entry.WaitAsync(segments.Count - 1, cancellationToken).ConfigureAwait(false);
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
    private async Task<Result<IReadOnlyList<Segment>, ProviderError>> MeasureAsync(TidalManifest manifest, string name,
        CancellationToken cancellationToken)
    {
        var urls = manifest.Segments;
        if (urls.Count == 1)
        {
            var probed = await ProbeAsync(urls[0], name, cancellationToken).ConfigureAwait(false);
            if (!probed.IsSuccess)
                return probed.Failure;
            var (length, ranges) = probed.Value;
            if (!ranges && length > MaxSegmentBytes)
                return Result.Fail(ProviderError.Failed, $"{name}: the stream takes no ranges and is {length} bytes");
            if (!ranges)
                return new[] { new Segment(urls[0], 0, length, Ranged: false) };
            var chunks = new List<Segment>();
            for (long offset = 0; offset < length; offset += ChunkBytes)
                chunks.Add(new Segment(urls[0], offset, Math.Min(ChunkBytes, length - offset), Ranged: true));
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

        var segments = new List<Segment>(urls.Count);
        long at = 0;
        for (var i = 0; i < probes.Length; i++)
        {
            if (!probes[i].IsSuccess)
                return probes[i].Failure;
            var length = probes[i].Value.Length;
            if (length > MaxSegmentBytes)
                return Result.Fail(ProviderError.Failed, $"{name}: segment {i} is {length} bytes");
            segments.Add(new Segment(urls[i], at, length, Ranged: false));
            at += length;
        }
        return segments;
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

    // Room for `length` more bytes: what is not in use goes, least recently
    // played first, until it fits. Null when it fits.
    private Failure<ProviderError>? Reserve(Entry entry, long length)
    {
        var evicted = new List<Entry>();
        Failure<ProviderError>? refused = null;
        lock (_lock)
        {
            var held = _entries.Values.Where(e => e != entry).Sum(e => e.Length);
            foreach (var old in _entries.Values.Where(e => e != entry && e.Users == 0).OrderBy(e => e.LastPlayed).ToList())
            {
                if (held + length <= _capacity)
                    break;
                _entries.Remove(old.Key);
                held -= old.Length;
                evicted.Add(old);
            }
            if (held + length <= _capacity)
                entry.Length = length;
            else
                refused = Result.Fail(ProviderError.Unavailable,
                    $"{entry.Name}: {length} bytes do not fit the track cache ({held} of {_capacity} bytes in use)");
        }
        foreach (var old in evicted)
        {
            old.Evict();
            LogEvicted(_log, old.Key, old.Length);
        }
        return refused;
    }

    private void Unpin(Entry entry)
    {
        lock (_lock)
            entry.Users--;
    }

    // A failed entry: out of the cache, its file deleted.
    private void Drop(Entry entry)
    {
        lock (_lock)
        {
            if (_entries.TryGetValue(entry.Key, out var held) && held == entry)
                _entries.Remove(entry.Key);
        }
        entry.Evict();
    }

    // What an earlier run did not delete, after a crash or a kill.
    private void DeleteLeftovers()
    {
        var directory = new DirectoryInfo(_directory);
        if (!directory.Exists)
            return;
        foreach (var file in directory.EnumerateFiles())
        {
            try
            {
                file.Delete();
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException)
            {
                LogCacheFileFailed(_log, file.FullName, e.Message);
            }
        }
    }

    private static string ExtensionOf(string contentType) =>
        Extensions.TryGetValue(contentType, out var extension) ? extension : ".bin";

    [LoggerMessage(Level = LogLevel.Debug, Message = "tidal: evicted {Key} ({Length} bytes) from the track cache")]
    private static partial void LogEvicted(ILogger log, string key, long length);

    [LoggerMessage(Level = LogLevel.Warning, Message = "tidal: track {Key} failed: {Reason}")]
    private static partial void LogTrackFailed(ILogger log, string key, string reason);

    [LoggerMessage(Level = LogLevel.Warning, Message = "tidal: cache file {Path}: {Reason}")]
    private static partial void LogCacheFileFailed(ILogger log, string path, string reason);

    // One piece: a whole URL, or a range of one.
    private sealed record Segment(Uri? Url, long Offset, long Length, bool Ranged);

    // One track in the cache. Its own lock guards the download state; the
    // cache's lock guards Users, LastPlayed and Length.
    private sealed class Entry(string key, string name, TrackCache cache)
    {
        private readonly Lock _gate = new();
        private IReadOnlyList<Segment> _segments = [];
        private bool[] _done = [];
        private bool[] _taken = [];
        private int[] _order = [];
        private int _requested = -1;
        private TaskCompletionSource _changed = new(TaskCreationOptions.RunContinuationsAsynchronously);
        private IOException? _failure;
        private bool _complete;
        private string _path = "";
        private SafeFileHandle? _writer;
        private CancellationTokenSource? _cancel;
        private int _left;

        public string Key { get; } = key;

        public string Name { get; } = name;

        public string ContentType { get; private set; } = "application/octet-stream";

        public string Grant { get; private set; } = TidalManifest.UnknownGrant;

        public long Length { get; set; }

        public int Users { get; set; }

        public DateTime LastPlayed { get; set; }

        // Null on success; set once, by whoever created the entry.
        public TaskCompletionSource<Failure<ProviderError>?> Prepared { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        // First and last piece first, the rest in order.
        public void Begin(string contentType, string grant, string path, IReadOnlyList<Segment> segments,
            SafeFileHandle writer, CancellationToken stop)
        {
            lock (_gate)
            {
                ContentType = contentType;
                Grant = grant;
                _path = path;
                _segments = segments;
                _done = new bool[segments.Count];
                _taken = new bool[segments.Count];
                _order = [0, .. segments.Count > 1 ? [segments.Count - 1] : Array.Empty<int>(), .. Enumerable.Range(1, Math.Max(0, segments.Count - 2))];
                _left = segments.Count;
                _writer = writer;
                _cancel = CancellationTokenSource.CreateLinkedTokenSource(stop);
            }
            for (var i = 0; i < Math.Min(Workers, segments.Count); i++)
                _ = WorkAsync(_cancel.Token);
        }

        public Result<Stream, ProviderError> Open()
        {
            for (var attempt = 0; attempt < 2; attempt++)
            {
                string path;
                lock (_gate)
                {
                    if (_failure is not null && !_complete)
                        return Result.Fail(ProviderError.Gone, $"{Name}: {_failure.Message}");
                    path = _path;
                }
                try
                {
                    var handle = File.OpenHandle(path, FileMode.Open, FileAccess.Read,
                        FileShare.ReadWrite | FileShare.Delete, FileOptions.Asynchronous);
                    return new Reader(this, handle);
                }
                catch (FileNotFoundException)
                {
                    // Renamed complete in between: once more, by its new name.
                }
                catch (Exception e) when (e is IOException or UnauthorizedAccessException)
                {
                    return Result.Fail(ProviderError.Gone, $"{Name}: the cache file cannot be read: {e.Message}");
                }
            }
            return Result.Fail(ProviderError.Gone, $"{Name}: the cache file is gone");
        }

        public int SegmentAt(long position)
        {
            var segments = _segments;
            int low = 0, high = segments.Count - 1;
            while (low < high)
            {
                var middle = (low + high + 1) / 2;
                if (segments[middle].Offset <= position)
                    low = middle;
                else
                    high = middle - 1;
            }
            return low;
        }

        public long EndOf(int segment) => _segments[segment].Offset + _segments[segment].Length;

        // Until the piece is there; it is fetched next. Throws IOException
        // when the track failed or was evicted before it came.
        public async ValueTask WaitAsync(int segment, CancellationToken cancellationToken)
        {
            while (true)
            {
                Task changed;
                lock (_gate)
                {
                    if (_complete || _done[segment])
                        return;
                    if (_failure is not null)
                        throw new IOException(_failure.Message, _failure);
                    if (!_taken[segment])
                        _requested = segment;
                    changed = _changed.Task;
                }
                await changed.WaitAsync(cancellationToken).ConfigureAwait(false);
            }
        }

        // Out of the cache: downloads stop, waiting readers fail, the file is
        // deleted. A reader already open on a complete file reads on.
        public void Evict()
        {
            string path;
            lock (_gate)
            {
                _failure ??= new IOException($"{Name}: evicted from the track cache");
                path = _path;
                Pulse();
            }
            Stop();
            if (path.Length == 0)
                return;
            try
            {
                File.Delete(path);
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException)
            {
                LogCacheFileFailed(cache._log, path, e.Message);
            }
        }

        public void Stop()
        {
            lock (_gate)
            {
                try
                {
                    _cancel?.Cancel();
                }
                catch (ObjectDisposedException)
                {
                    // Already finished.
                }
                _writer?.Dispose();
                _writer = null;
            }
        }

        private async Task WorkAsync(CancellationToken stop)
        {
            try
            {
                while (Take() is { } index)
                {
                    await FetchAsync(index, stop).ConfigureAwait(false);
                    Finish(index);
                }
            }
            catch (OperationCanceledException) when (stop.IsCancellationRequested)
            {
                // Evicted, or the provider stopped.
            }
            catch (Exception e) when (e is IOException or HttpRequestException or ObjectDisposedException)
            {
                Fail(e.Message);
            }
        }

        private int? Take()
        {
            lock (_gate)
            {
                if (_failure is not null)
                    return null;
                if (_requested >= 0 && !_done[_requested] && !_taken[_requested])
                {
                    _taken[_requested] = true;
                    return _requested;
                }
                foreach (var index in _order)
                {
                    if (!_taken[index])
                    {
                        _taken[index] = true;
                        return index;
                    }
                }
                return null;
            }
        }

        // One piece, into its place in the file. A piece whose length is not
        // what was measured is fetched again, then fails the track: one URL
        // must keep its bytes.
        private async Task FetchAsync(int index, CancellationToken stop)
        {
            var segment = _segments[index];
            var buffer = new byte[segment.Length];
            string? reason = null;
            for (var attempt = 1; attempt <= Attempts; attempt++)
            {
                reason = await TryFetchAsync(segment, buffer, stop).ConfigureAwait(false);
                if (reason is null)
                    break;
            }
            if (reason is not null)
                throw new IOException($"{Name}: piece {index + 1} of {_segments.Count} failed: {reason}");

            SafeFileHandle writer;
            lock (_gate)
                writer = _writer ?? throw new OperationCanceledException(stop);
            await RandomAccess.WriteAsync(writer, buffer, segment.Offset, stop).ConfigureAwait(false);
        }

        private async Task<string?> TryFetchAsync(Segment segment, byte[] buffer, CancellationToken stop)
        {
            try
            {
                using var request = new HttpRequestMessage(HttpMethod.Get, segment.Url);
                if (segment.Ranged)
                    request.Headers.Range = new RangeHeaderValue(segment.Offset, segment.Offset + segment.Length - 1);
                using var response = await cache._http.SendAsync(request, HttpCompletionOption.ResponseHeadersRead, stop)
                    .ConfigureAwait(false);
                var expected = segment.Ranged ? HttpStatusCode.PartialContent : HttpStatusCode.OK;
                if (response.StatusCode != expected)
                    return $"HTTP {(int)response.StatusCode}";
                if (segment.Ranged && response.Content.Headers.ContentRange?.From != segment.Offset)
                    return "the answer is not the range asked for";

                await using var body = await response.Content.ReadAsStreamAsync(stop).ConfigureAwait(false);
                var read = 0;
                while (read < buffer.Length)
                {
                    var n = await body.ReadAsync(buffer.AsMemory(read), stop).ConfigureAwait(false);
                    if (n == 0)
                        return $"{read} of {buffer.Length} bytes";
                    read += n;
                }
                var extra = new byte[1];
                if (await body.ReadAsync(extra, stop).ConfigureAwait(false) != 0)
                    return $"longer than {buffer.Length} bytes";
                return null;
            }
            catch (OperationCanceledException) when (!stop.IsCancellationRequested)
            {
                return "no answer in time";
            }
            catch (HttpRequestException e)
            {
                return $"cannot be reached ({e.HttpRequestError})";
            }
            catch (IOException e)
            {
                return e.Message;
            }
        }

        private void Finish(int index)
        {
            string? part = null;
            lock (_gate)
            {
                _done[index] = true;
                if (--_left == 0)
                {
                    _writer?.Dispose();
                    _writer = null;
                    part = _path;
                }
                Pulse();
            }
            if (part is null)
                return;

            // Complete: renamed to its final name. Readers hold their own
            // handles, opened to allow it.
            var final = part[..^PartSuffix.Length];
            try
            {
                File.Move(part, final, overwrite: true);
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException)
            {
                LogCacheFileFailed(cache._log, part, e.Message);
                final = part;
            }
            lock (_gate)
            {
                if (_failure is null)
                {
                    _path = final;
                    _complete = true;
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
            LogTrackFailed(cache._log, Key, reason);
            cache.Drop(this);
        }

        // Wakes every waiter; called under _gate.
        private void Pulse()
        {
            var changed = _changed;
            _changed = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
            changed.TrySetResult();
        }
    }

    // One GET's reader: seekable, of the track's length. A read waits for the
    // piece it starts in, and answers no further than that piece's end.
    private sealed class Reader(Entry entry, SafeFileHandle handle) : Stream
    {
        private long _position;

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
            }
        }

        public override async ValueTask<int> ReadAsync(Memory<byte> buffer, CancellationToken cancellationToken = default)
        {
            if (buffer.IsEmpty || _position >= entry.Length)
                return 0;
            var segment = entry.SegmentAt(_position);
            await entry.WaitAsync(segment, cancellationToken).ConfigureAwait(false);
            var count = (int)Math.Min(buffer.Length, Math.Min(entry.EndOf(segment), entry.Length) - _position);
            var read = await RandomAccess.ReadAsync(handle, buffer[..count], _position, cancellationToken).ConfigureAwait(false);
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
            if (disposing)
                handle.Dispose();
            base.Dispose(disposing);
        }
    }
}
