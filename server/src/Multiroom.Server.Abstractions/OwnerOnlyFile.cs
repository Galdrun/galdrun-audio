using System.Security.AccessControl;
using System.Security.Principal;

namespace Multiroom.Server.Abstractions;

// A file only this account may read, for what rule 7 keeps out of every
// message: provider tokens (the Tidal link) and share passwords (library
// folders).
public static class OwnerOnlyFile
{
    // Written whole under a temporary name, then moved over the old file, so
    // a reader never sees half of it.
    public static void Write(string path, ReadOnlySpan<byte> bytes)
    {
        var temporary = path + ".tmp";
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        File.Delete(temporary);
        using (var file = Create(temporary))
            file.Write(bytes);
        File.Move(temporary, path, overwrite: true);
    }

    // A new file this account alone may read and write: an ACL with inherited
    // rules removed on Windows, mode 600 elsewhere.
    public static FileStream Create(string path)
    {
        if (OperatingSystem.IsWindows())
        {
            var security = new FileSecurity();
            security.SetAccessRuleProtection(isProtected: true, preserveInheritance: false);
            using var identity = WindowsIdentity.GetCurrent();
            security.AddAccessRule(new FileSystemAccessRule(identity.User!, FileSystemRights.FullControl,
                AccessControlType.Allow));
            return new FileInfo(path).Create(FileMode.CreateNew, FileSystemRights.FullControl, FileShare.None, 4096,
                FileOptions.None, security);
        }
        return new FileStream(path, new FileStreamOptions
        {
            Mode = FileMode.CreateNew,
            Access = FileAccess.Write,
            UnixCreateMode = UnixFileMode.UserRead | UnixFileMode.UserWrite,
        });
    }
}
