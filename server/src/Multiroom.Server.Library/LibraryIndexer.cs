using System.Diagnostics;
using System.Security.Cryptography;
using System.Threading.Channels;
using Microsoft.Data.Sqlite;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;
using Multiroom.Server.Abstractions;
using Multiroom.Server.Library.Smb;

namespace Multiroom.Server.Library;

public enum LibraryFolderState
{
    // Enabled, and not tried yet.
    Pending,

    // Its last pass completed.
    Ready,

    // Not there, or it stopped answering: what was indexed is kept.
    Unreachable,

    // The NAS refused the folder's login: what was indexed is kept.
    AuthFailed,

    // Not indexed; what was indexed from it is forgotten.
    Disabled,
}

// What a folder's latest completed pass found: the audio files in it
// (Files), those of them that are not indexed because they do not probe,
// now or when they last changed (Unreadable), and what the pass did: files
// probed, moved, failing their probe, and recorded files it did not find.
public sealed record LibraryPassResult(int Files, int Unreadable, int Probed, int Moved, int Failed, int Missing,
    double Seconds);

// One folder as the API lists it: where it is, where it came from, and what
// the indexer has made of it. Scanned, Indexed and Failed are counted over
// every pass since this server started, so a folder scanned twice counts its
// files twice; LastPassResult is the folder as its latest pass found it (null
// before one completes). An smb folder names its server, share and directory,
// and whether it has a password -- never the password. MediaType is the
// type the owner declared for it (phase 30), null when the tags decide.
public sealed record LibraryFolderView(
    string Id,
    LibraryFolderType Type,
    string Path,
    bool Enabled,
    LibraryFolderOrigin Origin,
    LibraryFolderState State,
    string? Detail,
    DateTimeOffset? LastPass,
    long Scanned,
    long Indexed,
    long Failed,
    string? Host = null,
    int? Port = null,
    string? Share = null,
    string? Directory = null,
    string? User = null,
    string? Domain = null,
    bool HasPassword = false,
    LibraryPassResult? LastPassResult = null,
    MediaType? MediaType = null);

// Progress since the server started. QueueDepth is at most the folder count.
public sealed record IndexerStatus(long Scanned, long Indexed, long Failed, int QueueDepth, long Passes);

public static class LibraryServiceCollectionExtensions
{
    // `credentialsPath` keeps the passwords of folders added through the API.
    public static IServiceCollection AddLibrary(this IServiceCollection services, LibrarySettings settings,
        string databasePath, string credentialsPath)
    {
        services.AddSingleton(settings);
        services.AddSingleton(new LibraryDatabase(databasePath));
        services.AddSingleton<SmbSessions>();
        services.AddSingleton(s => new LibraryFolders(settings, s.GetRequiredService<LibraryDatabase>(), credentialsPath,
            s.GetRequiredService<SmbSessions>()));
        services.AddSingleton<PlaylistStore>();
        services.AddSingleton<StationStore>();
        services.AddSingleton<ServerProfileStore>();
        services.AddSingleton<UserStore>();
        services.AddSingleton<LikeStore>();
        services.AddSingleton<PlayHistoryStore>();
        services.AddSingleton<AiUsageStore>();
        services.AddSingleton<BookPositionStore>();
        services.AddSingleton<ProposalStore>();
        services.AddSingleton<LibraryIndexer>();
        return services.AddHostedService(s => s.GetRequiredService<LibraryIndexer>());
    }
}

// The database's one writer: a dedicated thread fed by a channel of folders
// to scan.
//
// Every enabled folder is scanned once at startup, config folders first. After
// that a local folder is scanned again when its watcher reports anything
// (after SettleDelay, so one copy is one pass), and a network or smb folder
// on every poll interval; any folder when the API asks. A folder is queued at
// most once at a time, so the queue never holds more than the folder count.
// A folder added through the API starts the same way; one removed or
// disabled is forgotten on the same thread.
//
// A pass probes only files whose size or write time changed since they were
// recorded. A file under a new path whose fingerprint matches a recorded file
// that no longer exists is that file, moved: its row, and so its track id,
// follows it. A file a pass does not find is hidden, and forgotten once every
// folder has completed a pass since without finding it. A file that does not probe is
// recorded with the reason and not probed again until it changes. A folder
// that cannot be reached keeps what was recorded and is tried again on its
// next trigger; so does one whose NAS refuses the login.
public sealed partial class LibraryIndexer : IHostedService, IDisposable
{
    private static readonly TimeSpan SettleDelay = TimeSpan.FromMilliseconds(500);

    private static readonly HashSet<string> AudioExtensions = new(StringComparer.OrdinalIgnoreCase)
    {
        ".flac", ".wav", ".mp3", ".m4a", ".mp4", ".aac", ".ogg", ".oga", ".opus", ".aif", ".aiff", ".wv", ".ape",
        ".wma", ".dsf", ".m4b",
    };

    private readonly LibraryFolders _registry;
    private readonly LibraryDatabase _database;
    private readonly ILogger _log;
    private readonly Channel<IndexedFolder> _jobs;
    private readonly CancellationTokenSource _stopping = new();
    private readonly TaskCompletionSource _stopped = new(TaskCreationOptions.RunContinuationsAsynchronously);
    private readonly Lock _lock = new();
    // The enabled folders by id; replaced whole, read without the lock.
    private IndexedFolder[] _folders = [];
    // The fingerprint's read buffer, used on the indexer thread only.
    private readonly byte[] _head = new byte[64 * 1024];
    private Thread? _thread;
    private long _scanned;
    private long _indexed;
    private long _failed;
    private long _passes;

    public LibraryIndexer(LibraryFolders folders, LibraryDatabase database, ILogger<LibraryIndexer> log)
    {
        _registry = folders;
        _database = database;
        _log = log;
        // Not SingleReader: that channel cannot count what it holds, and
        // QueueDepth is its count.
        _jobs = Channel.CreateUnbounded<IndexedFolder>();
    }

    // Raised with a folder's id when it is added, changed or removed, and when
    // its state changes. Never waits on the handler's work.
    public event EventHandler<string>? FolderChanged;

    // The database's schema once migrated at start; 0 before.
    public int SchemaVersion { get; private set; }

    public IndexerStatus Status => new(Interlocked.Read(ref _scanned), Interlocked.Read(ref _indexed),
        Interlocked.Read(ref _failed), _jobs.Reader.Count, Interlocked.Read(ref _passes));

    // Every folder of both lists, in the registry's order.
    public IReadOnlyList<LibraryFolderView> Folders
    {
        get
        {
            var active = Volatile.Read(ref _folders);
            return [.. _registry.All.Select(e => ViewOf(e, active.FirstOrDefault(f => f.Id == e.Id)))];
        }
    }

    public LibraryFolderView? Folder(string id) => Folders.FirstOrDefault(f => f.Id == id);

    public Task StartAsync(CancellationToken cancellationToken)
    {
        var migrated = _database.Migrate();
        if (!migrated.IsSuccess)
            throw new InvalidOperationException($"library database: {migrated.Failure}");
        SchemaVersion = migrated.Value;
        var loaded = _registry.Load();
        if (!loaded.IsSuccess)
            throw new InvalidOperationException($"library database: {loaded.Failure}");

        lock (_lock)
            _folders = [.. _registry.All.Where(e => e.Files is not null)
                .Select(e => new IndexedFolder(e.Id, e.Folder.Type, e.Files!, e.Folder.MediaType))];
        LogDatabase(_log, _database.Path, migrated.Value, _folders.Length);
        foreach (var folder in _folders)
            Enqueue(folder);
        foreach (var folder in _folders)
            Trigger(folder);
        _registry.Changed += OnFolderChanged;

        _thread = new Thread(Run) { IsBackground = true, Name = "galdrun-indexer" };
        _thread.Start();
        return Task.CompletedTask;
    }

    // Finishes the file in hand, commits, and stops.
    public async Task StopAsync(CancellationToken cancellationToken)
    {
        _registry.Changed -= OnFolderChanged;
        await _stopping.CancelAsync().ConfigureAwait(false);
        foreach (var folder in Volatile.Read(ref _folders))
            folder.Dispose();
        if (_thread is not null)
            await _stopped.Task.WaitAsync(cancellationToken).ConfigureAwait(false);
    }

    public void Dispose()
    {
        _registry.Changed -= OnFolderChanged;
        foreach (var folder in Volatile.Read(ref _folders))
            folder.Dispose();
        _stopping.Dispose();
    }

    // A pass of an enabled folder now, as soon as the thread gets to it.
    public Result<LibraryFolderView, LibraryError> Scan(string id)
    {
        var folder = Volatile.Read(ref _folders).FirstOrDefault(f => f.Id == id);
        if (folder is null)
        {
            return _registry.Find(id) is null
                ? Result.Fail(LibraryError.NotFound, $"no library folder {id}")
                : Result.Fail(LibraryError.InvalidArgument, $"folder {id} is disabled");
        }
        Enqueue(folder);
        return Folder(id)!;
    }

    // The registry changed: start what was added or enabled, forget what was
    // removed or disabled, and pass again a folder whose login changed.
    private void OnFolderChanged(object? sender, LibraryFolderChange change)
    {
        var id = (change.After ?? change.Before)!.Id;
        IndexedFolder? stopped = null;
        IndexedFolder? started = null;
        lock (_lock)
        {
            var current = _folders.FirstOrDefault(f => f.Id == id);
            if (current is not null && change.After?.Files is null)
            {
                stopped = current;
                _folders = [.. _folders.Where(f => f != current)];
            }
            else if (current is not null)
            {
                current.Files = change.After!.Files!;
                if (current.Declared != change.After.Folder.MediaType)
                {
                    current.Declared = change.After.Folder.MediaType;
                    Volatile.Write(ref current.Reclassify, 1);
                }
                current.SetState(LibraryFolderState.Pending, null);
                started = current;
            }
            else if (change.After?.Files is { } files)
            {
                started = new IndexedFolder(id, change.After.Folder.Type, files, change.After.Folder.MediaType);
                _folders = [.. _folders, started];
                Trigger(started);
            }
        }
        if (stopped is not null)
        {
            stopped.Dispose();
            stopped.Removed = true;
            Enqueue(stopped);
        }
        if (started is not null)
            Enqueue(started);
        Raise(id);
    }

    // A local folder is watched, the others polled.
    private void Trigger(IndexedFolder folder)
    {
        if (folder.Type == LibraryFolderType.Local)
            Watch(folder);
        else
            folder.Timer = new Timer(_ => Enqueue(folder), null, _registry.PollInterval, _registry.PollInterval);
    }

    private void Watch(IndexedFolder folder)
    {
        if (!Directory.Exists(folder.Root))
        {
            LogNotWatched(_log, folder.Root);
            return;
        }

        folder.Timer = new Timer(_ =>
        {
            Volatile.Write(ref folder.Settling, 0);
            Enqueue(folder);
        });
        var watcher = new FileSystemWatcher(folder.Root)
        {
            IncludeSubdirectories = true,
            InternalBufferSize = 64 * 1024,
            NotifyFilter = NotifyFilters.FileName | NotifyFilters.DirectoryName | NotifyFilters.LastWrite |
                NotifyFilters.Size,
        };
        void Changed()
        {
            if (Interlocked.Exchange(ref folder.Settling, 1) == 0)
                folder.Timer?.Change(SettleDelay, Timeout.InfiniteTimeSpan);
        }
        watcher.Created += (_, _) => Changed();
        watcher.Changed += (_, _) => Changed();
        watcher.Deleted += (_, _) => Changed();
        watcher.Renamed += (_, _) => Changed();
        // An overflow lost events: a pass finds what they said.
        watcher.Error += (_, _) => Changed();
        watcher.EnableRaisingEvents = true;
        folder.Watcher = watcher;
    }

    private void Enqueue(IndexedFolder folder)
    {
        // Unbounded, but a folder is queued at most once at a time.
        if (Interlocked.Exchange(ref folder.Queued, 1) == 0)
            _jobs.Writer.TryWrite(folder);
    }

    private void Raise(string id) => FolderChanged?.Invoke(this, id);

    private void Run()
    {
        var token = _stopping.Token;
        try
        {
            using var connection = _database.Open();
            using var writer = new LibraryWriter(connection);
            var pruned = writer.Prune([.. _registry.All.Where(e => e.Files is not null).Select(e => e.Files!.Root)]);
            if (pruned > 0)
                LogPruned(_log, pruned);

            // The thread is dedicated, so it may block on the channel.
            while (_jobs.Reader.WaitToReadAsync(token).AsTask().GetAwaiter().GetResult())
            {
                while (!token.IsCancellationRequested && _jobs.Reader.TryRead(out var folder))
                {
                    Volatile.Write(ref folder.Queued, 0);
                    try
                    {
                        if (folder.Removed)
                            Forget(writer, folder);
                        else
                            Scan(writer, folder, token);
                    }
                    catch (SqliteException e)
                    {
                        // Rolls back the batch; the next trigger scans again.
                        writer.Dispose();
                        LogScanFailed(_log, e, folder.Root);
                    }
                }
            }
        }
        catch (OperationCanceledException)
        {
        }
        catch (SqliteException e)
        {
            LogIndexerFailed(_log, e);
        }
        finally
        {
            _stopped.TrySetResult();
        }
    }

    // Unless the same folder is enabled again meanwhile.
    private void Forget(LibraryWriter writer, IndexedFolder folder)
    {
        if (Volatile.Read(ref _folders).Any(f => f.Root == folder.Root))
            return;
        writer.Forget(folder.Root);
        LogForgotten(_log, folder.Root);
    }

    private void Scan(LibraryWriter writer, IndexedFolder folder, CancellationToken token)
    {
        // What is stored is classified again, reachable or not: the folder's
        // declared type changed, or it was never weighed in this run.
        if (Interlocked.Exchange(ref folder.Reclassify, 0) == 1 &&
            writer.Reclassify(folder.Root, folder.Declared) is var reclassified and > 0)
        {
            LogReclassified(_log, folder.Root, reclassified);
        }

        var files = folder.Files;
        if (files.Reach() is { } unreachable)
        {
            Unreachable(folder, unreachable);
            return;
        }

        var clock = Stopwatch.StartNew();
        var started = DateTime.UtcNow.Ticks;
        var known = writer.FilesOf(folder.Root);
        var seen = new HashSet<string>(StringComparer.Ordinal);
        int probedCount = 0, movedCount = 0, failedCount = 0, unreadableCount = 0;
        try
        {
            foreach (var file in files.Enumerate())
            {
                if (token.IsCancellationRequested || folder.Removed)
                {
                    writer.Flush();
                    return;
                }
                if (!AudioExtensions.Contains(file.Extension))
                    continue;

                seen.Add(file.Path);
                Interlocked.Increment(ref _scanned);
                Interlocked.Increment(ref folder.Scanned);
                string? fingerprint = null;
                if (known.TryGetValue(file.Path, out var recorded))
                {
                    if (Unchanged(recorded, file))
                    {
                        // Back where it was, or recorded before fingerprints.
                        if (recorded.Missing || recorded.Fingerprint is null)
                            writer.Confirm(file.Path, recorded.Fingerprint ?? Fingerprint(files, file));
                        if (recorded.Unreadable)
                            unreadableCount++;
                        continue;
                    }
                }
                else if ((fingerprint = Fingerprint(files, file)) is not null &&
                         MovedFrom(writer, fingerprint, file.Path) is { } from)
                {
                    // The same file under a new path: its row and track id
                    // follow it. Probed only if it changed on the way.
                    var before = writer.Move(from, folder.Root, file);
                    movedCount++;
                    LogMoved(_log, from, file.Path);
                    if (Unchanged(before, file))
                    {
                        if (before.Unreadable)
                            unreadableCount++;
                        continue;
                    }
                }

                var probed = Probe(files, file);
                if (probed.IsSuccess && probed.Value.ChaptersCut)
                    LogChaptersCut(_log, file.Path, ChapterReader.MaxChapters);
                writer.Store(folder.Root, file, fingerprint ?? Fingerprint(files, file), probed, folder.Declared);
                probedCount++;
                if (probed.IsSuccess)
                {
                    Interlocked.Increment(ref _indexed);
                    Interlocked.Increment(ref folder.Indexed);
                }
                else
                {
                    failedCount++;
                    unreadableCount++;
                    Interlocked.Increment(ref _failed);
                    Interlocked.Increment(ref folder.Failed);
                    LogNotIndexed(_log, file.Path, probed.Failure.Detail);
                }
            }
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            // The folder went away mid-pass: keep everything recorded.
            writer.Flush();
            Unreachable(folder, Result.Fail(LibraryError.Unreachable, e.Message));
            return;
        }

        // Not deleted yet: a file moved to another folder is found there by
        // that folder's pass, which may come later.
        var now = DateTime.UtcNow.Ticks;
        var missingCount = known.Keys.Where(p => !seen.Contains(p)).Count(p => writer.MarkMissing(p, now));

        // A file missing since before every folder's latest completed pass
        // began was found by none of them: it is gone. While a folder has not
        // completed a pass (a share down since startup), nothing is forgotten.
        folder.LastPassStarted = started;
        var active = Volatile.Read(ref _folders);
        var cutoff = active.Length == 0 ? 0 : active.Min(f => f.LastPassStarted);
        if (cutoff > 0)
            writer.Purge(cutoff);
        writer.RemoveOrphans();
        writer.Flush();
        Interlocked.Increment(ref _passes);
        folder.LastPassResult = new LibraryPassResult(seen.Count, unreadableCount, probedCount, movedCount, failedCount,
            missingCount, clock.Elapsed.TotalSeconds);
        folder.LastPass = DateTimeOffset.UtcNow;
        if (folder.State is LibraryFolderState.Unreachable or LibraryFolderState.AuthFailed)
            LogReachable(_log, folder.Root);
        if (folder.SetState(LibraryFolderState.Ready, null))
            Raise(folder.Id);
        LogPass(_log, folder.Root, seen.Count, probedCount, movedCount, failedCount, missingCount,
            clock.Elapsed.TotalSeconds);
    }

    // Logged once per change of state, by folder: never the login.
    private void Unreachable(IndexedFolder folder, Failure<LibraryError> failure)
    {
        var state = failure.Code == LibraryError.AuthFailed ? LibraryFolderState.AuthFailed : LibraryFolderState.Unreachable;
        if (folder.State != state)
        {
            if (state == LibraryFolderState.AuthFailed)
                LogAuthFailed(_log, folder.Root, failure.Detail);
            else
                LogUnreachable(_log, folder.Root, failure.Detail);
        }
        if (folder.SetState(state, failure.Detail))
            Raise(folder.Id);
    }

    // Probed with the tags read now, too: a file probed before is probed again.
    private static bool Unchanged(RecordedFile recorded, LibraryFile file) =>
        recorded.Size == file.Length && recorded.Modified == file.LastWriteUtc.Ticks &&
        recorded.Tags >= LibraryWriter.TagVersion;

    // A recorded file with the same fingerprint whose path no longer exists:
    // this file, moved. One that still exists is a copy, and the copy is a
    // track of its own.
    private string? MovedFrom(LibraryWriter writer, string fingerprint, string path) =>
        writer.PathsWithFingerprint(fingerprint).FirstOrDefault(p => p != path && !Exists(p));

    // Asked of the folder the path lies in; a path in no folder is gone.
    private bool Exists(string path) =>
        Volatile.Read(ref _folders).Select(f => f.Files).FirstOrDefault(f => f.Contains(path)) is { } files &&
        files.Exists(path);

    // "<size>:<sha-256 of the first 64 KiB>". Null when the file cannot be
    // read; such a file is then not recognised when it moves.
    private string? Fingerprint(FolderFiles files, LibraryFile file)
    {
        try
        {
            using var stream = files.OpenRead(file.Path);
            var read = stream.ReadAtLeast(_head, _head.Length, throwOnEndOfStream: false);
            return $"{file.Length}:{Convert.ToHexStringLower(SHA256.HashData(_head.AsSpan(0, read)))}";
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            return null;
        }
    }

    private static Result<ProbedTrack, LibraryError> Probe(FolderFiles files, LibraryFile file)
    {
        try
        {
            using var tagged = TagLib.File.Create(new TagSource(file.Path, () => files.OpenRead(file.Path)));
            var tag = tagged.Tag;
            var duration = tagged.Properties?.Duration;
            var (chapters, cut) = ChapterReader.Read(tagged, file.Path, () => files.OpenRead(file.Path));
            return new ProbedTrack(
                NullIfEmpty(tag.Title) ?? Path.GetFileNameWithoutExtension(file.Path),
                NullIfEmpty(tag.FirstPerformer),
                NullIfEmpty(tag.FirstAlbumArtist),
                NullIfEmpty(tag.Album),
                tag.Year == 0 ? null : (int)tag.Year,
                tag.Disc == 0 ? null : (int)tag.Disc,
                tag.Track == 0 ? null : (int)tag.Track,
                duration > TimeSpan.Zero ? duration : null,
                NullIfEmpty(tag.FirstGenre),
                NullIfEmpty(tag.FirstComposer),
                FormatOf(file.Path, tagged.Properties),
                MediaKindOf(tagged),
                chapters,
                cut,
                new MusicBrainzIds(MusicBrainzId(tag.MusicBrainzReleaseGroupId), MusicBrainzId(tag.MusicBrainzReleaseId),
                    MusicBrainzId(tag.MusicBrainzArtistId), MusicBrainzId(tag.MusicBrainzReleaseArtistId)));
        }
#pragma warning disable CA1031 // A file's bytes are untrusted: whatever TagLib throws on them is that file's failure.
        catch (Exception e)
#pragma warning restore CA1031
        {
            return Result.Fail(LibraryError.UnreadableFile, $"{e.GetType().Name}: {e.Message}");
        }
    }

    // MP4's `stik` (2 is an audiobook), where the file has an Apple tag.
    private static int? MediaKindOf(TagLib.File tagged) =>
        tagged.GetTag(TagLib.TagTypes.Apple) is TagLib.Mpeg4.AppleTag apple &&
        apple.DataBoxes("stik").FirstOrDefault() is { Data.Count: > 0 } box
            ? box.Data[0]
            : null;

    private static string? NullIfEmpty(string? value) => string.IsNullOrWhiteSpace(value) ? null : value.Trim();

    // The first MusicBrainz id of a tag, which may hold several ("/" or ";"
    // between them, a featured artist's after the first); anything else is none.
    private static string? MusicBrainzId(string? value) =>
        value?.Split(['/', ';', ','], StringSplitOptions.TrimEntries | StringSplitOptions.RemoveEmptyEntries)
            .FirstOrDefault() is { } first && Guid.TryParseExact(first, "D", out var id)
            ? id.ToString("D")
            : null;

    // The codec by the file's extension, told apart by TagLib's description
    // where one container holds several (ALAC or AAC in MP4, Opus, Vorbis or
    // FLAC in Ogg); the rest from TagLib's audio properties. Bits per sample
    // is only a lossless file's: a lossy codec has none.
    private static AudioFormat? FormatOf(string path, TagLib.Properties? properties)
    {
        var description = properties?.Description ?? "";
        bool Says(string word) => description.Contains(word, StringComparison.OrdinalIgnoreCase);
        var codec = Path.GetExtension(path).ToUpperInvariant() switch
        {
            ".FLAC" => "FLAC",
            ".WAV" => "WAV",
            ".MP3" => "MP3",
            ".M4A" or ".MP4" => Says("alac") ? "ALAC" : "AAC",
            ".AAC" => "AAC",
            ".OGG" or ".OGA" => Says("opus") ? "Opus" : Says("flac") ? "FLAC" : "Vorbis",
            ".OPUS" => "Opus",
            ".AIF" or ".AIFF" => "AIFF",
            ".WV" => "WavPack",
            ".APE" => "APE",
            ".WMA" => "WMA",
            ".DSF" => "DSD",
            _ => null,
        };
        int? Positive(int? value) => value > 0 ? value : null;
        var lossy = codec is "MP3" or "AAC" or "Opus" or "Vorbis" or "WMA";
        var format = new AudioFormat(codec, Positive(properties?.AudioSampleRate),
            lossy ? null : Positive(properties?.BitsPerSample), Positive(properties?.AudioChannels),
            Positive(properties?.AudioBitrate));
        return format == new AudioFormat() ? null : format;
    }

    private static LibraryFolderView ViewOf(LibraryFolderEntry entry, IndexedFolder? folder)
    {
        var smb = entry.Folder.Smb;
        return new LibraryFolderView(entry.Id, entry.Folder.Type, entry.Folder.Path, entry.Folder.Enabled, entry.Origin,
            folder?.State ?? LibraryFolderState.Disabled, folder?.Detail, folder?.LastPass,
            folder is null ? 0 : Interlocked.Read(ref folder.Scanned),
            folder is null ? 0 : Interlocked.Read(ref folder.Indexed),
            folder is null ? 0 : Interlocked.Read(ref folder.Failed),
            smb?.Login.Host, smb?.Login.Port, smb?.Share, smb?.Directory, smb?.Login.User, smb?.Login.Domain,
            smb?.Login.Password is not null, folder?.LastPassResult, entry.Folder.MediaType);
    }

    [LoggerMessage(Level = LogLevel.Information,
        Message = "self-check: library database {Path} at schema {Version}; {Folders} folder(s) to index")]
    private static partial void LogDatabase(ILogger log, string path, int version, int folders);

    [LoggerMessage(Level = LogLevel.Information, Message = "{Folder}: {Count} track(s) classified again")]
    private static partial void LogReclassified(ILogger log, string folder, int count);

    [LoggerMessage(Level = LogLevel.Information, Message = "forgot {Count} folder(s) the config no longer names")]
    private static partial void LogPruned(ILogger log, int count);

    [LoggerMessage(Level = LogLevel.Information, Message = "forgot library folder {Folder}")]
    private static partial void LogForgotten(ILogger log, string folder);

    [LoggerMessage(Level = LogLevel.Information,
        Message = "indexed {Folder}: {Files} file(s), {Probed} probed, {Moved} moved, {Failed} failed, {Missing} missing, in {Seconds:F1} s")]
    private static partial void LogPass(ILogger log, string folder, int files, int probed, int moved, int failed,
        int missing, double seconds);

    [LoggerMessage(Level = LogLevel.Debug, Message = "moved {From} to {To}")]
    private static partial void LogMoved(ILogger log, string from, string to);

    [LoggerMessage(Level = LogLevel.Warning, Message = "{Path} has more than {Max} chapters; the rest are left out")]
    private static partial void LogChaptersCut(ILogger log, string path, int max);

    [LoggerMessage(Level = LogLevel.Warning, Message = "not indexed {Path}: {Reason}")]
    private static partial void LogNotIndexed(ILogger log, string path, string reason);

    [LoggerMessage(Level = LogLevel.Warning,
        Message = "library folder {Folder} is not reachable; what was indexed is kept: {Reason}")]
    private static partial void LogUnreachable(ILogger log, string folder, string reason);

    [LoggerMessage(Level = LogLevel.Warning,
        Message = "library folder {Folder}: the login was refused; what was indexed is kept: {Reason}")]
    private static partial void LogAuthFailed(ILogger log, string folder, string reason);

    [LoggerMessage(Level = LogLevel.Information, Message = "library folder {Folder} is reachable again")]
    private static partial void LogReachable(ILogger log, string folder);

    [LoggerMessage(Level = LogLevel.Warning,
        Message = "local library folder {Folder} does not exist; it is scanned at startup only")]
    private static partial void LogNotWatched(ILogger log, string folder);

    [LoggerMessage(Level = LogLevel.Error, Message = "indexing {Folder} failed")]
    private static partial void LogScanFailed(ILogger log, Exception exception, string folder);

    [LoggerMessage(Level = LogLevel.Critical, Message = "the indexer stopped: the library database failed")]
    private static partial void LogIndexerFailed(ILogger log, Exception exception);

    private sealed class IndexedFolder(string id, LibraryFolderType type, FolderFiles files, MediaType? declared)
        : IDisposable
    {
        public int Queued;
        // 1 until its stored tracks are classified with Declared on the
        // indexer thread: once per run, and again when Declared changes.
        public int Reclassify = 1;
        public int Settling;
        public long Scanned;
        public long Indexed;
        public long Failed;
        private readonly Lock _state = new();

        public string Id { get; } = id;

        public LibraryFolderType Type { get; } = type;

        // The folder's declared media type (phase 30); replaced when it
        // changes, before Reclassify is set.
        public MediaType? Declared { get; set; } = declared;

        // Replaced when an smb folder's login changes.
        public FolderFiles Files { get; set; } = files;

        public string Root => Files.Root;

        // Removed or disabled: its next job forgets it.
        public volatile bool Removed;

        public LibraryFolderState State { get; private set; } = LibraryFolderState.Pending;

        public string? Detail { get; private set; }

        public DateTimeOffset? LastPass { get; set; }

        // Written on the indexer thread, read by the API: replaced whole.
        public volatile LibraryPassResult? LastPassResult;

        // UTC ticks when its latest completed pass began; 0 before the first.
        // Read and written on the indexer thread only.
        public long LastPassStarted { get; set; }

        // The poll timer of a network or smb folder, the settle timer of a local one.
        public Timer? Timer { get; set; }
        public FileSystemWatcher? Watcher { get; set; }

        // Whether the state changed.
        public bool SetState(LibraryFolderState state, string? detail)
        {
            lock (_state)
            {
                var changed = State != state;
                State = state;
                Detail = detail;
                return changed;
            }
        }

        public void Dispose()
        {
            Watcher?.Dispose();
            Timer?.Dispose();
        }
    }
}
