using System.Buffers;
using System.IO.Pipelines;
using System.Net;
using System.Text;
using Multiroom.Server.Abstractions;

namespace Multiroom.Server.Providers.WebRadio;

// One station's upstream connection, relayed to one GET of the core. It is
// connected at stage time, and a pump reads it into the jitter buffer from
// then on: a Pipe that stops the pump at bufferBytes, so the upstream is held
// back by TCP while the core is not reading, as the core holds back the media
// endpoint. What is buffered is at most bufferBytes plus one read.
//
// ICY metadata is asked for (Icy-MetaData: 1), cut out of the bytes and kept
// as Title: the rooms keep the station name they were staged with, and the
// core never sees a metadata block. The bytes are otherwise relayed as the
// station sends them.
//
// A station does not end: the upstream closing, cleanly or not, fails the
// reader with an IOException, which the media endpoint turns into a dropped
// connection so the core fails the source. The URL is in no message here,
// because a station's URL may carry its own credentials (rule 7).
internal sealed class StationRelay : IDisposable
{
    public const int ReadSize = 16 * 1024;
    private const int MaxMetadata = 255 * 16;

    private readonly HttpResponseMessage _response;
    private readonly Stream _upstream;
    private readonly int _metaInterval;
    private readonly Pipe _pipe;
    private readonly CancellationTokenSource _stop = new();
    private readonly Action<StationRelay> _closed;
    private readonly byte[] _metadata = new byte[MaxMetadata];
    private Part _part = Part.Audio;
    private int _left;
    private int _metaLength;
    private int _opened;
    private int _disposed;
    private long _buffered;
    private long _peak;
    private volatile string? _title;
    private volatile IOException? _failure;

    private enum Part
    {
        Audio,
        Length,
        Metadata,
    }

    private StationRelay(string itemId, HttpResponseMessage response, Stream upstream, int metaInterval,
        int bufferBytes, Action<StationRelay> closed)
    {
        ItemId = itemId;
        ContentType = response.Content.Headers.ContentType?.MediaType ?? "application/octet-stream";
        _response = response;
        _upstream = upstream;
        _metaInterval = metaInterval;
        _left = metaInterval;
        _closed = closed;
        _pipe = new Pipe(new PipeOptions(pauseWriterThreshold: bufferBytes, resumeWriterThreshold: bufferBytes / 2,
            useSynchronizationContext: false));
    }

    // The station's ProviderItemId.
    public string ItemId { get; }

    // The station's own Content-Type: a hint, as for any source.
    public string ContentType { get; }

    // The last StreamTitle the station sent, if it sends any.
    public string? Title => _title;

    public long Buffered => Interlocked.Read(ref _buffered);

    // The most the jitter buffer has held.
    public long PeakBuffered => Interlocked.Read(ref _peak);

    // Bounded by `timeout`, because it runs while the queue engine stages.
    public static async Task<Result<StationRelay, ProviderError>> ConnectAsync(HttpClient http, string itemId,
        string name, Uri url, int bufferBytes, TimeSpan timeout, Action<StationRelay> closed,
        CancellationToken cancellationToken)
    {
        using var bounded = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        bounded.CancelAfter(timeout);
        HttpResponseMessage? response = null;
        try
        {
            using var request = new HttpRequestMessage(HttpMethod.Get, url);
            request.Headers.Add("Icy-MetaData", "1");
            response = await http.SendAsync(request, HttpCompletionOption.ResponseHeadersRead, bounded.Token)
                .ConfigureAwait(false);
            if (!response.IsSuccessStatusCode)
            {
                var status = response.StatusCode;
                response.Dispose();
                return Result.Fail(
                    status is HttpStatusCode.NotFound or HttpStatusCode.Gone ? ProviderError.NotFound : ProviderError.Unavailable,
                    $"{name}: the station answered HTTP {(int)status}");
            }

            var upstream = await response.Content.ReadAsStreamAsync(bounded.Token).ConfigureAwait(false);
            var metaInterval = response.Headers.TryGetValues("icy-metaint", out var values) &&
                int.TryParse(values.First(), out var interval) && interval > 0
                    ? interval
                    : 0;
            var relay = new StationRelay(itemId, response, upstream, metaInterval, bufferBytes, closed);
            _ = relay.PumpAsync();
            return relay;
        }
        catch (OperationCanceledException) when (!cancellationToken.IsCancellationRequested)
        {
            response?.Dispose();
            return Result.Fail(ProviderError.Unavailable,
                $"{name}: the station did not answer within {timeout.TotalSeconds:0} s");
        }
        catch (HttpRequestException e)
        {
            // Its message names the host; the kind of failure is enough.
            response?.Dispose();
            return Result.Fail(ProviderError.Unavailable, $"{name}: the station cannot be reached ({e.HttpRequestError})");
        }
    }

    // The one GET's reader. A live source cannot be resumed, so a second GET
    // gets nothing.
    public Result<Stream, ProviderError> Open()
    {
        if (Volatile.Read(ref _disposed) != 0 || Interlocked.Exchange(ref _opened, 1) != 0)
            return Result.Fail(ProviderError.Gone, $"the relay of {ItemId} has been read");
        return new Reader(this, _pipe.Reader.AsStream());
    }

    // Retired: no GET will come. An open reader is its GET's to close.
    public void Release()
    {
        if (Volatile.Read(ref _opened) == 0)
            Dispose();
    }

    public void Dispose()
    {
        if (Interlocked.Exchange(ref _disposed, 1) != 0)
            return;
        _stop.Cancel();
        _stop.Dispose();
        _closed(this);
    }

    private async Task PumpAsync()
    {
        // Taken once: Dispose cancels, then disposes the source.
        var stop = _stop.Token;
        IOException? failure = null;
        var buffer = new byte[ReadSize];
        try
        {
            while (true)
            {
                var read = await _upstream.ReadAsync(buffer, stop).ConfigureAwait(false);
                if (read == 0)
                {
                    failure = new IOException("the station ended its stream");
                    break;
                }
                var audio = WriteAudio(buffer.AsSpan(0, read));
                var buffered = Interlocked.Add(ref _buffered, audio);
                if (buffered > Interlocked.Read(ref _peak))
                    Interlocked.Exchange(ref _peak, buffered);
                // Waits while the buffer is full: the upstream is held back.
                var flushed = await _pipe.Writer.FlushAsync(stop).ConfigureAwait(false);
                if (flushed.IsCompleted)
                    break;
            }
        }
        catch (OperationCanceledException) when (stop.IsCancellationRequested)
        {
            // Closed: the reader is gone, or it was never opened and retired.
        }
        catch (IOException e)
        {
            failure = new IOException($"the station's stream failed: {e.Message}", e);
        }
        finally
        {
            // Completed cleanly even on a failure: a pipe completed with an
            // exception throws it before the bytes still buffered are read.
            // The reader throws it once they are (Reader.ReadAsync).
            _failure = failure;
            await _pipe.Writer.CompleteAsync().ConfigureAwait(false);
            _response.Dispose();
        }
    }

    // The audio of `data` to the pipe, the metadata blocks to Title. Answers
    // how many audio bytes it wrote.
    private int WriteAudio(ReadOnlySpan<byte> data)
    {
        if (_metaInterval == 0)
        {
            _pipe.Writer.Write(data);
            return data.Length;
        }

        var audio = 0;
        while (!data.IsEmpty)
        {
            switch (_part)
            {
                case Part.Audio:
                    var take = Math.Min(data.Length, _left);
                    _pipe.Writer.Write(data[..take]);
                    audio += take;
                    data = data[take..];
                    _left -= take;
                    if (_left == 0)
                        _part = Part.Length;
                    break;

                case Part.Length:
                    _metaLength = data[0] * 16;
                    data = data[1..];
                    (_part, _left) = _metaLength == 0 ? (Part.Audio, _metaInterval) : (Part.Metadata, _metaLength);
                    break;

                case Part.Metadata:
                    var part = Math.Min(data.Length, _left);
                    data[..part].CopyTo(_metadata.AsSpan(_metaLength - _left));
                    data = data[part..];
                    _left -= part;
                    if (_left == 0)
                    {
                        ReadTitle(_metadata.AsSpan(0, _metaLength));
                        (_part, _left) = (Part.Audio, _metaInterval);
                    }
                    break;
            }
        }
        return audio;
    }

    // StreamTitle='Artist - Song';StreamUrl='';, padded with zeros.
    private void ReadTitle(ReadOnlySpan<byte> block)
    {
        const string key = "StreamTitle='";
        var text = Encoding.UTF8.GetString(block).TrimEnd('\0');
        var start = text.IndexOf(key, StringComparison.Ordinal);
        if (start < 0)
            return;
        start += key.Length;
        var end = text.IndexOf("';", start, StringComparison.Ordinal);
        var title = (end < 0 ? text[start..].TrimEnd('\'') : text[start..end]).Trim();
        _title = title.Length == 0 ? null : title;
    }

    // Past the last byte the upstream sent: its failure, or the end of a relay
    // that was closed.
    private int Consumed(int count)
    {
        if (count == 0 && _failure is { } failure)
            throw new IOException(failure.Message, failure);
        Interlocked.Add(ref _buffered, -count);
        return count;
    }

    // The pipe's reader, counting what leaves the buffer. Closing it closes
    // the relay and its upstream.
    private sealed class Reader(StationRelay relay, Stream inner) : Stream
    {
        public override bool CanRead => true;

        public override bool CanSeek => false;

        public override bool CanWrite => false;

        public override long Length => throw new NotSupportedException();

        public override long Position
        {
            get => throw new NotSupportedException();
            set => throw new NotSupportedException();
        }

        public override async ValueTask<int> ReadAsync(Memory<byte> buffer, CancellationToken cancellationToken = default) =>
            buffer.IsEmpty ? 0 : relay.Consumed(await inner.ReadAsync(buffer, cancellationToken).ConfigureAwait(false));

        public override Task<int> ReadAsync(byte[] buffer, int offset, int count, CancellationToken cancellationToken) =>
            ReadAsync(buffer.AsMemory(offset, count), cancellationToken).AsTask();

        public override int Read(byte[] buffer, int offset, int count) =>
            count == 0 ? 0 : relay.Consumed(inner.Read(buffer, offset, count));

        public override void Flush()
        {
        }

        public override long Seek(long offset, SeekOrigin origin) => throw new NotSupportedException();

        public override void SetLength(long value) => throw new NotSupportedException();

        public override void Write(byte[] buffer, int offset, int count) => throw new NotSupportedException();

        protected override void Dispose(bool disposing)
        {
            if (disposing)
            {
                relay.Dispose();
                inner.Dispose();
            }
            base.Dispose(disposing);
        }
    }
}
