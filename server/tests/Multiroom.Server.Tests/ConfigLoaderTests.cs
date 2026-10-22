using System.Net;
using System.Text.Json;
using Multiroom.Server.Abstractions;
using Multiroom.Server.Configuration;
using Multiroom.Server.Library;
using Multiroom.Server.Providers.Tidal;
using Multiroom.Server.Web.Gui;

namespace Multiroom.Server.Tests;

public sealed class ConfigLoaderTests : IDisposable
{
    private readonly TempDirectory _dir = new();

    public void Dispose() => _dir.Dispose();

    private string Config(string json) => _dir.Write("server.json", json);

    [Fact]
    public void Defaults_apply_when_nothing_is_configured()
    {
        var data = Path.Combine(_dir.Path, "data");

        var result = ConfigLoader.Load([$"--dataDirectory={data}"], _dir.Path);

        Assert.True(result.IsSuccess, result.ToString());
        var settings = result.Value;
        Assert.Null(settings.ConfigPath);
        Assert.Equal(data, settings.DataDirectory);
        Assert.True(Directory.Exists(data));
        Assert.Equal(LogVerbosity.Info, settings.LogLevel);
        Assert.Equal(Path.Combine(data, "server.log"), settings.LogFile);
        Assert.Equal(LoggingOptions.DefaultMaxBytes, settings.LogMaxBytes);
        Assert.Equal("127.0.0.1", settings.CoreHost);
        Assert.Equal(4713, settings.CorePort);
        Assert.Equal(IPAddress.Loopback, settings.Media.Bind);
        Assert.Equal(4714, settings.Media.Port);
        Assert.Null(settings.Media.Token);
        Assert.Equal(IPAddress.Loopback, settings.Api.Bind);
        Assert.Equal(4725, settings.Api.Port);
        Assert.Null(settings.Api.Token);
        Assert.Equal(TimeSpan.FromSeconds(5), settings.Api.SearchTimeout);
        Assert.Empty(settings.Library.Folders);
        Assert.Equal(TimeSpan.FromMinutes(5), settings.Library.PollInterval);
        Assert.Null(settings.Tidal.ClientId);
        Assert.Equal(TidalQuality.Lossless, settings.Tidal.Quality);
    }

    [Fact]
    public void Tidal_is_read_from_the_file_with_defaults_and_its_secret_is_in_no_ToString()
    {
        var result = ConfigLoader.Load(["--config", Config("""
            { "providers": { "tidal": { "clientId": " app-id ", "clientSecret": "app-secret", "quality": "hiResLossless",
              "playbackClientId": " play-id ", "playbackClientSecret": "play-secret" } } }
            """)], _dir.Path);

        Assert.True(result.IsSuccess, result.ToString());
        var tidal = result.Value.Tidal!;
        Assert.Equal("app-id", tidal.ClientId);
        Assert.Equal("app-secret", tidal.ClientSecret);
        Assert.Equal("play-id", tidal.PlaybackClientId);
        Assert.Equal("play-secret", tidal.PlaybackClientSecret);
        Assert.Equal(TidalQuality.HiResLossless, tidal.Quality);
        Assert.Equal(TidalSettings.DefaultCacheBytes, tidal.CacheBytes);
        Assert.Equal(TidalSettings.DefaultArtworkCacheBytes, tidal.ArtworkCacheBytes);
        Assert.DoesNotContain("app-secret", result.Value.ToString(), StringComparison.Ordinal);
        Assert.DoesNotContain("play-secret", result.Value.ToString(), StringComparison.Ordinal);
    }

    [Theory]
    [InlineData("""{ "clientSecret": "s" }""", ConfigError.MissingValue, "providers.tidal.clientId")]
    [InlineData("""{ "clientId": "a", "playbackClientSecret": "s" }""", ConfigError.MissingValue, "providers.tidal.playbackClientId")]
    [InlineData("""{ "clientId": "a", "cacheBytes": 1000 }""", ConfigError.OutOfRange, "providers.tidal.cacheBytes")]
    [InlineData("""{ "clientId": "a", "artworkCacheBytes": 1000 }""", ConfigError.OutOfRange, "providers.tidal.artworkCacheBytes")]
    [InlineData("""{ "clientId": "a", "quality": "master" }""", ConfigError.MalformedValue, "")]
    public void A_tidal_section_that_cannot_be_used_is_refused(string tidal, ConfigError code, string key)
    {
        var result = ConfigLoader.Load(["--config", Config($$"""{ "providers": { "tidal": {{tidal}} } }""")], _dir.Path);

        Assert.False(result.IsSuccess);
        Assert.Equal(code, result.Failure.Code);
        Assert.StartsWith(key, result.Failure.Detail, StringComparison.Ordinal);
    }

    [Fact]
    public void Library_folders_are_read_in_order_with_relative_paths_against_the_file_and_disabled_ones_marked()
    {
        // A share as each OS mounts it: a UNC path, or a mount point.
        var share = OperatingSystem.IsWindows() ? @"\\nas\music" : "/mnt/nas/music";
        var path = Config($$"""
            { "dataDirectory": "data",
              "library": {
                "pollInterval": "00:01:30",
                "folders": [
                  { "path": "music/" },
                  { "path": {{JsonSerializer.Serialize(share)}}, "type": "network" },
                  { "path": "old", "enabled": false } ] } }
            """);

        var result = ConfigLoader.Load(["--config", path], _dir.Path);

        Assert.True(result.IsSuccess, result.ToString());
        Assert.Equal(
            [
                new LibraryFolder(Path.Combine(_dir.Path, "music"), LibraryFolderType.Local),
                new LibraryFolder(share, LibraryFolderType.Network),
                new LibraryFolder(Path.Combine(_dir.Path, "old"), LibraryFolderType.Local, Enabled: false),
            ],
            result.Value.Library.Folders);
        Assert.Equal(TimeSpan.FromSeconds(90), result.Value.Library.PollInterval);
    }

    [Theory]
    [InlineData("""[ { "path": "music" }, { "path": "music/rock" } ]""", "library.folders.1.path")]
    [InlineData("""[ { "path": "music/rock" }, { "path": "MUSIC", "enabled": false } ]""", "library.folders.1.path")]
    public void Overlapping_library_folders_are_refused(string folders, string key)
    {
        var result = ConfigLoader.Load(["--config", Config($$"""{ "library": { "folders": {{folders}} } }""")],
            _dir.Path);

        Assert.False(result.IsSuccess);
        Assert.Equal(ConfigError.MalformedValue, result.Failure.Code);
        Assert.StartsWith(key, result.Failure.Detail, StringComparison.Ordinal);
    }

    [Fact]
    public void A_library_folder_declares_its_media_type_and_radio_is_refused_by_key()
    {
        var result = ConfigLoader.Load(["--config", Config("""
            { "library": { "folders": [ { "path": "books", "mediaType": "audiobook" }, { "path": "music", "mediaType": "Music" } ] } }
            """)], _dir.Path);
        Assert.True(result.IsSuccess, result.ToString());
        Assert.Equal([MediaType.Audiobook, MediaType.Music], result.Value.Library.Folders.Select(f => f.MediaType));

        var radio = ConfigLoader.Load(["--config", Config("""
            { "library": { "folders": [ { "path": "books", "mediaType": "radio" } ] } }
            """)], _dir.Path);
        Assert.Equal("OutOfRange: library.folders.0.mediaType: music or audiobook, was Radio", radio.Failure.ToString());
    }

    [Fact]
    public void A_library_folder_without_a_path_is_refused()
    {
        var result = ConfigLoader.Load(
            ["--config", Config("""{ "library": { "folders": [ { "type": "network" } ] } }""")], _dir.Path);

        Assert.False(result.IsSuccess);
        Assert.Equal(ConfigError.MissingValue, result.Failure.Code);
        Assert.Equal("library.folders.0.path", result.Failure.Detail);
    }

    [Fact]
    public void An_smb_folder_is_read_with_its_login_and_its_password_is_in_no_ToString()
    {
        var path = Config("""
            { "library": { "folders": [
                { "type": "smb", "host": "NAS", "share": "music", "path": "\\flac\\", "user": "house", "domain": "home" },
                { "type": "smb", "host": "nas", "port": 1445, "share": "photos" } ] } }
            """);
        // As GALDRUN_library__folders__0__password would set it; an environment
        // variable would reach the other tests, which run in parallel.
        var result = ConfigLoader.Load(["--config", path, "--library:folders:0:password=nas-s3cret"], _dir.Path);

        Assert.True(result.IsSuccess, result.ToString());
        var folders = result.Value.Library.Folders;
        Assert.Equal(["smb://nas/music/flac", "smb://nas:1445/photos"], folders.Select(f => f.Path));
        var login = folders[0].Smb!.Login;
        Assert.Equal(("nas", 445, "house", "nas-s3cret", "home"), (login.Host, login.Port, login.User, login.Password, login.Domain));
        Assert.Null(folders[1].Smb!.Login.User);
        Assert.DoesNotContain("nas-s3cret", result.Value.ToString(), StringComparison.Ordinal);
        Assert.DoesNotContain("nas-s3cret", string.Join(" ", folders), StringComparison.Ordinal);
        Assert.DoesNotContain("house", string.Join(" ", folders), StringComparison.Ordinal);
    }

    [Theory]
    [InlineData("""{ "path": "music", "host": "nas" }""", ConfigError.MalformedValue, "library.folders.0.host: only an smb folder takes it")]
    [InlineData("""{ "path": "music", "password": "x" }""", ConfigError.MalformedValue, "library.folders.0.password: only an smb folder takes it")]
    [InlineData("""{ "type": "smb", "share": "music" }""", ConfigError.MissingValue, "library.folders.0.host")]
    [InlineData("""{ "type": "smb", "host": "nas" }""", ConfigError.MissingValue, "library.folders.0.share")]
    [InlineData("""{ "type": "smb", "host": "nas", "share": "m", "port": 0 }""", ConfigError.OutOfRange, "library.folders.0.port: 1 to 65535, was 0")]
    [InlineData("""{ "type": "smb", "host": "nas:445", "share": "m" }""", ConfigError.MalformedValue, "library.folders.0.host: a name or an address, was nas:445")]
    [InlineData("""{ "type": "smb", "host": "nas", "share": "m", "path": "a/../b" }""", ConfigError.MalformedValue, "library.folders.0.path: no . or .. segments, was a/../b")]
    public void An_smb_folder_that_cannot_be_used_is_refused_by_key(string folder, ConfigError code, string detail)
    {
        var result = ConfigLoader.Load(["--config", Config($$"""{ "library": { "folders": [ {{folder}} ] } }""")], _dir.Path);

        Assert.False(result.IsSuccess);
        Assert.Equal((code, detail), (result.Failure.Code, result.Failure.Detail));
    }

    [Fact]
    public void Smb_folders_on_one_share_may_not_overlap()
    {
        var result = ConfigLoader.Load(["--config", Config("""
            { "library": { "folders": [
                { "type": "smb", "host": "nas", "share": "music", "path": "flac" },
                { "type": "smb", "host": "nas", "share": "MUSIC", "path": "FLAC/new", "user": "other" } ] } }
            """)], _dir.Path);

        Assert.Equal(ConfigError.MalformedValue, result.Failure.Code);
        Assert.StartsWith("library.folders.1.path", result.Failure.Detail, StringComparison.Ordinal);
    }

    [Fact]
    public void The_media_endpoint_is_read_from_the_file_and_its_token_is_never_printed()
    {
        var path = Config("""
            { "dataDirectory": "data",
              "media": { "bind": "0.0.0.0", "port": 0, "token": "s3cret-token" } }
            """);

        var result = ConfigLoader.Load(["--config", path], _dir.Path);

        Assert.True(result.IsSuccess, result.ToString());
        var media = result.Value.Media;
        Assert.Equal(IPAddress.Any, media.Bind);
        Assert.Equal(0, media.Port);
        Assert.Equal("s3cret-token", media.Token);
        Assert.DoesNotContain("s3cret-token", result.ToString(), StringComparison.Ordinal);
    }

    [Fact]
    public void An_empty_media_token_means_none()
    {
        var result = ConfigLoader.Load(["--config", Config("""{ "dataDirectory": "data", "media": { "token": "" } }""")],
            _dir.Path);

        Assert.True(result.IsSuccess, result.ToString());
        Assert.Null(result.Value.Media.Token);
    }

    [Fact]
    public void A_media_bind_that_is_not_an_address_is_refused()
    {
        var result = ConfigLoader.Load(["--config", Config("""{ "media": { "bind": "localhost" } }""")], _dir.Path);

        Assert.False(result.IsSuccess);
        Assert.Equal(ConfigError.MalformedValue, result.Failure.Code);
        Assert.StartsWith("media.bind", result.Failure.Detail, StringComparison.Ordinal);
    }

    [Fact]
    public void The_bench_is_gone_and_media_directory_is_an_unknown_key()
    {
        var result = ConfigLoader.Load(["--config", Config("""{ "media": { "directory": "bench" } }""")], _dir.Path);

        Assert.False(result.IsSuccess);
        Assert.Equal("UnknownKey: media.directory", result.Failure.ToString());
    }

    [Fact]
    public void The_outward_api_is_read_from_the_file_and_its_token_is_never_printed()
    {
        var result = ConfigLoader.Load(["--config", Config("""
            { "dataDirectory": "data",
              "api": { "bind": "0.0.0.0", "port": 0, "token": "api-s3cret", "searchTimeout": "00:00:00.250" } }
            """)], _dir.Path);

        Assert.True(result.IsSuccess, result.ToString());
        var api = result.Value.Api;
        Assert.Equal(IPAddress.Any, api.Bind);
        Assert.Equal(0, api.Port);
        Assert.Equal("api-s3cret", api.Token);
        Assert.Equal(TimeSpan.FromMilliseconds(250), api.SearchTimeout);
        Assert.DoesNotContain("api-s3cret", result.ToString(), StringComparison.Ordinal);
    }

    [Theory]
    [InlineData("4711", "api.port: 4711 is the core's MATP control port")]
    [InlineData("4712", "api.port: 4712 is the core's MATP audio port")]
    [InlineData("4713", "api.port: 4713 is the core's control API")]
    [InlineData("4714", "api.port: 4714 is the media endpoint's port")]
    [InlineData("5000", "api.port: 5000 is media.port")]
    [InlineData("70000", "api.port: 0 to 65535, was 70000")]
    public void The_core_s_ports_and_the_media_endpoint_s_are_refused_to_the_api_by_name(string port, string detail)
    {
        var result = ConfigLoader.Load(["--config", Config($$"""{ "media": { "port": 5000 }, "api": { "port": {{port}} } }""")],
            _dir.Path);

        Assert.False(result.IsSuccess);
        Assert.Equal(ConfigError.OutOfRange, result.Failure.Code);
        Assert.Equal(detail, result.Failure.Detail);
    }

    [Fact]
    public void The_gui_tails_this_server_s_log_and_the_files_it_names()
    {
        var result = ConfigLoader.Load(["--config", Config("""
            { "dataDirectory": "data",
              "gui": { "bind": "0.0.0.0", "port": 4730, "logs": [ { "name": "core", "path": "core.log" } ] } }
            """)], _dir.Path);

        Assert.True(result.IsSuccess, result.ToString());
        var gui = result.Value.Gui!;
        Assert.Equal(IPAddress.Any, gui.Bind);
        Assert.Equal(4730, gui.Port);
        Assert.Equal(Path.Combine(AppContext.BaseDirectory, "gui"), gui.Root);
        Assert.Equal([new GuiLog("server", Path.Combine(_dir.Path, "data", "server.log")), new GuiLog("core", Path.Combine(_dir.Path, "core.log"))],
            gui.Logs);
        Assert.Equal(GuiOptions.DefaultPort, ConfigLoader.Load(["--config", Config("{}")], _dir.Path).Value.Gui!.Port);
    }

    [Theory]
    [InlineData("""{ "gui": { "port": 4713 } }""", "OutOfRange: gui.port: 4713 is the core's control API")]
    [InlineData("""{ "gui": { "port": 4725 } }""", "OutOfRange: gui.port: 4725 is api.port")]
    [InlineData("""{ "gui": { "bind": "localhost" } }""", "MalformedValue: gui.bind: not an IP address: localhost")]
    [InlineData("""{ "gui": { "logs": [ { "path": "a.log" } ] } }""", "MissingValue: gui.logs.0.name")]
    [InlineData("""{ "gui": { "logs": [ { "name": "server", "path": "a.log" } ] } }""", "MalformedValue: gui.logs.0.name: server is named twice")]
    [InlineData("""{ "gui": { "logs": [ { "name": "core", "pth": "a.log" } ] } }""", "UnknownKey: gui.logs.0.pth")]
    public void A_gui_section_that_cannot_work_is_refused_by_name(string json, string failure)
    {
        var result = ConfigLoader.Load(["--config", Config(json)], _dir.Path);

        Assert.False(result.IsSuccess);
        Assert.Equal(failure, result.Failure.ToString());
    }

    [Fact]
    public void An_api_bind_that_is_not_an_address_and_a_search_timeout_of_zero_are_refused()
    {
        var bind = ConfigLoader.Load(["--config", Config("""{ "api": { "bind": "localhost" } }""")], _dir.Path);
        var timeout = ConfigLoader.Load(["--config", Config("""{ "api": { "searchTimeout": "00:00:00" } }""")], _dir.Path);

        Assert.Equal(ConfigError.MalformedValue, bind.Failure.Code);
        Assert.StartsWith("api.bind", bind.Failure.Detail, StringComparison.Ordinal);
        Assert.Equal(ConfigError.OutOfRange, timeout.Failure.Code);
        Assert.StartsWith("api.searchTimeout", timeout.Failure.Detail, StringComparison.Ordinal);
    }

    [Fact]
    public void The_core_s_address_is_read_from_the_file()
    {
        var path = Config("""{ "dataDirectory": "data", "core": { "host": "10.0.0.2", "port": 5000 } }""");

        var result = ConfigLoader.Load(["--config", path], _dir.Path);

        Assert.True(result.IsSuccess, result.ToString());
        Assert.Equal("10.0.0.2", result.Value.CoreHost);
        Assert.Equal(5000, result.Value.CorePort);
    }

    [Fact]
    public void A_file_is_read_and_its_relative_paths_resolve_against_it()
    {
        var path = Config("""{ "dataDirectory": "data", "logging": { "level": "debug", "file": "logs/x.log", "maxBytes": 0 } }""");

        var result = ConfigLoader.Load(["--config", path], AppContext.BaseDirectory);

        Assert.True(result.IsSuccess, result.ToString());
        Assert.Equal(path, result.Value.ConfigPath);
        Assert.Equal(Path.Combine(_dir.Path, "data"), result.Value.DataDirectory);
        Assert.Equal(Path.Combine(_dir.Path, "logs", "x.log"), result.Value.LogFile);
        Assert.Equal(LogVerbosity.Debug, result.Value.LogLevel);
        Assert.Equal(0, result.Value.LogMaxBytes);
    }

    [Fact]
    public void Appsettings_beside_the_executable_is_the_default_file()
    {
        var path = _dir.Write(ConfigLoader.DefaultFileName, """{ "dataDirectory": "data" }""");

        var result = ConfigLoader.Load([], _dir.Path);

        Assert.True(result.IsSuccess, result.ToString());
        Assert.Equal(path, result.Value.ConfigPath);
    }

    [Fact]
    public void The_command_line_overrides_the_file()
    {
        var path = Config("""{ "dataDirectory": "data", "logging": { "level": "debug" } }""");

        var result = ConfigLoader.Load(["--config", path, "--logging:level=error"], _dir.Path);

        Assert.True(result.IsSuccess, result.ToString());
        Assert.Equal(LogVerbosity.Error, result.Value.LogLevel);
    }

    [Fact]
    public void An_empty_log_file_means_stderr_only()
    {
        var path = Config("""{ "dataDirectory": "data", "logging": { "file": "" } }""");

        var result = ConfigLoader.Load(["--config", path], _dir.Path);

        Assert.True(result.IsSuccess, result.ToString());
        Assert.Null(result.Value.LogFile);
    }

    [Theory]
    [InlineData("""{ "dataDirectry": "x" }""", "dataDirectry")]
    [InlineData("""{ "logging": { "levle": "debug" } }""", "logging.levle")]
    [InlineData("""{ "logging": { "level": { "nested": 1 } } }""", "logging.level.nested")]
    [InlineData("""{ "api": { "prot": 4720 } }""", "api.prot")]
    [InlineData("""{ "media": { "prot": 4714 } }""", "media.prot")]
    [InlineData("""{ "core": { "prot": 4713 } }""", "core.prot")]
    [InlineData("""{ "library": { "folders": [ { "path": "a" }, { "pth": "b" } ] } }""", "library.folders.1.pth")]
    [InlineData("""{ "library": { "pollIntervl": "00:01:00" } }""", "library.pollIntervl")]
    [InlineData("""{ "providers": { "spotify": { "clientId": "x" } } }""", "providers.spotify")]
    [InlineData("""{ "providers": { "tidal": { "clientSecrt": "x" } } }""", "providers.tidal.clientSecrt")]
    public void An_unknown_key_is_refused_by_name(string json, string key)
    {
        var result = ConfigLoader.Load(["--config", Config(json)], _dir.Path);

        Assert.False(result.IsSuccess);
        Assert.Equal(ConfigError.UnknownKey, result.Failure.Code);
        Assert.Equal(key, result.Failure.Detail);
    }

    [Fact]
    public void An_unknown_command_line_key_is_refused_by_name()
    {
        var result = ConfigLoader.Load(["--logging:colour=red"], _dir.Path);

        Assert.False(result.IsSuccess);
        Assert.Equal(ConfigError.UnknownKey, result.Failure.Code);
        Assert.Equal("logging.colour", result.Failure.Detail);
    }

    [Theory]
    [InlineData("""{ "logging": { "maxBytes": "lots" } }""", "logging:maxBytes")]
    [InlineData("""{ "logging": { "level": "loud" } }""", "logging:level")]
    [InlineData("""{ "library": { "folders": [ { "path": "a", "type": "cloud" } ] } }""", "library:folders:0:type")]
    public void A_malformed_value_is_refused_naming_its_key(string json, string key)
    {
        var result = ConfigLoader.Load(["--config", Config(json)], _dir.Path);

        Assert.False(result.IsSuccess);
        Assert.Equal(ConfigError.MalformedValue, result.Failure.Code);
        Assert.Contains(key, result.Failure.Detail, StringComparison.OrdinalIgnoreCase);
    }

    [Theory]
    [InlineData("""{ "logging": { "maxBytes": 1000 } }""", "logging.maxBytes")]
    [InlineData("""{ "logging": { "maxBytes": -1 } }""", "logging.maxBytes")]
    [InlineData("""{ "logging": { "level": "9" } }""", "logging.level")]
    [InlineData("""{ "core": { "port": 0 } }""", "core.port")]
    [InlineData("""{ "core": { "port": 65536 } }""", "core.port")]
    [InlineData("""{ "media": { "port": -1 } }""", "media.port")]
    [InlineData("""{ "media": { "port": 65536 } }""", "media.port")]
    [InlineData("""{ "library": { "pollInterval": "00:00:00.5" } }""", "library.pollInterval")]
    public void A_value_out_of_range_is_refused_naming_its_key(string json, string key)
    {
        var result = ConfigLoader.Load(["--config", Config(json)], _dir.Path);

        Assert.False(result.IsSuccess);
        Assert.Equal(ConfigError.OutOfRange, result.Failure.Code);
        Assert.StartsWith(key, result.Failure.Detail, StringComparison.Ordinal);
    }

    [Fact]
    public void A_missing_config_file_is_refused()
    {
        var result = ConfigLoader.Load(["--config", Path.Combine(_dir.Path, "nope.json")], _dir.Path);

        Assert.False(result.IsSuccess);
        Assert.Equal(ConfigError.FileNotFound, result.Failure.Code);
    }

    [Fact]
    public void Config_without_a_path_is_refused()
    {
        var result = ConfigLoader.Load(["--config"], _dir.Path);

        Assert.False(result.IsSuccess);
        Assert.Equal(ConfigError.MissingValue, result.Failure.Code);
    }

    [Fact]
    public void Broken_json_is_refused()
    {
        var result = ConfigLoader.Load(["--config", Config("""{ "logging": """)], _dir.Path);

        Assert.False(result.IsSuccess);
        Assert.Equal(ConfigError.UnreadableFile, result.Failure.Code);
    }

    [Fact]
    public void Playback_auto_resume_is_off_unless_the_file_turns_it_on()
    {
        var off = ConfigLoader.Load(["--config", Config("""{ "dataDirectory": "data" }""")], _dir.Path);
        var on = ConfigLoader.Load(["--config", Config("""{ "dataDirectory": "data", "playback": { "autoResume": true } }""")],
            _dir.Path);

        Assert.False(off.Value.AutoResume);
        Assert.True(on.Value.AutoResume);
    }

    [Fact]
    public void TuneIn_is_registered_only_when_enabled_and_its_partner_id_is_never_printed()
    {
        var none = ConfigLoader.Load(["--config", Config("""{ "dataDirectory": "data" }""")], _dir.Path);
        var on = ConfigLoader.Load(["--config", Config(
            """{ "dataDirectory": "data", "providers": { "tunein": { "enabled": true, "partnerId": "p-secret", "locale": "de-DE" } } }""")],
            _dir.Path);
        var notEnabled = ConfigLoader.Load(["--config", Config(
            """{ "dataDirectory": "data", "providers": { "tunein": { "locale": "de-DE" } } }""")], _dir.Path);
        var badLocale = ConfigLoader.Load(["--config", Config(
            """{ "dataDirectory": "data", "providers": { "tunein": { "enabled": true, "locale": "de_DE!" } } }""")], _dir.Path);
        var unknown = ConfigLoader.Load(["--config", Config(
            """{ "dataDirectory": "data", "providers": { "tunein": { "enabled": true, "token": "x" } } }""")], _dir.Path);

        Assert.Null(none.Value.TuneIn);
        Assert.Equal(("p-secret", "de-DE"), (on.Value.TuneIn!.PartnerId, on.Value.TuneIn.Locale));
        Assert.DoesNotContain("p-secret", on.Value.ToString(), StringComparison.Ordinal);
        Assert.Equal("MissingValue: providers.tunein.enabled", notEnabled.Failure.ToString());
        Assert.Equal(ConfigError.MalformedValue, badLocale.Failure.Code);
        Assert.Equal("UnknownKey: providers.tunein.token", unknown.Failure.ToString());
    }

    [Fact]
    public void MusicBrainz_is_asked_by_default_and_fanart_tv_once_its_key_is_set_which_is_never_printed()
    {
        var byDefault = ConfigLoader.Load(["--config", Config("""{ "dataDirectory": "data" }""")], _dir.Path);
        var set = ConfigLoader.Load(["--config", Config(
            """{ "dataDirectory": "data", "artwork": { "musicBrainz": false, "fanartTvKey": " fanart-secret " } }""")],
            _dir.Path);
        var blank = ConfigLoader.Load(["--config", Config("""{ "dataDirectory": "data", "artwork": { "fanartTvKey": " " } }""")],
            _dir.Path);
        var unknown = ConfigLoader.Load(["--config", Config("""{ "dataDirectory": "data", "artwork": { "discogs": true } }""")],
            _dir.Path);

        Assert.Equal((true, (string?)null), (byDefault.Value.Artwork!.MusicBrainz, byDefault.Value.Artwork.FanartTvKey));
        Assert.Equal((false, "fanart-secret"), (set.Value.Artwork!.MusicBrainz, set.Value.Artwork.FanartTvKey));
        Assert.DoesNotContain("fanart-secret", set.Value.ToString(), StringComparison.Ordinal);
        Assert.Null(blank.Value.Artwork!.FanartTvKey);
        Assert.Equal("UnknownKey: artwork.discogs", unknown.Failure.ToString());
    }

    [Fact]
    public void Ai_providers_take_a_key_or_a_base_url_each_once_and_the_key_is_never_printed()
    {
        ConfigError? Refused(string providers) => ConfigLoader.Load(["--config", Config(
            $$"""{ "dataDirectory": "data", "ai": { "providers": [ {{providers}} ] } }""")], _dir.Path) is { IsSuccess: false } r
            ? r.Failure.Code
            : null;

        var loaded = ConfigLoader.Load(["--config", Config("""
            { "dataDirectory": "data", "ai": { "providers": [
              { "id": "openai", "apiKey": "sk-never-printed" },
              { "id": "ollama", "baseUrl": "http://nas:11434/v1" } ] } }
            """)], _dir.Path);
        var fromEnvironment = ConfigLoader.Load(["--config", Config("""{ "dataDirectory": "data" }"""),
            "--ai:providers:0:id=groq", "--ai:providers:0:apiKey=gsk-x"], _dir.Path);

        Assert.True(loaded.IsSuccess, loaded.ToString());
        var ai = loaded.Value.AiProviders!;
        Assert.Equal(("openai", "sk-never-printed", null), (ai[0].Id, ai[0].ApiKey, ai[0].BaseUrl));
        Assert.Equal(("ollama", null, new Uri("http://nas:11434/v1/")), (ai[1].Id, ai[1].ApiKey, ai[1].BaseUrl));
        Assert.DoesNotContain("sk-never-printed", loaded.Value.ToString(), StringComparison.Ordinal);
        Assert.Equal("gsk-x", fromEnvironment.Value.AiProviders![0].ApiKey);
        Assert.Equal(ConfigError.MissingValue, Refused("""{ "apiKey": "k" }"""));
        Assert.Equal(ConfigError.MissingValue, Refused("""{ "id": "openai" }"""));
        Assert.Equal(ConfigError.MalformedValue, Refused("""{ "id": "a", "apiKey": "k" }, { "id": "a", "apiKey": "j" }"""));
        Assert.Equal(ConfigError.MalformedValue, Refused("""{ "id": "ollama", "baseUrl": "ftp://nas/" }"""));
        Assert.Equal(ConfigError.MalformedValue, Refused("""{ "id": "ollama", "baseUrl": "http://u:p@nas/" }"""));
        Assert.Equal("UnknownKey: ai.providers.0.apiKee", ConfigLoader.Load(["--config", Config(
            """{ "ai": { "providers": [ { "id": "openai", "apiKee": "k" } ] } }""")], _dir.Path).Failure.ToString());
    }

    [Fact]
    public void A_service_refuses_to_start_without_config_even_with_appsettings_beside_the_executable()
    {
        _dir.Write(ConfigLoader.DefaultFileName, """{ "dataDirectory": "data" }""");

        var refused = ConfigLoader.Load([], _dir.Path, service: true);
        var named = ConfigLoader.Load(["--config", Config("""{ "dataDirectory": "data" }""")], _dir.Path, service: true);

        Assert.Equal("MissingValue: --config is required when running as a service", refused.Failure.ToString());
        Assert.True(named.IsSuccess, named.ToString());
        Assert.Equal(Path.Combine(_dir.Path, "data"), named.Value.DataDirectory);
    }
}
