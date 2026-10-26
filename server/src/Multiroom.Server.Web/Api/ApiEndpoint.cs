using System.Globalization;
using System.Net;
using System.Reflection;
using System.Security.Cryptography;
using System.Text;
using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Hosting;
using Microsoft.AspNetCore.Http;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;
using Multiroom.Server.Abstractions;
using Multiroom.Server.Ai;
using Multiroom.Server.Ai.Agent;
using Multiroom.Server.Library;
using Multiroom.Server.Mcp;
using Multiroom.Server.Playback;
using Multiroom.Server.Playback.Core;
using Multiroom.Server.Providers;
using Multiroom.Server.Providers.AddIns;
using Multiroom.Server.Providers.Covers;
using Multiroom.Server.Web.Media;

namespace Multiroom.Server.Web.Api;

// A class, not a record: a record's ToString would print the token (rule 7).
// Port 0 takes any free port; the tests use it.
public sealed class ApiEndpointOptions(IPAddress bind, int port, string? token, TimeSpan searchTimeout)
{
    public IPAddress Bind { get; } = bind;

    public int Port { get; } = port;

    public string? Token { get; } = token;

    public TimeSpan SearchTimeout { get; } = searchTimeout;
}

// The outward API: REST under /api/v1 and the
// event stream, on its own Kestrel listener. It is its own WebApplication, as
// the media endpoint is, so neither can route into the other: /media/* is 404
// here and /api/* is 404 there.
//
// Every answer that is not a success is {error, message}: the failing
// module's code in lower camel case, with the HTTP status it maps to
// (StatusOf). Room and zone state answers both halves: what this server asked
// for (desired) and what the core reports (actual).
public sealed partial class ApiEndpoint(
    ApiEndpointOptions options,
    CoreLink link,
    CoreDiscovery cores,
    QueueEngine queues,
    ZoneKeeper zones,
    PlaylistPlayer player,
    PlaylistStore playlists,
    StationStore stations,
    ArtworkSettingsStore artwork,
    ArtworkService pictures,
    ServerProfileStore profile,
    LikeStore likes,
    UserStore users,
    AudiobookLikes audiobookLikes,
    ProposalRunner proposals,
    PlayHistoryStore history,
    AiUsageStore aiUsage,
    BookPositionStore bookPositions,
    ProviderRegistry providers,
    UnifiedSearch search,
    House house,
    EqualizerProfiles equalizerProfiles,
    LibraryIndexer indexer,
    LibraryFolders folders,
    MediaEndpoint media,
    MediaHandles handles,
    AddInRegistry addIns,
    EnrichmentStore enrichment,
    ApiEvents events,
    SystemMonitor system,
    AiProviderRegistry ai,
    McpClientStore mcpClients,
    SongResolver resolver,
    SongSuggestions suggestions,
    MultiroomAgent agent,
    AgentChatStore chats,
    ILoggerFactory loggerFactory) : IHostedService, IAsyncDisposable
{
    public const int DefaultLimit = 20;
    public const int MaxLimit = 100;

    private static readonly string Version =
        typeof(ApiEndpoint).Assembly.GetCustomAttribute<AssemblyInformationalVersionAttribute>()
            ?.InformationalVersion ?? "unknown";

    private readonly ILogger _log = loggerFactory.CreateLogger<ApiEndpoint>();
    private readonly byte[]? _authorization =
        options.Token is null ? null : Encoding.UTF8.GetBytes($"Bearer {options.Token}");
    private WebApplication? _app;

    // http://<bind>:<port>/api/v1/, with the port actually bound. Set once started.
    public Uri? BaseUri { get; private set; }

    public async Task StartAsync(CancellationToken cancellationToken)
    {
        var builder = WebApplication.CreateEmptyBuilder(new WebApplicationOptions
        {
            ApplicationName = "Multiroom.Server.Api",
            ContentRootPath = AppContext.BaseDirectory,
        });
        builder.WebHost.UseKestrelCore().ConfigureKestrel(kestrel =>
        {
            kestrel.Listen(options.Bind, options.Port);
            kestrel.AddServerHeader = false;
            kestrel.Limits.MaxRequestBodySize = ApiJson.MaxBodyBytes;
        });
        builder.Services.AddRoutingCore();
        AddMcp(builder.Services);
        builder.Services.AddSingleton(loggerFactory);
        builder.Services.AddSingleton<IHostLifetime, OwnedLifetime>();

        var app = builder.Build();
        app.Use(GuardAsync);
        MapRoutes(app);
        await app.StartAsync(cancellationToken).ConfigureAwait(false);
        _app = app;

        BaseUri = new Uri(new Uri(app.Urls.First()), "/api/v1/");
        LogListening(_log, BaseUri, options.Token is null ? "none" : "required");
        if (!IPAddress.IsLoopback(options.Bind) && options.Token is null)
            LogUnprotected(_log, options.Bind);
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
    }

    private void MapRoutes(WebApplication app)
    {
        app.MapGet("/api/v1/status", (Delegate)StatusAsync);
        app.MapGet("/api/v1/events", StreamAsync);
        app.MapGet("/api/v1/system", (Delegate)(async (HttpContext c) =>
            Json(await system.LatestAsync(c.RequestAborted).ConfigureAwait(false))));
        MapCatalog(app);
        MapZones(app);
        MapRooms(app);
        MapCores(app);
        MapCollections(app);
        MapLibrary(app);
        MapTidal(app);
        MapArtwork(app);
        MapServer(app);
        MapLikes(app);
        MapAddIns(app);
        MapAi(app);
        MapSuggestions(app);
        MapProposals(app);
        MapMcp(app);
    }

    // The token before anything else; afterwards, any error answer the
    // framework produced without a body (no route, wrong method) gets one.
    private async Task GuardAsync(HttpContext context, RequestDelegate next)
    {
        // A login's redirect carries no header: its state authorises it (phase 22).
        var authorized = IsMcp(context.Request)
            ? AuthorizeMcp(context)
            : IsAuthorized(context.Request) || IsLoginCallback(context.Request);
        if (!authorized)
        {
            await Error(Result.Fail(ApiRequestError.Unauthorized, "a bearer token is required"))
                .ExecuteAsync(context).ConfigureAwait(false);
            return;
        }
        await next(context).ConfigureAwait(false);
        var response = context.Response;
        if (response.HasStarted || response.ContentType is not null)
            return;
        if (response.StatusCode == StatusCodes.Status404NotFound)
        {
            await Error(Result.Fail(ApiRequestError.NotFound,
                $"no route {context.Request.Method} {context.Request.Path}")).ExecuteAsync(context).ConfigureAwait(false);
        }
        else if (response.StatusCode == StatusCodes.Status405MethodNotAllowed)
        {
            await Error(Result.Fail(ApiRequestError.MethodNotAllowed,
                $"{context.Request.Path} does not take {context.Request.Method}")).ExecuteAsync(context).ConfigureAwait(false);
        }
    }

    private bool IsAuthorized(HttpRequest request)
    {
        if (_authorization is null)
            return true;
        var header = request.Headers.Authorization.ToString();
        return CryptographicOperations.FixedTimeEquals(Encoding.UTF8.GetBytes(header), _authorization);
    }

    private async Task<IResult> StatusAsync(HttpContext context)
    {
        var model = link.Model.Current;
        return Json(new
        {
            version = Version,
            core = new
            {
                connected = link.IsConnected,
                version = link.CoreVersion,
                connections = link.Connections,
                zones = model.Zones.Count,
                rooms = model.Rooms.Count,
                queuedEvents = link.QueuedEvents,
                droppedEvents = link.DroppedEvents,
                selected = cores.SelectedCoreId,
                host = link.IsConnected ? $"{link.Host}:{link.Port}" : null,
            },
            media = new { baseUri = media.BaseUri, handles = handles.Count, reachability = media.Reachability },
            library = new
            {
                schemaVersion = indexer.SchemaVersion,
                indexer = indexer.Status,
                smb = new { sessionsPerServer = Library.Smb.SmbSessions.SessionsPerServer, servers = folders.Sessions.Status },
            },
            providers = await search.ProvidersAsync(withRoot: false, context.RequestAborted).ConfigureAwait(false),
            addIns = AddInsStatus(),
            ai = new
            {
                providers = ai.Usage,
                maxModels = AiProviderRegistry.MaxModels,
                pendingLogins = ai.PendingLogins,
                maxPendingLogins = AiProviderRegistry.MaxPendingLogins,
                suggestions = suggestions.Stats,
                proposals = proposals.Stats,
                agent = new
                {
                    running = agent.Running,
                    conversations = agent.Conversations,
                    maxConversations = MultiroomAgent.MaxConversations,
                },
            },
            events = events.Status,
        });
    }

    private Task StreamAsync(HttpContext context)
    {
        if (UnknownQuery(context.Request) is { } unknown)
            return Error(unknown).ExecuteAsync(context);
        var header = context.Request.Headers["Last-Event-ID"].ToString();
        long? last = long.TryParse(header, NumberStyles.None, CultureInfo.InvariantCulture, out var id) ? id : null;
        return events.StreamAsync(context, last);
    }

    // --- answers ------------------------------------------------------------

    internal static IResult Json(object value, int status = StatusCodes.Status200OK) =>
        Results.Json(value, ApiJson.Options, statusCode: status);

    internal static IResult Error<TCode>(Failure<TCode> failure)
        where TCode : struct, Enum
    {
        var error = ApiError.From(failure);
        return Results.Json(error, ApiJson.Options, statusCode: StatusOf(error.Error));
    }

    // One table for every module's codes, by their outward name.
    internal static int StatusOf(string code) => code switch
    {
        "notFound" => StatusCodes.Status404NotFound,
        "invalidArgument" or "invalidRequest" or "invalidJson" => StatusCodes.Status400BadRequest,
        "unauthorized" => StatusCodes.Status401Unauthorized,
        "authFailed" => StatusCodes.Status403Forbidden,
        "notConfigured" or "alreadyExists" or "conflict" => StatusCodes.Status409Conflict,
        "rateLimited" or "busy" => StatusCodes.Status429TooManyRequests,
        "invalidAnswer" => StatusCodes.Status502BadGateway,
        "methodNotAllowed" => StatusCodes.Status405MethodNotAllowed,
        "gone" => StatusCodes.Status410Gone,
        "unavailable" or "notConnected" or "connectionLost" or "timeout" or "unreachable" =>
            StatusCodes.Status503ServiceUnavailable,
        _ => StatusCodes.Status500InternalServerError,
    };

    internal static IResult Answer<T, TCode>(Result<T, TCode> result, int status = StatusCodes.Status200OK)
        where TCode : struct, Enum =>
        result.IsSuccess ? Json(result.Value!, status) : Error(result.Failure);

    // --- the query string ---------------------------------------------------

    // A query key no route takes is refused by name, as a body field is.
    // ?only=true (phase 36): a user's own rows, without the household's.
    internal static Result<bool, ApiRequestError> Only(HttpRequest request) =>
        request.Query["only"].ToString() switch
        {
            "" or "false" => false,
            "true" => true,
            var text => Result.Fail(ApiRequestError.InvalidRequest, $"only: true or false, was {text}"),
        };

    internal static Failure<ApiRequestError>? UnknownQuery(HttpRequest request, params string[] known)
    {
        var unknown = request.Query.Keys.FirstOrDefault(k => !known.Contains(k, StringComparer.Ordinal));
        return unknown is null
            ? null
            : Result.Fail(ApiRequestError.InvalidRequest, $"unknown query parameter {unknown}");
    }

    internal static Result<int, ApiRequestError> IntQuery(HttpRequest request, string name, int fallback, int min,
        int max)
    {
        var text = request.Query[name].ToString();
        if (text.Length == 0)
            return fallback;
        if (!int.TryParse(text, NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture, out var value) ||
            value < min || value > max)
        {
            return Result.Fail(ApiRequestError.InvalidRequest, $"{name}: {min} to {max}, was {text}");
        }
        return value;
    }

    internal static Result<long, ApiRequestError> IdOf(string text, string what) =>
        long.TryParse(text, NumberStyles.None, CultureInfo.InvariantCulture, out var id)
            ? id
            : Result.Fail(ApiRequestError.NotFound, $"no {what} {text}");

    [LoggerMessage(Level = LogLevel.Information, Message = "outward API listening on {BaseUri}; token {Token}")]
    private static partial void LogListening(ILogger log, Uri baseUri, string token);

    [LoggerMessage(Level = LogLevel.Warning,
        Message = "outward API bound to {Bind}, which is not loopback, and api.token is empty: anything that reaches it can control the house")]
    private static partial void LogUnprotected(ILogger log, IPAddress bind);

    private sealed class OwnedLifetime : IHostLifetime
    {
        public Task WaitForStartAsync(CancellationToken cancellationToken) => Task.CompletedTask;

        public Task StopAsync(CancellationToken cancellationToken) => Task.CompletedTask;
    }
}
