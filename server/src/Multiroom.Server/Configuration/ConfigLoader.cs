using System.Net;
using System.Reflection;
using Microsoft.Extensions.Configuration;
using Multiroom.Server.Abstractions;
using Multiroom.Server.Ai;
using Multiroom.Server.Library;
using Multiroom.Server.Library.Smb;
using Multiroom.Server.Providers.Covers;
using Multiroom.Server.Providers.Tidal;
using Multiroom.Server.Providers.TuneIn;
using Multiroom.Server.Web.Api;
using Multiroom.Server.Web.Gui;
using Multiroom.Server.Web.Media;

namespace Multiroom.Server.Configuration;

public enum ConfigError
{
    MissingValue,
    FileNotFound,
    UnreadableFile,
    UnknownKey,
    MalformedValue,
    OutOfRange,
    UnusableDataDirectory,
    UnusableLogFile,
}

// What the rest of the server reads: every path absolute, every default applied.
// Media, Api and Tidal are classes, so this record's ToString never prints
// media.token, api.token or the Tidal client secret; AiProviderConfig is
// a class for the same reason.
public sealed record ServerSettings(
    string? ConfigPath,
    string DataDirectory,
    LogVerbosity LogLevel,
    string? LogFile,
    long LogMaxBytes,
    string CoreHost,
    int CorePort,
    MediaEndpointOptions Media,
    ApiEndpointOptions Api,
    LibrarySettings Library,
    TidalSettings Tidal,
    bool AutoResume,
    TuneInSettings? TuneIn = null,
    int CoreDiscoveryPort = 4722,
    IReadOnlyList<AiProviderConfig>? AiProviders = null,
    ArtworkSettings? Artwork = null,
    GuiEndpointOptions? Gui = null);

// Config is the JSON file, then GALDRUN_-prefixed environment variables
// (GALDRUN_logging__level=debug), then the command line (--logging:level=debug),
// each overriding the one before. The file is `--config <path>`, or
// appsettings.json beside the executable when that exists, or none.
//
// Binding is strict, the core's discipline: an unknown key is a startup error
// naming the key, because a silently ignored field is a bug that surfaces as
// "nothing plays" hours later. The generic host's own sources are not used --
// they read every environment variable of the process into the same tree.
public static class ConfigLoader
{
    public const string DefaultFileName = "appsettings.json";
    public const string EnvironmentPrefix = Branding.EnvironmentPrefix;

    // Under a service, --config is required: the service account's profile,
    // where the defaults live, is not the installer's (the core's phase 29).
    public static Result<ServerSettings, ConfigError> Load(string[] args, string executableDirectory,
        bool service = false)
    {
        var split = SplitConfigArgument(args);
        if (!split.IsSuccess)
            return split.Failure;
        var (explicitPath, rest) = split.Value;
        if (service && explicitPath is null)
            return Result.Fail(ConfigError.MissingValue, "--config is required when running as a service");

        string? configPath = null;
        if (explicitPath is not null)
        {
            configPath = Path.GetFullPath(explicitPath);
            if (!File.Exists(configPath))
                return Result.Fail(ConfigError.FileNotFound, configPath);
        }
        else if (File.Exists(Path.Combine(executableDirectory, DefaultFileName)))
        {
            configPath = Path.Combine(executableDirectory, DefaultFileName);
        }

        IConfigurationRoot root;
        try
        {
            var builder = new ConfigurationBuilder();
            if (configPath is not null)
                builder.AddJsonFile(configPath, optional: false, reloadOnChange: false);
            root = builder
                .AddEnvironmentVariables(EnvironmentPrefix)
                .AddCommandLine(rest)
                .Build();
        }
        catch (Exception e) when (e is InvalidDataException or FormatException or IOException)
        {
            return Result.Fail(ConfigError.UnreadableFile, $"{configPath}: {e.Message}");
        }

        if (FindUnknownKey(root, typeof(ServerOptions), "") is { } unknown)
            return Result.Fail(ConfigError.UnknownKey, unknown);

        var options = new ServerOptions();
        try
        {
            // The binder drops a list item it cannot bind instead of throwing,
            // so each folder and AI provider is bound alone first, where a bad
            // value throws.
            foreach (var folder in root.GetSection("library:folders").GetChildren())
                folder.Get<LibraryFolderOptions>();
            foreach (var provider in root.GetSection("ai:providers").GetChildren())
                provider.Get<AiProviderOptions>();
            foreach (var log in root.GetSection("gui:logs").GetChildren())
                log.Get<GuiLogOptions>();
            root.Bind(options);
        }
        catch (InvalidOperationException e)
        {
            return Result.Fail(ConfigError.MalformedValue, e.Message);
        }

        return Resolve(options, configPath);
    }

    private static Result<(string? Path, string[] Remaining), ConfigError> SplitConfigArgument(string[] args)
    {
        string? path = null;
        var rest = new List<string>();
        for (var i = 0; i < args.Length; i++)
        {
            if (args[i] == "--config")
            {
                if (i + 1 >= args.Length)
                    return Result.Fail(ConfigError.MissingValue, "--config needs a path");
                path = args[++i];
            }
            else if (args[i].StartsWith("--config=", StringComparison.Ordinal))
            {
                path = args[i]["--config=".Length..];
            }
            else
            {
                rest.Add(args[i]);
            }
        }
        return (path, rest.ToArray());
    }

    // The first key, as a dotted path, that no property of the options type
    // names. Keys compare case-insensitively, as the binder binds them.
    private static string? FindUnknownKey(IConfiguration section, Type type, string prefix)
    {
        foreach (var child in section.GetChildren())
        {
            var path = prefix + child.Key;
            var property = type.GetProperty(child.Key,
                BindingFlags.Public | BindingFlags.Instance | BindingFlags.IgnoreCase);
            if (property is null)
                return path;

            var propertyType = property.PropertyType;
            var isList = propertyType.IsGenericType && propertyType.GetGenericTypeDefinition() == typeof(List<>);
            var isSection = propertyType.IsClass && propertyType != typeof(string);
            if (isList)
            {
                // Each item is a section of the element type: library.folders.0.path.
                var itemType = propertyType.GetGenericArguments()[0];
                foreach (var item in child.GetChildren())
                {
                    if (FindUnknownKey(item, itemType, $"{path}.{item.Key}.") is { } unknown)
                        return unknown;
                }
            }
            else if (isSection)
            {
                if (FindUnknownKey(child, propertyType, path + ".") is { } unknown)
                    return unknown;
            }
            else if (child.GetChildren().FirstOrDefault() is { } nested)
            {
                return $"{path}.{nested.Key}";
            }
        }
        return null;
    }

    private static Result<ServerSettings, ConfigError> Resolve(ServerOptions options, string? configPath)
    {
        var logging = options.Logging;
        if (!Enum.IsDefined(logging.Level))
            return Result.Fail(ConfigError.OutOfRange, $"logging.level: {logging.Level}");
        if (logging.MaxBytes != 0 && logging.MaxBytes < LoggingOptions.MinimumMaxBytes)
        {
            return Result.Fail(ConfigError.OutOfRange,
                $"logging.maxBytes: 0 or at least {LoggingOptions.MinimumMaxBytes}, was {logging.MaxBytes}");
        }

        var core = options.Core;
        if (string.IsNullOrWhiteSpace(core.Host))
            return Result.Fail(ConfigError.MissingValue, "core.host");
        if (core.Port is < 1 or > 65535)
            return Result.Fail(ConfigError.OutOfRange, $"core.port: 1 to 65535, was {core.Port}");
        if (core.DiscoveryPort is < 0 or > 65535)
            return Result.Fail(ConfigError.OutOfRange, $"core.discoveryPort: 0 to 65535, was {core.DiscoveryPort}");

        // A path written in a file is read against that file. The working
        // directory of a service is System32 (the core's phase 29 lesson).
        var baseDirectory = configPath is not null
            ? Path.GetDirectoryName(configPath)!
            : Directory.GetCurrentDirectory();

        // An account with no profile (a systemd DynamicUser, some service
        // accounts) has no local application data folder, and so no default:
        // it is refused by name rather than put in the working directory or
        // beside a config file the account may not write to.
        var localData = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
        if (string.IsNullOrEmpty(options.DataDirectory) && string.IsNullOrEmpty(localData))
            return Result.Fail(ConfigError.MissingValue, "dataDirectory: this account has no local application data folder");
        var dataDirectory = string.IsNullOrEmpty(options.DataDirectory)
            ? Path.Combine(localData, Branding.Name, "Server")
            : Path.GetFullPath(options.DataDirectory, baseDirectory);
        try
        {
            Directory.CreateDirectory(dataDirectory);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or NotSupportedException)
        {
            return Result.Fail(ConfigError.UnusableDataDirectory, $"{dataDirectory}: {e.Message}");
        }

        var logFile = logging.File switch
        {
            null => Path.Combine(dataDirectory, "server.log"),
            "" => null,
            var file => Path.GetFullPath(file, baseDirectory),
        };

        var media = options.Media;
        if (!IPAddress.TryParse(media.Bind, out var mediaBind))
            return Result.Fail(ConfigError.MalformedValue, $"media.bind: not an IP address: {media.Bind}");
        if (media.Port is < 0 or > 65535)
            return Result.Fail(ConfigError.OutOfRange, $"media.port: 0 to 65535, was {media.Port}");
        var mediaToken = string.IsNullOrEmpty(media.Token) ? null : media.Token;

        var api = ResolveApi(options.Api, media.Port);
        if (!api.IsSuccess)
            return api.Failure;

        var gui = ResolveGui(options.Gui, media.Port, api.Value.Port, logFile, baseDirectory, dataDirectory);
        if (!gui.IsSuccess)
            return gui.Failure;

        var library = ResolveLibrary(options.Library, baseDirectory);
        if (!library.IsSuccess)
            return library.Failure;

        var tidal = ResolveTidal(options.Providers.Tidal);
        if (!tidal.IsSuccess)
            return tidal.Failure;

        var ai = ResolveAi(options.Ai);
        if (!ai.IsSuccess)
            return ai.Failure;

        var tuneIn = ResolveTuneIn(options.Providers.TuneIn);
        if (!tuneIn.IsSuccess)
            return tuneIn.Failure;

        return new ServerSettings(configPath, dataDirectory, logging.Level, logFile, logging.MaxBytes, core.Host,
            core.Port, new MediaEndpointOptions(mediaBind, media.Port, mediaToken), api.Value, library.Value,
            tidal.Value, options.Playback.AutoResume, tuneIn.Value, core.DiscoveryPort,
            ai.Value, new ArtworkSettings(options.Artwork.MusicBrainz,
                string.IsNullOrWhiteSpace(options.Artwork.FanartTvKey) ? null : options.Artwork.FanartTvKey.Trim()),
            gui.Value);
    }

    // The same ports as the API's are refused, and the API's own. This
    // server's log is always tailed as "server"; each other log needs both
    // a name of its own and a path.
    private static Result<GuiEndpointOptions, ConfigError> ResolveGui(GuiOptions gui, int mediaPort, int apiPort,
        string? logFile, string baseDirectory, string dataDirectory)
    {
        if (!IPAddress.TryParse(gui.Bind, out var bind))
            return Result.Fail(ConfigError.MalformedValue, $"gui.bind: not an IP address: {gui.Bind}");
        if (gui.Port is < 0 or > 65535)
            return Result.Fail(ConfigError.OutOfRange, $"gui.port: 0 to 65535, was {gui.Port}");
        var taken = gui.Port switch
        {
            0 => null,
            4711 => "the core's MATP control port",
            4712 => "the core's MATP audio port",
            4713 => "the core's control API",
            4714 => "the media endpoint's port",
            _ when gui.Port == mediaPort => "media.port",
            _ when gui.Port == apiPort => "api.port",
            _ => null,
        };
        if (taken is not null)
            return Result.Fail(ConfigError.OutOfRange, $"gui.port: {gui.Port} is {taken}");

        var logs = new List<GuiLog>();
        if (logFile is not null)
            logs.Add(new GuiLog("server", logFile));
        for (var i = 0; i < gui.Logs.Count; i++)
        {
            var key = $"gui.logs.{i}";
            var log = gui.Logs[i];
            if (string.IsNullOrWhiteSpace(log.Name))
                return Result.Fail(ConfigError.MissingValue, $"{key}.name");
            if (string.IsNullOrWhiteSpace(log.Path))
                return Result.Fail(ConfigError.MissingValue, $"{key}.path");
            var name = log.Name.Trim();
            if (logs.Any(l => l.Name == name))
                return Result.Fail(ConfigError.MalformedValue, $"{key}.name: {name} is named twice");
            logs.Add(new GuiLog(name, Path.GetFullPath(log.Path, baseDirectory)));
        }
        return new GuiEndpointOptions(bind, gui.Port, Path.Combine(AppContext.BaseDirectory, "gui"), logs,
            Path.Combine(dataDirectory, "gui-settings.json"));
    }

    // Not enabled, no TuneIn: then nothing else of the section may be set,
    // because it would be silently ignored.
    private static Result<TuneInSettings?, ConfigError> ResolveTuneIn(TuneInOptions tuneIn)
    {
        var partnerId = string.IsNullOrWhiteSpace(tuneIn.PartnerId) ? null : tuneIn.PartnerId.Trim();
        var locale = string.IsNullOrWhiteSpace(tuneIn.Locale) ? null : tuneIn.Locale.Trim();
        if (!tuneIn.Enabled)
        {
            return partnerId is not null || locale is not null
                ? Result.Fail(ConfigError.MissingValue, "providers.tunein.enabled")
                : (TuneInSettings?)null;
        }
        if (locale is not null && !IsLocale(locale))
            return Result.Fail(ConfigError.MalformedValue, $"providers.tunein.locale: not a locale such as de-DE: {locale}");
        return new TuneInSettings(partnerId, locale);
    }

    // Each provider once, with a key or a base URL: a block with neither
    // would be silently ignored. Whether the id is a provider is the AI
    // registry's to say at start, since a test may register its own.
    private static Result<IReadOnlyList<AiProviderConfig>, ConfigError> ResolveAi(AiOptions ai)
    {
        var providers = new List<AiProviderConfig>();
        for (var i = 0; i < ai.Providers.Count; i++)
        {
            var key = $"ai.providers.{i}";
            var provider = ai.Providers[i];
            if (string.IsNullOrWhiteSpace(provider.Id))
                return Result.Fail(ConfigError.MissingValue, $"{key}.id");
            var id = provider.Id.Trim();
            if (providers.Any(p => p.Id == id))
                return Result.Fail(ConfigError.MalformedValue, $"{key}.id: {id} is named twice");
            var apiKey = string.IsNullOrWhiteSpace(provider.ApiKey) ? null : provider.ApiKey.Trim();
            Uri? baseUrl = null;
            if (!string.IsNullOrWhiteSpace(provider.BaseUrl))
            {
                var text = provider.BaseUrl.Trim();
                if (!Uri.TryCreate(text.EndsWith('/') ? text : text + "/", UriKind.Absolute, out baseUrl) ||
                    baseUrl.Scheme is not ("http" or "https") || baseUrl.UserInfo.Length > 0)
                    return Result.Fail(ConfigError.MalformedValue, $"{key}.baseUrl: an http or https URL, was {text}");
            }
            if (apiKey is null && baseUrl is null)
                return Result.Fail(ConfigError.MissingValue, $"{key}.apiKey or {key}.baseUrl");
            providers.Add(new AiProviderConfig(id, apiKey, baseUrl));
        }
        return providers;
    }

    // A language, then subtags of letters and digits: de, de-DE, zh-Hant-TW.
    private static bool IsLocale(string locale)
    {
        var parts = locale.Split('-');
        return parts[0].Length is 2 or 3 && parts[0].All(char.IsAsciiLetter) &&
            parts.Skip(1).All(p => p.Length is >= 2 and <= 8 && p.All(char.IsAsciiLetterOrDigit));
    }

    // The core's listeners and the media endpoint's are refused by name: a
    // listener there would fail at startup, or answer the wrong caller.
    private static Result<ApiEndpointOptions, ConfigError> ResolveApi(ApiOptions api, int mediaPort)
    {
        if (!IPAddress.TryParse(api.Bind, out var bind))
            return Result.Fail(ConfigError.MalformedValue, $"api.bind: not an IP address: {api.Bind}");
        if (api.Port is < 0 or > 65535)
            return Result.Fail(ConfigError.OutOfRange, $"api.port: 0 to 65535, was {api.Port}");
        var taken = api.Port switch
        {
            4711 => "the core's MATP control port",
            4712 => "the core's MATP audio port",
            4713 => "the core's control API",
            4714 => "the media endpoint's port",
            _ when api.Port != 0 && api.Port == mediaPort => "media.port",
            _ => null,
        };
        if (taken is not null)
            return Result.Fail(ConfigError.OutOfRange, $"api.port: {api.Port} is {taken}");
        if (api.SearchTimeout <= TimeSpan.Zero)
            return Result.Fail(ConfigError.OutOfRange, $"api.searchTimeout: more than 0, was {api.SearchTimeout}");
        return new ApiEndpointOptions(bind, api.Port, string.IsNullOrEmpty(api.Token) ? null : api.Token,
            api.SearchTimeout);
    }

    // A secret without a client id would be silently ignored.
    private static Result<TidalSettings, ConfigError> ResolveTidal(TidalOptions tidal)
    {
        var clientId = string.IsNullOrWhiteSpace(tidal.ClientId) ? null : tidal.ClientId.Trim();
        if (clientId is null && !string.IsNullOrEmpty(tidal.ClientSecret))
            return Result.Fail(ConfigError.MissingValue, "providers.tidal.clientId");
        var playbackId = string.IsNullOrWhiteSpace(tidal.PlaybackClientId) ? null : tidal.PlaybackClientId.Trim();
        if (playbackId is null && !string.IsNullOrEmpty(tidal.PlaybackClientSecret))
            return Result.Fail(ConfigError.MissingValue, "providers.tidal.playbackClientId");
        if (!Enum.IsDefined(tidal.Quality))
            return Result.Fail(ConfigError.OutOfRange, $"providers.tidal.quality: {tidal.Quality}");
        if (tidal.CacheBytes < TidalSettings.MinimumCacheBytes)
        {
            return Result.Fail(ConfigError.OutOfRange,
                $"providers.tidal.cacheBytes: at least {TidalSettings.MinimumCacheBytes}, was {tidal.CacheBytes}");
        }
        if (tidal.ArtworkCacheBytes < TidalSettings.MinimumArtworkCacheBytes)
        {
            return Result.Fail(ConfigError.OutOfRange,
                $"providers.tidal.artworkCacheBytes: at least {TidalSettings.MinimumArtworkCacheBytes}, was {tidal.ArtworkCacheBytes}");
        }
        return new TidalSettings(clientId, string.IsNullOrEmpty(tidal.ClientSecret) ? null : tidal.ClientSecret,
            tidal.Quality, tidal.CacheBytes, tidal.ArtworkCacheBytes, playbackId,
            string.IsNullOrEmpty(tidal.PlaybackClientSecret) ? null : tidal.PlaybackClientSecret);
    }

    // A folder need not exist: a share that is down at startup is the
    // indexer's to retry, not a config fault. Folders may not overlap, because
    // one file would then belong to two of them.
    private static Result<LibrarySettings, ConfigError> ResolveLibrary(LibraryOptions library, string baseDirectory)
    {
        if (library.PollInterval < TimeSpan.FromSeconds(1))
        {
            return Result.Fail(ConfigError.OutOfRange,
                $"library.pollInterval: at least 00:00:01, was {library.PollInterval}");
        }

        var folders = new List<LibraryFolder>();
        for (var i = 0; i < library.Folders.Count; i++)
        {
            var resolved = ResolveFolder(library.Folders[i], $"library.folders.{i}", baseDirectory);
            if (!resolved.IsSuccess)
                return resolved.Failure;
            var folder = resolved.Value;
            var overlapped = folders.FindIndex(f => LibraryFolder.Overlap(f, folder));
            if (overlapped >= 0)
            {
                return Result.Fail(ConfigError.MalformedValue,
                    $"library.folders.{i}.path: {folder.Path} overlaps library.folders.{overlapped}.path");
            }
            folders.Add(folder);
        }
        return new LibrarySettings(folders, library.PollInterval);
    }

    // A local or network folder is a path; an smb folder a host, a share and
    // a login, and each refuses the other's keys, which would be ignored.
    private static Result<LibraryFolder, ConfigError> ResolveFolder(LibraryFolderOptions folder, string key,
        string baseDirectory)
    {
        if (!Enum.IsDefined(folder.Type))
            return Result.Fail(ConfigError.OutOfRange, $"{key}.type: {folder.Type}");
        if (folder.MediaType is { } declared && !LibraryFolder.Declarable(declared))
            return Result.Fail(ConfigError.OutOfRange, $"{key}.mediaType: music or audiobook, was {declared}");
        if (folder.Type != LibraryFolderType.Smb)
        {
            var smbKey = (folder.Host, folder.Port, folder.Share, folder.User, folder.Password, folder.Domain) switch
            {
                ({ }, _, _, _, _, _) => "host",
                (_, { }, _, _, _, _) => "port",
                (_, _, { }, _, _, _) => "share",
                (_, _, _, { }, _, _) => "user",
                (_, _, _, _, { }, _) => "password",
                (_, _, _, _, _, { }) => "domain",
                _ => null,
            };
            if (smbKey is not null)
                return Result.Fail(ConfigError.MalformedValue, $"{key}.{smbKey}: only an smb folder takes it");
            if (string.IsNullOrWhiteSpace(folder.Path))
                return Result.Fail(ConfigError.MissingValue, $"{key}.path");
            var path = Path.TrimEndingDirectorySeparator(Path.GetFullPath(folder.Path, baseDirectory));
            return new LibraryFolder(path, folder.Type, folder.Enabled, MediaType: folder.MediaType);
        }

        if (string.IsNullOrWhiteSpace(folder.Host))
            return Result.Fail(ConfigError.MissingValue, $"{key}.host");
        if (string.IsNullOrWhiteSpace(folder.Share))
            return Result.Fail(ConfigError.MissingValue, $"{key}.share");
        if (folder.Port is < 1 or > 65535)
            return Result.Fail(ConfigError.OutOfRange, $"{key}.port: 1 to 65535, was {folder.Port}");
        if (folder.Host.Contains(':', StringComparison.Ordinal) || folder.Host.Contains('/', StringComparison.Ordinal) ||
            folder.Host.Contains('\\', StringComparison.Ordinal))
            return Result.Fail(ConfigError.MalformedValue, $"{key}.host: a name or an address, was {folder.Host}");
        try
        {
            var login = new SmbLogin(folder.Host, folder.Port ?? SmbLogin.DefaultPort, folder.User, folder.Password,
                folder.Domain);
            return LibraryFolder.OnShare(new SmbLocation(login, folder.Share, folder.Path), folder.Enabled,
                folder.MediaType);
        }
        catch (ArgumentException)
        {
            return Result.Fail(ConfigError.MalformedValue, $"{key}.path: no . or .. segments, was {folder.Path}");
        }
    }
}
