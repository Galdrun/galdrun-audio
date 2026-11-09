using System.Security.Cryptography;
using System.Text.Json;
using System.Text.RegularExpressions;
using Microsoft.Extensions.Logging;
using Multiroom.Server.Abstractions;

namespace Multiroom.Server.Web.Updates;

// For client and server updates alike.
public enum UpdateError
{
    NotFound,
    Unavailable,
    Unreachable,
    Conflict,
    InvalidAnswer,
    Failed,
}

// One release archive: LocalPath for a package dropped into the directory,
// Download (and the digest GitHub publishes) for a release asset.
internal sealed record ReleasePackage(Version Version, string Tag, string Target, string Name, string? LocalPath,
    Uri? Download, string? Sha256);

internal sealed record DownloadedPackage(string Path, string Sha256);

// The release archives of one kind (galdrun-<kind>-v1.2.3-<target>.tar.gz or
// .zip): the GitHub releases (listed at most hourly) plus the ones dropped into
// the directory by hand, which win over a release of the same version. A
// downloaded archive is kept in the directory, one folder per release tag.
internal sealed partial class ReleasePackages
{
    private static readonly TimeSpan ReleasesFor = TimeSpan.FromHours(1);
    private static readonly TimeSpan FailedReleasesFor = TimeSpan.FromMinutes(5);

    private readonly string _directory;
    private readonly Uri _releasesUrl;
    private readonly HttpClient _internet;
    private readonly ILogger _log;
    private readonly Regex _name;
    private IReadOnlyList<ReleasePackage> _released = [];
    private DateTimeOffset _releasedUntil = DateTimeOffset.MinValue;

    public ReleasePackages(string kind, string directory, Uri releasesUrl, HttpClient internet, ILogger log)
    {
        _directory = directory;
        _releasesUrl = releasesUrl;
        _internet = internet;
        _log = log;
        _name = new Regex($@"^galdrun-{Regex.Escape(kind)}-(v?(\d+\.\d+\.\d+))-(.+?)\.(tar\.gz|zip)$",
            RegexOptions.CultureInvariant);
    }

    // "v1.2.3", "1.2.3", "1.2.3-dev" or "1.2.3+abc": the numbers only.
    public static Version? VersionOf(string? text)
    {
        if (text is null)
            return null;
        var plain = text.TrimStart('v');
        var end = plain.IndexOfAny(['-', '+']);
        return Version.TryParse(end < 0 ? plain : plain[..end], out var version) ? version : null;
    }

    // An unknown current version is older than any package.
    public static bool IsNewer(Version candidate, string? current) =>
        VersionOf(current) is not { } running || candidate > running;

    public async Task<ReleasePackage?> LatestAsync(string target, CancellationToken ct)
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

    private IEnumerable<ReleasePackage> Local()
    {
        if (!Directory.Exists(_directory))
            yield break;
        foreach (var path in Directory.EnumerateFiles(_directory))
        {
            var name = Path.GetFileName(path);
            var match = _name.Match(name);
            if (match.Success && VersionOf(match.Groups[2].Value) is { } version)
                yield return new ReleasePackage(version, match.Groups[1].Value, match.Groups[3].Value, name, path,
                    null, null);
        }
    }

    public async Task<IReadOnlyList<ReleasePackage>> ReleasedAsync(CancellationToken ct)
    {
        if (DateTimeOffset.UtcNow < _releasedUntil)
            return _released;
        var packages = new List<ReleasePackage>();
        try
        {
            using var request = new HttpRequestMessage(HttpMethod.Get, _releasesUrl);
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
                        var match = _name.Match(name);
                        if (!match.Success || VersionOf(match.Groups[2].Value) is not { } version ||
                            !Uri.TryCreate(String(asset, "browser_download_url"), UriKind.Absolute, out var url))
                            continue;
                        var digest = String(asset, "digest");
                        packages.Add(new ReleasePackage(version, match.Groups[1].Value, match.Groups[3].Value, name,
                            null, url,
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
            LogReleasesFailed(_log, _releasesUrl, e.Message);
            _releasedUntil = DateTimeOffset.UtcNow + FailedReleasesFor;
        }
        return _released;
    }

    // The archive on disk with its SHA-256, downloaded first when needed and
    // checked against the digest GitHub published. Callers serialize this.
    public async Task<Result<DownloadedPackage, UpdateError>> DownloadAsync(ReleasePackage package, CancellationToken ct)
    {
        var path = package.LocalPath ?? Path.Combine(_directory, package.Tag, package.Name);
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
                    return Result.Fail(UpdateError.Failed, $"{package.Name}: SHA-256 {sha} is not the published {package.Sha256}");
                }
                File.Move(part, path, overwrite: true);
                LogDownloaded(_log, package.Name);
            }
            var hash = await HashAsync(path, ct).ConfigureAwait(false);
            if (package.LocalPath is null && package.Sha256 is not null && hash != package.Sha256)
            {
                // A damaged copy from before: fetched again next time.
                File.Delete(path);
                return Result.Fail(UpdateError.Failed, $"{package.Name}: the kept copy is damaged; try again");
            }
            return new DownloadedPackage(path, hash);
        }
        catch (Exception e) when (e is HttpRequestException or IOException or UnauthorizedAccessException
                                      or TaskCanceledException && !ct.IsCancellationRequested)
        {
            return Result.Fail(UpdateError.Unavailable, $"{package.Name}: {e.Message}");
        }
    }

    private static async Task<string> HashAsync(string path, CancellationToken ct)
    {
        var stream = File.OpenRead(path);
        await using (stream.ConfigureAwait(false))
            return Convert.ToHexStringLower(await SHA256.HashDataAsync(stream, ct).ConfigureAwait(false));
    }

    private static string? String(JsonElement root, string name) =>
        root.TryGetProperty(name, out var value) && value.ValueKind == JsonValueKind.String ? value.GetString() : null;

    [LoggerMessage(Level = LogLevel.Information, Message = "update: downloaded {Name}")]
    private static partial void LogDownloaded(ILogger logger, string name);

    [LoggerMessage(Level = LogLevel.Warning, Message = "update: cannot list the releases at {Url}: {Error}")]
    private static partial void LogReleasesFailed(ILogger logger, Uri url, string error);
}
