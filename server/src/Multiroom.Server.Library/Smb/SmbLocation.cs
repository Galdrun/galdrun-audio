using System.Globalization;
using System.Security.Cryptography;
using System.Text;

namespace Multiroom.Server.Library.Smb;

// Who logs in to a NAS: the server (host and port) and the account. No user
// means guest. A class, not a record: its ToString names the server only,
// never the user or the password (rule 7).
public sealed class SmbLogin
{
    public const int DefaultPort = 445;

    public SmbLogin(string host, int port = DefaultPort, string? user = null, string? password = null,
        string? domain = null)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(host);
        ArgumentOutOfRangeException.ThrowIfLessThan(port, 1);
        ArgumentOutOfRangeException.ThrowIfGreaterThan(port, 65535);
        Host = host.Trim().ToLowerInvariant();
        Port = port;
        User = string.IsNullOrEmpty(user) ? null : user;
        Password = string.IsNullOrEmpty(password) ? null : password;
        Domain = string.IsNullOrEmpty(domain) ? null : domain;
    }

    public string Host { get; }

    public int Port { get; }

    public string? User { get; }

    public string? Password { get; }

    public string? Domain { get; }

    // "nas" or "nas:1445": what messages and status name.
    public string Server => Port == DefaultPort ? Host : $"{Host}:{Port.ToString(CultureInfo.InvariantCulture)}";

    // The session pool's key. The password enters only as a hash, so a key
    // that is printed by mistake still does not carry it.
    internal string Key => string.Join('|', Server, Domain ?? "", User ?? "",
        Password is null ? "" : Convert.ToHexStringLower(SHA256.HashData(Encoding.UTF8.GetBytes(Password))));

    public override string ToString() => Server;
}

// A directory on a share, and the login that reads it: an smb library folder.
// Its URI (smb://nas/music/flac) is the folder's path in the library
// database, and every file below it is stored as a URI under it. No user
// or password is ever part of a URI.
public sealed class SmbLocation
{
    public const string Scheme = "smb://";

    public SmbLocation(SmbLogin login, string share, string? directory = null)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(share);
        Login = login;
        Share = share.Trim();
        Directory = Normalize(directory);
    }

    public SmbLogin Login { get; }

    public string Share { get; }

    // Inside the share, '/' separated, without a leading or trailing one; ""
    // is the share's root.
    public string Directory { get; }

    public string Uri => Scheme + Login.Server + "/" + Share + (Directory.Length == 0 ? "" : "/" + Directory);

    // The same share and login, another directory.
    public SmbLocation With(SmbLogin login) => new(login, Share, Directory);

    // The share-relative path (backslashes, as SMB names them) of a URI at or
    // below this location, or null for one outside it.
    public string? PathOf(string uri)
    {
        var root = Uri;
        if (uri.Equals(root, StringComparison.OrdinalIgnoreCase))
            return ToShare(Directory);
        if (!uri.StartsWith(root + "/", StringComparison.OrdinalIgnoreCase))
            return null;
        var below = uri[(root.Length + 1)..];
        if (below.Split('/').Any(s => s is "" or "." or ".."))
            return null;
        return ToShare(Directory.Length == 0 ? below : Directory + "/" + below);
    }

    // The URI of a share-relative path.
    internal string UriOf(string sharePath) =>
        Scheme + Login.Server + "/" + Share + (sharePath.Length == 0 ? "" : "/" + sharePath.Replace('\\', '/'));

    // Null, "", "/", "a\b/" and "/a/b" are all written as "" or "a/b". A ".."
    // segment is refused: the directory stays inside the share.
    public static string Normalize(string? directory)
    {
        var segments = (directory ?? "").Split('/', '\\').Where(s => s.Length > 0).ToList();
        if (segments.Any(s => s is "." or ".."))
            throw new ArgumentException($"{directory}: no . or .. segments", nameof(directory));
        return string.Join('/', segments);
    }

    public static bool IsUri(string path) => path.StartsWith(Scheme, StringComparison.OrdinalIgnoreCase);

    public override string ToString() => Uri;

    private static string ToShare(string path) => path.Replace('/', '\\');
}
