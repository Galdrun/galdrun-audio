using SMBLibrary;
using SMBLibrary.Client;
using Multiroom.Server.Abstractions;
using FileAttributes = SMBLibrary.FileAttributes;

namespace Multiroom.Server.Library.Smb;

// A directory entry, or what a stat answers.
public sealed record SmbEntry(string Name, bool IsDirectory, long Length, DateTime LastWriteUtc);

// A file opened on a share: its handle on one session, and its length and
// write time when it was opened.
internal sealed record SmbHandle(ISMBFileStore Tree, object FileId, string Name, long Length, DateTime LastWriteUtc);

// A session held from SmbSessions until disposed. Every request runs under
// the session's lock. A request the connection did not survive marks the
// session broken: the pool closes it once nobody holds it, and the next lease
// opens a new one. Paths are share-relative with backslashes; "" is the
// share's root. Messages name the server, the share and the path, never the
// login.
public sealed class SmbLease : IDisposable
{
    private readonly SmbSessions _pool;
    private readonly SmbSessions.Session _session;
    private int _disposed;

    internal SmbLease(SmbSessions pool, SmbSessions.Session session)
    {
        _pool = pool;
        _session = session;
    }

    public string Server => _session.Server;

    // The largest read one request asks for: what the server allows, at most 1 MiB.
    internal int MaxRead => (int)Math.Min(_session.Client.MaxReadSize, 1024 * 1024);

    // The disk shares the login sees, as the server lists them.
    public Result<IReadOnlyList<string>, LibraryError> ListShares() => Run<IReadOnlyList<string>>(() =>
    {
        var shares = _session.Client.ListShares(out var status);
        return status == NTStatus.STATUS_SUCCESS && shares is not null
            ? shares.Order(StringComparer.OrdinalIgnoreCase).ToList()
            : Fail(status, "the list of shares");
    });

    // A directory's entries, without "." and "..".
    public Result<IReadOnlyList<SmbEntry>, LibraryError> List(string share, string path) =>
        Run<IReadOnlyList<SmbEntry>>(() =>
        {
            var tree = Tree(share, out var noTree);
            if (tree is null)
                return noTree;
            var status = tree.CreateFile(out var handle, out _, path,
                AccessMask.GENERIC_READ | AccessMask.SYNCHRONIZE, FileAttributes.Directory,
                ShareAccess.Read | ShareAccess.Write | ShareAccess.Delete, CreateDisposition.FILE_OPEN,
                CreateOptions.FILE_DIRECTORY_FILE | CreateOptions.FILE_SYNCHRONOUS_IO_ALERT, null);
            if (status != NTStatus.STATUS_SUCCESS)
                return Fail(status, Name(share, path));
            try
            {
                status = tree.QueryDirectory(out var found, handle, "*",
                    FileInformationClass.FileDirectoryInformation);
                if (status is not (NTStatus.STATUS_SUCCESS or NTStatus.STATUS_NO_MORE_FILES or NTStatus.STATUS_NO_SUCH_FILE))
                    return Fail(status, Name(share, path));
                return found.OfType<FileDirectoryInformation>()
                    .Where(f => f.FileName is not ("." or ".."))
                    .Select(f => new SmbEntry(f.FileName, (f.FileAttributes & FileAttributes.Directory) != 0,
                        f.EndOfFile, Utc(f.LastWriteTime)))
                    .ToList();
            }
            finally
            {
                tree.CloseFile(handle);
            }
        });

    public Result<SmbEntry, LibraryError> Stat(string share, string path) => Run<SmbEntry>(() =>
    {
        var tree = Tree(share, out var noTree);
        if (tree is null)
            return noTree;
        var status = tree.CreateFile(out var handle, out _, path,
            AccessMask.SYNCHRONIZE | (AccessMask)FileAccessMask.FILE_READ_ATTRIBUTES, FileAttributes.Normal,
            ShareAccess.Read | ShareAccess.Write | ShareAccess.Delete, CreateDisposition.FILE_OPEN,
            CreateOptions.FILE_SYNCHRONOUS_IO_ALERT, null);
        if (status != NTStatus.STATUS_SUCCESS)
            return Fail(status, Name(share, path));
        try
        {
            return Info(tree, handle, share, path);
        }
        finally
        {
            tree.CloseFile(handle);
        }
    });

    internal Result<SmbHandle, LibraryError> Open(string share, string path) => Run<SmbHandle>(() =>
    {
        var tree = Tree(share, out var noTree);
        if (tree is null)
            return noTree;
        var status = tree.CreateFile(out var handle, out _, path, AccessMask.GENERIC_READ | AccessMask.SYNCHRONIZE,
            FileAttributes.Normal, ShareAccess.Read | ShareAccess.Write | ShareAccess.Delete,
            CreateDisposition.FILE_OPEN, CreateOptions.FILE_NON_DIRECTORY_FILE | CreateOptions.FILE_SYNCHRONOUS_IO_ALERT,
            null);
        if (status != NTStatus.STATUS_SUCCESS)
            return Fail(status, Name(share, path));
        var info = Info(tree, handle, share, path);
        if (!info.IsSuccess)
        {
            tree.CloseFile(handle);
            return info.Failure;
        }
        return new SmbHandle(tree, handle, Name(share, path), info.Value.Length, info.Value.LastWriteUtc);
    });

    // Up to `into.Length` bytes at `offset`; 0 at the end of the file.
    internal Result<int, LibraryError> Read(SmbHandle file, long offset, Span<byte> into)
    {
        var count = Math.Min(into.Length, MaxRead);
        byte[]? data = null;
        var read = Run<int>(() =>
        {
            var status = file.Tree.ReadFile(out data, file.FileId, offset, count);
            if (status == NTStatus.STATUS_END_OF_FILE)
                return 0;
            return status == NTStatus.STATUS_SUCCESS ? data?.Length ?? 0 : Fail(status, file.Name);
        });
        if (read.IsSuccess && read.Value > 0)
            data.AsSpan(0, Math.Min(read.Value, count)).CopyTo(into);
        return read.IsSuccess ? Math.Min(read.Value, count) : read;
    }

    internal void Close(SmbHandle file) => Run(() => (Result<bool, LibraryError>)(file.Tree.CloseFile(file.FileId) == NTStatus.STATUS_SUCCESS));

    public void Dispose()
    {
        if (Interlocked.Exchange(ref _disposed, 1) == 0)
            _pool.Release(_session);
    }

    private Result<SmbEntry, LibraryError> Info(ISMBFileStore tree, object handle, string share, string path)
    {
        var status = tree.GetFileInformation(out var information, handle, FileInformationClass.FileNetworkOpenInformation);
        if (status != NTStatus.STATUS_SUCCESS || information is not FileNetworkOpenInformation info)
            return Fail(status, Name(share, path));
        return new SmbEntry(Path.GetFileName(path.Replace('\\', '/')), info.IsDirectory, info.EndOfFile,
            Utc(info.LastWriteTime ?? DateTime.MinValue));
    }

    // The share's tree connection, made once per session.
    private ISMBFileStore? Tree(string share, out Failure<LibraryError> failure)
    {
        failure = default;
        if (_session.Trees.TryGetValue(share, out var tree))
            return tree;
        tree = _session.Client.TreeConnect(share, out var status);
        if (tree is null || status != NTStatus.STATUS_SUCCESS)
        {
            failure = status == NTStatus.STATUS_BAD_NETWORK_NAME
                ? Result.Fail(LibraryError.NotFound, $"{_session.Server} has no share {share}")
                : Fail(status, $"share {share}");
            return null;
        }
        _session.Trees[share] = tree;
        return tree;
    }

    private Result<T, LibraryError> Run<T>(Func<Result<T, LibraryError>> request)
    {
        ObjectDisposedException.ThrowIf(_disposed != 0, this);
        lock (_session.Gate)
        {
            if (_session.Broken)
                return Result.Fail(LibraryError.Unreachable, $"the connection to {_session.Server} was lost");
            try
            {
                var result = request();
                if (!_session.Client.IsConnected)
                    _session.Broken = true;
                return result;
            }
#pragma warning disable CA1031 // SMBLibrary throws plain Exception when the connection breaks under a request.
            catch (Exception e)
#pragma warning restore CA1031
            {
                _session.Broken = true;
                return Result.Fail(LibraryError.Unreachable, $"{_session.Server}: {e.Message}");
            }
        }
    }

    private Failure<LibraryError> Fail(NTStatus status, string what)
    {
        switch (status)
        {
            case NTStatus.STATUS_OBJECT_NAME_NOT_FOUND or NTStatus.STATUS_OBJECT_PATH_NOT_FOUND
                or NTStatus.STATUS_NO_SUCH_FILE or NTStatus.STATUS_NOT_A_DIRECTORY
                or NTStatus.STATUS_FILE_IS_A_DIRECTORY or NTStatus.STATUS_OBJECT_NAME_INVALID:
                return Result.Fail(LibraryError.NotFound, $"{what} does not exist");
            case var _ when SmbSessions.IsRefusedLogin(status):
                return Result.Fail(LibraryError.AuthFailed, $"{what}: access refused ({status})");
            case NTStatus.STATUS_INVALID_SMB or NTStatus.STATUS_IO_TIMEOUT or NTStatus.STATUS_NETWORK_NAME_DELETED
                or NTStatus.STATUS_USER_SESSION_DELETED:
                _session.Broken = true;
                return Result.Fail(LibraryError.Unreachable, $"{what}: the connection to {_session.Server} was lost ({status})");
            default:
                return Result.Fail(LibraryError.Unreachable, $"{what}: {status}");
        }
    }

    private string Name(string share, string path) =>
        $"{_session.Server}/{share}" + (path.Length == 0 ? "" : "/" + path.Replace('\\', '/'));

    private static DateTime Utc(DateTime time) => DateTime.SpecifyKind(time, DateTimeKind.Utc);
}

// A file on a share as a seekable reader: each read is one request (at most
// MaxRead) at the position. The reader holds its own lease, so the session
// stays open while it is read; disposing it closes the file and lets the
// session go.
internal sealed class SmbReadStream(SmbLease lease, SmbHandle file) : Stream
{
    private long _position;
    private int _disposed;

    public override bool CanRead => true;

    public override bool CanSeek => true;

    public override bool CanWrite => false;

    public override long Length => file.Length;

    public DateTime LastWriteUtc => file.LastWriteUtc;

    public override long Position
    {
        get => _position;
        set => Seek(value, SeekOrigin.Begin);
    }

    public override int Read(byte[] buffer, int offset, int count) => Read(buffer.AsSpan(offset, count));

    public override int Read(Span<byte> buffer)
    {
        ObjectDisposedException.ThrowIf(_disposed != 0, this);
        if (buffer.Length == 0 || _position >= file.Length)
            return 0;
        var wanted = (int)Math.Min(buffer.Length, file.Length - _position);
        var read = lease.Read(file, _position, buffer[..wanted]);
        if (!read.IsSuccess)
            throw new IOException(read.Failure.Detail);
        _position += read.Value;
        return read.Value;
    }

    public override long Seek(long offset, SeekOrigin origin)
    {
        var target = origin switch
        {
            SeekOrigin.Begin => offset,
            SeekOrigin.Current => _position + offset,
            SeekOrigin.End => file.Length + offset,
            _ => throw new ArgumentOutOfRangeException(nameof(origin)),
        };
        ArgumentOutOfRangeException.ThrowIfNegative(target, nameof(offset));
        _position = target;
        return target;
    }

    public override void Flush()
    {
    }

    public override void SetLength(long value) => throw new NotSupportedException();

    public override void Write(byte[] buffer, int offset, int count) => throw new NotSupportedException();

    protected override void Dispose(bool disposing)
    {
        if (disposing && Interlocked.Exchange(ref _disposed, 1) == 0)
        {
            lease.Close(file);
            lease.Dispose();
        }
        base.Dispose(disposing);
    }
}
