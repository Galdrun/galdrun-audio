using System.Net;
using System.Security.Cryptography;
using System.Text;
using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Hosting;
using Microsoft.AspNetCore.Http;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;
using Multiroom.Server.Abstractions;

namespace Multiroom.Server.Web.Media;

// A class, not a record: a record's ToString would print the token (rule 7).
// Port 0 takes any free port; the tests use it.
public sealed class MediaEndpointOptions(IPAddress bind, int port, string? token)
{
    public IPAddress Bind { get; } = bind;

    public int Port { get; } = port;

    public string? Token { get; } = token;
}

// The media endpoint: the phase 21 contract on its own Kestrel listener. It is
// its own WebApplication, so nothing routed here can be reached from the outward API and back; it
// serves /media/<handle> and nothing else.
//
// Nothing here waits on CoreLink: zone.play opens its first source while the
// caller waits on 4713, so a GET must be served from Kestrel's own threads.
// Nothing here logs once per request either, only failures.
public sealed partial class MediaEndpoint(
    MediaEndpointOptions options,
    MediaHandles handles,
    ILoggerFactory loggerFactory) : IHostedService, IMediaPublisher, IAsyncDisposable
{
    private readonly ILogger _log = loggerFactory.CreateLogger<MediaEndpoint>();
    private readonly byte[]? _authorization =
        options.Token is null ? null : Encoding.UTF8.GetBytes($"Bearer {options.Token}");
    private WebApplication? _app;

    // http://<bind>:<port>/media/, with the port actually bound. Set once started.
    public Uri? BaseUri { get; private set; }

    public async Task StartAsync(CancellationToken cancellationToken)
    {
        var builder = WebApplication.CreateEmptyBuilder(new WebApplicationOptions
        {
            ApplicationName = "Multiroom.Server.Media",
            ContentRootPath = AppContext.BaseDirectory,
        });
        builder.WebHost.UseKestrelCore().ConfigureKestrel(kestrel =>
        {
            kestrel.Listen(options.Bind, options.Port);
            kestrel.AddServerHeader = false;
            // A paused zone holds its connection open and unread for as long as
            // the pause lasts (the contract, Pace and time). Kestrel's default
            // minimum response rate would drop it; TCP backpressure alone paces.
            kestrel.Limits.MinResponseDataRate = null;
        });
        builder.Services.AddRoutingCore();
        // The server's own logger, filters included. An instance registration,
        // so this container does not dispose it.
        builder.Services.AddSingleton(loggerFactory);
        // Ctrl+C and SIGTERM belong to the server's host, which stops this one.
        builder.Services.AddSingleton<IHostLifetime, OwnedLifetime>();

        var app = builder.Build();
        app.MapGet("/media/{handle}", (string handle, HttpContext context) => Serve(handle, context));
        await app.StartAsync(cancellationToken).ConfigureAwait(false);
        _app = app;

        BaseUri = new Uri(new Uri(app.Urls.First()), "/media/");
        LogListening(_log, BaseUri, options.Token is null ? "none" : "required");
        if (!IPAddress.IsLoopback(options.Bind) && options.Token is null)
            LogUnprotected(_log, options.Bind);

        Reachability = await ProbeAsync(cancellationToken).ConfigureAwait(false);
        if (Reachability.StartsWith("reachable", StringComparison.Ordinal))
            LogReachable(_log, Reachability);
        else
            LogUnreachable(_log, Reachability);
    }

    // The startup self-check: what a GET of a handle nobody published got,
    // as the core would send it. "reachable" when it was the 404 it must be.
    public string? Reachability { get; private set; }

    private async Task<string> ProbeAsync(CancellationToken cancellationToken)
    {
        var host = options.Bind.Equals(IPAddress.Any) ? IPAddress.Loopback
            : options.Bind.Equals(IPAddress.IPv6Any) ? IPAddress.IPv6Loopback
            : options.Bind;
        var uri = new UriBuilder(BaseUri!) { Host = host.ToString() }.Uri;
        using var client = new HttpClient(new SocketsHttpHandler { AllowAutoRedirect = false, UseProxy = false })
        {
            Timeout = TimeSpan.FromSeconds(2),
        };
        using var request = new HttpRequestMessage(HttpMethod.Get, new Uri(uri, "self-check"));
        request.Headers.Range = new System.Net.Http.Headers.RangeHeaderValue(0, null);
        if (options.Token is not null)
            request.Headers.Authorization = new System.Net.Http.Headers.AuthenticationHeaderValue("Bearer", options.Token);
        try
        {
            using var response = await client.SendAsync(request, cancellationToken).ConfigureAwait(false);
            return response.StatusCode == HttpStatusCode.NotFound
                ? $"reachable at {uri}"
                : $"{uri} answered {(int)response.StatusCode} to an unknown handle, not 404";
        }
        catch (Exception e) when (e is HttpRequestException or TaskCanceledException)
        {
            return $"{uri} did not answer: {e.Message}";
        }
    }

    public async Task StopAsync(CancellationToken cancellationToken)
    {
        if (_app is null)
            return;
        // Already cancelled: a paused zone's connection never finishes by
        // itself, so Kestrel is not given time to wait for it. The core treats
        // the close as a dropped connection, as the contract says.
        await _app.StopAsync(new CancellationToken(canceled: true)).ConfigureAwait(false);
    }

    // Nothing can be published before the endpoint has a port: the host starts
    // it before anything that stages.
    public Uri Publish(ResolvedStream stream) =>
        new(BaseUri ?? throw new InvalidOperationException("the media endpoint has not started"),
            handles.Register(stream));

    public void Retire(Uri uri) => handles.Retire(uri.Segments[^1]);

    public async ValueTask DisposeAsync()
    {
        if (_app is not null)
            await _app.DisposeAsync().ConfigureAwait(false);
    }

    private IResult Serve(string handle, HttpContext context)
    {
        if (!IsAuthorized(context.Request))
            return Results.Unauthorized();
        if (!handles.TryGet(handle, out var source))
            return Results.NotFound();

        var opened = source.Open();
        if (!opened.IsSuccess)
        {
            // 404, not 5xx: a source that will not come back must not be
            // retried by the core while its rooms drain (the contract).
            LogOpenFailed(_log, handle, opened.Failure.ToString());
            return Results.NotFound();
        }

        // Known length: 206 + Content-Range for a Range, or 200 with
        // Content-Length and Accept-Ranges without one. Live: 200, chunked,
        // because a live reader cannot seek (ResolvedStream.Open holds it to
        // that). No compression is registered, so none is ever applied, and
        // Icy-MetaData is never read.
        if (source.Shape == StreamShape.KnownLength)
            return Results.Stream(opened.Value, source.ContentType, enableRangeProcessing: true);
        return Results.Stream(body => RelayAsync(handle, opened.Value, body, context), source.ContentType);
    }

    // A live reader that fails (an upstream drop) aborts the connection, so
    // the core sees a dropped connection and fails the source, as the
    // contract says for a non-seekable one. A clean end would be a source
    // played to its end, and the zone would move on as if it had.
    private async Task RelayAsync(string handle, Stream reader, Stream body, HttpContext context)
    {
        await using (reader.ConfigureAwait(false))
        {
            try
            {
                await reader.CopyToAsync(body, context.RequestAborted).ConfigureAwait(false);
            }
            catch (OperationCanceledException) when (context.RequestAborted.IsCancellationRequested)
            {
                // The core closed it: a stop, a skip or a stall of its own.
            }
            catch (IOException e)
            {
                LogRelayDropped(_log, handle, e.Message);
                context.Abort();
            }
        }
    }

    private bool IsAuthorized(HttpRequest request)
    {
        if (_authorization is null)
            return true;
        var header = request.Headers.Authorization.ToString();
        return CryptographicOperations.FixedTimeEquals(Encoding.UTF8.GetBytes(header), _authorization);
    }

    [LoggerMessage(Level = LogLevel.Information, Message = "media endpoint listening on {BaseUri}; token {Token}")]
    private static partial void LogListening(ILogger log, Uri baseUri, string token);

    [LoggerMessage(Level = LogLevel.Warning,
        Message = "media endpoint bound to {Bind}, which is not loopback, and media.token is empty: anything that reaches it can fetch the media")]
    private static partial void LogUnprotected(ILogger log, IPAddress bind);

    [LoggerMessage(Level = LogLevel.Information, Message = "self-check: media endpoint {Result}")]
    private static partial void LogReachable(ILogger log, string result);

    [LoggerMessage(Level = LogLevel.Error, Message = "self-check: media endpoint failed: {Result}")]
    private static partial void LogUnreachable(ILogger log, string result);

    [LoggerMessage(Level = LogLevel.Warning, Message = "media {Handle} answered 404: {Failure}")]
    private static partial void LogOpenFailed(ILogger log, string handle, string failure);

    [LoggerMessage(Level = LogLevel.Warning, Message = "media {Handle}: the live source failed, connection dropped: {Reason}")]
    private static partial void LogRelayDropped(ILogger log, string handle, string reason);

    private sealed class OwnedLifetime : IHostLifetime
    {
        public Task WaitForStartAsync(CancellationToken cancellationToken) => Task.CompletedTask;

        public Task StopAsync(CancellationToken cancellationToken) => Task.CompletedTask;
    }
}
