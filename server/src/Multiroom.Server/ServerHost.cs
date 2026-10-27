using System.Diagnostics;
using System.Reflection;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Hosting.WindowsServices;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Logging.EventLog;
using Multiroom.Server.Abstractions;
using Multiroom.Server.Ai;
using Multiroom.Server.Ai.Providers.Anthropic;
using Multiroom.Server.Ai.Providers.Gemini;
using Multiroom.Server.Ai.Providers.Groq;
using Multiroom.Server.Ai.Providers.Mistral;
using Multiroom.Server.Ai.Providers.Ollama;
using Multiroom.Server.Ai.Providers.OpenAi;
using Multiroom.Server.Ai.Providers.OpenRouter;
using Multiroom.Server.Configuration;
using Multiroom.Server.Library;
using Multiroom.Server.Mcp;
using Multiroom.Server.Logging;
using Multiroom.Server.Playback;
using Multiroom.Server.Playback.Core;
using Multiroom.Server.Providers;
using Multiroom.Server.Providers.AddIns;
using Multiroom.Server.Providers.Covers;
using Multiroom.Server.Providers.Local;
using Multiroom.Server.Providers.Tidal;
using Multiroom.Server.Providers.TuneIn;
using Multiroom.Server.Providers.WebRadio;
using Multiroom.Server.Web.Api;
using Multiroom.Server.Web.Gui;
using Multiroom.Server.Web.Media;

namespace Multiroom.Server;

// The composition root. Every later phase registers its hosted services here;
// the generic host starts them in registration order and stops them in reverse.
public static partial class ServerHost
{
    public static string Version { get; } =
        typeof(ServerHost).Assembly.GetCustomAttribute<AssemblyInformationalVersionAttribute>()
            ?.InformationalVersion ?? "unknown";

    public static Result<HostApplicationBuilder, ConfigError> CreateBuilder(string[] args, TextWriter console)
    {
        var service = WindowsServiceHelpers.IsWindowsService() || IsSystemdService();
        var loaded = ConfigLoader.Load(args, AppContext.BaseDirectory, service);
        if (!loaded.IsSuccess)
            return loaded.Failure;
        var settings = loaded.Value;

        var logger = ServerLoggerProvider.Open(console, settings.LogFile, settings.LogMaxBytes);
        if (!logger.IsSuccess)
            return logger.Failure;

        // No defaults: they would add the framework's console logger and read
        // every environment variable into the configuration (ConfigLoader).
        // The console lifetime -- Ctrl+C and SIGTERM -- is registered anyway.
        var builder = new HostApplicationBuilder(new HostApplicationBuilderSettings
        {
            DisableDefaults = true,
            ApplicationName = "Multiroom.Server",
            ContentRootPath = AppContext.BaseDirectory,
        });
        builder.ConfigureContainer(new DefaultServiceProviderFactory(new ServiceProviderOptions
        {
            ValidateOnBuild = true,
            ValidateScopes = true,
        }));

        // Registered by factory, not as an instance, so the container owns it
        // and closes the file when the host is disposed.
        var provider = logger.Value;
        builder.Services.AddSingleton<ILoggerProvider>(_ => provider);
        // The level set through the API wins over the configured one and
        // changes while the server runs, so the provider filters it and the
        // factory lets everything through to it. LogVerbosity is in the order
        // of LogSettings.Levels.
        var logSettings = new LogSettings(Path.Combine(settings.DataDirectory, "logging.json"),
            LogSettings.Levels[(int)settings.LogLevel]);
        var savedLevel = logSettings.Load();
        provider.SetMinimumLevel(ToLogLevel(logSettings.Level));
        logSettings.Changed += (_, level) => provider.SetMinimumLevel(ToLogLevel(level));
        if (!savedLevel.IsSuccess)
            provider.Write(LogLevel.Warning, "ServerHost", $"saved log level not used: {savedLevel.Failure}", null);
        builder.Services.AddSingleton(logSettings);
        builder.Logging.SetMinimumLevel(LogLevel.Trace);
        // Kestrel's hosting diagnostics log every request at info, and nothing
        // on the media path may log once per request.
        builder.Logging.AddFilter("Microsoft.AspNetCore", LogLevel.Warning);
        // The MCP SDK logs each request it handles; tool arguments are user
        // content, so only its warnings.
        builder.Logging.AddFilter("ModelContextProtocol", LogLevel.Warning);

        builder.Services.AddSingleton(settings);
        builder.Services.Configure<ConsoleLifetimeOptions>(o => o.SuppressStatusMessages = true);

        // Started by the Service Control Manager: its stop and shutdown take
        // the same path Ctrl+C does. Outside a service this adds nothing. It
        // adds the Event Log as a second log, kept to warnings and above.
        builder.Services.AddWindowsService(o => o.ServiceName = Branding.ServerName);
        if (service && OperatingSystem.IsWindows())
            builder.Logging.AddFilter<EventLogLoggerProvider>(level => level >= LogLevel.Warning);

        // The library first: its database is migrated before any provider can
        // read it, and its indexer thread is stopped last.
        builder.Services.AddLibrary(settings.Library, Path.Combine(settings.DataDirectory, "library.db"),
            Path.Combine(settings.DataDirectory, "library", "credentials.json"));

        // Providers next: everything after may resolve through them, and they
        // are disposed last. One AddXxx() per provider.
        builder.Services.AddLocalFiles(Path.Combine(settings.DataDirectory, "local"));
        builder.Services.AddWebRadio(Path.Combine(settings.DataDirectory, "webradio"));
        // Tidal's apps are the config's alone: a change is an edit of the file.
        builder.Services.AddTidal(settings.Tidal, Path.Combine(settings.DataDirectory, "tidal"));
        if (settings.TuneIn is not null)
            builder.Services.AddTuneIn(settings.TuneIn, Path.Combine(settings.DataDirectory, "tunein"));
        builder.Services.AddSingleton<ProviderRegistry>();
        builder.Services.AddHostedService(services => services.GetRequiredService<ProviderRegistry>());

        // Add-ins after the providers, which they may use; one AddAddIn per
        // add-in. Enrichment asks them on its own task once they are started.
        builder.Services.AddAddIn(LibraryRecommender.AddInId, services => new LibraryRecommender(
            new LibraryCatalog(services.GetRequiredService<LibraryDatabase>(), LocalFilesProvider.ProviderId)));
        // Always: it keeps the library's covers on this server; the net is
        // asked only through the sources artwork.* turns on.
        builder.Services.AddArtwork(settings.Artwork ?? ArtworkSettings.Default,
            Path.Combine(settings.DataDirectory, "artwork"));
        builder.Services.AddSingleton(services => new EnrichmentStore(services.GetRequiredService<LibraryDatabase>(),
            new LibraryCatalog(services.GetRequiredService<LibraryDatabase>(), LocalFilesProvider.ProviderId)));
        builder.Services.AddSingleton<AddInRegistry>();
        builder.Services.AddHostedService(services => services.GetRequiredService<AddInRegistry>());
        builder.Services.AddHostedService<EnrichmentRunner>();

        // AI providers: one AddXxx() per provider, then the key and settings
        // stores and the registry every call goes through.
        builder.Services.AddOpenAi(Path.Combine(settings.DataDirectory, "ai"));
        builder.Services.AddAnthropic();
        builder.Services.AddGemini();
        builder.Services.AddOpenRouter();
        builder.Services.AddGroq();
        builder.Services.AddMistral();
        builder.Services.AddOllama();
        builder.Services.AddAi(settings.AiProviders ?? [], Path.Combine(settings.DataDirectory, "ai"));

        // Before CoreLink: started before anything can stage a URL, stopped after.
        builder.Services.AddSingleton(settings.Media);
        builder.Services.AddSingleton<MediaHandles>();
        builder.Services.AddSingleton<MediaEndpoint>();
        builder.Services.AddSingleton<IMediaPublisher>(services => services.GetRequiredService<MediaEndpoint>());
        builder.Services.AddHostedService(services => services.GetRequiredService<MediaEndpoint>());

        // Before CoreLink: a selected core is found by its beacon, and the
        // listener is up before the first connect looks for it.
        builder.Services.AddSingleton(new CoreDiscoveryOptions(settings.CoreDiscoveryPort,
            Path.Combine(settings.DataDirectory, "core.json")));
        builder.Services.AddSingleton<CoreDiscovery>();
        builder.Services.AddHostedService(services => services.GetRequiredService<CoreDiscovery>());

        builder.Services.AddSingleton(new CoreLinkOptions(settings.CoreHost, settings.CorePort));
        builder.Services.AddSingleton<CoreLink>();

        // Subscribes itself to CoreLink's events when it is first resolved,
        // which is before it holds a queue.
        builder.Services.AddSingleton<QueueEngine>();
        builder.Services.AddSingleton<IQueueEngine>(services => services.GetRequiredService<QueueEngine>());
        builder.Services.AddSingleton<PlaybackSettings>();
        builder.Services.AddSingleton<ZoneUsers>();
        builder.Services.AddSingleton<EqualizerProfiles>();
        builder.Services.AddSingleton<PlaylistPlayer>();

        // Buttons and encoders on the rooms (MATP section 57): subscribes to
        // CoreLink when resolved, and runs the actions on its own task.
        builder.Services.AddSingleton<InputActions>();
        // Every room in a zone of its own until moved, and no zone without a
        // room: subscribes to CoreLink when resolved, and passes on its own task.
        builder.Services.AddSingleton<ZoneKeeper>();
        builder.Services.AddHostedService(services => services.GetRequiredService<ZoneKeeper>());
        builder.Services.AddHostedService(services => services.GetRequiredService<InputActions>());

        // Phase 26: the play history, written as tracks become audible, and
        // AutoFill's trigger, which subscribes to the queue engine when made.
        // The chooser joins the suggestions and the recommendations (Web).
        builder.Services.AddHostedService<PlayHistoryRecorder>();
        // Phase 33: where a listener stopped in each book, always saved.
        builder.Services.AddHostedService<BookPositions>();
        // Phase 37: a liked album that is a book is an audiobook like; the
        // likes from before are asked once, at start.
        builder.Services.AddSingleton<AudiobookLikes>();
        builder.Services.AddHostedService(services => services.GetRequiredService<AudiobookLikes>());
        builder.Services.AddSingleton<IAutoFillChooser, AutoFillChooser>();
        builder.Services.AddSingleton<AutoFill>();
        builder.Services.AddHostedService(services => services.GetRequiredService<AutoFill>());

        // Before CoreLink: the queues and the desired state are restored
        // before its first connect asserts them, and written after it stops.
        builder.Services.AddSingleton(new PlaybackStoreOptions(Path.Combine(settings.DataDirectory, "playback.json"),
            settings.AutoResume));
        builder.Services.AddHostedService<PlaybackStore>();
        builder.Services.AddHostedService(services => services.GetRequiredService<CoreLink>());

        // Last: everything it drives is up when the first request can arrive,
        // and it is stopped first.
        // The MCP server's external callers: their tiers and named clients (phase 23).
        builder.Services.AddSingleton(new McpClientStore(Path.Combine(settings.DataDirectory, "ai", "mcp.json")));
        // What the REST API and the MCP tools both read and set (phase 23).
        builder.Services.AddSingleton<House>();
        // Search answers the tracks the house keeps first: playlists, queues,
        // favourites and likes.
        builder.Services.AddSingleton<KeptTracks>();
        builder.Services.AddSingleton(services => new UnifiedSearch(services.GetRequiredService<ProviderRegistry>(),
            settings.Api.SearchTimeout, services.GetRequiredService<KeptTracks>().Of));
        // Phase 24: the resolver finds named songs in the user's provider
        // order; the loop joins it to the suggester. Neither half knows the other.
        builder.Services.AddSingleton(services => new SongResolver(services.GetRequiredService<UnifiedSearch>(),
            services.GetRequiredService<ProviderRegistry>(),
            () => services.GetRequiredService<AiProviderRegistry>().Settings.SongProviders));
        // Phase 40: the artwork store's front, GET artwork/{id}; what it does
        // not hold it looks for on its own task, the providers first.
        builder.Services.AddSingleton<ArtworkService>();
        builder.Services.AddHostedService(services => services.GetRequiredService<ArtworkService>());
        // A song liked as a radio played it, found among the providers (settings/playback findRadioLikes).
        builder.Services.AddHostedService<RadioLikeFinder>();
        builder.Services.AddSingleton<SongSuggestions>();
        builder.Services.AddSingleton(settings.Api);
        builder.Services.AddSingleton<ApiEvents>();
        builder.Services.AddSingleton<AiUsageRecorder>();
        builder.Services.AddHostedService(services => services.GetRequiredService<AiUsageRecorder>());
        // Phase 38: what the house might like next, a stored result run on
        // its own task; the MCP tools reach it as IProposals.
        builder.Services.AddSingleton<ProposalRunner>();
        builder.Services.AddSingleton<IProposals>(services => services.GetRequiredService<ProposalRunner>());
        builder.Services.AddHostedService(services => services.GetRequiredService<ProposalRunner>());
        // After CoreLink, which it reads status through, and before the API,
        // which answers what it sampled.
        builder.Services.AddSingleton(new SystemMonitorOptions(settings.DataDirectory));
        builder.Services.AddSingleton<SystemMonitor>();
        builder.Services.AddHostedService(services => services.GetRequiredService<SystemMonitor>());
        builder.Services.AddSingleton<ApiEndpoint>();
        builder.Services.AddHostedService(services => services.GetRequiredService<ApiEndpoint>());
        // After the API, which it forwards /api to; stopped before it.
        if (settings.Gui is not null)
        {
            builder.Services.AddSingleton(settings.Gui);
            builder.Services.AddSingleton<GuiEndpoint>();
            builder.Services.AddHostedService(services => services.GetRequiredService<GuiEndpoint>());
        }
        // After the API: it announces the port the API actually bound.
        builder.Services.AddHostedService<ServiceAnnouncer>();
        return builder;
    }

    // Runs until the stop signal (Ctrl+C, SIGTERM) or `stop`, then stops the
    // hosted services in reverse order. 0 on a clean stop, 1 when the host
    // itself failed.
    public static async Task<int> RunAsync(HostApplicationBuilder builder, CancellationToken stop)
    {
        using var host = builder.Build();
        var settings = host.Services.GetRequiredService<ServerSettings>();
        var log = host.Services.GetRequiredService<ILoggerFactory>().CreateLogger("Multiroom.Server.Host");

        LogStarting(log, Version, settings.ConfigPath ?? "none (defaults)", settings.DataDirectory,
            settings.LogFile ?? "none (stderr only)");

        var clock = Stopwatch.StartNew();
        try
        {
            // Not host.RunAsync: it disposes the host, and the logger with it,
            // before the last line below could be written.
            await host.StartAsync(stop).ConfigureAwait(false);
            await host.WaitForShutdownAsync(stop).ConfigureAwait(false);
        }
#pragma warning disable CA1031 // The process edge: whatever a hosted service threw is logged and becomes the exit code.
        catch (Exception e)
#pragma warning restore CA1031
        {
            LogFailed(log, e, clock.Elapsed.TotalSeconds);
            return 1;
        }
        LogStopped(log, clock.Elapsed.TotalSeconds);
        return 0;
    }

    [LoggerMessage(Level = LogLevel.Information,
        Message = Branding.ServerName + " {Version} starting; config {ConfigPath}; data directory {DataDirectory}; log file {LogFile}")]
    private static partial void LogStarting(ILogger log, string version, string configPath, string dataDirectory,
        string logFile);

    [LoggerMessage(Level = LogLevel.Critical, Message = "host failed after {Seconds:F1} s")]
    private static partial void LogFailed(ILogger log, Exception exception, double seconds);

    [LoggerMessage(Level = LogLevel.Information, Message = "stopped after {Seconds:F1} s")]
    private static partial void LogStopped(ILogger log, double seconds);

    // systemd sets INVOCATION_ID for every process of a unit it starts. No
    // package: this is all Microsoft.Extensions.Hosting.Systemd would be used
    // for, since SIGTERM already takes the Ctrl+C path.
    private static bool IsSystemdService() =>
        OperatingSystem.IsLinux() && !string.IsNullOrEmpty(Environment.GetEnvironmentVariable("INVOCATION_ID"));

    private static LogLevel ToLogLevel(string level) => ToLogLevel(Enum.Parse<LogVerbosity>(level, ignoreCase: true));

    private static LogLevel ToLogLevel(LogVerbosity verbosity) => verbosity switch
    {
        LogVerbosity.Error => LogLevel.Error,
        LogVerbosity.Warn => LogLevel.Warning,
        LogVerbosity.Info => LogLevel.Information,
        LogVerbosity.Debug => LogLevel.Debug,
        LogVerbosity.Trace => LogLevel.Trace,
        _ => throw new ArgumentOutOfRangeException(nameof(verbosity)),
    };
}
