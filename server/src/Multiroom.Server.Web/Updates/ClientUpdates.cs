using System.Globalization;
using System.Net.Http.Headers;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using Microsoft.Extensions.Logging;
using Multiroom.Server.Abstractions;
using Multiroom.Server.Playback.Core;

namespace Multiroom.Server.Web.Updates;

public enum ClientUpdateError
{
    NotFound,
    Unavailable,
    Unreachable,
    Conflict,
    InvalidAnswer,
    Failed,
}

// Directory keeps the downloaded packages, one folder per release tag; a
// package dropped into it by hand (galdrun-client-v1.2.3-linux-armv7.tar.gz)
// is offered too, ahead of a release of the same version. ReleasesUrl is the
// GitHub API list of releases.
public sealed record ClientUpdatesOptions(string Directory, Uri ReleasesUrl);

// One room's client as an update sees it. Latest is the newest package for its
// target, Available whether that is newer and the client can take it. Trial:
// the client runs an update that has not confirmed yet.
public sealed record ClientUpdateView(
    string Client,
    string Version,
    string Target,
    bool Supported,
    string? Reason,
    string? Latest,
    bool Available,
    bool Trial,
    bool Updating);

// Client updates without the clients ever reaching the Internet: this server
// fetches the release package and uploads it to the client's control port
// (POST /api/update), which hands it to the updater shipped with the client
// (client/include/app/SelfUpdate.h). The client is found by its beacon,
// through the core, like its other settings.
public sealed partial class ClientUpdates : IDisposable
{
    private static readonly TimeSpan ReleasesFor = TimeSpan.FromHours(1);
    private static readonly TimeSpan FailedReleasesFor = TimeSpan.FromMinutes(5);
    // What a client answered, for the clients list; asked again sooner when
    // its beacon names another version.
    private static readonly TimeSpan ClientFor = TimeSpan.FromMinutes(10);
    private static readonly TimeSpan ClientPatience = TimeSpan.FromSeconds(5);

    private readonly ClientUpdatesOptions _options;
    private readonly CoreLink _link;
    private readonly ILogger _log;
    private readonly HttpClient _internet;
    private readonly HttpClient _clients;
    private readonly SemaphoreSlim _gate = new(1, 1);
    private IReadOnlyList<Package> _released = [];
    private DateTimeOffset _releasedUntil = DateTimeOffset.MinValue;
    private readonly Dictionary<string, Kept> _kept = [];

    // View is null for a client that could not say (too old, unreachable).
    private sealed record Kept(ClientUpdateView? View, string? BeaconVersion, DateTimeOffset Until);

    public ClientUpdates(ClientUpdatesOptions options, CoreLink link, ILogger<ClientUpdates> log)
    {
        _options = options;
        _link = link;
        _log = log;
        _internet = new HttpClient(new SocketsHttpHandler { ConnectTimeout = TimeSpan.FromSeconds(10) })
        {
            Timeout = TimeSpan.FromMinutes(5),
        };
        _internet.DefaultRequestHeaders.UserAgent.ParseAdd("Galdrun-Server");
        // The clients are on the LAN: never through a proxy.
        _clients = new HttpClient(new SocketsHttpHandler
        {
            UseProxy = false,
            AllowAutoRedirect = false,
            ConnectTimeout = TimeSpan.FromSeconds(5),
        })
        {
            Timeout = TimeSpan.FromMinutes(2),
        };
    }

    public void Dispose()
    {
        _internet.Dispose();
        _clients.Dispose();
        _gate.Dispose();
    }

    // Every present client that can say its version, for the clients list:
    // asked in parallel, each at most every ClientFor. One that does not
    // answer in time is left out.
    public async Task<Result<IReadOnlyList<ClientUpdateView>, ClientUpdateError>> ListAsync(CancellationToken ct)
    {
        var listed = await _link.SendAsync(new GetDiscoveredClients(), ct).ConfigureAwait(false);
        if (!listed.IsSuccess)
            return Result.Fail(ClientUpdateError.Unavailable, listed.Failure.ToString());
        await ReleasedAsync(ct).ConfigureAwait(false); // once, before the clients ask for it
        var views = await Task.WhenAll(listed.Value.Clients.Where(c => c.Present)
            .Select(c => KeptAsync(c, ct))).ConfigureAwait(false);
        return views.OfType<ClientUpdateView>().ToList();
    }

    private async Task<ClientUpdateView?> KeptAsync(DiscoveredClient client, CancellationToken ct)
    {
        lock (_kept)
        {
            if (_kept.TryGetValue(client.Client, out var kept) && kept.Until > DateTimeOffset.UtcNow &&
                kept.BeaconVersion == client.ClientVersion)
                return kept.View;
        }
        using var patience = CancellationTokenSource.CreateLinkedTokenSource(ct);
        patience.CancelAfter(ClientPatience);
        ClientUpdateView? view = null;
        try
        {
            var info = await CallAsync(new Target(client.Address, client.ControlPort), "getUpdate", patience.Token)
                .ConfigureAwait(false);
            if (info.IsSuccess)
                view = ViewOf(client.Client, info.Value,
                    await LatestAsync(info.Value.Target, patience.Token).ConfigureAwait(false), info.Value.Updating);
        }
        catch (OperationCanceledException) when (!ct.IsCancellationRequested)
        {
            // Too slow: left out, and not asked again until ClientFor passes.
        }
        lock (_kept)
            _kept[client.Client] = new Kept(view, client.ClientVersion, DateTimeOffset.UtcNow + ClientFor);
        return view;
    }

    private void Forget(string client)
    {
        lock (_kept)
            _kept.Remove(client);
    }

    public async Task<Result<ClientUpdateView, ClientUpdateError>> GetAsync(string client, CancellationToken ct)
    {
        Forget(client);
        var found = await FindAsync(client, ct).ConfigureAwait(false);
        if (!found.IsSuccess)
            return found.Failure;
        var info = await CallAsync(found.Value, "getUpdate", ct).ConfigureAwait(false);
        if (!info.IsSuccess)
            return info.Failure;
        var latest = await LatestAsync(info.Value.Target, ct).ConfigureAwait(false);
        return ViewOf(client, info.Value, latest, info.Value.Updating);
    }

    public async Task<Result<ClientUpdateView, ClientUpdateError>> UpdateAsync(string client, CancellationToken ct)
    {
        var found = await FindAsync(client, ct).ConfigureAwait(false);
        if (!found.IsSuccess)
            return found.Failure;
        var info = await CallAsync(found.Value, "getUpdate", ct).ConfigureAwait(false);
        if (!info.IsSuccess)
            return info.Failure;
        if (!info.Value.Supported)
            return Result.Fail(ClientUpdateError.Unavailable, $"{client}: {info.Value.Reason}");
        var latest = await LatestAsync(info.Value.Target, ct).ConfigureAwait(false);
        if (latest is null || !IsNewer(latest.Version, info.Value.Version))
            return Result.Fail(ClientUpdateError.Conflict,
                $"{client} runs {info.Value.Version}; there is no newer package for {info.Value.Target}");

        // One at a time: two rooms of the same target share the download.
        await _gate.WaitAsync(ct).ConfigureAwait(false);
        try
        {
            var file = await DownloadAsync(latest, ct).ConfigureAwait(false);
            if (!file.IsSuccess)
                return file.Failure;
            var sent = await UploadAsync(found.Value, file.Value.Path, file.Value.Sha256, ct).ConfigureAwait(false);
            if (!sent.IsSuccess)
                return sent.Failure;
        }
        finally
        {
            _gate.Release();
        }
        Forget(client);
        LogUpdating(_log, client, info.Value.Version, latest.Tag);
        return ViewOf(client, info.Value, latest, updating: true);
    }

    private static ClientUpdateView ViewOf(string client, ClientInfo info, Package? latest, bool updating) =>
        new(client, info.Version, info.Target, info.Supported, info.Reason, latest?.Tag,
            info.Supported && latest is not null && IsNewer(latest.Version, info.Version), info.Trial, updating);

    // --- the client --------------------------------------------------------

    private sealed record Target(string Address, int Port);

    private sealed record ClientInfo(string Version, string Target, bool Supported, string? Reason, bool Trial,
        bool Updating);

    private async Task<Result<Target, ClientUpdateError>> FindAsync(string client, CancellationToken ct)
    {
        var listed = await _link.SendAsync(new GetDiscoveredClients(), ct).ConfigureAwait(false);
        if (!listed.IsSuccess)
            return Result.Fail(ClientUpdateError.Unavailable, listed.Failure.ToString());
        var found = listed.Value.Clients.FirstOrDefault(c => c.Client == client && c.Present);
        return found is null
            ? Result.Fail(ClientUpdateError.NotFound, $"no recent beacon from {client}")
            : new Target(found.Address, found.ControlPort);
    }

    private static Uri ApiOf(Target target, string path) =>
        new(string.Create(CultureInfo.InvariantCulture, $"http://{target.Address}:{target.Port}{path}"));

    private async Task<Result<ClientInfo, ClientUpdateError>> CallAsync(Target target, string command,
        CancellationToken ct)
    {
        using var body = new StringContent($"{{\"command\":\"{command}\"}}", Encoding.UTF8, "application/json");
        string text;
        try
        {
            using var response = await _clients.PostAsync(ApiOf(target, "/api"), body, ct).ConfigureAwait(false);
            text = await response.Content.ReadAsStringAsync(ct).ConfigureAwait(false);
        }
        catch (Exception e) when (e is HttpRequestException or TaskCanceledException && !ct.IsCancellationRequested)
        {
            return Result.Fail(ClientUpdateError.Unreachable, $"{target.Address}:{target.Port}: {e.Message}");
        }
        return ParseAnswer(text, root => new ClientInfo(
            String(root, "version") ?? "unknown",
            String(root, "target") ?? "",
            root.TryGetProperty("supported", out var s) && s.ValueKind == JsonValueKind.True,
            String(root, "reason"),
            root.TryGetProperty("trial", out var t) && t.ValueKind == JsonValueKind.True,
            root.TryGetProperty("updating", out var u) && u.ValueKind == JsonValueKind.True));
    }

    private async Task<Result<bool, ClientUpdateError>> UploadAsync(Target target, string path, string sha256,
        CancellationToken ct)
    {
        var stream = File.OpenRead(path);
        await using (stream.ConfigureAwait(false))
        {
            using var body = new StreamContent(stream);
            body.Headers.ContentType = new MediaTypeHeaderValue("application/octet-stream");
            body.Headers.Add("X-Galdrun-Sha256", sha256);
            try
            {
                using var response = await _clients.PostAsync(ApiOf(target, "/api/update"), body, ct)
                    .ConfigureAwait(false);
                var text = await response.Content.ReadAsStringAsync(ct).ConfigureAwait(false);
                // Refused before the update command: a plain-text reason.
                if (!response.IsSuccessStatusCode)
                    return Result.Fail(response.StatusCode == System.Net.HttpStatusCode.ServiceUnavailable
                        ? ClientUpdateError.Unavailable : ClientUpdateError.Failed, text.Trim());
                return ParseAnswer(text, _ => true);
            }
            catch (Exception e) when (e is HttpRequestException or TaskCanceledException && !ct.IsCancellationRequested)
            {
                return Result.Fail(ClientUpdateError.Unreachable, $"{target.Address}:{target.Port}: {e.Message}");
            }
        }
    }

    // A control response: {"ok":true,...} or {"ok":false,"error":...,"message":...}.
    private static Result<T, ClientUpdateError> ParseAnswer<T>(string text, Func<JsonElement, T> read)
    {
        try
        {
            using var document = JsonDocument.Parse(text);
            var root = document.RootElement;
            if (root.ValueKind != JsonValueKind.Object || !root.TryGetProperty("ok", out var ok))
                return Result.Fail(ClientUpdateError.InvalidAnswer, "the client answered no control response");
            if (ok.ValueKind == JsonValueKind.True)
                return read(root);
            var code = String(root, "error");
            return Result.Fail(code is "unavailable" or "unknownCommand"
                    ? ClientUpdateError.Unavailable : ClientUpdateError.Failed,
                code == "unknownCommand" ? "this client is too old to update itself; update it once by hand"
                    : String(root, "message") ?? code ?? "refused");
        }
        catch (JsonException)
        {
            return Result.Fail(ClientUpdateError.InvalidAnswer, "the client answered no control response");
        }
    }

    private static string? String(JsonElement root, string name) =>
        root.TryGetProperty(name, out var value) && value.ValueKind == JsonValueKind.String ? value.GetString() : null;

    // --- the packages ------------------------------------------------------

    // LocalPath for a package dropped into the directory, Download (and the
    // digest GitHub publishes) for a release asset.
    private sealed record Package(Version Version, string Tag, string Target, string Name, string? LocalPath,
        Uri? Download, string? Sha256);

    [GeneratedRegex(@"^galdrun-client-(v?(\d+\.\d+\.\d+))-(.+?)\.(tar\.gz|zip)$")]
    private static partial Regex PackageName();

    // "v1.2.3", "1.2.3" or "1.2.3-dev": the numbers only.
    private static Version? VersionOf(string text)
    {
        var plain = text.TrimStart('v');
        var end = plain.IndexOfAny(['-', '+']);
        return Version.TryParse(end < 0 ? plain : plain[..end], out var version) ? version : null;
    }

    private static bool IsNewer(Version candidate, string current) =>
        VersionOf(current) is not { } running || candidate > running;

    private async Task<Package?> LatestAsync(string target, CancellationToken ct)
    {
        if (target.Length == 0)
            return null;
        var released = await ReleasedAsync(ct).ConfigureAwait(false);
        return Local().Concat(released)
            .Where(p => p.Target == target)
            // Local first among equal versions: nothing to download.
            .OrderByDescending(p => p.Version).ThenBy(p => p.LocalPath is null)
            .FirstOrDefault();
    }

    private IEnumerable<Package> Local()
    {
        if (!Directory.Exists(_options.Directory))
            yield break;
        foreach (var path in Directory.EnumerateFiles(_options.Directory))
        {
            var name = Path.GetFileName(path);
            var match = PackageName().Match(name);
            if (match.Success && VersionOf(match.Groups[2].Value) is { } version)
                yield return new Package(version, match.Groups[1].Value, match.Groups[3].Value, name, path, null, null);
        }
    }

    private async Task<IReadOnlyList<Package>> ReleasedAsync(CancellationToken ct)
    {
        if (DateTimeOffset.UtcNow < _releasedUntil)
            return _released;
        var packages = new List<Package>();
        try
        {
            using var request = new HttpRequestMessage(HttpMethod.Get, _options.ReleasesUrl);
            request.Headers.Accept.ParseAdd("application/vnd.github+json");
            using var response = await _internet.SendAsync(request, ct).ConfigureAwait(false);
            response.EnsureSuccessStatusCode();
            var stream = await response.Content.ReadAsStreamAsync(ct).ConfigureAwait(false);
            await using (stream.ConfigureAwait(false))
            {
                using var document = await JsonDocument.ParseAsync(stream, cancellationToken: ct).ConfigureAwait(false);
                foreach (var release in document.RootElement.EnumerateArray())
                {
                    if (release.TryGetProperty("draft", out var draft) && draft.ValueKind == JsonValueKind.True)
                        continue;
                    if (!release.TryGetProperty("assets", out var assets) || assets.ValueKind != JsonValueKind.Array)
                        continue;
                    foreach (var asset in assets.EnumerateArray())
                    {
                        var name = String(asset, "name") ?? "";
                        var match = PackageName().Match(name);
                        if (!match.Success || VersionOf(match.Groups[2].Value) is not { } version ||
                            !Uri.TryCreate(String(asset, "browser_download_url"), UriKind.Absolute, out var url))
                            continue;
                        var digest = String(asset, "digest");
                        packages.Add(new Package(version, match.Groups[1].Value, match.Groups[3].Value, name, null, url,
                            digest is not null && digest.StartsWith("sha256:", StringComparison.Ordinal)
                                ? digest["sha256:".Length..].ToLowerInvariant() : null));
                    }
                }
            }
            _released = packages;
            _releasedUntil = DateTimeOffset.UtcNow + ReleasesFor;
        }
        catch (Exception e) when (e is HttpRequestException or JsonException or InvalidOperationException
                                      or TaskCanceledException && !ct.IsCancellationRequested)
        {
            // The last list stays; a server without Internet still offers
            // the packages in its directory.
            LogReleasesFailed(_log, _options.ReleasesUrl, e.Message);
            _releasedUntil = DateTimeOffset.UtcNow + FailedReleasesFor;
        }
        return _released;
    }

    private sealed record Downloaded(string Path, string Sha256);

    private async Task<Result<Downloaded, ClientUpdateError>> DownloadAsync(Package package, CancellationToken ct)
    {
        var path = package.LocalPath ?? Path.Combine(_options.Directory, package.Tag, package.Name);
        try
        {
            if (package.LocalPath is null && !File.Exists(path))
            {
                Directory.CreateDirectory(Path.GetDirectoryName(path)!);
                var part = path + ".part";
                using (var response = await _internet.GetAsync(package.Download!, HttpCompletionOption.ResponseHeadersRead, ct)
                           .ConfigureAwait(false))
                {
                    response.EnsureSuccessStatusCode();
                    var file = File.Create(part);
                    await using (file.ConfigureAwait(false))
                        await response.Content.CopyToAsync(file, ct).ConfigureAwait(false);
                }
                var sha = await HashAsync(part, ct).ConfigureAwait(false);
                if (package.Sha256 is not null && sha != package.Sha256)
                {
                    File.Delete(part);
                    return Result.Fail(ClientUpdateError.Failed,
                        $"{package.Name}: SHA-256 {sha} is not the published {package.Sha256}");
                }
                File.Move(part, path, overwrite: true);
                LogDownloaded(_log, package.Name);
            }
            var hash = await HashAsync(path, ct).ConfigureAwait(false);
            if (package.LocalPath is null && package.Sha256 is not null && hash != package.Sha256)
            {
                // A damaged copy from before: fetched again next time.
                File.Delete(path);
                return Result.Fail(ClientUpdateError.Failed, $"{package.Name}: the kept copy is damaged; try again");
            }
            return new Downloaded(path, hash);
        }
        catch (Exception e) when (e is HttpRequestException or IOException or UnauthorizedAccessException
                                      or TaskCanceledException && !ct.IsCancellationRequested)
        {
            return Result.Fail(ClientUpdateError.Unavailable, $"{package.Name}: {e.Message}");
        }
    }

    private static async Task<string> HashAsync(string path, CancellationToken ct)
    {
        var stream = File.OpenRead(path);
        await using (stream.ConfigureAwait(false))
            return Convert.ToHexStringLower(await SHA256.HashDataAsync(stream, ct).ConfigureAwait(false));
    }

    [LoggerMessage(Level = LogLevel.Information, Message = "client update: {Client} from {Version} to {Tag}")]
    private static partial void LogUpdating(ILogger logger, string client, string version, string tag);

    [LoggerMessage(Level = LogLevel.Information, Message = "client update: downloaded {Name}")]
    private static partial void LogDownloaded(ILogger logger, string name);

    [LoggerMessage(Level = LogLevel.Warning, Message = "client update: cannot list the releases at {Url}: {Error}")]
    private static partial void LogReleasesFailed(ILogger logger, Uri url, string error);
}
