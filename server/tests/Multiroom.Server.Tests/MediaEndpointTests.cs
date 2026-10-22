using System.Collections.Concurrent;
using System.Diagnostics;
using System.Net;
using System.Net.Http.Headers;
using System.Net.Sockets;
using System.Text;
using Microsoft.Extensions.Logging;
using Multiroom.Server.Abstractions;
using Multiroom.Server.Web.Media;

namespace Multiroom.Server.Tests;

// The media endpoint contract, one test per MUST, against a
// real in-process Kestrel on port 0. Every request carries what FFmpeg sends:
// a Range, Icy-MetaData: 1 and Connection: close. Every source is one the fake
// provider resolved, as the queue engine will resolve one at stage time.
public sealed class MediaEndpointTests : IAsyncDisposable
{
    private const string Token = "t0ken-that-must-not-leak";

    private readonly TempDirectory _dir = new();
    private readonly MediaHandles _handles = new();
    private readonly ConcurrentQueue<string> _logLines = new();
    private readonly ILoggerFactory _loggers;
    private readonly HttpClient _http;
    private readonly List<MediaEndpoint> _endpoints = [];
    private readonly FakeProvider _fake = new();
    private readonly byte[] _data;
    private readonly string _file;
    private readonly Track _track;

    public MediaEndpointTests()
    {
        _loggers = LoggerFactory.Create(b => b.SetMinimumLevel(LogLevel.Trace)
            .AddProvider(new CapturingProvider(_logLines)));
        _http = new HttpClient(new SocketsHttpHandler
        {
            AllowAutoRedirect = false,
            AutomaticDecompression = DecompressionMethods.None,
            UseProxy = false,
        });
        _data = new byte[300_001];
        new Random(21).NextBytes(_data);
        _file = Path.Combine(_dir.Path, "track.flac");
        File.WriteAllBytes(_file, _data);
        _track = _fake.AddTrack(_file, "Track");
    }

    public async ValueTask DisposeAsync()
    {
        foreach (var endpoint in _endpoints)
        {
            await endpoint.StopAsync(CancellationToken.None);
            await endpoint.DisposeAsync();
        }
        _http.Dispose();
        _loggers.Dispose();
        _dir.Dispose();
    }

    private static CancellationToken Ct => TestContext.Current.CancellationToken;

    [Fact]
    public async Task The_probe_pattern_returns_the_same_bytes_from_exactly_the_offset_asked()
    {
        var uri = await Serve(await _fake.ResolveAsync(_track, Ct));
        var n = _data.Length;

        // First from 0, then the last 128 bytes, then the middle, then odd ones.
        foreach (var offset in new long[] { 0, n - 128, n / 2, 12345, n - 1, 1 })
        {
            using var response = await Get(uri, range: offset);

            Assert.Equal(HttpStatusCode.PartialContent, response.StatusCode);
            Assert.Equal($"bytes {offset}-{n - 1}/{n}", response.Content.Headers.ContentRange?.ToString());
            Assert.Equal(_data[(int)offset..], await response.Content.ReadAsByteArrayAsync(Ct));
        }
    }

    [Fact]
    public async Task Without_a_range_a_known_length_is_announced_as_seekable()
    {
        var uri = await Serve(await _fake.ResolveAsync(_track, Ct));

        using var response = await Get(uri, range: null);

        Assert.Equal(HttpStatusCode.OK, response.StatusCode);
        Assert.Equal(_data.Length, response.Content.Headers.ContentLength);
        Assert.Equal(["bytes"], response.Headers.AcceptRanges);
        Assert.Equal("audio/flac", response.Content.Headers.ContentType?.MediaType);
        Assert.Equal(_data, await response.Content.ReadAsByteArrayAsync(Ct));
    }

    [Fact]
    public async Task Concurrent_gets_mid_source_agree_with_the_first_at_every_offset()
    {
        var uri = await Serve(await _fake.ResolveAsync(_track, Ct));
        using var first = await Get(uri, range: 0, HttpCompletionOption.ResponseHeadersRead);
        await using var firstBody = await first.Content.ReadAsStreamAsync(Ct);
        var head = new byte[100_000];
        await firstBody.ReadExactlyAsync(head, Ct);

        var offsets = new[] { 7L, 99_999, 100_000, 150_001, 299_000, 3, 250_000, 42 };
        var others = await Task.WhenAll(offsets.Select(async offset =>
        {
            using var response = await Get(uri, range: offset);
            return (offset, bytes: await response.Content.ReadAsByteArrayAsync(Ct));
        }));
        var rest = new MemoryStream();
        await firstBody.CopyToAsync(rest, Ct);

        Assert.Equal(_data, (byte[])[.. head, .. rest.ToArray()]);
        foreach (var (offset, bytes) in others)
            Assert.Equal(_data[(int)offset..], bytes);
    }

    [Fact]
    public async Task Nothing_is_ever_compressed_and_no_icy_metadata_is_interleaved()
    {
        var uri = await Serve(await _fake.ResolveAsync(_track, Ct));

        using var response = await Get(uri, range: 0);

        Assert.Empty(response.Content.Headers.ContentEncoding);
        Assert.DoesNotContain(response.Headers, h => h.Key.StartsWith("icy-", StringComparison.OrdinalIgnoreCase));
        Assert.Equal(_data, await response.Content.ReadAsByteArrayAsync(Ct));
    }

    [Fact]
    public async Task No_request_is_ever_answered_with_a_redirect_and_only_media_routes_exist()
    {
        var uri = await Serve(await _fake.ResolveAsync(_track, Ct));
        var root = new Uri(uri, "/");
        var handle = uri.Segments[^1];

        foreach (var path in new[] { "/media", "/media/", $"/media/{handle}/x", "/media/unknownhandle",
                     "/api/v1/status", "/api/v1/media/" + handle, "/" })
        {
            using var response = await Get(new Uri(root, path), range: 0);
            Assert.Equal(HttpStatusCode.NotFound, response.StatusCode);
        }
        // Routing matches a trailing slash; it is served, not redirected.
        using var slash = await Get(new Uri(root, $"/media/{handle}/"), range: 0);
        Assert.Equal(HttpStatusCode.PartialContent, slash.StatusCode);
    }

    [Fact]
    public async Task The_token_is_required_when_set_and_is_in_no_log_line()
    {
        var uri = await Serve(await _fake.ResolveAsync(_track, Ct), Token);

        using (var none = await Get(uri, range: 0))
            Assert.Equal(HttpStatusCode.Unauthorized, none.StatusCode);
        using (var wrong = await Get(uri, range: 0, token: Token + "x"))
            Assert.Equal(HttpStatusCode.Unauthorized, wrong.StatusCode);
        using (var unknown = await Get(new Uri(uri, "unknownhandle"), range: 0))
            Assert.Equal(HttpStatusCode.Unauthorized, unknown.StatusCode);
        using (var right = await Get(uri, range: 0, token: Token))
        {
            Assert.Equal(HttpStatusCode.PartialContent, right.StatusCode);
            Assert.Equal(_data, await right.Content.ReadAsByteArrayAsync(Ct));
        }

        Assert.NotEmpty(_logLines);
        Assert.DoesNotContain(_logLines, line => line.Contains(Token, StringComparison.Ordinal));
    }

    [Fact]
    public async Task A_live_source_is_chunked_without_length_or_ranges()
    {
        var uri = await Serve(await _fake.ResolveAsync(_fake.AddStation("Live", _data), Ct));

        // Headers only first: once the body is buffered, HttpClient fills in a length.
        using var response = await Get(uri, range: 0, HttpCompletionOption.ResponseHeadersRead);

        Assert.Equal(HttpStatusCode.OK, response.StatusCode);
        Assert.Null(response.Content.Headers.ContentLength);
        Assert.True(response.Headers.TransferEncodingChunked);
        Assert.Empty(response.Headers.AcceptRanges);
        Assert.Null(response.Content.Headers.ContentRange);
        Assert.Equal(_data, await response.Content.ReadAsByteArrayAsync(Ct));
    }

    [Fact]
    public async Task A_file_that_changed_or_vanished_is_gone()
    {
        var uri = await Serve(await _fake.ResolveAsync(_track, Ct));

        await File.WriteAllBytesAsync(_file, _data[..1000], Ct);
        using (var changed = await Get(uri, range: 0))
            Assert.Equal(HttpStatusCode.NotFound, changed.StatusCode);

        File.Delete(_file);
        using (var vanished = await Get(uri, range: 0))
            Assert.Equal(HttpStatusCode.NotFound, vanished.StatusCode);
    }

    [Fact]
    public async Task A_published_url_serves_until_it_is_retired_and_is_gone_after()
    {
        var endpoint = new MediaEndpoint(new MediaEndpointOptions(IPAddress.Loopback, 0, null), _handles, _loggers);
        _endpoints.Add(endpoint);
        await endpoint.StartAsync(Ct);
        var publisher = endpoint;

        var uri = publisher.Publish((await _fake.ResolveAsync(_track, Ct)).Value);
        Assert.StartsWith(endpoint.BaseUri!.AbsoluteUri, uri.AbsoluteUri, StringComparison.Ordinal);
        using (var served = await Get(uri, range: 7))
            Assert.Equal(HttpStatusCode.PartialContent, served.StatusCode);

        publisher.Retire(uri);
        using (var retired = await Get(uri, range: 0))
            Assert.Equal(HttpStatusCode.NotFound, retired.StatusCode);
        Assert.Equal(0, _handles.Count);
    }

    [Fact]
    public async Task A_reader_that_pauses_is_not_cut_and_resumes_with_the_right_bytes()
    {
        var uri = await Serve(await _fake.ResolveAsync(_track, Ct));
        using var socket = await RawGet(uri, receiveBuffer: 4096);
        await using var stream = new NetworkStream(socket);

        var start = new byte[4096];
        await stream.ReadExactlyAsync(start, Ct);
        await Task.Delay(TimeSpan.FromSeconds(2), Ct);
        var rest = new MemoryStream();
        await stream.CopyToAsync(rest, Ct);

        var response = (byte[])[.. start, .. rest.ToArray()];
        var headerEnd = response.AsSpan().IndexOf("\r\n\r\n"u8) + 4;
        Assert.StartsWith("HTTP/1.1 206", Encoding.ASCII.GetString(response, 0, headerEnd), StringComparison.Ordinal);
        Assert.Equal(_data, response[headerEnd..]);
    }

    [Fact]
    public async Task Stopping_does_not_wait_for_a_held_connection()
    {
        var uri = await Serve(await _fake.ResolveAsync(_track, Ct));
        var endpoint = _endpoints[^1];
        using var socket = await RawGet(uri, receiveBuffer: 4096);
        await Task.Delay(200, Ct); // Kestrel is now writing into a full socket.

        var clock = Stopwatch.StartNew();
        await endpoint.StopAsync(CancellationToken.None);

        Assert.True(clock.Elapsed < TimeSpan.FromSeconds(5), $"stop took {clock.Elapsed}");
    }

    private async Task<Uri> Serve(Result<ResolvedStream, ProviderError> resolved, string? token = null)
    {
        var endpoint = new MediaEndpoint(new MediaEndpointOptions(IPAddress.Loopback, 0, token), _handles, _loggers);
        _endpoints.Add(endpoint);
        await endpoint.StartAsync(Ct);
        return new Uri(endpoint.BaseUri!, _handles.Register(resolved.Value));
    }

    private async Task<HttpResponseMessage> Get(Uri uri, long? range, HttpCompletionOption completion =
        HttpCompletionOption.ResponseContentRead, string? token = null)
    {
        using var request = new HttpRequestMessage(HttpMethod.Get, uri);
        request.Headers.UserAgent.ParseAdd("Lavf/62.3.100");
        request.Headers.Accept.ParseAdd("*/*");
        request.Headers.ConnectionClose = true;
        request.Headers.Add("Icy-MetaData", "1");
        // FFmpeg sends none; this proves none is honoured if something did.
        request.Headers.AcceptEncoding.ParseAdd("gzip");
        if (range is not null)
            request.Headers.Range = new RangeHeaderValue(range, null);
        if (token is not null)
            request.Headers.Authorization = new AuthenticationHeaderValue("Bearer", token);
        return await _http.SendAsync(request, completion, Ct);
    }

    // FFmpeg's request, byte for byte, on a socket whose receive buffer is small
    // enough that the server's writes block on TCP flow control.
    private static async Task<Socket> RawGet(Uri uri, int receiveBuffer)
    {
        var socket = new Socket(AddressFamily.InterNetwork, SocketType.Stream, ProtocolType.Tcp)
        {
            ReceiveBufferSize = receiveBuffer,
        };
        await socket.ConnectAsync(IPAddress.Loopback, uri.Port, Ct);
        var request = $"GET {uri.PathAndQuery} HTTP/1.1\r\nUser-Agent: Lavf/62.3.100\r\nAccept: */*\r\n" +
            $"Range: bytes=0-\r\nConnection: close\r\nHost: {uri.Authority}\r\nIcy-MetaData: 1\r\n\r\n";
        await socket.SendAsync(Encoding.ASCII.GetBytes(request), Ct);
        return socket;
    }

    internal sealed class CapturingProvider(ConcurrentQueue<string> lines) : ILoggerProvider
    {
        public ILogger CreateLogger(string categoryName) => new Capturing(categoryName, lines);

        public void Dispose()
        {
        }

        private sealed class Capturing(string category, ConcurrentQueue<string> lines) : ILogger
        {
            public IDisposable? BeginScope<TState>(TState state)
                where TState : notnull => null;

            public bool IsEnabled(LogLevel logLevel) => true;

            public void Log<TState>(LogLevel logLevel, EventId eventId, TState state, Exception? exception,
                Func<TState, Exception?, string> formatter) =>
                lines.Enqueue($"{category}: {formatter(state, exception)} {exception}");
        }
    }
}
