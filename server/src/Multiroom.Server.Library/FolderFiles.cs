using Multiroom.Server.Abstractions;
using Multiroom.Server.Library.Smb;

namespace Multiroom.Server.Library;

// A file as a pass sees it. For an smb folder, Path and Directory are URIs
// (smb://nas/music/a.flac); otherwise full paths.
public sealed record LibraryFile(string Path, string Directory, long Length, DateTime LastWriteUtc)
{
    public string Extension => System.IO.Path.GetExtension(Path);
}

// The one seam for file access that the indexer and the local provider share:
// is the folder there, enumerate it, does a file exist, open a reader that
// can seek, and resolve a file at stage time. local and network folders keep
// System.IO; smb folders go through the SMB client. Nothing more general.
public abstract class FolderFiles
{
    // A network folder, and a NAS, get this long to answer whether they are
    // there, so a dead one holds the indexer's thread only this long.
    public static readonly TimeSpan ReachTimeout = TimeSpan.FromSeconds(5);

    // The folder's path: a full path, or the smb folder's URI. It is what the
    // library database names the folder by.
    public abstract string Root { get; }

    // Null when the folder can be read now; otherwise why not, Unreachable or
    // AuthFailed.
    public abstract Failure<LibraryError>? Reach();

    // Every file below the folder. An IOException means the folder went away
    // mid-way; what cannot be read below it is left out.
    public abstract IEnumerable<LibraryFile> Enumerate();

    // Whether a file of this folder is still there. An error is taken as gone.
    public abstract bool Exists(string path);

    // A seekable reader, for tags and the fingerprint. Throws IOException or
    // UnauthorizedAccessException.
    public abstract Stream OpenRead(string path);

    // Whether the path names a file below this folder: no escape by "..".
    public abstract bool Contains(string path);

    // The file as the media endpoint serves it, at stage time (the contract,
    // Pace and time): what can be served at once when the core's GET comes.
    public abstract Task<Result<ResolvedStream, ProviderError>> ResolveAsync(string path,
        CancellationToken cancellationToken);

    // The picture of a file (phase 14): the one embedded in it, the front
    // cover first, else a cover or folder image beside it. Synchronous: SMB
    // is, so a caller runs it off any thread that must not wait.
    public Result<Artwork, ProviderError> ReadPicture(string path)
    {
        try
        {
            using var tagged = TagLib.File.Create(new TagSource(path, () => OpenRead(path)));
            var pictures = tagged.Tag.Pictures;
            var picture = pictures.FirstOrDefault(p => p.Type == TagLib.PictureType.FrontCover) ?? pictures.FirstOrDefault();
            if (picture?.Data?.Data is { Length: > 0 } embedded && ImageType(embedded) is { } type)
                return new Artwork(type, embedded);
        }
#pragma warning disable CA1031 // A file's bytes are untrusted: whatever TagLib throws on them only means no embedded picture.
        catch (Exception)
#pragma warning restore CA1031
        {
        }

        var separator = SmbLocation.IsUri(path) ? '/' : System.IO.Path.DirectorySeparatorChar;
        var directory = path[..(path.LastIndexOf(separator) + 1)];
        foreach (var name in BesideNames)
        {
            try
            {
                using var stream = OpenRead(directory + name);
                if (stream.Length > MaxPictureBytes)
                    continue;
                var bytes = new byte[stream.Length];
                stream.ReadExactly(bytes);
                if (ImageType(bytes) is { } type)
                    return new Artwork(type, bytes);
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException)
            {
                // Not there, or not readable: the next name.
            }
        }
        return Result.Fail(ProviderError.NotFound, $"{path} has no picture, in it or beside it");
    }

    // An image beside a file, as players name it.
    private static readonly string[] BesideNames = ["cover.jpg", "folder.jpg", "cover.png", "folder.png", "front.jpg"];

    private const long MaxPictureBytes = 8 * 1024 * 1024;

    // By its first bytes: a tag's MIME type is often missing or wrong.
    internal static string? ImageType(ReadOnlySpan<byte> bytes) => bytes switch
    {
        [0xFF, 0xD8, 0xFF, ..] => "image/jpeg",
        [0x89, 0x50, 0x4E, 0x47, ..] => "image/png",
        [0x47, 0x49, 0x46, 0x38, ..] => "image/gif",
        [0x52, 0x49, 0x46, 0x46, _, _, _, _, 0x57, 0x45, 0x42, 0x50, ..] => "image/webp",
        _ => null,
    };
}

// A local folder (watched) or a share the OS mounts (polled), through System.IO.
internal sealed class DiskFiles(string root, bool network) : FolderFiles
{
    public override string Root => root;

    public override Failure<LibraryError>? Reach()
    {
        var exists = network
            ? Task.Run(() => Directory.Exists(root)) is var probe && probe.Wait(ReachTimeout) && probe.Result
            : Directory.Exists(root);
        return exists ? null : Result.Fail(LibraryError.Unreachable, $"{root} does not exist or cannot be reached");
    }

    public override IEnumerable<LibraryFile> Enumerate() =>
        new DirectoryInfo(root).EnumerateFiles("*",
                new EnumerationOptions { RecurseSubdirectories = true, IgnoreInaccessible = true })
            .Select(f => new LibraryFile(f.FullName, f.DirectoryName!, f.Length, f.LastWriteTimeUtc));

    public override bool Exists(string path) => File.Exists(path);

    public override Stream OpenRead(string path) =>
        new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete, bufferSize: 0);

    // GetRelativePath compares as the file system does (case-insensitively on
    // Windows) and normalizes ".." segments first.
    public override bool Contains(string path)
    {
        if (!Path.IsPathFullyQualified(path))
            return false;
        var relative = Path.GetRelativePath(root, path);
        return relative != "." && !Path.IsPathRooted(relative) && relative != ".." &&
            !relative.StartsWith(".." + Path.DirectorySeparatorChar, StringComparison.Ordinal);
    }

    public override Task<Result<ResolvedStream, ProviderError>> ResolveAsync(string path,
        CancellationToken cancellationToken) => Task.FromResult(ResolvedStream.FromFile(path));
}

// A directory on a NAS, read over SMB with the folder's own login (phase 13).
internal sealed class SmbFiles(SmbLocation location, SmbSessions sessions) : FolderFiles
{
    // Resolving wakes the NAS: a disk that spins up takes seconds, longer than
    // the core's stall timeout, so the first and the last bytes are read
    // before the URL is staged. Past this bound the file is Unavailable.
    public static readonly TimeSpan ResolveTimeout = TimeSpan.FromSeconds(15);

    // What resolving reads: the head the demuxer probes first, and the tail
    // where MP3 keeps its ID3v1 tag.
    private const int Head = 64 * 1024;
    private const int Tail = 128;

    public SmbLocation Location => location;

    public override string Root => location.Uri;

    public override Failure<LibraryError>? Reach()
    {
        var attempt = Task.Run<Failure<LibraryError>?>(() =>
        {
            var leased = sessions.Lease(location.Login);
            if (!leased.IsSuccess)
                return leased.Failure;
            using var lease = leased.Value;
            var root = lease.Stat(location.Share, location.PathOf(location.Uri)!);
            if (!root.IsSuccess)
                return root.Failure;
            return root.Value.IsDirectory
                ? (Failure<LibraryError>?)null
                : Result.Fail(LibraryError.NotFound, $"{location.Uri} is not a directory");
        });
        if (!attempt.Wait(ReachTimeout))
            return Result.Fail(LibraryError.Unreachable, $"{location.Uri} did not answer within {ReachTimeout.TotalSeconds:0} s");
        return attempt.Result is { Code: LibraryError.NotFound } missing
            ? Result.Fail(LibraryError.Unreachable, missing.Detail)
            : attempt.Result;
    }

    // Directory by directory, on one lease. A directory that vanished or may
    // not be read is left out; a server that stops answering ends the pass.
    public override IEnumerable<LibraryFile> Enumerate()
    {
        var leased = sessions.Lease(location.Login);
        if (!leased.IsSuccess)
            throw new IOException(leased.Failure.Detail);
        using var lease = leased.Value;
        var pending = new Queue<string>([location.PathOf(location.Uri)!]);
        while (pending.TryDequeue(out var directory))
        {
            var listed = lease.List(location.Share, directory);
            if (!listed.IsSuccess)
            {
                if (listed.Failure.Code is LibraryError.NotFound or LibraryError.AuthFailed)
                    continue;
                throw new IOException(listed.Failure.Detail);
            }
            foreach (var entry in listed.Value.OrderBy(e => e.Name, StringComparer.Ordinal))
            {
                var path = directory.Length == 0 ? entry.Name : directory + "\\" + entry.Name;
                if (entry.IsDirectory)
                    pending.Enqueue(path);
                else
                    yield return new LibraryFile(location.UriOf(path), location.UriOf(directory), entry.Length, entry.LastWriteUtc);
            }
        }
    }

    public override bool Exists(string path)
    {
        if (location.PathOf(path) is not { } file)
            return false;
        var leased = sessions.Lease(location.Login);
        if (!leased.IsSuccess)
            return false;
        using var lease = leased.Value;
        return lease.Stat(location.Share, file) is { IsSuccess: true, Value.IsDirectory: false };
    }

    // Buffered: TagLib reads a few bytes at a time, and each read of the
    // share is a request.
    public override Stream OpenRead(string path)
    {
        var opened = Open(path);
        if (!opened.IsSuccess)
        {
            throw opened.Failure.Code == LibraryError.NotFound
                ? new FileNotFoundException(opened.Failure.Detail)
                : new IOException(opened.Failure.Detail);
        }
        return new BufferedStream(opened.Value, Head);
    }

    public override bool Contains(string path) => location.PathOf(path) is { Length: > 0 };

    // Size and write time are taken here; a GET that finds either changed is
    // Gone, as for a file on disk. The lease is held until the stream is
    // released, so the session to the NAS stays open between staging and the
    // core's GETs.
    public override async Task<Result<ResolvedStream, ProviderError>> ResolveAsync(string path,
        CancellationToken cancellationToken)
    {
        var attempt = Task.Run(() => Warm(path), CancellationToken.None);
        try
        {
            var warmed = await attempt.WaitAsync(ResolveTimeout, cancellationToken).ConfigureAwait(false);
            if (!warmed.IsSuccess)
            {
                return Result.Fail(warmed.Failure.Code == LibraryError.NotFound ? ProviderError.NotFound : ProviderError.Unavailable,
                    warmed.Failure.Detail);
            }
            var (length, written, lease) = warmed.Value;
            return ResolvedStream.KnownLength(length, ResolvedStream.ContentTypeOf(Path.GetExtension(path)),
                () => OpenUnchanged(path, written), lease.Dispose);
        }
        catch (Exception e) when (e is TimeoutException or OperationCanceledException)
        {
            // Let the session go once the NAS answers after all.
            _ = attempt.ContinueWith(t =>
            {
                if (t.Result.IsSuccess)
                    t.Result.Value.Lease.Dispose();
            }, TaskScheduler.Default);
            if (e is OperationCanceledException)
                throw;
            return Result.Fail(ProviderError.Unavailable,
                $"{path} could not be read within {ResolveTimeout.TotalSeconds:0} s");
        }
    }

    private Result<(long Length, DateTime Written, SmbLease Lease), LibraryError> Warm(string path)
    {
        if (location.PathOf(path) is not { Length: > 0 } file)
            return Result.Fail(LibraryError.NotFound, $"{path} is not in {location.Uri}");
        var leased = sessions.Lease(location.Login);
        if (!leased.IsSuccess)
            return leased.Failure;
        var lease = leased.Value;
        var opened = lease.Open(location.Share, file);
        if (!opened.IsSuccess)
        {
            lease.Dispose();
            return opened.Failure;
        }
        var handle = opened.Value;
        try
        {
            var buffer = new byte[Head];
            var head = lease.Read(handle, 0, buffer.AsSpan(0, (int)Math.Min(Head, handle.Length)));
            var tail = head.IsSuccess && handle.Length > Tail
                ? lease.Read(handle, handle.Length - Tail, buffer.AsSpan(0, Tail))
                : head;
            if (!tail.IsSuccess)
            {
                lease.Dispose();
                return tail.Failure;
            }
            return (handle.Length, handle.LastWriteUtc, lease);
        }
        finally
        {
            lease.Close(handle);
        }
    }

    private Result<SmbReadStream, LibraryError> Open(string path)
    {
        if (location.PathOf(path) is not { Length: > 0 } file)
            return Result.Fail(LibraryError.NotFound, $"{path} is not in {location.Uri}");
        var leased = sessions.Lease(location.Login);
        if (!leased.IsSuccess)
            return leased.Failure;
        var opened = leased.Value.Open(location.Share, file);
        if (!opened.IsSuccess)
        {
            leased.Value.Dispose();
            return opened.Failure;
        }
        return new SmbReadStream(leased.Value, opened.Value);
    }

    private Result<Stream, ProviderError> OpenUnchanged(string path, DateTime written)
    {
        var opened = Open(path);
        if (!opened.IsSuccess)
        {
            return Result.Fail(opened.Failure.Code == LibraryError.NotFound ? ProviderError.Gone : ProviderError.Failed,
                opened.Failure.Detail);
        }
        if (opened.Value.LastWriteUtc != written)
        {
            opened.Value.Dispose();
            return Result.Fail(ProviderError.Gone, $"{path} changed since it was resolved");
        }
        return opened.Value;
    }
}

// TagLib reads a file through this, so a file on a share is probed as one on
// disk is.
internal sealed class TagSource(string name, Func<Stream> open) : TagLib.File.IFileAbstraction
{
    public string Name => name;

    public Stream ReadStream => open();

    public Stream WriteStream => throw new NotSupportedException("the library never writes tags");

    public void CloseStream(Stream stream) => stream.Dispose();
}
