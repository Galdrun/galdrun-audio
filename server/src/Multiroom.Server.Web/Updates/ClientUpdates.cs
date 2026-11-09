using System.Globalization;
using System.Net.Http.Headers;
using System.Text;
using System.Text.Json;
using Microsoft.Extensions.Logging;
using Multiroom.Server.Abstractions;
using Multiroom.Server.Playback.Core;

namespace Multiroom.Server.Web.Updates;

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
    private readonly ReleasePackages _packages;
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
        _packages = new ReleasePackages("client", options.Directory, options.ReleasesUrl, _internet, log);
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
    public async Task<Result<IReadOnlyList<ClientUpdateView>, UpdateError>> ListAsync(CancellationToken ct)
    {
        var listed = await _link.SendAsync(new GetDiscoveredClients(), ct).ConfigureAwait(false);
        if (!listed.IsSuccess)
            return Result.Fail(UpdateError.Unavailable, listed.Failure.ToString());
        await _packages.ReleasedAsync(ct).ConfigureAwait(false); // once, before the clients ask for it
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

    public async Task<Result<ClientUpdateView, UpdateError>> GetAsync(string client, CancellationToken ct)
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

    public async Task<Result<ClientUpdateView, UpdateError>> UpdateAsync(string client, CancellationToken ct)
    {
        var found = await FindAsync(client, ct).ConfigureAwait(false);
        if (!found.IsSuccess)
            return found.Failure;
        var info = await CallAsync(found.Value, "getUpdate", ct).ConfigureAwait(false);
        if (!info.IsSuccess)
            return info.Failure;
        if (!info.Value.Supported)
            return Result.Fail(UpdateError.Unavailable, $"{client}: {info.Value.Reason}");
        var latest = await LatestAsync(info.Value.Target, ct).ConfigureAwait(false);
        if (latest is null || !ReleasePackages.IsNewer(latest.Version, info.Value.Version))
            return Result.Fail(UpdateError.Conflict,
                $"{client} runs {info.Value.Version}; there is no newer package for {info.Value.Target}");

        // One at a time: two rooms of the same target share the download.
        await _gate.WaitAsync(ct).ConfigureAwait(false);
        try
        {
            var file = await _packages.DownloadAsync(latest, ct).ConfigureAwait(false);
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

    private static ClientUpdateView ViewOf(string client, ClientInfo info, ReleasePackage? latest, bool updating) =>
        new(client, info.Version, info.Target, info.Supported, info.Reason, latest?.Tag,
            info.Supported && latest is not null && ReleasePackages.IsNewer(latest.Version, info.Version), info.Trial, updating);

    // --- the client --------------------------------------------------------

    private sealed record Target(string Address, int Port);

    private sealed record ClientInfo(string Version, string Target, bool Supported, string? Reason, bool Trial,
        bool Updating);

    private async Task<Result<Target, UpdateError>> FindAsync(string client, CancellationToken ct)
    {
        var listed = await _link.SendAsync(new GetDiscoveredClients(), ct).ConfigureAwait(false);
        if (!listed.IsSuccess)
            return Result.Fail(UpdateError.Unavailable, listed.Failure.ToString());
        var found = listed.Value.Clients.FirstOrDefault(c => c.Client == client && c.Present);
        return found is null
            ? Result.Fail(UpdateError.NotFound, $"no recent beacon from {client}")
            : new Target(found.Address, found.ControlPort);
    }

    private static Uri ApiOf(Target target, string path) =>
        new(string.Create(CultureInfo.InvariantCulture, $"http://{target.Address}:{target.Port}{path}"));

    private async Task<Result<ClientInfo, UpdateError>> CallAsync(Target target, string command,
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
            return Result.Fail(UpdateError.Unreachable, $"{target.Address}:{target.Port}: {e.Message}");
        }
        return ParseAnswer(text, root => new ClientInfo(
            String(root, "version") ?? "unknown",
            String(root, "target") ?? "",
            root.TryGetProperty("supported", out var s) && s.ValueKind == JsonValueKind.True,
            String(root, "reason"),
            root.TryGetProperty("trial", out var t) && t.ValueKind == JsonValueKind.True,
            root.TryGetProperty("updating", out var u) && u.ValueKind == JsonValueKind.True));
    }

    private async Task<Result<bool, UpdateError>> UploadAsync(Target target, string path, string sha256,
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
                        ? UpdateError.Unavailable : UpdateError.Failed, text.Trim());
                return ParseAnswer(text, _ => true);
            }
            catch (Exception e) when (e is HttpRequestException or TaskCanceledException && !ct.IsCancellationRequested)
            {
                return Result.Fail(UpdateError.Unreachable, $"{target.Address}:{target.Port}: {e.Message}");
            }
        }
    }

    // A control response: {"ok":true,...} or {"ok":false,"error":...,"message":...}.
    private static Result<T, UpdateError> ParseAnswer<T>(string text, Func<JsonElement, T> read)
    {
        try
        {
            using var document = JsonDocument.Parse(text);
            var root = document.RootElement;
            if (root.ValueKind != JsonValueKind.Object || !root.TryGetProperty("ok", out var ok))
                return Result.Fail(UpdateError.InvalidAnswer, "the client answered no control response");
            if (ok.ValueKind == JsonValueKind.True)
                return read(root);
            var code = String(root, "error");
            return Result.Fail(code is "unavailable" or "unknownCommand"
                    ? UpdateError.Unavailable : UpdateError.Failed,
                code == "unknownCommand" ? "this client is too old to update itself; update it once by hand"
                    : String(root, "message") ?? code ?? "refused");
        }
        catch (JsonException)
        {
            return Result.Fail(UpdateError.InvalidAnswer, "the client answered no control response");
        }
    }

    private static string? String(JsonElement root, string name) =>
        root.TryGetProperty(name, out var value) && value.ValueKind == JsonValueKind.String ? value.GetString() : null;

    private Task<ReleasePackage?> LatestAsync(string target, CancellationToken ct) =>
        _packages.LatestAsync(target, ct);

    [LoggerMessage(Level = LogLevel.Information, Message = "client update: {Client} from {Version} to {Tag}")]
    private static partial void LogUpdating(ILogger logger, string client, string version, string tag);
}
