using System.Diagnostics;
using System.Formats.Tar;
using System.IO.Compression;
using System.Net;
using System.Net.NetworkInformation;
using System.Runtime.InteropServices;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;
using Multiroom.Server.Abstractions;
using Multiroom.Server.Playback.Core;

namespace Multiroom.Server.Web.Updates;

// Directory keeps the downloaded bundles (and takes one dropped in by hand,
// galdrun-server-bundle-v1.2.3-linux-armv8.tar.gz). ServerDirectory is where
// this server's executable is; Target its release name (windows-x64,
// linux-armv8, ...), null for a runtime without a release.
public sealed record ServerUpdatesOptions(string Directory, Uri ReleasesUrl, string ServerDirectory, string? Target,
    string ServerVersion, bool WindowsService);

// This server and its core as an update sees them. Available: a newer bundle
// exists, whether or not this installation can take it itself (Supported,
// else Reason says why).
public sealed record ServerUpdateView(
    string Version,
    string? CoreVersion,
    string? Target,
    bool Supported,
    string? Reason,
    string? Latest,
    bool Available,
    bool Updating);

// The server and the core are updated together, from the release's
// galdrun-server-bundle archive: both folders get the bundle's server/ and
// core/ (only what the archive holds; the replaced entries go to
// .update/previous), the core restarts in place through core.restart, then
// this server stops as usual and starts again (Program.cs). Only for a core on
// this machine that reports its executable, and folders this process may write.
public sealed partial class ServerUpdates : IDisposable
{
    private static readonly TimeSpan StartCheckPatience = TimeSpan.FromSeconds(30);
    // Time for the POST's answer to leave before the server stops.
    private static readonly TimeSpan StopDelay = TimeSpan.FromSeconds(1);

    private readonly ServerUpdatesOptions _options;
    private readonly CoreLink _link;
    private readonly IHostApplicationLifetime _lifetime;
    private readonly ILogger _log;
    private readonly HttpClient _internet;
    private readonly ReleasePackages _packages;
    private readonly SemaphoreSlim _gate = new(1, 1);

    public ServerUpdates(ServerUpdatesOptions options, CoreLink link, IHostApplicationLifetime lifetime,
        ILogger<ServerUpdates> log)
    {
        _options = options;
        _link = link;
        _lifetime = lifetime;
        _log = log;
        _internet = new HttpClient(new SocketsHttpHandler { ConnectTimeout = TimeSpan.FromSeconds(10) })
        {
            Timeout = TimeSpan.FromMinutes(10),
        };
        _internet.DefaultRequestHeaders.UserAgent.ParseAdd("Galdrun-Server");
        _packages = new ReleasePackages("server-bundle", options.Directory, options.ReleasesUrl, _internet, log);
    }

    // Set once the files are replaced and the core restarted: Program.cs
    // starts the server again after its normal stop.
    public bool RestartRequested { get; private set; }

    public void Dispose()
    {
        _internet.Dispose();
        _gate.Dispose();
    }

    // The release name of a .NET runtime identifier, as build-server.ps1 names them.
    public static string? TargetOf(string runtimeIdentifier) => runtimeIdentifier switch
    {
        "win-x64" => "windows-x64",
        "win-arm64" => "windows-armv8",
        "linux-x64" => "linux-x64",
        "linux-arm" => "linux-armv7",
        "linux-arm64" => "linux-armv8",
        _ => null,
    };

    public async Task<ServerUpdateView> GetAsync(CancellationToken ct)
    {
        var core = await CoreAsync(ct).ConfigureAwait(false);
        var latest = _options.Target is null ? null
            : await _packages.LatestAsync(_options.Target, ct).ConfigureAwait(false);
        var reason = Unsupported(core);
        var available = latest is not null &&
                        (ReleasePackages.IsNewer(latest.Version, _options.ServerVersion) ||
                         (core is not null && ReleasePackages.IsNewer(latest.Version, core.Version)));
        return new ServerUpdateView(_options.ServerVersion, core?.Version, _options.Target, reason is null, reason,
            latest?.Tag, available, RestartRequested);
    }

    public async Task<Result<ServerUpdateView, UpdateError>> UpdateAsync(CancellationToken ct)
    {
        if (!await _gate.WaitAsync(0, ct).ConfigureAwait(false))
            return Result.Fail(UpdateError.Conflict, "an update is already running");
        var restarting = false;
        try
        {
            if (RestartRequested)
                return Result.Fail(UpdateError.Conflict, "the server is restarting into its update");
            var core = await CoreAsync(ct).ConfigureAwait(false);
            if (Unsupported(core) is { } reason)
                return Result.Fail(UpdateError.Unavailable, reason);
            var latest = await _packages.LatestAsync(_options.Target!, ct).ConfigureAwait(false);
            if (latest is null || (!ReleasePackages.IsNewer(latest.Version, _options.ServerVersion) &&
                                   !ReleasePackages.IsNewer(latest.Version, core!.Version)))
                return Result.Fail(UpdateError.Conflict,
                    $"server {_options.ServerVersion} and core {core!.Version}: there is no newer bundle for {_options.Target}");

            var file = await _packages.DownloadAsync(latest, ct).ConfigureAwait(false);
            if (!file.IsSuccess)
                return file.Failure;
            var installed = await InstallAsync(file.Value.Path, Path.GetDirectoryName(core!.Executable!)!, ct)
                .ConfigureAwait(false);
            if (!installed.IsSuccess)
                return installed.Failure;

            var restarted = await _link.SendAsync(new CoreRestart(), ct).ConfigureAwait(false);
            if (!restarted.IsSuccess)
            {
                installed.Value.ForEach(s => s.Restore());
                return Result.Fail(UpdateError.Failed, $"the core did not restart: {restarted.Failure}");
            }

            LogUpdating(_log, _options.ServerVersion, core.Version, latest.Tag);
            RestartRequested = restarting = true;
            _ = Task.Delay(StopDelay, CancellationToken.None).ContinueWith(_ => _lifetime.StopApplication(),
                CancellationToken.None, TaskContinuationOptions.None, TaskScheduler.Default);
            return new ServerUpdateView(_options.ServerVersion, core.Version, _options.Target, true, null, latest.Tag,
                true, true);
        }
        finally
        {
            // Held while restarting: nothing else may start meanwhile.
            if (!restarting)
                _gate.Release();
        }
    }

    // --- what can be updated ----------------------------------------------

    private async Task<CoreServerInfo?> CoreAsync(CancellationToken ct)
    {
        if (!_link.IsConnected)
            return null;
        var status = await _link.SendAsync(new GetStatus(), ct).ConfigureAwait(false);
        return status.IsSuccess ? status.Value.Server : null;
    }

    private string? Unsupported(CoreServerInfo? core)
    {
        if (_options.Target is null)
            return $"there is no server release for {RuntimeInformation.RuntimeIdentifier}";
        if (_options.WindowsService)
            return "the server runs as a Windows service; update it by hand";
        // build-server.ps1/.sh publish it; dotnet run and tests have none.
        if (!File.Exists(Path.Combine(_options.ServerDirectory, "build-info.txt")))
            return "this server is not a published release";
        if (core is null)
            return "the core is not connected";
        if (core.Executable is null)
            return "the core is too old to say where it runs; update it once by hand";
        if (!core.Restartable)
            return "the core runs as a Windows service; update it by hand";
        if (!IsThisMachine(_link.Host))
            return "the core runs on another machine; update it there";
        var coreDirectory = Path.GetDirectoryName(core.Executable);
        if (coreDirectory is null || !Directory.Exists(coreDirectory))
            return $"the core's folder {coreDirectory} is not on this machine";
        foreach (var directory in new[] { _options.ServerDirectory, coreDirectory })
        {
            if (!IsWritable(directory))
                return $"{directory} is not writable for the server";
        }
        return null;
    }

    private static bool IsThisMachine(string host)
    {
        if (host.Equals("localhost", StringComparison.OrdinalIgnoreCase) ||
            host.Equals(Environment.MachineName, StringComparison.OrdinalIgnoreCase))
            return true;
        if (!IPAddress.TryParse(host, out var address))
            return false;
        return IPAddress.IsLoopback(address) || NetworkInterface.GetAllNetworkInterfaces()
            .SelectMany(i => i.GetIPProperties().UnicastAddresses).Any(a => a.Address.Equals(address));
    }

    private static bool IsWritable(string directory)
    {
        try
        {
            var work = Directory.CreateDirectory(Path.Combine(directory, ".update")).FullName;
            var probe = Path.Combine(work, "probe");
            File.WriteAllBytes(probe, []);
            File.Delete(probe);
            return true;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            return false;
        }
    }

    // --- the files ----------------------------------------------------------

    // Unpacks the bundle beside each folder (so every move is a rename on its
    // own volume), checks that both new programs start here, then swaps them
    // in. A failure puts back what was swapped. Renaming a running program's
    // files is allowed on Linux and Windows; it keeps running from the moved ones.
    private async Task<Result<List<Swap>, UpdateError>> InstallAsync(string archive, string coreDirectory,
        CancellationToken ct)
    {
        var serverStaged = Path.Combine(_options.ServerDirectory, ".update", "staged");
        var coreStaged = Path.Combine(coreDirectory, ".update", "staged");
        var swaps = new List<Swap>();
        try
        {
            var serverRoot = await UnpackAsync(archive, serverStaged, ct).ConfigureAwait(false);
            var coreRoot = coreStaged == serverStaged ? serverRoot
                : await UnpackAsync(archive, coreStaged, ct).ConfigureAwait(false);
            if (serverRoot is null || coreRoot is null)
                return Result.Fail(UpdateError.Failed, $"{Path.GetFileName(archive)} has no server/ and core/");
            var newServer = Path.Combine(serverRoot, "server");
            var newCore = Path.Combine(coreRoot, "core");
            if (await StartsAsync(Path.Combine(newServer, Executable("galdrun-server")), null, ct)
                    .ConfigureAwait(false) is { } serverProblem)
                return Result.Fail(UpdateError.Failed, $"the new server does not start here: {serverProblem}");
            if (await StartsAsync(Path.Combine(newCore, Executable("galdrun-core")), Path.Combine(newCore, "lib"), ct)
                    .ConfigureAwait(false) is { } coreProblem)
                return Result.Fail(UpdateError.Failed, $"the new core does not start here: {coreProblem}");

            swaps.Add(new Swap(_options.ServerDirectory));
            swaps[^1].Install(newServer, []);
            swaps.Add(new Swap(coreDirectory));
            swaps[^1].Install(newCore, []);
            // A bundle folder (start script beside server/ and core/) gets the
            // bundle's own files too: start scripts, README, licences.
            var bundle = Path.GetDirectoryName(Path.TrimEndingDirectorySeparator(_options.ServerDirectory));
            if (bundle is not null &&
                bundle == Path.GetDirectoryName(Path.TrimEndingDirectorySeparator(coreDirectory)) &&
                (File.Exists(Path.Combine(bundle, "start.sh")) || File.Exists(Path.Combine(bundle, "start.cmd"))))
            {
                swaps.Add(new Swap(bundle));
                swaps[^1].Install(serverRoot, [
                    "server", "core", Path.GetFileName(Path.TrimEndingDirectorySeparator(_options.ServerDirectory)),
                    Path.GetFileName(Path.TrimEndingDirectorySeparator(coreDirectory)),
                ]);
            }
            return swaps;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or InvalidDataException)
        {
            for (var i = swaps.Count - 1; i >= 0; i--)
                swaps[i].Restore();
            return Result.Fail(UpdateError.Failed, $"cannot install the update: {e.Message}");
        }
        finally
        {
            TryDelete(serverStaged);
            TryDelete(coreStaged);
        }
    }

    private static string Executable(string name) => OperatingSystem.IsWindows() ? name + ".exe" : name;

    // The folder in the archive that holds server/ and core/ (a release
    // archive has one top folder), or null.
    private static async Task<string?> UnpackAsync(string archive, string destination, CancellationToken ct)
    {
        TryDelete(destination);
        Directory.CreateDirectory(destination);
        if (archive.EndsWith(".zip", StringComparison.OrdinalIgnoreCase))
        {
            await ZipFile.ExtractToDirectoryAsync(archive, destination, ct).ConfigureAwait(false);
        }
        else
        {
            var file = File.OpenRead(archive);
            await using (file.ConfigureAwait(false))
            {
                var gzip = new GZipStream(file, CompressionMode.Decompress);
                await using (gzip.ConfigureAwait(false))
                    await TarFile.ExtractToDirectoryAsync(gzip, destination, overwriteFiles: false, ct)
                        .ConfigureAwait(false);
            }
        }
        foreach (var folder in Directory.EnumerateDirectories(destination).Prepend(destination))
        {
            if (Directory.Exists(Path.Combine(folder, "server")) && Directory.Exists(Path.Combine(folder, "core")))
                return folder;
        }
        return null;
    }

    // Null when `executable --version` exits 0 in time, else what went wrong.
    private static async Task<string?> StartsAsync(string executable, string? libraries, CancellationToken ct)
    {
        if (!File.Exists(executable))
            return $"{Path.GetFileName(executable)} is missing";
        var start = new ProcessStartInfo(executable, "--version")
        {
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false,
        };
        if (libraries is not null && !OperatingSystem.IsWindows())
            start.Environment["LD_LIBRARY_PATH"] = libraries;
        using var patience = CancellationTokenSource.CreateLinkedTokenSource(ct);
        patience.CancelAfter(StartCheckPatience);
        try
        {
            using var process = Process.Start(start)!;
            var output = process.StandardOutput.ReadToEndAsync(patience.Token);
            var error = process.StandardError.ReadToEndAsync(patience.Token);
            await process.WaitForExitAsync(patience.Token).ConfigureAwait(false);
            return process.ExitCode == 0 ? null
                : $"exit code {process.ExitCode}: {(await error.ConfigureAwait(false) + await output.ConfigureAwait(false)).Trim()}";
        }
        catch (OperationCanceledException) when (!ct.IsCancellationRequested)
        {
            return "it did not answer --version in time";
        }
        catch (System.ComponentModel.Win32Exception e)
        {
            return e.Message;
        }
    }

    private static void TryDelete(string directory)
    {
        try
        {
            if (Directory.Exists(directory))
                Directory.Delete(directory, recursive: true);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            // Left for the next update, which deletes it first.
        }
    }

    // One folder's swap: every entry of the source replaces the one of the
    // same name, which moves to .update/previous (the last update's copy goes
    // first). Restore puts the previous entries back.
    private sealed class Swap(string directory)
    {
        private readonly string _previous = Path.Combine(directory, ".update", "previous");
        private readonly List<string> _installed = [];

        public void Install(string source, IReadOnlyCollection<string> skip)
        {
            if (Directory.Exists(_previous))
                Directory.Delete(_previous, recursive: true);
            Directory.CreateDirectory(_previous);
            foreach (var entry in Directory.EnumerateFileSystemEntries(source).ToList())
            {
                var name = Path.GetFileName(entry);
                if (skip.Contains(name, StringComparer.Ordinal))
                    continue;
                var target = Path.Combine(directory, name);
                if (Path.Exists(target))
                    Move(target, Path.Combine(_previous, name));
                _installed.Add(name);
                Move(entry, target);
            }
        }

        public void Restore()
        {
            foreach (var name in _installed)
            {
                var path = Path.Combine(directory, name);
                if (Directory.Exists(path))
                    Directory.Delete(path, recursive: true);
                else if (File.Exists(path))
                    File.Delete(path);
            }
            _installed.Clear();
            if (!Directory.Exists(_previous))
                return;
            foreach (var entry in Directory.EnumerateFileSystemEntries(_previous).ToList())
                Move(entry, Path.Combine(directory, Path.GetFileName(entry)));
        }

        private static void Move(string from, string to)
        {
            if (Directory.Exists(from))
                Directory.Move(from, to);
            else
                File.Move(from, to);
        }
    }

    [LoggerMessage(Level = LogLevel.Information,
        Message = "server update: server {ServerVersion} and core {CoreVersion} to {Tag}; restarting both")]
    private static partial void LogUpdating(ILogger logger, string serverVersion, string coreVersion, string tag);
}
