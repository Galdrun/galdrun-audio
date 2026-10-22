using System.Net;
using System.Text;
using System.Threading.Channels;
using Multiroom.App.Api;

namespace Multiroom.App.Tests;

public sealed class StateAndArtworkTests
{
    [Theory]
    [InlineData("events.lost")] [InlineData("core.eventsLost")] [InlineData("core.synchronised")]
    [InlineData("queue.changed")] [InlineData("queue.deleted")]
    public async Task Events_invalidate_and_reread_authoritative_state(string eventType)
    {
        using var stream = new FixtureEventStream();
        var first = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var updated = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var changed = false; var reads = 0; var providerReads = 0;
        var api = new MultiroomClient("http://fake", handler: new FakeHttp((r, _) =>
        {
            switch (r.RequestUri!.AbsolutePath)
            {
                case "/api/v1/events":
                    var content = new StreamContent(stream); content.Headers.ContentType = new("text/event-stream");
                    return Task.FromResult(new HttpResponseMessage(HttpStatusCode.OK) { Content = content });
                case "/api/v1/zones":
                    Interlocked.Increment(ref reads);
                    return Task.FromResult(BrowserTests.Reply(changed ? Array.Empty<ZoneView>() : PlaybackTests.Snapshot().Zones));
                case "/api/v1/rooms": return Task.FromResult(FakeHttp.Json("[]"));
                case "/api/v1/providers": Interlocked.Increment(ref providerReads); return Task.FromResult(FakeHttp.Json("[]"));
                default: throw new InvalidOperationException("Unexpected request");
            }
        }));
        await using var session = new ControllerSession(api);
        session.PlaybackChanged += snapshot => { if (snapshot.Zones.Count == 0) updated.TrySetResult(); };
        session.ProvidersChanged += _ => first.TrySetResult();
        session.Start();
        await first.Task.WaitAsync(TimeSpan.FromSeconds(5), TestContext.Current.CancellationToken);
        changed = true;
        // Event data deliberately disagrees with REST. The event is not a complete snapshot.
        stream.Publish($"event: {eventType}\ndata: {{\"zone\":\"wrong\"}}\n\n");
        await updated.Task.WaitAsync(TimeSpan.FromSeconds(5), TestContext.Current.CancellationToken);
        Assert.Empty(session.Playback.Zones); Assert.True(reads >= 2); Assert.True(providerReads >= 1);
    }

    [Fact]
    public async Task Artwork_is_fetched_once_and_entry_count_is_bounded()
    {
        var requests = 0;
        using var api = new MultiroomClient("http://fake", "art-token", new FakeHttp((r, _) =>
        {
            Assert.Equal("Bearer art-token", r.Headers.Authorization?.ToString());
            Assert.StartsWith("/api/v1/artwork/album", r.RequestUri!.AbsolutePath);
            requests++; return Task.FromResult(new HttpResponseMessage(HttpStatusCode.OK) { Content = new ByteArrayContent([1, 2, 3]) });
        }));
        using var cache = new ArtworkCache(api); var ct = TestContext.Current.CancellationToken;
        var cover = "album:1";
        await Task.WhenAll(cache.GetAsync(cover, ct), cache.GetAsync(cover, ct)); Assert.Equal(1, requests);
        for (var i = 2; i <= 129; i++) await cache.GetAsync($"album:{i}", ct);
        await cache.GetAsync(cover, ct); Assert.Equal(130, requests);
    }

    [Fact]
    public async Task Oversized_artwork_is_refused_without_caching_it()
    {
        using var api = new MultiroomClient("http://fake", handler: new FakeHttp((_, _) => Task.FromResult(
            new HttpResponseMessage(HttpStatusCode.OK) { Content = new ByteArrayContent(new byte[8 * 1024 * 1024 + 1]) })));
        using var cache = new ArtworkCache(api);
        await Assert.ThrowsAsync<IOException>(() => cache.GetAsync("album:large", TestContext.Current.CancellationToken));
    }

    [Fact]
    public async Task Artwork_cancellation_interrupts_a_pending_download()
    {
        using var stop = CancellationTokenSource.CreateLinkedTokenSource(TestContext.Current.CancellationToken);
        using var api = new MultiroomClient("http://fake", handler: new FakeHttp(async (_, ct) =>
        { stop.Cancel(); await Task.Delay(Timeout.Infinite, ct); return FakeHttp.Json("{}"); }));
        using var cache = new ArtworkCache(api);
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => cache.GetAsync("album:pending", stop.Token));
    }

    // artwork.changed: the kept picture goes, views are told, and the next ask fetches again.
    [Fact]
    public async Task An_announced_artwork_id_is_fetched_again_and_its_views_told()
    {
        var requests = 0;
        using var api = new MultiroomClient("http://fake", handler: new FakeHttp((_, _) =>
        { requests++; return Task.FromResult(new HttpResponseMessage(HttpStatusCode.OK) { Content = new ByteArrayContent([1]) }); }));
        using var cache = new ArtworkCache(api); var ct = TestContext.Current.CancellationToken;
        string? told = null; cache.Found += id => told = id;
        await cache.GetAsync("album:1", ct); await cache.GetAsync("album:1", ct);
        await cache.AnnounceAsync("album:1");
        await cache.GetAsync("album:1", ct);
        Assert.Equal((2, "album:1"), (requests, told));
    }

    // Server phase 40: a list entry is its item, of any kind, its picture by artworkId.
    [Fact]
    public void List_entries_read_their_item_of_every_kind_and_its_artwork_id()
    {
        var likes = System.Text.Json.JsonSerializer.Deserialize<LikedItem[]>("""
            [{"id":1,"item":{"kind":"track","providerId":"radio-metadata","providerItemId":"s:1","title":"Song","artworkId":"song:1"},"added":"2026-10-09T00:00:00Z"},
             {"id":2,"item":{"providerId":"local","providerItemId":"a:1","title":"Album","kind":"album","artworkId":"album:1"},"added":"2026-10-09T00:00:00Z"},
             {"id":3,"item":{"kind":"audiobook","reference":"local:book:1","title":"Book","author":"Author"},"added":"2026-10-09T00:00:00Z"}]
            """, MultiroomClient.Json)!;
        Assert.Equal(("Song", "song:1"), (Assert.IsType<Track>(likes[0].Item).Title, likes[0].Item.ArtworkId));
        Assert.Equal("album:1", Assert.IsType<Album>(likes[1].Item).ArtworkId);
        Assert.Equal("Book", Assert.IsType<Audiobook>(likes[2].Item).Title);
        var queued = System.Text.Json.JsonSerializer.Deserialize<QueueItem>(
            """{"id":4,"item":{"kind":"group","title":"Book","artist":"Author","artworkId":"album:2"},"title":"Book"}""", MultiroomClient.Json)!;
        Assert.Equal(("Book", null, null), (Assert.IsType<MediaGroup>(queued.Item).Title, queued.Track, queued.Station));
    }
}

internal sealed class FixtureEventStream : Stream
{
    private readonly Channel<byte[]> _frames = Channel.CreateUnbounded<byte[]>();
    private byte[] _current = [];
    private int _position;
    public void Publish(string frame) => _frames.Writer.TryWrite(Encoding.UTF8.GetBytes(frame));
    public override async ValueTask<int> ReadAsync(Memory<byte> buffer, CancellationToken cancellationToken = default)
    {
        if (_position == _current.Length)
        {
            if (!await _frames.Reader.WaitToReadAsync(cancellationToken)) return 0;
            _current = await _frames.Reader.ReadAsync(cancellationToken); _position = 0;
        }
        var length = Math.Min(buffer.Length, _current.Length - _position);
        _current.AsMemory(_position, length).CopyTo(buffer); _position += length; return length;
    }
    public override int Read(byte[] buffer, int offset, int count) => throw new NotSupportedException();
    public override bool CanRead => true;
    public override bool CanSeek => false;
    public override bool CanWrite => false;
    public override long Length => throw new NotSupportedException();
    public override long Position { get => throw new NotSupportedException(); set => throw new NotSupportedException(); }
    public override void Flush() => throw new NotSupportedException();
    public override long Seek(long offset, SeekOrigin origin) => throw new NotSupportedException();
    public override void SetLength(long value) => throw new NotSupportedException();
    public override void Write(byte[] buffer, int offset, int count) => throw new NotSupportedException();
    protected override void Dispose(bool disposing) { if (disposing) _frames.Writer.TryComplete(); base.Dispose(disposing); }
}
