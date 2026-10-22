using System.Net;
using System.Security.Cryptography;
using System.Text;
using Multiroom.Server.Abstractions;

namespace Multiroom.Server.Providers;

// Artwork fetched once and kept in a directory, bounded by `capacity` bytes
// (rule 6): the phase 8 decision was a cache, not a proxy, so the outward API
// works offline for artwork it has shown before. A file's write time is when
// it was last served, and the least recently served files go first. The
// directory survives a restart.
//
// A file is named by a hash of its key, so a key carries no path, and by its
// image type, which is what it is served as. Only images are kept. Messages
// name `name`, never the source URL, which may carry credentials. Without an
// HttpClient it caches what its caller reads (the local provider's pictures).
#pragma warning disable CA1001 // Its SemaphoreSlim never has its wait handle read, so it holds nothing to release (as ZoneSession's).
internal sealed class ArtworkCache(string directory, long capacity, HttpClient? http = null)
#pragma warning restore CA1001
{
    // One image never takes more than this, whatever the capacity.
    public const long MaxImageBytes = 8 * 1024 * 1024;

    private static readonly TimeSpan FetchTimeout = TimeSpan.FromSeconds(10);

    internal static readonly Dictionary<string, string> Extensions = new(StringComparer.OrdinalIgnoreCase)
    {
        ["image/jpeg"] = ".jpg",
        ["image/png"] = ".png",
        ["image/gif"] = ".gif",
        ["image/webp"] = ".webp",
    };

    // One fetch or eviction at a time: artwork is small and rare, and one
    // writer keeps the bound exact.
    private readonly SemaphoreSlim _gate = new(1, 1);

    public long Capacity => capacity;

    // What the directory holds now: files and bytes.
    public (int Files, long Bytes) Held
    {
        get
        {
            var files = Files();
            return (files.Count, files.Sum(f => f.Length));
        }
    }

    public Task<Result<Artwork, ProviderError>> GetAsync(string key, Uri source, string name,
        CancellationToken cancellationToken) =>
        GetAsync(key, ct => FetchAsync(source, name, ct), cancellationToken);

    // `produce` answers the image when it is not kept yet. What it answers is
    // held to the same rules as a fetch: one of the kept types, at most
    // MaxImageBytes.
    public async Task<Result<Artwork, ProviderError>> GetAsync(string key,
        Func<CancellationToken, Task<Result<Artwork, ProviderError>>> produce, CancellationToken cancellationToken)
    {
        var stem = Stem(key);
        await _gate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            if (Find(stem) is { } cached)
            {
                var read = Read(cached);
                if (read is not null)
                    return read;
            }

            var fetched = await produce(cancellationToken).ConfigureAwait(false);
            if (!fetched.IsSuccess)
                return fetched.Failure;
            if (!Extensions.ContainsKey(fetched.Value.ContentType))
                return Result.Fail(ProviderError.Failed, $"artwork of {key}: not an image this server keeps ({fetched.Value.ContentType})");
            if (fetched.Value.Bytes.Length > Math.Min(MaxImageBytes, capacity))
                return Result.Fail(ProviderError.Failed, $"artwork of {key}: larger than {Math.Min(MaxImageBytes, capacity)} bytes");
            Store(stem, fetched.Value);
            return fetched.Value;
        }
        finally
        {
            _gate.Release();
        }
    }

    private static Artwork? Read(FileInfo file)
    {
        var type = Extensions.FirstOrDefault(e => e.Value.Equals(file.Extension, StringComparison.OrdinalIgnoreCase)).Key;
        if (type is null)
            return null;
        try
        {
            var bytes = File.ReadAllBytes(file.FullName);
            File.SetLastWriteTimeUtc(file.FullName, DateTime.UtcNow);
            return new Artwork(type, bytes);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            // Unreadable: fetched again, and the new file replaces it.
            return null;
        }
    }

    public Task<Result<Artwork, ProviderError>> FetchAsync(Uri source, string name,
        CancellationToken cancellationToken) =>
        FetchAsync(http!, source, name, Math.Min(MaxImageBytes, capacity), cancellationToken);

    // One image by URL, of a kept type and at most `limit` bytes: what the
    // artwork add-in fetches too.
    public static async Task<Result<Artwork, ProviderError>> FetchAsync(HttpClient http, Uri source, string name,
        long limit, CancellationToken cancellationToken)
    {
        using var bounded = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        bounded.CancelAfter(FetchTimeout);
        try
        {
            using var response = await http.GetAsync(source, HttpCompletionOption.ResponseHeadersRead, bounded.Token)
                .ConfigureAwait(false);
            if (!response.IsSuccessStatusCode)
            {
                return Result.Fail(
                    response.StatusCode is HttpStatusCode.NotFound or HttpStatusCode.Gone
                        ? ProviderError.NotFound
                        : ProviderError.Unavailable,
                    $"artwork of {name}: HTTP {(int)response.StatusCode}");
            }
            var type = response.Content.Headers.ContentType?.MediaType;
            if (type is null || !Extensions.ContainsKey(type))
                return Result.Fail(ProviderError.Failed, $"artwork of {name}: not an image this server keeps ({type ?? "no type"})");
            if (response.Content.Headers.ContentLength > limit)
                return Result.Fail(ProviderError.Failed, $"artwork of {name}: larger than {limit} bytes");

            var body = new MemoryStream();
            await using (var stream = await response.Content.ReadAsStreamAsync(bounded.Token).ConfigureAwait(false))
            {
                var buffer = new byte[64 * 1024];
                int read;
                while ((read = await stream.ReadAsync(buffer, bounded.Token).ConfigureAwait(false)) > 0)
                {
                    if (body.Length + read > limit)
                        return Result.Fail(ProviderError.Failed, $"artwork of {name}: larger than {limit} bytes");
                    body.Write(buffer, 0, read);
                }
            }
            return new Artwork(Extensions.Keys.First(k => k.Equals(type, StringComparison.OrdinalIgnoreCase)), body.ToArray());
        }
        catch (OperationCanceledException) when (!cancellationToken.IsCancellationRequested)
        {
            return Result.Fail(ProviderError.Unavailable,
                $"artwork of {name}: no answer within {FetchTimeout.TotalSeconds:0} s");
        }
        catch (HttpRequestException e)
        {
            // Its message names the host; the kind is enough.
            return Result.Fail(ProviderError.Unavailable, $"artwork of {name}: cannot be reached ({e.HttpRequestError})");
        }
        catch (IOException e)
        {
            return Result.Fail(ProviderError.Unavailable, $"artwork of {name}: {e.Message}");
        }
    }

    // Written whole under a temporary name, then evicted down to the bound,
    // oldest first; the file just written is kept. A write that fails is not
    // kept, and the artwork is still answered.
    private void Store(string stem, Artwork artwork)
    {
        var path = Path.Combine(directory, stem + Extensions[artwork.ContentType]);
        var temporary = path + ".tmp";
        try
        {
            Directory.CreateDirectory(directory);
            if (Find(stem) is { } stale)
                stale.Delete();
            File.WriteAllBytes(temporary, artwork.Bytes.Span);
            File.Move(temporary, path, overwrite: true);

            var files = Files().Where(f => !f.FullName.Equals(path, StringComparison.OrdinalIgnoreCase))
                .OrderBy(f => f.LastWriteTimeUtc).ToList();
            var held = files.Sum(f => f.Length) + artwork.Bytes.Length;
            foreach (var file in files)
            {
                if (held <= capacity)
                    break;
                file.Delete();
                held -= file.Length;
            }
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            try
            {
                File.Delete(temporary);
            }
            catch (Exception again) when (again is IOException or UnauthorizedAccessException)
            {
                // Left for the next Store of the same key to overwrite.
            }
        }
    }

    private FileInfo? Find(string stem) =>
        Files().FirstOrDefault(f => Path.GetFileNameWithoutExtension(f.Name).Equals(stem, StringComparison.Ordinal));

    private List<FileInfo> Files()
    {
        var info = new DirectoryInfo(directory);
        return info.Exists ? [.. info.EnumerateFiles().Where(f => Extensions.Values.Contains(f.Extension, StringComparer.OrdinalIgnoreCase))] : [];
    }

    private static string Stem(string key) =>
        Convert.ToHexStringLower(SHA256.HashData(Encoding.UTF8.GetBytes(key)), 0, 16);
}
