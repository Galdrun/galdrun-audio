using System.Net;
using System.Net.Sockets;
using SMBLibrary;
using SMBLibrary.Authentication.GSSAPI;
using SMBLibrary.Authentication.NTLM;
using SMBLibrary.Server;
using FileAttributes = SMBLibrary.FileAttributes;

namespace Multiroom.Server.Tests;

// A NAS in the test process: SMBLibrary's own SMB 2/3 server on a free
// loopback port, one share over a directory of the test, and one user with a
// password (NTLM). Stop drops every connection, as a NAS going away does; Start
// again listens on the same port. Sleep makes the next read of a file wait,
// as a disk that has to spin up does.
internal sealed class FakeNas : IDisposable
{
    public const string User = "nas-listener";
    public const string Password = "nas-pa55word-never-logged";
    public const string ShareName = "music";

    private readonly ShareStore _store;
    private readonly SMBShareCollection _shares;
    private PortedServer? _server;

    public FakeNas(string root)
    {
        Root = root;
        _store = new ShareStore(root);
        _shares = [new FileSystemShare(ShareName, _store), new FileSystemShare("photos", new ShareStore(root))];
        using (var probe = new TcpListener(IPAddress.Loopback, 0))
        {
            probe.Start();
            Port = ((IPEndPoint)probe.LocalEndpoint).Port;
        }
        Start();
    }

    public string Root { get; }

    public int Port { get; }

    public const string Host = "127.0.0.1";

    // How many reads of file data the share has answered.
    public int Reads => _store.Reads;

    public void Start()
    {
        _server = new PortedServer(_shares,
            new GSSProvider(new IndependentNTLMAuthenticationProvider(user => user == User ? Password : null)));
        _server.Listen(IPAddress.Loopback, Port);
    }

    public void Stop()
    {
        _server?.Stop();
        _server = null;
    }

    // The next read of a file's data waits this long first.
    public void Sleep(TimeSpan wake) => _store.Wake = wake;

    public void Dispose() => Stop();

    // SMBLibrary listens on 445 only; its port overload is for subclasses.
    private sealed class PortedServer(SMBShareCollection shares, GSSProvider security) : SMBServer(shares, security)
    {
        public void Listen(IPAddress address, int port) =>
            Start(address, SMBTransportType.DirectTCPTransport, port, enableSMB1: false, enableSMB2: true,
                enableSMB3: true, connectionInactivityTimeout: null);
    }

    // A directory served read-only: open, read, list and stat, which is all
    // the library asks of a share. Everything else is refused.
    private sealed class ShareStore(string root) : INTFileStore
    {
        private int _reads;

        public TimeSpan Wake { get; set; }

        public int Reads => Volatile.Read(ref _reads);

        public NTStatus CreateFile(out object handle, out FileStatus fileStatus, string path, AccessMask desiredAccess,
            FileAttributes fileAttributes, ShareAccess shareAccess, CreateDisposition createDisposition,
            CreateOptions createOptions, SecurityContext securityContext)
        {
            handle = null!;
            fileStatus = FileStatus.FILE_DOES_NOT_EXIST;
            if (createDisposition != CreateDisposition.FILE_OPEN)
                return NTStatus.STATUS_ACCESS_DENIED;
            var full = Full(path);
            if (full is null)
                return NTStatus.STATUS_OBJECT_PATH_NOT_FOUND;
            if (Directory.Exists(full))
            {
                if ((createOptions & CreateOptions.FILE_NON_DIRECTORY_FILE) != 0)
                    return NTStatus.STATUS_FILE_IS_A_DIRECTORY;
                handle = new Handle(full, null);
            }
            else if (File.Exists(full))
            {
                if ((createOptions & CreateOptions.FILE_DIRECTORY_FILE) != 0)
                    return NTStatus.STATUS_NOT_A_DIRECTORY;
                try
                {
                    handle = new Handle(full, new FileStream(full, FileMode.Open, FileAccess.Read,
                        FileShare.ReadWrite | FileShare.Delete));
                }
                catch (IOException)
                {
                    return NTStatus.STATUS_SHARING_VIOLATION;
                }
            }
            else
            {
                return Directory.Exists(Path.GetDirectoryName(full))
                    ? NTStatus.STATUS_OBJECT_NAME_NOT_FOUND
                    : NTStatus.STATUS_OBJECT_PATH_NOT_FOUND;
            }
            fileStatus = FileStatus.FILE_OPENED;
            return NTStatus.STATUS_SUCCESS;
        }

        public NTStatus CloseFile(object handle)
        {
            ((Handle)handle).Stream?.Dispose();
            return NTStatus.STATUS_SUCCESS;
        }

        public NTStatus ReadFile(out byte[] data, object handle, long offset, int maxCount)
        {
            data = [];
            var stream = ((Handle)handle).Stream;
            if (stream is null)
                return NTStatus.STATUS_INVALID_DEVICE_REQUEST;
            if (Wake > TimeSpan.Zero)
            {
                Thread.Sleep(Wake);
                Wake = TimeSpan.Zero;
            }
            Interlocked.Increment(ref _reads);
            lock (stream)
            {
                if (offset >= stream.Length)
                    return NTStatus.STATUS_END_OF_FILE;
                stream.Position = offset;
                data = new byte[(int)Math.Min(maxCount, stream.Length - offset)];
                stream.ReadExactly(data);
            }
            return NTStatus.STATUS_SUCCESS;
        }

        public NTStatus QueryDirectory(out List<QueryDirectoryFileInformation> result, object handle, string fileName,
            FileInformationClass informationClass)
        {
            result = [];
            var directory = (Handle)handle;
            if (directory.Stream is not null || fileName != "*")
                return NTStatus.STATUS_INVALID_PARAMETER;
            if (informationClass != FileInformationClass.FileDirectoryInformation)
                return NTStatus.STATUS_INVALID_INFO_CLASS;
            var self = new DirectoryInfo(directory.Path);
            result.Add(Entry(self, "."));
            result.Add(Entry(self, ".."));
            foreach (var entry in self.EnumerateFileSystemInfos())
                result.Add(Entry(entry, entry.Name));
            return NTStatus.STATUS_SUCCESS;
        }

        public NTStatus GetFileInformation(out FileInformation result, object handle, FileInformationClass informationClass)
        {
            var info = Info(((Handle)handle).Path);
            var directory = info is DirectoryInfo;
            var length = info is FileInfo file ? file.Length : 0;
            var attributes = directory ? FileAttributes.Directory : FileAttributes.Normal;
            result = informationClass switch
            {
                FileInformationClass.FileNetworkOpenInformation => new FileNetworkOpenInformation
                {
                    CreationTime = info.CreationTimeUtc, LastAccessTime = info.LastAccessTimeUtc,
                    LastWriteTime = info.LastWriteTimeUtc, ChangeTime = info.LastWriteTimeUtc,
                    AllocationSize = length, EndOfFile = length, FileAttributes = attributes,
                },
                FileInformationClass.FileBasicInformation => new FileBasicInformation
                {
                    CreationTime = info.CreationTimeUtc, LastAccessTime = info.LastAccessTimeUtc,
                    LastWriteTime = info.LastWriteTimeUtc, ChangeTime = info.LastWriteTimeUtc,
                    FileAttributes = attributes,
                },
                FileInformationClass.FileStandardInformation => new FileStandardInformation
                {
                    AllocationSize = length, EndOfFile = length, Directory = directory, NumberOfLinks = 1,
                },
                _ => null!,
            };
            return result is null ? NTStatus.STATUS_INVALID_INFO_CLASS : NTStatus.STATUS_SUCCESS;
        }

        public NTStatus GetFileSystemInformation(out FileSystemInformation result, FileSystemInformationClass informationClass)
        {
            result = null!;
            return NTStatus.STATUS_NOT_SUPPORTED;
        }

        public NTStatus WriteFile(out int numberOfBytesWritten, object handle, long offset, byte[] data)
        {
            numberOfBytesWritten = 0;
            return NTStatus.STATUS_ACCESS_DENIED;
        }

        public NTStatus FlushFileBuffers(object handle) => NTStatus.STATUS_SUCCESS;

        public NTStatus LockFile(object handle, long byteOffset, long length, bool exclusiveLock) => NTStatus.STATUS_NOT_SUPPORTED;

        public NTStatus UnlockFile(object handle, long byteOffset, long length) => NTStatus.STATUS_NOT_SUPPORTED;

        public NTStatus SetFileInformation(object handle, FileInformation information) => NTStatus.STATUS_ACCESS_DENIED;

        public NTStatus SetFileSystemInformation(FileSystemInformation information) => NTStatus.STATUS_ACCESS_DENIED;

        public NTStatus GetSecurityInformation(out SecurityDescriptor result, object handle, SecurityInformation securityInformation)
        {
            result = null!;
            return NTStatus.STATUS_NOT_SUPPORTED;
        }

        public NTStatus SetSecurityInformation(object handle, SecurityInformation securityInformation,
            SecurityDescriptor securityDescriptor) => NTStatus.STATUS_ACCESS_DENIED;

        public NTStatus NotifyChange(out object ioRequest, object handle, NotifyChangeFilter completionFilter, bool watchTree,
            int outputBufferSize, OnNotifyChangeCompleted onNotifyChangeCompleted, object context)
        {
            ioRequest = null!;
            return NTStatus.STATUS_NOT_SUPPORTED;
        }

        public NTStatus Cancel(object ioRequest) => NTStatus.STATUS_NOT_SUPPORTED;

        public NTStatus DeviceIOControl(object handle, uint ctlCode, byte[] input, out byte[] output, int maxOutputLength)
        {
            output = null!;
            return NTStatus.STATUS_NOT_SUPPORTED;
        }

        // The share-relative path under the root, or null for one that leaves it.
        private string? Full(string path)
        {
            var relative = path.TrimStart('\\').Replace('\\', Path.DirectorySeparatorChar);
            var full = Path.GetFullPath(Path.Combine(root, relative));
            var inside = Path.GetRelativePath(root, full);
            return inside == "." || !(inside.StartsWith("..", StringComparison.Ordinal) || Path.IsPathRooted(inside))
                ? full
                : null;
        }

        private static FileSystemInfo Info(string path) =>
            Directory.Exists(path) ? new DirectoryInfo(path) : new FileInfo(path);

        private static FileDirectoryInformation Entry(FileSystemInfo info, string name) => new()
        {
            FileName = name,
            CreationTime = info.CreationTimeUtc,
            LastAccessTime = info.LastAccessTimeUtc,
            LastWriteTime = info.LastWriteTimeUtc,
            ChangeTime = info.LastWriteTimeUtc,
            EndOfFile = info is FileInfo file ? file.Length : 0,
            AllocationSize = info is FileInfo sized ? sized.Length : 0,
            FileAttributes = info is DirectoryInfo ? FileAttributes.Directory : FileAttributes.Normal,
        };

        private sealed record Handle(string Path, FileStream? Stream);
    }
}
