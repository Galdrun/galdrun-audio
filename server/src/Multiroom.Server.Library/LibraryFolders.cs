using System.Globalization;
using System.Text.Json;
using Microsoft.Data.Sqlite;
using Multiroom.Server.Abstractions;
using Multiroom.Server.Library.Smb;

namespace Multiroom.Server.Library;

public enum LibraryFolderType
{
    // A disk of this machine: watched.
    Local,

    // A share the OS mounts (\\server\share): polled, because watchers are
    // unreliable on SMB. Read as this server's own account.
    Network,

    // A share this server reads itself over SMB, with the folder's own login
    // (phase 13): polled.
    Smb,
}

public enum LibraryFolderOrigin
{
    // From the config file: read-only to the API.
    Config,

    // Added through the API, kept in the library database.
    Api,
}

// A folder of the library. Path is a full path, or an smb folder's URI; Smb
// says where that share is and how to log in. A disabled folder is not
// indexed, and what was indexed from it is forgotten. MediaType is the type
// the owner declared for everything in it (phase 30: music or audiobook),
// which beats any tag; null lets the tags decide.
public sealed record LibraryFolder(string Path, LibraryFolderType Type, bool Enabled = true, SmbLocation? Smb = null,
    MediaType? MediaType = null)
{
    public static LibraryFolder OnShare(SmbLocation location, bool enabled = true, MediaType? mediaType = null) =>
        new(location.Uri, LibraryFolderType.Smb, enabled, location, mediaType);

    // What a folder may be declared as: radio is a station's, never a file's.
    public static bool Declarable(MediaType type) =>
        type is Abstractions.MediaType.Music or Abstractions.MediaType.Audiobook;

    // The same folder, or one inside the other: one file would then belong to
    // two folders. Paths compare as the file system does, shares as SMB does
    // (without case), and a share never overlaps a disk.
    public static bool Overlap(LibraryFolder a, LibraryFolder b)
    {
        if (a.Smb is { } x && b.Smb is { } y)
        {
            if (x.Login.Server != y.Login.Server || !x.Share.Equals(y.Share, StringComparison.OrdinalIgnoreCase))
                return false;
            return Within(x.Directory, y.Directory) || Within(y.Directory, x.Directory);
        }
        if (a.Smb is not null || b.Smb is not null)
            return false;
        return Contains(a.Path, b.Path) || Contains(b.Path, a.Path);
    }

    private static bool Contains(string outer, string inner)
    {
        var relative = System.IO.Path.GetRelativePath(outer, inner);
        return relative == "." || !(System.IO.Path.IsPathRooted(relative) || relative == ".." ||
            relative.StartsWith(".." + System.IO.Path.DirectorySeparatorChar, StringComparison.Ordinal));
    }

    private static bool Within(string outer, string inner) =>
        outer.Length == 0 || inner.Equals(outer, StringComparison.OrdinalIgnoreCase) ||
        inner.StartsWith(outer + "/", StringComparison.OrdinalIgnoreCase);
}

// The config's folders, in config order, disabled ones included, and how
// often a network or smb folder is polled.
public sealed record LibrarySettings(IReadOnlyList<LibraryFolder> Folders, TimeSpan PollInterval);

// One folder as the registry holds it: its API id ("config:<n>", or the
// library_folder row's id), where it came from, and its files while enabled.
public sealed record LibraryFolderEntry(string Id, LibraryFolderOrigin Origin, LibraryFolder Folder, FolderFiles? Files);

// A folder added, changed or removed: before, after, or both.
public sealed record LibraryFolderChange(LibraryFolderEntry? Before, LibraryFolderEntry? After);

// Every library folder: the config file's, read-only, then those added
// through the API, kept in the library database. Folders may not overlap
// across both lists. An API folder's password is kept in its own file,
// owner-only, written whole (the Tidal token's way), never in the database;
// a config folder's comes from the config (best from an environment
// variable).
public sealed class LibraryFolders
{
    public const string ConfigPrefix = "config:";

    private readonly LibraryDatabase _database;
    private readonly string _credentialsPath;
    private readonly Lock _lock = new();
    private readonly List<LibraryFolderEntry> _config;
    private List<LibraryFolderEntry> _api = [];
    private Dictionary<string, string> _passwords = new(StringComparer.Ordinal);

    public LibraryFolders(LibrarySettings settings, LibraryDatabase database, string credentialsPath, SmbSessions sessions)
    {
        _database = database;
        _credentialsPath = credentialsPath;
        Sessions = sessions;
        PollInterval = settings.PollInterval;
        _config = [.. settings.Folders.Select((f, i) =>
            new LibraryFolderEntry(ConfigPrefix + i.ToString(CultureInfo.InvariantCulture), LibraryFolderOrigin.Config, f, FilesOf(f)))];
    }

    public TimeSpan PollInterval { get; }

    public SmbSessions Sessions { get; }

    public string CredentialsPath => _credentialsPath;

    // Raised after a folder was added, changed or removed, outside the lock.
    public event EventHandler<LibraryFolderChange>? Changed;

    // The config's folders in config order, then the API's in the order added.
    public IReadOnlyList<LibraryFolderEntry> All
    {
        get
        {
            lock (_lock)
                return [.. _config, .. _api];
        }
    }

    public LibraryFolderEntry? Find(string id) => All.FirstOrDefault(e => e.Id == id);

    // The enabled folder a file lies in, and its files.
    public FolderFiles? FilesFor(string path) =>
        All.Select(e => e.Files).FirstOrDefault(f => f is not null && f.Contains(path));

    // The API's folders, from the database the indexer has just migrated, and
    // their passwords. A credentials file that cannot be read leaves those
    // folders without a password: their state then says the login failed.
    public Result<int, LibraryError> Load()
    {
        List<LibraryFolderEntry> loaded;
        try
        {
            _passwords = ReadPasswords();
            using var connection = _database.Open();
            using var command = connection.CreateCommand();
            command.CommandText = "SELECT id, type, path, host, port, share, user, domain, enabled, media_type FROM library_folder ORDER BY id";
            using var reader = command.ExecuteReader();
            loaded = [];
            while (reader.Read())
            {
                var id = reader.GetInt64(0).ToString(CultureInfo.InvariantCulture);
                var type = Enum.Parse<LibraryFolderType>(reader.GetString(1), ignoreCase: true);
                var enabled = reader.GetBoolean(8);
                var mediaType = LibraryCatalog.EnumOrNull<MediaType>(reader, 9);
                var folder = type == LibraryFolderType.Smb
                    ? LibraryFolder.OnShare(new SmbLocation(
                        new SmbLogin(reader.GetString(3), reader.GetInt32(4), Text(reader, 6), _passwords.GetValueOrDefault(id),
                            Text(reader, 7)),
                        reader.GetString(5), reader.GetString(2)), enabled, mediaType)
                    : new LibraryFolder(reader.GetString(2), type, enabled, MediaType: mediaType);
                loaded.Add(new LibraryFolderEntry(id, LibraryFolderOrigin.Api, folder, FilesOf(folder)));
            }
        }
        catch (SqliteException e)
        {
            return Result.Fail(LibraryError.UnusableDatabase, $"library folders: {e.Message}");
        }
        lock (_lock)
            _api = loaded;
        return loaded.Count;
    }

    // A local or network folder by its full path, or an smb folder by its
    // location. Refused when it overlaps any folder of either list.
    public Result<LibraryFolderEntry, LibraryError> Add(LibraryFolderType type, string? path, SmbLocation? smb,
        bool enabled, MediaType? mediaType = null)
    {
        if (mediaType is { } declared && !LibraryFolder.Declarable(declared))
            return NotDeclarable(declared);
        LibraryFolder folder;
        if (type == LibraryFolderType.Smb)
        {
            if (smb is null)
                return Result.Fail(LibraryError.InvalidArgument, "an smb folder needs host and share");
            folder = LibraryFolder.OnShare(smb, enabled, mediaType);
        }
        else
        {
            if (smb is not null)
                return Result.Fail(LibraryError.InvalidArgument, $"a {Name(type)} folder takes a path, not host or share");
            if (string.IsNullOrWhiteSpace(path) || !Path.IsPathFullyQualified(path))
                return Result.Fail(LibraryError.InvalidArgument, $"a {Name(type)} folder needs a full path, was {path}");
            folder = new LibraryFolder(Path.TrimEndingDirectorySeparator(Path.GetFullPath(path)), type, enabled,
                MediaType: mediaType);
        }

        LibraryFolderEntry entry;
        lock (_lock)
        {
            if (_config.Concat(_api).FirstOrDefault(e => LibraryFolder.Overlap(e.Folder, folder)) is { } other)
                return Result.Fail(LibraryError.InvalidArgument, $"{folder.Path} overlaps folder {other.Id} ({other.Folder.Path})");
            try
            {
                using var connection = _database.Open();
                using var command = connection.CreateCommand();
                command.CommandText = """
                    INSERT INTO library_folder (type, path, host, port, share, user, domain, enabled, added, media_type)
                    VALUES ($type, $path, $host, $port, $share, $user, $domain, $enabled, $added, $mediaType) RETURNING id
                    """;
                command.Parameters.AddWithValue("$type", Name(type));
                command.Parameters.AddWithValue("$path", smb?.Directory ?? folder.Path);
                command.Parameters.AddWithValue("$host", (object?)smb?.Login.Host ?? DBNull.Value);
                command.Parameters.AddWithValue("$port", (object?)smb?.Login.Port ?? DBNull.Value);
                command.Parameters.AddWithValue("$share", (object?)smb?.Share ?? DBNull.Value);
                command.Parameters.AddWithValue("$user", (object?)smb?.Login.User ?? DBNull.Value);
                command.Parameters.AddWithValue("$domain", (object?)smb?.Login.Domain ?? DBNull.Value);
                command.Parameters.AddWithValue("$enabled", enabled);
                command.Parameters.AddWithValue("$added", DateTime.UtcNow.Ticks);
                command.Parameters.AddWithValue("$mediaType", (object?)LibraryCatalog.NameOf(mediaType) ?? DBNull.Value);
                var id = ((long)command.ExecuteScalar()!).ToString(CultureInfo.InvariantCulture);
                entry = new LibraryFolderEntry(id, LibraryFolderOrigin.Api, folder, FilesOf(folder));
                if (smb?.Login.Password is { } password)
                    SavePasswords(new Dictionary<string, string>(_passwords) { [id] = password });
            }
            catch (Exception e) when (e is SqliteException or IOException or UnauthorizedAccessException)
            {
                return Result.Fail(LibraryError.UnusableDatabase, $"library folder {folder.Path}: {e.Message}");
            }
            _api = [.. _api, entry];
        }
        Changed?.Invoke(this, new LibraryFolderChange(null, entry));
        return entry;
    }

    // Enabled, an smb folder's login and the declared media type: null keeps
    // what is there; an empty user logs in as guest, an empty password clears
    // it, and `mediaType` set to null declares none. Where a folder is is not
    // changed here: that is a different folder, removed and added.
    public Result<LibraryFolderEntry, LibraryError> Update(string id, bool? enabled, string? user, string? password,
        string? domain, (MediaType? Value, bool Set) mediaType = default)
    {
        if (mediaType is { Set: true, Value: { } declared } && !LibraryFolder.Declarable(declared))
            return NotDeclarable(declared);
        LibraryFolderEntry before, after;
        lock (_lock)
        {
            var index = _api.FindIndex(e => e.Id == id);
            if (index < 0)
                return Missing(id);
            before = _api[index];
            var folder = before.Folder with
            {
                Enabled = enabled ?? before.Folder.Enabled,
                MediaType = mediaType.Set ? mediaType.Value : before.Folder.MediaType,
            };
            if (before.Folder.Smb is { } smb)
            {
                var login = smb.Login;
                folder = LibraryFolder.OnShare(smb.With(new SmbLogin(login.Host, login.Port, user ?? login.User,
                    password ?? login.Password, domain ?? login.Domain)), folder.Enabled, folder.MediaType);
            }
            else if (user is not null || password is not null || domain is not null)
            {
                return Result.Fail(LibraryError.InvalidArgument, $"folder {id} is no smb folder: it has no login");
            }

            var passwords = new Dictionary<string, string>(_passwords);
            if (folder.Smb?.Login.Password is { } kept)
                passwords[id] = kept;
            else
                passwords.Remove(id);
            try
            {
                using var connection = _database.Open();
                using var command = connection.CreateCommand();
                command.CommandText =
                    "UPDATE library_folder SET enabled = $enabled, user = $user, domain = $domain, media_type = $mediaType WHERE id = $id";
                command.Parameters.AddWithValue("$id", id);
                command.Parameters.AddWithValue("$enabled", folder.Enabled);
                command.Parameters.AddWithValue("$user", (object?)folder.Smb?.Login.User ?? DBNull.Value);
                command.Parameters.AddWithValue("$domain", (object?)folder.Smb?.Login.Domain ?? DBNull.Value);
                command.Parameters.AddWithValue("$mediaType", (object?)LibraryCatalog.NameOf(folder.MediaType) ?? DBNull.Value);
                command.ExecuteNonQuery();
                if (!passwords.SequenceEqual(_passwords))
                    SavePasswords(passwords);
            }
            catch (Exception e) when (e is SqliteException or IOException or UnauthorizedAccessException)
            {
                return Result.Fail(LibraryError.UnusableDatabase, $"library folder {id}: {e.Message}");
            }
            after = before with { Folder = folder, Files = FilesOf(folder) };
            _api[index] = after;
        }
        Changed?.Invoke(this, new LibraryFolderChange(before, after));
        return after;
    }

    // The indexer forgets what was indexed from it.
    public Result<bool, LibraryError> Remove(string id)
    {
        LibraryFolderEntry removed;
        lock (_lock)
        {
            var index = _api.FindIndex(e => e.Id == id);
            if (index < 0)
                return Missing(id);
            removed = _api[index];
            try
            {
                using var connection = _database.Open();
                using var command = connection.CreateCommand();
                command.CommandText = "DELETE FROM library_folder WHERE id = $id";
                command.Parameters.AddWithValue("$id", id);
                command.ExecuteNonQuery();
                if (_passwords.ContainsKey(id))
                {
                    var passwords = new Dictionary<string, string>(_passwords);
                    passwords.Remove(id);
                    SavePasswords(passwords);
                }
            }
            catch (Exception e) when (e is SqliteException or IOException or UnauthorizedAccessException)
            {
                return Result.Fail(LibraryError.UnusableDatabase, $"library folder {id}: {e.Message}");
            }
            _api = [.. _api.Where(e => e.Id != id)];
        }
        Changed?.Invoke(this, new LibraryFolderChange(removed, null));
        return true;
    }

    public static string Name(LibraryFolderType type) => JsonNamingPolicy.CamelCase.ConvertName(type.ToString());

    private static Failure<LibraryError> NotDeclarable(MediaType type) => Result.Fail(LibraryError.InvalidArgument,
        $"a folder is declared music or audiobook, not {LibraryCatalog.NameOf<MediaType>(type)}");

    private Failure<LibraryError> Missing(string id) => id.StartsWith(ConfigPrefix, StringComparison.Ordinal) &&
        _config.Any(e => e.Id == id)
        ? Result.Fail(LibraryError.InvalidArgument, $"folder {id} is from the config file; change it there")
        : Result.Fail(LibraryError.NotFound, $"no library folder {id}");

    private FolderFiles? FilesOf(LibraryFolder folder) => !folder.Enabled
        ? null
        : folder.Smb is { } smb
            ? new SmbFiles(smb, Sessions)
            : new DiskFiles(folder.Path, folder.Type == LibraryFolderType.Network);

    private Dictionary<string, string> ReadPasswords()
    {
        try
        {
            return JsonSerializer.Deserialize<Dictionary<string, string>>(File.ReadAllBytes(_credentialsPath)) ??
                new Dictionary<string, string>(StringComparer.Ordinal);
        }
        catch (Exception e) when (e is FileNotFoundException or DirectoryNotFoundException or JsonException
            or IOException or UnauthorizedAccessException)
        {
            return new Dictionary<string, string>(StringComparer.Ordinal);
        }
    }

    private void SavePasswords(Dictionary<string, string> passwords)
    {
        OwnerOnlyFile.Write(_credentialsPath, JsonSerializer.SerializeToUtf8Bytes(passwords));
        _passwords = passwords;
    }

    private static string? Text(SqliteDataReader reader, int column) =>
        reader.IsDBNull(column) ? null : reader.GetString(column);
}
