using Multiroom.Server.Abstractions;
using Multiroom.Server.Library;
using Multiroom.Server.Providers.Tidal;

namespace Multiroom.Server.Configuration;

// The config file's shape, bound key by key. Every property here is a key an
// operator may write; anything else in the file is refused by name
// (ConfigLoader). A section arrives in the phase that first reads it.
public sealed class ServerOptions
{
    // Null: %LOCALAPPDATA%/Galdrun/Server. Relative: against the config file.
    public string? DataDirectory { get; set; }

    public LoggingOptions Logging { get; set; } = new();

    public CoreOptions Core { get; set; } = new();

    public MediaOptions Media { get; set; } = new();

    public ApiOptions Api { get; set; } = new();

    public GuiOptions Gui { get; set; } = new();

    public LibraryOptions Library { get; set; } = new();

    public PlaybackOptions Playback { get; set; } = new();

    public ProvidersOptions Providers { get; set; } = new();

    public AiOptions Ai { get; set; } = new();

    public ArtworkOptions Artwork { get; set; } = new();
}

// The queues, kept in <dataDirectory>/playback.json across a restart.
public sealed class PlaybackOptions
{
    // Queues that were playing when this server stopped start again by
    // themselves. Off: they come back stopped, where they were, and a caller
    // plays them.
    public bool AutoResume { get; set; }
}

// One block per provider that has something to configure.
public sealed class ProvidersOptions
{
    public TidalOptions Tidal { get; set; } = new();

    public TuneInOptions TuneIn { get; set; } = new();
}

// TuneIn is registered only when enabled. The partner id is optional and
// never logged (rule 7); the locale is TuneIn's own (de-DE).
public sealed class TuneInOptions
{
    public bool Enabled { get; set; }

    public string? PartnerId { get; set; }

    public string? Locale { get; set; }
}

// The client id and optional secret are those of a Tidal app supporting device authorization; they are never logged (rule 7). They are
// only ever read from here: without a client id, Tidal waits for one. The optional playback client, when set, refreshes the tokens.
public sealed class TidalOptions
{
    public string? ClientId { get; set; }

    public string? ClientSecret { get; set; }

    public string? PlaybackClientId { get; set; }

    public string? PlaybackClientSecret { get; set; }

    public TidalQuality Quality { get; set; } = TidalQuality.Lossless;

    public long CacheBytes { get; set; } = TidalSettings.DefaultCacheBytes;

    public long ArtworkCacheBytes { get; set; } = TidalSettings.DefaultArtworkCacheBytes;
}

// The folders the indexer keeps in the library database, searched and browsed
// through the local provider.
public sealed class LibraryOptions
{
    // In order: the order of the first scan. Folders may not overlap.
    public List<LibraryFolderOptions> Folders { get; } = [];

    // How often a network or smb folder is scanned. A local folder is watched.
    public TimeSpan PollInterval { get; set; } = TimeSpan.FromMinutes(5);
}

public sealed class LibraryFolderOptions
{
    // A local or network folder's path; relative: against the config file. A
    // network folder is a path the OS mounts (\\server\share, or a mount
    // point on Linux), reached as the server's account. An smb folder's
    // directory inside its share; none is the share's root.
    public string? Path { get; set; }

    public LibraryFolderType Type { get; set; } = LibraryFolderType.Local;

    // A disabled folder is not indexed, and what was indexed from it is forgotten.
    public bool Enabled { get; set; } = true;

    // An smb folder only: the NAS, its port (445 unless set), the share, and
    // the login. No user is guest. The password is best not written in the
    // file but set as GALDRUN_library__folders__<n>__password; it is never
    // logged.
    public string? Host { get; set; }

    public int? Port { get; set; }

    public string? Share { get; set; }

    public string? User { get; set; }

    public string? Password { get; set; }

    public string? Domain { get; set; }

    // What everything in the folder is (phase 30): music or audiobook. Unset,
    // the tags decide.
    public MediaType? MediaType { get; set; }
}

// The media endpoint the core fetches from: its media.baseUrl names it.
public sealed class MediaOptions
{
    // An IP address. Not loopback without a token is logged as a warning.
    public string Bind { get; set; } = "127.0.0.1";

    // 0 takes any free port, which only a test has a use for.
    public int Port { get; set; } = 4714;

    // The core's media.token (its phase 23). Null or empty: no token.
    public string? Token { get; set; }
}

// The outward API that UIs and remote controls talk to (/api/v1). Loopback
// by default; not loopback without a token is logged as a warning.
public sealed class ApiOptions
{
    public const int DefaultPort = 4725;

    public string Bind { get; set; } = "127.0.0.1";

    // 0 takes any free port, which only a test has a use for. The core's
    // 4711 to 4713 and the media endpoint's 4714 are refused by name.
    public int Port { get; set; } = DefaultPort;

    // Null or empty: no token. Never logged.
    public string? Token { get; set; }

    // How long unified search waits for each provider before it answers
    // without that provider's results.
    public TimeSpan SearchTimeout { get; set; } = TimeSpan.FromSeconds(5);
}

// The ServerGui, served from gui/ beside the executable, with /api forwarded
// to the outward API and api.token added. It has no authentication of its
// own: loopback by default, and not loopback is logged as a warning.
public sealed class GuiOptions
{
    // Not 4720: that is every client's settings page, and a client often runs
    // on the server's machine.
    public const int DefaultPort = 4723;

    public string Bind { get; set; } = "127.0.0.1";

    // 0 takes any free port, which only a test has a use for. The core's
    // ports, media.port and api.port are refused by name.
    public int Port { get; set; } = DefaultPort;

    // Files its log page tails besides this server's own log (as "server"),
    // e.g. the core's. A relative path: against the config file.
    public List<GuiLogOptions> Logs { get; } = [];
}

public sealed class GuiLogOptions
{
    public string? Name { get; set; }

    public string? Path { get; set; }
}

// Where the core's control API listens. Loopback by default: 4713 has no
// authentication, and CoreLink warns when this names another machine. A core
// selected through the API (CoreDiscovery) takes its place.
public sealed class CoreOptions
{
    public string Host { get; set; } = "127.0.0.1";

    public int Port { get; set; } = 4713;

    // The UDP port the cores' beacons arrive on (CoreDiscovery). 0 takes any
    // free port, which is for the tests: the cores send to 4722.
    public int DiscoveryPort { get; set; } = 4722;
}

public sealed class LoggingOptions
{
    public const long DefaultMaxBytes = 10 * 1024 * 1024;
    public const long MinimumMaxBytes = 64 * 1024;

    public LogVerbosity Level { get; set; } = LogVerbosity.Warn;

    // Null: server.log in the data directory. Empty: no file, stderr only.
    // Relative: against the config file, as the core does since its phase 29.
    public string? File { get; set; }

    // Zero never rotates. Otherwise at least MinimumMaxBytes, because a log that
    // rotates every few lines is of no use for diagnosis.
    public long MaxBytes { get; set; } = DefaultMaxBytes;
}

// The core's five level names, so one config vocabulary serves both servers.
public enum LogVerbosity
{
    Error,
    Warn,
    Info,
    Debug,
    Trace,
}

// The AI providers (phase 21): a key the house keeps in the file, read-only
// through the API, and a base URL for a server of one's own (Ollama on
// another machine, LM Studio). Keys set through the API are kept in
// <dataDirectory>/ai/credentials.json instead.
public sealed class AiOptions
{
    public List<AiProviderOptions> Providers { get; } = [];
}

public sealed class AiProviderOptions
{
    // The provider's id: openai, anthropic, gemini, ...
    public string? Id { get; set; }

    // Best not written in the file but set as GALDRUN_ai__providers__<n>__apiKey;
    // it is never logged.
    public string? ApiKey { get; set; }

    // Absolute, http or https, ending in the API's version path (/v1/).
    public string? BaseUrl { get; set; }
}

// Artwork the library lacks, looked up on the net by the artwork add-in (it
// always keeps the library's own covers). MusicBrainz is asked by default;
// fanart.tv when its key is set. Each is sent the library's album and artist
// names, or the MusicBrainz ids its tags carry.
public sealed class ArtworkOptions
{
    // MusicBrainz, the Cover Art Archive and Wikidata: open data, no key. It
    // also finds the ids every other source takes.
    public bool MusicBrainz { get; set; } = true;

    // A fanart.tv project key; set, fanart.tv is asked first. Never logged.
    public string? FanartTvKey { get; set; }
}
