namespace Multiroom.Server.Abstractions;

public enum StreamShape
{
    // Seekable, with a length: served with Range support.
    KnownLength,

    // Endless, or of a length not known yet: served chunked, no length, no ranges.
    Live,
}

// What a provider hands the media endpoint for one staged source: exactly the
// two shapes the media contract names. Open() gives a fresh reader at 0 for
// each GET and may be called by any number of requests at once, because the
// core opens one source with several GETs, each on its own connection, in any
// order.
//
// Open() holds the provider to its shape: a KnownLength reader must seek and
// have the length announced (a different length is Gone, never different
// bytes under the same URL), and a Live reader must not seek.
#pragma warning disable CA1711 // Not a Stream but the roadmap's name for what resolving answers; it opens Streams.
public sealed class ResolvedStream
#pragma warning restore CA1711
{
    private readonly Func<Result<Stream, ProviderError>> _open;
    private readonly Action? _release;

    private ResolvedStream(StreamShape shape, string contentType, long? length,
        Func<Result<Stream, ProviderError>> open, Action? release = null, AudioFormat? format = null)
    {
        Shape = shape;
        ContentType = contentType;
        Length = length;
        Format = format;
        _open = open;
        _release = release;
    }

    public StreamShape Shape { get; }

    // The container hint. Advisory: the core's decoder probes the bytes.
    public string ContentType { get; }

    // Known for KnownLength, null for Live.
    public long? Length { get; }

    // What the bytes are, when the provider learns it only on resolving (a
    // streaming service may grant a lower tier than the track is listed in).
    // Null when the track's own Format stands.
    public AudioFormat? Format { get; }

    // `release` is called once no GET will come any more (Release), so a cache
    // may evict what backs it.
    public static ResolvedStream KnownLength(long length, string contentType,
        Func<Result<Stream, ProviderError>> open, Action? release = null, AudioFormat? format = null)
    {
        ArgumentOutOfRangeException.ThrowIfNegative(length);
        return new(StreamShape.KnownLength, contentType, length, open, release, format);
    }

    // `release` is called once no GET will come any more (Release), so a relay
    // connected at stage time and never opened lets its upstream go.
    public static ResolvedStream Live(string contentType, Func<Result<Stream, ProviderError>> open,
        Action? release = null) =>
        new(StreamShape.Live, contentType, null, open, release);

    // A file on disk, as it is now. A file whose length or write time differs
    // when a GET opens it is Gone.
    public static Result<ResolvedStream, ProviderError> FromFile(string path)
    {
        FileInfo info;
        try
        {
            info = new FileInfo(path);
            if (!info.Exists)
                return Result.Fail(ProviderError.NotFound, $"{path} does not exist");
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or ArgumentException
            or NotSupportedException)
        {
            return Result.Fail(ProviderError.Failed, $"{path}: {e.Message}");
        }

        var fullPath = info.FullName;
        var writtenUtc = info.LastWriteTimeUtc;
        return KnownLength(info.Length, ContentTypeOf(info.Extension), () => OpenFile(fullPath, writtenUtc));
    }

    public Result<Stream, ProviderError> Open()
    {
        var opened = _open();
        if (!opened.IsSuccess)
            return opened;

        var stream = opened.Value;
        if (Shape == StreamShape.Live)
        {
            if (!stream.CanSeek)
                return stream;
            stream.Dispose();
            return Result.Fail(ProviderError.Failed, "a live stream opened as seekable");
        }

        if (!stream.CanSeek)
        {
            stream.Dispose();
            return Result.Fail(ProviderError.Failed, "a known-length stream opened as not seekable");
        }
        if (stream.Length != Length)
        {
            var actual = stream.Length;
            stream.Dispose();
            return Result.Fail(ProviderError.Gone, $"length is {actual}, was {Length} when resolved");
        }
        return stream;
    }

    // Retired from the media endpoint: no GET will open it again. A reader
    // already open is its GET's to dispose.
    public void Release() => _release?.Invoke();

    private static Result<Stream, ProviderError> OpenFile(string path, DateTime writtenUtc)
    {
        FileStream? stream = null;
        try
        {
            // No FileStream buffer: the response copies in its own 64 KiB blocks.
            stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read, bufferSize: 0,
                FileOptions.Asynchronous | FileOptions.SequentialScan);
            if (File.GetLastWriteTimeUtc(stream.SafeFileHandle) != writtenUtc)
            {
                stream.Dispose();
                return Result.Fail(ProviderError.Gone, $"{path} changed since it was resolved");
            }
            return stream;
        }
        catch (Exception e) when (e is FileNotFoundException or DirectoryNotFoundException)
        {
            return Result.Fail(ProviderError.Gone, $"{path} no longer exists");
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            stream?.Dispose();
            return Result.Fail(ProviderError.Failed, $"{path}: {e.Message}");
        }
    }

    // The container hint of a file name's extension.
    public static string ContentTypeOf(string extension) => extension.ToUpperInvariant() switch
    {
        ".FLAC" => "audio/flac",
        ".WAV" => "audio/wav",
        ".MP3" => "audio/mpeg",
        ".M4A" or ".MP4" => "audio/mp4",
        ".AAC" => "audio/aac",
        ".OGG" or ".OGA" or ".OPUS" => "audio/ogg",
        _ => "application/octet-stream",
    };
}
