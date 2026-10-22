using System.Collections.Concurrent;
using System.Net;
using System.Net.Sockets;
using System.Text;

namespace Multiroom.Server.Tests;

// An internet radio station on a free loopback port, over a plain socket as a
// SHOUTcast or Icecast server is: any GET answers an endless chunked stream,
// audio byte i being Pattern(i), as fast as the reader takes it. With
// MetaInterval set, it interleaves ICY metadata blocks: Title in the first
// block, empty ones after. DropAfter cuts the body off after that many audio
// bytes, every one of them sent first (FIN, no last chunk); EndAfter ends it
// cleanly with the last chunk.
internal sealed class FakeStation : IAsyncDisposable
{
    private readonly TcpListener _listener = new(IPAddress.Loopback, 0);
    private readonly CancellationTokenSource _stop = new();
    private readonly ConcurrentDictionary<Socket, byte> _sockets = new();
    private readonly Task _accepting;
    private int _open;

    public FakeStation()
    {
        _listener.Start();
        Url = new Uri($"http://127.0.0.1:{((IPEndPoint)_listener.LocalEndpoint).Port}/stream");
        _accepting = AcceptAsync();
    }

    public Uri Url { get; }

    public int MetaInterval { get; set; }

    public string Title { get; set; } = "";

    public long? DropAfter { get; set; }

    public long? EndAfter { get; set; }

    public HttpStatusCode Status { get; set; } = HttpStatusCode.OK;

    // Each request's headers, in order.
    public ConcurrentQueue<Dictionary<string, string>> Requests { get; } = new();

    // Responses being written now.
    public int Open => Volatile.Read(ref _open);

    public static byte Pattern(long i) => (byte)(i * 7 % 251);

    public async ValueTask DisposeAsync()
    {
        _stop.Cancel();
        _listener.Stop();
        await _accepting;
        foreach (var socket in _sockets.Keys)
            socket.Dispose();
        _stop.Dispose();
    }

    private async Task AcceptAsync()
    {
        while (true)
        {
            Socket socket;
            try
            {
                socket = await _listener.AcceptSocketAsync(_stop.Token);
            }
            catch (Exception e) when (e is OperationCanceledException or SocketException or ObjectDisposedException)
            {
                return;
            }
            _sockets[socket] = 0;
            _ = ServeAsync(socket);
        }
    }

    private async Task ServeAsync(Socket socket)
    {
        Interlocked.Increment(ref _open);
        var stream = new NetworkStream(socket, ownsSocket: true);
        try
        {
            Requests.Enqueue(await ReadHeadersAsync(stream));
            if (Status != HttpStatusCode.OK)
            {
                await WriteAsync(stream, $"HTTP/1.1 {(int)Status} No\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                return;
            }
            await WriteAsync(stream, "HTTP/1.1 200 OK\r\nContent-Type: audio/mpeg\r\nTransfer-Encoding: chunked\r\n" +
                (MetaInterval > 0 ? $"icy-metaint: {MetaInterval}\r\n" : "") + "Connection: close\r\n\r\n");

            var audio = new byte[4096];
            long sent = 0;
            var untilMeta = MetaInterval;
            var firstBlock = true;
            while (true)
            {
                if (DropAfter is { } drop && sent >= drop)
                {
                    socket.Shutdown(SocketShutdown.Send);
                    return;
                }
                if (EndAfter is { } end && sent >= end)
                {
                    await WriteAsync(stream, "0\r\n\r\n");
                    return;
                }

                var size = audio.Length;
                if (MetaInterval > 0)
                    size = Math.Min(size, untilMeta);
                if ((DropAfter ?? EndAfter) is { } limit)
                    size = (int)Math.Min(size, limit - sent);
                for (var i = 0; i < size; i++)
                    audio[i] = Pattern(sent + i);
                var chunk = new MemoryStream();
                chunk.Write(audio, 0, size);
                sent += size;
                if (MetaInterval > 0 && (untilMeta -= size) == 0)
                {
                    chunk.Write(Metadata(firstBlock ? Title : null));
                    firstBlock = false;
                    untilMeta = MetaInterval;
                }
                await WriteAsync(stream, $"{chunk.Length:x}\r\n");
                await stream.WriteAsync(chunk.GetBuffer().AsMemory(0, (int)chunk.Length), _stop.Token);
                await WriteAsync(stream, "\r\n");
            }
        }
        catch (Exception e) when (e is IOException or SocketException or OperationCanceledException or ObjectDisposedException)
        {
            // The relay went away, or the station was stopped.
        }
        finally
        {
            _sockets.TryRemove(socket, out _);
            await stream.DisposeAsync();
            Interlocked.Decrement(ref _open);
        }
    }

    private async Task WriteAsync(NetworkStream stream, string text) =>
        await stream.WriteAsync(Encoding.ASCII.GetBytes(text), _stop.Token);

    // Up to the blank line; a GET has no body.
    private async Task<Dictionary<string, string>> ReadHeadersAsync(NetworkStream stream)
    {
        var received = new List<byte>();
        var one = new byte[1];
        while (received.Count < 4 || !(received[^4] == '\r' && received[^3] == '\n' && received[^2] == '\r' && received[^1] == '\n'))
        {
            if (await stream.ReadAsync(one, _stop.Token) == 0)
                throw new IOException("the request ended early");
            received.Add(one[0]);
        }
        return Encoding.ASCII.GetString([.. received]).Split("\r\n", StringSplitOptions.RemoveEmptyEntries).Skip(1)
            .Select(line => line.Split(':', 2))
            .ToDictionary(p => p[0].Trim(), p => p[1].Trim(), StringComparer.OrdinalIgnoreCase);
    }

    // A length byte, then the text padded with zeros to a multiple of 16.
    private static byte[] Metadata(string? title)
    {
        if (title is null)
            return [0];
        var text = Encoding.UTF8.GetBytes($"StreamTitle='{title}';");
        var blocks = (text.Length + 15) / 16;
        var block = new byte[1 + (blocks * 16)];
        block[0] = (byte)blocks;
        text.CopyTo(block, 1);
        return block;
    }
}
