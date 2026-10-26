using System.Net;
using System.Net.Http.Headers;
using System.Text;
using System.Text.Json;
using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Hosting;
using Microsoft.AspNetCore.Http;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.FileProviders;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;
using Multiroom.Server.Web.Api;

namespace Multiroom.Server.Web.Gui;

// A file the log page tails, by the name it shows.
public sealed record GuiLog(string Name, string Path);

// Port 0 takes any free port; the tests use it. Root is the ServerGui's
// www folder, shipped beside the executable as gui/. SettingsPath keeps the
// page's own settings (gui-settings.json in the data directory).
public sealed record GuiEndpointOptions(IPAddress Bind, int Port, string Root, IReadOnlyList<GuiLog> Logs,
    string SettingsPath);

// The ServerGui (server-gui/www in the repository), served by this server: its static files, and
// /api/* forwarded to the outward API with api.token added, so the page is
// same-origin and the token never reaches the browser. It also tails log
// files as an event stream on /gui/logs, and keeps the page's settings,
// shared by every browser, on /gui/settings. The page still talks to /api/v1
// for the house: this listener draws nothing and adds no route of its own to the API.
public sealed partial class GuiEndpoint(
    GuiEndpointOptions options,
    ApiEndpointOptions apiOptions,
    ApiEndpoint api,
    ILoggerFactory loggerFactory) : IHostedService, IAsyncDisposable
{
    private const int TailStartBytes = 64 * 1024;
    private const int TailChunkBytes = 1024 * 1024;
    private static readonly TimeSpan TailInterval = TimeSpan.FromMilliseconds(500);

    // Never forwarded: the connection's own, and what would make the API see
    // a cross-origin caller. Authorization is replaced when api.token is set.
    private static readonly HashSet<string> SkippedRequestHeaders = new(StringComparer.OrdinalIgnoreCase)
    {
        "Host", "Origin", "Referer", "Connection", "Keep-Alive", "Transfer-Encoding", "Upgrade",
        "Proxy-Connection", "TE", "Trailer",
    };

    private static readonly HashSet<string> SkippedResponseHeaders = new(StringComparer.OrdinalIgnoreCase)
    {
        "Connection", "Keep-Alive", "Transfer-Encoding", "Upgrade", "Proxy-Connection", "Trailer",
    };

    private readonly ILogger _log = loggerFactory.CreateLogger<GuiEndpoint>();
    private readonly HttpClient _client = new(new SocketsHttpHandler
    {
        UseProxy = false,
        UseCookies = false,
        AllowAutoRedirect = false,
        AutomaticDecompression = DecompressionMethods.None,
    })
    {
        Timeout = Timeout.InfiniteTimeSpan,
    };
    private readonly Lock _settingsLock = new();
    private Uri? _api;
    private WebApplication? _app;

    // http://<bind>:<port>/, with the port actually bound. Set once started.
    public Uri? BaseUri { get; private set; }

    public async Task StartAsync(CancellationToken cancellationToken)
    {
        // The API is started first; a wildcard bind is reached on loopback.
        var apiBase = api.BaseUri ?? throw new InvalidOperationException("the outward API is not started");
        var apiHost = apiOptions.Bind.Equals(IPAddress.Any) ? IPAddress.Loopback
            : apiOptions.Bind.Equals(IPAddress.IPv6Any) ? IPAddress.IPv6Loopback
            : apiOptions.Bind;
        _api = new UriBuilder(Uri.UriSchemeHttp, apiHost.ToString(), apiBase.Port).Uri;

        var builder = WebApplication.CreateEmptyBuilder(new WebApplicationOptions
        {
            ApplicationName = "Multiroom.Server.Gui",
            ContentRootPath = AppContext.BaseDirectory,
        });
        builder.WebHost.UseKestrelCore().ConfigureKestrel(kestrel =>
        {
            kestrel.Listen(options.Bind, options.Port);
            kestrel.AddServerHeader = false;
            kestrel.Limits.MaxRequestBodySize = ApiJson.MaxBodyBytes;
        });
        builder.Services.AddSingleton(loggerFactory);

        var app = builder.Build();
        app.Use(async (context, next) =>
        {
            var path = context.Request.Path;
            if (path.StartsWithSegments("/api", StringComparison.Ordinal))
                await ForwardAsync(context).ConfigureAwait(false);
            else if (path.Equals("/gui/config", StringComparison.Ordinal))
                await ConfigAsync(context).ConfigureAwait(false);
            else if (path.Equals("/gui/logs", StringComparison.Ordinal))
                await TailAsync(context).ConfigureAwait(false);
            else if (path.Equals("/gui/settings", StringComparison.Ordinal))
                await SettingsAsync(context).ConfigureAwait(false);
            else
                await next(context).ConfigureAwait(false);
        });
        if (Directory.Exists(options.Root))
        {
            var files = new PhysicalFileProvider(options.Root);
            app.UseDefaultFiles(new DefaultFilesOptions { FileProvider = files });
            app.UseStaticFiles(new StaticFileOptions
            {
                FileProvider = files,
                OnPrepareResponse = c => c.Context.Response.Headers.CacheControl = "no-store",
            });
        }
        else
        {
            LogNoRoot(_log, options.Root);
        }
        await app.StartAsync(cancellationToken).ConfigureAwait(false);
        _app = app;

        BaseUri = new Uri(app.Urls.First());
        LogListening(_log, BaseUri, _api);
        if (!IPAddress.IsLoopback(options.Bind))
            LogNotLoopback(_log, options.Bind);
    }

    public async Task StopAsync(CancellationToken cancellationToken)
    {
        if (_app is null)
            return;
        // Already cancelled: an event stream never ends by itself.
        await _app.StopAsync(new CancellationToken(canceled: true)).ConfigureAwait(false);
    }

    public async ValueTask DisposeAsync()
    {
        if (_app is not null)
            await _app.DisposeAsync().ConfigureAwait(false);
        _client.Dispose();
    }

    // Streams both ways, so the event stream passes through as it is written.
    private async Task ForwardAsync(HttpContext context)
    {
        var request = context.Request;
        var response = context.Response;
        using var message = new HttpRequestMessage(new HttpMethod(request.Method),
            new Uri(_api!, request.Path.ToUriComponent() + request.QueryString.ToUriComponent()));
        if (request.ContentLength > 0 || request.Headers.TransferEncoding.Count > 0)
            message.Content = new StreamContent(request.Body);
        foreach (var (name, values) in request.Headers)
        {
            if (SkippedRequestHeaders.Contains(name) || (apiOptions.Token is not null && name.Equals("Authorization",
                    StringComparison.OrdinalIgnoreCase)))
                continue;
            if (!message.Headers.TryAddWithoutValidation(name, (IEnumerable<string?>)values))
                message.Content?.Headers.TryAddWithoutValidation(name, (IEnumerable<string?>)values);
        }
        if (apiOptions.Token is { } token)
            message.Headers.Authorization = new AuthenticationHeaderValue("Bearer", token);

        HttpResponseMessage answer;
        try
        {
            answer = await _client.SendAsync(message, HttpCompletionOption.ResponseHeadersRead, context.RequestAborted)
                .ConfigureAwait(false);
        }
        catch (HttpRequestException e)
        {
            response.StatusCode = StatusCodes.Status502BadGateway;
            response.ContentType = "application/json";
            await response.WriteAsync(JsonSerializer.Serialize(new { error = "proxyUnreachable", message = $"{_api}: {e.Message}" }),
                context.RequestAborted).ConfigureAwait(false);
            return;
        }
        catch (OperationCanceledException) when (context.RequestAborted.IsCancellationRequested)
        {
            return;
        }

        using (answer)
        {
            response.StatusCode = (int)answer.StatusCode;
            foreach (var (name, values) in answer.Headers.Concat(answer.Content.Headers))
            {
                if (!SkippedResponseHeaders.Contains(name))
                    response.Headers[name] = values.ToArray();
            }
            // An idle event stream would otherwise leave the browser with no answer at all.
            await response.StartAsync(context.RequestAborted).ConfigureAwait(false);
            try
            {
                var body = await answer.Content.ReadAsStreamAsync(context.RequestAborted).ConfigureAwait(false);
                await using (body.ConfigureAwait(false))
                    await body.CopyToAsync(response.Body, context.RequestAborted).ConfigureAwait(false);
            }
            catch (Exception e) when (e is OperationCanceledException or IOException or HttpRequestException)
            {
                // The browser left, or the API ended the stream: the response ends with it.
                context.Abort();
            }
        }
    }

    private Task ConfigAsync(HttpContext context)
    {
        context.Response.ContentType = "application/json";
        return context.Response.WriteAsync(JsonSerializer.Serialize(new
        {
            api = _api!.ToString().TrimEnd('/'),
            token = apiOptions.Token is not null,
            logs = options.Logs.Select(l => l.Name),
        }), context.RequestAborted);
    }

    // The page's settings: a JSON object of its own making, which this server
    // keeps without reading it. GET answers it ({} until one is PUT); PUT
    // replaces it whole and answers what it kept.
    private async Task SettingsAsync(HttpContext context)
    {
        var request = context.Request;
        var response = context.Response;
        response.ContentType = "application/json";
        if (HttpMethods.IsGet(request.Method))
        {
            string text;
            lock (_settingsLock)
                text = File.Exists(options.SettingsPath) ? File.ReadAllText(options.SettingsPath) : "{}";
            await response.WriteAsync(text, context.RequestAborted).ConfigureAwait(false);
            return;
        }
        if (!HttpMethods.IsPut(request.Method))
        {
            response.StatusCode = StatusCodes.Status405MethodNotAllowed;
            response.Headers.Allow = "GET, PUT";
            return;
        }

        JsonDocument document;
        try
        {
            document = await JsonDocument.ParseAsync(request.Body, cancellationToken: context.RequestAborted)
                .ConfigureAwait(false);
        }
        catch (JsonException e)
        {
            await RefuseAsync(context, $"not JSON: {e.Message}").ConfigureAwait(false);
            return;
        }
        using (document)
        {
            if (document.RootElement.ValueKind != JsonValueKind.Object)
            {
                await RefuseAsync(context, "the settings are a JSON object").ConfigureAwait(false);
                return;
            }
            var text = document.RootElement.GetRawText();
            // Written beside it first, so a crash never leaves half a file.
            lock (_settingsLock)
            {
                var temp = options.SettingsPath + ".tmp";
                File.WriteAllText(temp, text);
                File.Move(temp, options.SettingsPath, overwrite: true);
            }
            await response.WriteAsync(text, context.RequestAborted).ConfigureAwait(false);
        }
    }

    private static Task RefuseAsync(HttpContext context, string message)
    {
        context.Response.StatusCode = StatusCodes.Status400BadRequest;
        return context.Response.WriteAsync(JsonSerializer.Serialize(new { error = "invalidRequest", message }),
            context.RequestAborted);
    }

    // Each file is polled; what was appended goes out as a `log` event with
    // the file's offset after it. A file that shrank was rotated and is read
    // again from its start. `?from={"name":offset}` resumes a file there (the
    // page's Clear, or a reconnect), but never further back than the usual tail.
    private async Task TailAsync(HttpContext context)
    {
        var response = context.Response;
        var stop = context.RequestAborted;
        Dictionary<string, long>? from = null;
        if (context.Request.Query["from"] is { Count: > 0 } query)
        {
            try
            {
                from = JsonSerializer.Deserialize<Dictionary<string, long>>(query.ToString());
            }
            catch (JsonException e)
            {
                await RefuseAsync(context, $"from is not a JSON object of offsets: {e.Message}").ConfigureAwait(false);
                return;
            }
        }
        response.ContentType = "text/event-stream";
        response.Headers.CacheControl = "no-cache";
        await response.StartAsync(stop).ConfigureAwait(false);

        var tails = options.Logs.Select(l => new Tail(l)).ToList();
        try
        {
            foreach (var tail in tails)
            {
                if (File.Exists(tail.Log.Path))
                {
                    var size = new FileInfo(tail.Log.Path).Length;
                    var start = Math.Max(0, size - TailStartBytes);
                    // An offset past the end belongs to a file since rotated.
                    if (from?.TryGetValue(tail.Log.Name, out var offset) == true && offset <= size)
                        start = Math.Max(start, offset);
                    tail.Offset = start;
                }
                else
                    await SendAsync(response, tail.Log.Name, $"({tail.Log.Path} not found yet)\n", stop).ConfigureAwait(false);
            }
            using var timer = new PeriodicTimer(TailInterval);
            while (await timer.WaitForNextTickAsync(stop).ConfigureAwait(false))
            {
                foreach (var tail in tails)
                {
                    if (tail.Read() is { Length: > 0 } text)
                        await SendAsync(response, tail.Log.Name, text, stop, tail.Offset).ConfigureAwait(false);
                }
            }
        }
        catch (OperationCanceledException) when (stop.IsCancellationRequested)
        {
        }
    }

    private static Task SendAsync(HttpResponse response, string name, string text, CancellationToken stop, long? offset = null) =>
        response.WriteAsync($"event: log\ndata: {JsonSerializer.Serialize(new { name, text, offset })}\n\n", stop);

    private sealed class Tail(GuiLog log)
    {
        // Keeps a character split across two reads whole.
        private Decoder _decoder = Encoding.UTF8.GetDecoder();

        public GuiLog Log { get; } = log;

        public long Offset { get; set; }

        public string? Read()
        {
            try
            {
                using var file = new FileStream(Log.Path, FileMode.Open, FileAccess.Read,
                    FileShare.ReadWrite | FileShare.Delete);
                var size = file.Length;
                if (size < Offset)
                {
                    Offset = 0;
                    _decoder = Encoding.UTF8.GetDecoder();
                }
                if (size == Offset)
                    return null;
                var buffer = new byte[(int)Math.Min(size - Offset, TailChunkBytes)];
                file.Position = Offset;
                file.ReadExactly(buffer);
                Offset += buffer.Length;
                var chars = new char[_decoder.GetCharCount(buffer, flush: false)];
                _decoder.GetChars(buffer, chars, flush: false);
                return new string(chars);
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException)
            {
                // Not there yet, or being rotated: the next tick tries again.
                return null;
            }
        }
    }

    [LoggerMessage(Level = LogLevel.Information, Message = "ServerGui listening on {BaseUri}; API at {Api}")]
    private static partial void LogListening(ILogger log, Uri baseUri, Uri api);

    [LoggerMessage(Level = LogLevel.Warning,
        Message = "ServerGui bound to {Bind}, which is not loopback: it has no authentication of its own and adds api.token to what it forwards, so anything that reaches it can control the house")]
    private static partial void LogNotLoopback(ILogger log, IPAddress bind);

    [LoggerMessage(Level = LogLevel.Warning, Message = "ServerGui files not found at {Root}: only /api and /gui are served")]
    private static partial void LogNoRoot(ILogger log, string root);
}
