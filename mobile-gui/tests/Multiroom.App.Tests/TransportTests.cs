using System.Net;
using System.Text;
using System.Text.Json;
using Multiroom.App.Api;

namespace Multiroom.App.Tests;

public sealed class FakeHttp(Func<HttpRequestMessage, CancellationToken, Task<HttpResponseMessage>> respond) : HttpMessageHandler
{
    protected override Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken cancellationToken) => respond(request, cancellationToken);
    public static HttpResponseMessage Json(string text, HttpStatusCode status = HttpStatusCode.OK) =>
        new(status) { Content = new StringContent(text, Encoding.UTF8, "application/json") };
}

public sealed class TransportTests
{
    [Theory]
    [InlineData("http://localhost:4725", "http://localhost:4725/api/v1/")]
    [InlineData("https://music.example/api/v1/", "https://music.example/api/v1/")]
    public void Addresses_are_normalized(string input, string expected) => Assert.Equal(expected, MultiroomClient.NormalizeAddress(input).AbsoluteUri);

    [Theory]
    [InlineData("http://localhost:4713")]
    [InlineData("http://localhost:4714")]
    [InlineData("http://user:secret@host")]
    [InlineData("file:///tmp")]
    [InlineData("http://host/media")]
    public void Non_API_addresses_are_refused(string input) => Assert.Throws<ArgumentException>(() => MultiroomClient.NormalizeAddress(input));

    [Fact]
    public async Task Token_and_escaped_browse_route_are_correct()
    {
        using var api = new MultiroomClient("http://fake:4725", "test-secret", new FakeHttp((request, _) =>
        {
            Assert.Equal("Bearer test-secret", request.Headers.Authorization?.ToString());
            Assert.Equal("/api/v1/providers/local/browse?limit=60&offset=0&node=album%3A1&sort=name&filter=a%26b", request.RequestUri!.PathAndQuery);
            return Task.FromResult(FakeHttp.Json("""{"nodes":[],"items":{"tracks":[],"albums":[],"artists":[],"stations":[]},"layout":"grid"}"""));
        }));
        var page = await api.BrowseAsync("local", "album:1", 0, "name", "a&b", null, TestContext.Current.CancellationToken);
        Assert.Equal(BrowseLayout.Grid, page.Layout);
    }

    [Theory]
    [InlineData(400)] [InlineData(401)] [InlineData(403)] [InlineData(404)] [InlineData(410)] [InlineData(503)]
    public async Task Error_status_is_preserved_without_echoing_secrets(int code)
    {
        using var api = new MultiroomClient("http://fake", handler: new FakeHttp((_, _) => Task.FromResult(FakeHttp.Json(
            """{"error":"unavailable","message":"secret-password"}""", (HttpStatusCode)code))));
        var error = await Assert.ThrowsAsync<ApiException>(() => api.StatusAsync(TestContext.Current.CancellationToken));
        Assert.Equal(code, (int)error.Status);
        Assert.DoesNotContain("secret-password", error.ToString());
    }

    [Fact]
    public async Task Malformed_success_is_explicit()
    {
        using var api = new MultiroomClient("http://fake", handler: new FakeHttp((_, _) => Task.FromResult(FakeHttp.Json("{}"))));
        Assert.Equal("invalidResponse", (await Assert.ThrowsAsync<ApiException>(() => api.StatusAsync(TestContext.Current.CancellationToken))).Code);
    }

    [Fact]
    public async Task Cancellation_reaches_HTTP()
    {
        using var stop = CancellationTokenSource.CreateLinkedTokenSource(TestContext.Current.CancellationToken);
        using var api = new MultiroomClient("http://fake", handler: new FakeHttp(async (_, ct) =>
        { stop.Cancel(); await Task.Delay(Timeout.Infinite, ct); return FakeHttp.Json("{}"); }));
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => api.StatusAsync(stop.Token));
    }

    [Fact]
    public async Task SSE_handles_comments_multiline_lost_and_incomplete_frames()
    {
        using var stream = new MemoryStream(Encoding.UTF8.GetBytes(": heartbeat\r\nid: 7\r\nevent: queue.changed\r\ndata: first\r\ndata: second\r\n\r\nevent: events.lost\ndata: {}\n\nid: 8\ndata: partial"));
        var events = new List<ServerEvent>();
        await foreach (var e in EventStream.ParseAsync(stream, TestContext.Current.CancellationToken)) events.Add(e);
        Assert.Equal(2, events.Count);
        Assert.Equal(new ServerEvent("7", "queue.changed", "first\nsecond"), events[0]);
        Assert.Null(events[1].Id);
        Assert.Equal("events.lost", events[1].Type);
    }

    [Fact]
    public async Task SSE_reconnect_sends_last_ID()
    {
        using var stop = CancellationTokenSource.CreateLinkedTokenSource(TestContext.Current.CancellationToken);
        var calls = 0;
        using var api = new MultiroomClient("http://fake", "token", new FakeHttp((r, _) =>
        {
            if (++calls == 2)
            {
                Assert.Equal("42", r.Headers.GetValues("Last-Event-ID").Single());
                stop.Cancel();
            }
            return Task.FromResult(new HttpResponseMessage(HttpStatusCode.OK)
                { Content = new StringContent("id: 42\nevent: queue.changed\ndata: {}\n\n", Encoding.UTF8, "text/event-stream") });
        }));
        try { await new EventStream(api).RunAsync(_ => Task.CompletedTask, _ => Task.CompletedTask, stop.Token, TimeSpan.Zero); }
        catch (OperationCanceledException) when (stop.IsCancellationRequested) { }
        Assert.Equal(2, calls);
    }

    [Fact]
    public void Provider_state_and_omitted_nullable_properties_map()
    {
        var provider = JsonSerializer.Deserialize<ProviderView>("""{"id":"tidal","state":"authRequired","detail":"Open https://example.test"}""", MultiroomClient.Json)!;
        Assert.Equal(ProviderState.AuthRequired, provider.State);
        Assert.Null(provider.Capabilities);
        var state = JsonSerializer.Deserialize<QueueState>("""{"zone":"living","items":[],"repeat":"off","shuffle":false,"transport":"stopped"}""", MultiroomClient.Json)!;
        Assert.Null(state.Current);
    }
}
