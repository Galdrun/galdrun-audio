using System.Collections.Concurrent;
using System.Globalization;
using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Http;
using Microsoft.Extensions.Logging.Abstractions;
using Multiroom.Server.Abstractions;
using Multiroom.Server.Providers.Streaming;

namespace Multiroom.Server.Tests;

// The stream window against a fake CDN, independent of any provider, in both
// stores: what it fetches for a reader's window, what it holds, what a seek
// back costs, release, and signed URLs that expire.
public sealed class StreamWindowTests : IAsyncDisposable
{
    private const int Chunk = StreamWindow.ChunkBytes;
    private const long WindowBytes = StreamingSettings.MinimumWindowBytes;
    private static readonly TimeSpan Patience = TimeSpan.FromSeconds(10);

    // 13 pieces of 1 MiB ranges, the last of 1000 bytes; or 13 segments of
    // different lengths, the first and last small as DASH's are.
    private static readonly int[] SegmentLengths =
        [5000, 700_000, 1_500_000, 900_000, 1_200_000, 650_000, 1_100_000, 800_000, 1_300_000, 1_000_000, 750_000, 1_400_000, 3000];

    private readonly TempDirectory _dir = new();
    private readonly HttpClient _http = new(new SocketsHttpHandler { UseProxy = false });
    private readonly List<StreamWindow> _windows = [];
    private FakeCdn? _cdn;

    private static CancellationToken Ct => TestContext.Current.CancellationToken;

    private string Pieces => Path.Combine(_dir.Path, "streams", "fake");

    public async ValueTask DisposeAsync()
    {
        foreach (var window in _windows)
            window.Dispose();
        // Disk pieces are freed once the workers have stopped, on their own threads.
        await Until(() => !Directory.Exists(Pieces) || Directory.GetFileSystemEntries(Pieces).Length == 0);
        if (_cdn is not null)
            await _cdn.DisposeAsync();
        _http.Dispose();
        _dir.Dispose();
    }

    [Theory]
    [InlineData(TrackBuffer.Memory)]
    [InlineData(TrackBuffer.Disk)]
    public async Task A_reader_at_the_start_costs_only_the_first_and_last_piece_and_its_window(TrackBuffer buffer)
    {
        var cdn = await CdnAsync();
        var data = Bytes(12 * Chunk + 1000, 1);
        cdn.Files["t"] = data;
        var window = Window(buffer, WindowBytes);

        var resolved = await ResolveAsync(window, Manifest(cdn, "t"));
        await Until(() => cdn.PieceFetches == 2);
        Assert.Equal(Chunk + 1000L, window.Held.Bytes);

        await using var reader = resolved.Open().Value;
        Assert.Equal(data[..10], await ReadAsync(reader, 10));
        // Pieces 0 to 3 are its 4 MiB, and 12 is the last.
        await Until(() => cdn.PieceFetches == 5 && window.Held.Bytes == 4L * Chunk + 1000);
        await Task.Delay(200, Ct);
        Assert.Equal(5, cdn.PieceFetches);
        Assert.Equal((1, 1, 4L * Chunk + 1000), window.Held);
    }

    [Theory]
    [InlineData(TrackBuffer.Memory, false)]
    [InlineData(TrackBuffer.Disk, false)]
    [InlineData(TrackBuffer.Memory, true)]
    [InlineData(TrackBuffer.Disk, true)]
    public async Task Reading_through_holds_no_more_than_the_bound_and_a_seek_back_fetches_the_same_bytes_again(
        TrackBuffer buffer, bool segments)
    {
        var cdn = await CdnAsync();
        var (manifest, data, lengths) = Track(cdn, segments);
        var window = Window(buffer, WindowBytes);
        var resolved = await ResolveAsync(window, manifest);
        Assert.Equal(data.Length, resolved.Length);

        // The window ahead, one piece behind, and the first and the last.
        var bound = WindowBytes + lengths.Max() + lengths[0] + lengths[^1];
        await using var reader = resolved.Open().Value;
        var read = new MemoryStream();
        var chunk = new byte[64 * 1024];
        int n;
        while ((n = await reader.ReadAsync(chunk, Ct)) > 0)
        {
            read.Write(chunk, 0, n);
            Assert.True(window.Held.Bytes <= bound, $"{window.Held.Bytes} held, bound {bound}");
        }
        Assert.Equal(data, read.ToArray());

        var before = cdn.PieceFetches;
        var back = data.Length / 3;
        reader.Position = back;
        Assert.Equal(data[back..(back + 1000)], await ReadAsync(reader, 1000));
        Assert.True(cdn.PieceFetches > before);
    }

    [Theory]
    [InlineData(TrackBuffer.Memory)]
    [InlineData(TrackBuffer.Disk)]
    public async Task A_window_of_0_downloads_the_whole_track_without_a_reader(TrackBuffer buffer)
    {
        var cdn = await CdnAsync();
        var (manifest, data, lengths) = Track(cdn, segments: false);
        var window = Window(buffer, 0);

        await ResolveAsync(window, manifest);
        await Until(() => cdn.PieceFetches == lengths.Length && window.Held.Bytes == data.Length);
        Assert.Contains("window whole track", window.Describe(), StringComparison.Ordinal);
    }

    [Theory]
    [InlineData(TrackBuffer.Memory)]
    [InlineData(TrackBuffer.Disk)]
    public async Task Release_frees_everything_and_leftovers_are_deleted_at_start(TrackBuffer buffer)
    {
        var leftover = Path.Combine(Pieces, "old.TAG-7", "0");
        Directory.CreateDirectory(Path.GetDirectoryName(leftover)!);
        await File.WriteAllBytesAsync(leftover, [1, 2, 3], Ct);
        var cdn = await CdnAsync();
        var (manifest, data, _) = Track(cdn, segments: true);
        var window = Window(buffer, WindowBytes);
        Assert.False(File.Exists(leftover));

        var resolved = await ResolveAsync(window, manifest);
        var reader = resolved.Open().Value;
        Assert.Equal(data[..2000], await ReadAsync(reader, 2000));
        Assert.Equal(buffer == TrackBuffer.Disk ? 1 : 0, Directory.Exists(Pieces) ? Directory.GetDirectories(Pieces).Length : 0);

        // An open reader keeps what it holds until it closes.
        resolved.Release();
        Assert.Equal((0, 0, 0L), window.Held);
        Assert.Equal(ProviderError.Gone, resolved.Open().Failure.Code);
        await reader.DisposeAsync();
        await Until(() => !Directory.Exists(Pieces) || Directory.GetFileSystemEntries(Pieces).Length == 0);
        var fetched = cdn.PieceFetches;
        await Task.Delay(100, Ct);
        Assert.Equal(fetched, cdn.PieceFetches);
    }

    [Theory]
    [InlineData(TrackBuffer.Memory)]
    [InlineData(TrackBuffer.Disk)]
    public async Task Expired_urls_are_renewed_through_the_manifest_and_playback_goes_on(TrackBuffer buffer)
    {
        var cdn = await CdnAsync();
        var data = Bytes(12 * Chunk + 1000, 2);
        cdn.Files["t"] = data;
        var manifests = 0;
        var window = Window(buffer, WindowBytes);
        var resolved = await ResolveAsync(window, ct =>
        {
            Interlocked.Increment(ref manifests);
            return Manifest(cdn, "t")(ct);
        });

        cdn.Token = "renewed";
        await using var reader = resolved.Open().Value;
        Assert.Equal(data, await ReadAsync(reader, data.Length));
        Assert.Equal(2, manifests);
    }

    [Fact]
    public async Task Renewed_urls_of_another_length_fail_the_track_as_gone()
    {
        var cdn = await CdnAsync();
        var data = Bytes(12 * Chunk + 1000, 3);
        cdn.Files["t"] = data;
        var window = Window(TrackBuffer.Memory, WindowBytes);
        var resolved = await ResolveAsync(window, Manifest(cdn, "t"));

        cdn.Token = "renewed";
        cdn.Files["t"] = Bytes(12 * Chunk + 2000, 3);
        await using var reader = resolved.Open().Value;
        var failed = await Assert.ThrowsAnyAsync<IOException>(() => ReadAsync(reader, data.Length));
        Assert.Contains("not the same bytes", failed.Message, StringComparison.Ordinal);
        var gone = resolved.Open();
        Assert.Equal(ProviderError.Gone, gone.Failure.Code);
    }

    // --- helpers ------------------------------------------------------------

    private async Task<FakeCdn> CdnAsync() => _cdn = await FakeCdn.StartAsync();

    private StreamWindow Window(TrackBuffer buffer, long windowBytes)
    {
        var window = new StreamWindow(_http, new StreamingSettings(buffer, windowBytes, Path.Combine(_dir.Path, "streams")),
            Pieces, NullLogger.Instance);
        _windows.Add(window);
        return window;
    }

    private static async Task<ResolvedStream> ResolveAsync(StreamWindow window,
        Func<CancellationToken, Task<Result<StreamManifest, ProviderError>>> manifest)
    {
        var resolved = await window.GetAsync("track-1", "Fixture", manifest, Ct);
        Assert.True(resolved.IsSuccess, resolved.ToString());
        return resolved.Value;
    }

    // Each call names the URLs as the CDN signs them now.
    private static Func<CancellationToken, Task<Result<StreamManifest, ProviderError>>> Manifest(FakeCdn cdn,
        params string[] names) =>
        _ => Task.FromResult<Result<StreamManifest, ProviderError>>(
            new StreamManifest("audio/flac", [.. names.Select(cdn.Url)], Tag: "LOSSLESS"));

    private static (Func<CancellationToken, Task<Result<StreamManifest, ProviderError>>> Manifest, byte[] Data,
        int[] Lengths) Track(FakeCdn cdn, bool segments)
    {
        if (!segments)
        {
            var data = Bytes(12 * Chunk + 1000, 4);
            cdn.Files["t"] = data;
            return (Manifest(cdn, "t"), data, [.. Enumerable.Repeat(Chunk, 12), 1000]);
        }
        var names = new string[SegmentLengths.Length];
        for (var i = 0; i < names.Length; i++)
        {
            names[i] = $"s{i}";
            cdn.Files[names[i]] = Bytes(SegmentLengths[i], i);
        }
        return (Manifest(cdn, names), [.. names.SelectMany(n => cdn.Files[n])], SegmentLengths);
    }

    private static async Task<byte[]> ReadAsync(Stream reader, int count)
    {
        var buffer = new byte[count];
        await reader.ReadExactlyAsync(buffer, Ct);
        return buffer;
    }

    private static byte[] Bytes(int count, int seed) =>
        [.. Enumerable.Range(0, count).Select(i => (byte)((i * 7 + seed * 13 + i / 251) & 0xFF))];

    private static async Task Until(Func<bool> condition)
    {
        using var timeout = new CancellationTokenSource(Patience);
        while (!condition())
            await Task.Delay(10, timeout.Token);
    }

    // Files at cdn/<name>?token=<token>, with ranges; a URL signed with an
    // older token is refused with 403.
    private sealed class FakeCdn : IAsyncDisposable
    {
        private readonly WebApplication _app;
        private readonly Uri _base;
        private int _pieceFetches;

        private FakeCdn(WebApplication app, Uri baseUri)
        {
            _app = app;
            _base = baseUri;
        }

        public ConcurrentDictionary<string, byte[]> Files { get; } = new(StringComparer.Ordinal);

        public string Token { get; set; } = "first";

        // Fetches of a piece, not of a probe's first byte.
        public int PieceFetches => Volatile.Read(ref _pieceFetches);

        public static async Task<FakeCdn> StartAsync()
        {
            FakeCdn? cdn = null;
            var (app, uri) = await FixtureHttp.StartAsync(context => cdn!.HandleAsync(context));
            cdn = new FakeCdn(app, uri);
            return cdn;
        }

        public Uri Url(string name) => new(_base, $"cdn/{name}?token={Token}");

        public ValueTask DisposeAsync() => _app.DisposeAsync();

        private async Task HandleAsync(HttpContext context)
        {
            var name = (context.Request.Path.Value ?? "")["/cdn/".Length..];
            if (context.Request.Query["token"] != Token || !Files.TryGetValue(name, out var data))
            {
                context.Response.StatusCode = 403;
                return;
            }
            var range = context.Request.Headers.Range.ToString();
            if (range != "bytes=0-0")
                Interlocked.Increment(ref _pieceFetches);
            if (range.StartsWith("bytes=", StringComparison.Ordinal))
            {
                var bounds = range["bytes=".Length..].Split('-');
                var from = long.Parse(bounds[0], CultureInfo.InvariantCulture);
                var to = Math.Min(long.Parse(bounds[1], CultureInfo.InvariantCulture), data.Length - 1);
                context.Response.StatusCode = 206;
                context.Response.Headers.ContentRange = $"bytes {from}-{to}/{data.Length}";
                context.Response.ContentLength = to - from + 1;
                await context.Response.Body.WriteAsync(data.AsMemory((int)from, (int)(to - from + 1)));
                return;
            }
            context.Response.ContentLength = data.Length;
            await context.Response.Body.WriteAsync(data);
        }
    }
}
