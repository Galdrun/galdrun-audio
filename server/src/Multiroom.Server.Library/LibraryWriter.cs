using Microsoft.Data.Sqlite;
using Multiroom.Server.Abstractions;

namespace Multiroom.Server.Library;

// A library_file row as the indexer compares it with the disk.
// Tags is the version of the tags it was probed with (LibraryWriter.TagVersion);
// Unreadable, whether that probe failed (the row has an error and no track).
internal sealed record RecordedFile(long Size, long Modified, string? Fingerprint, bool Missing, int Tags,
    bool Unreadable = false);

// What one file's tags say, as the indexer read them.
internal sealed record ProbedTrack(
    string Title,
    string? Artist,
    string? AlbumArtist,
    string? Album,
    int? Year,
    int? Disc,
    int? Number,
    TimeSpan? Duration,
    string? Genre = null,
    string? Composer = null,
    AudioFormat? Format = null,
    int? MediaKind = null,
    IReadOnlyList<ProbedChapter>? Chapters = null,
    bool ChaptersCut = false,
    MusicBrainzIds? Ids = null);

// The MusicBrainz ids Picard writes: each the first id of its tag, lower
// case, or null.
internal sealed record MusicBrainzIds(string? ReleaseGroup, string? Release, string? Artist, string? AlbumArtist);

// The indexer's writes, on the indexer's one connection. Writes are batched:
// a transaction is opened by the first write and committed every BatchSize
// writes or by Flush().
internal sealed class LibraryWriter(SqliteConnection connection) : IDisposable
{
    private const int BatchSize = 100;

    // What a probe reads: 2 since genre, composer and the track's year are
    // read (phase 14), 3 since the audio format is, 4 since the MP4 media
    // kind is (phase 30), 5 since chapter marks are (phase 32), 6 since
    // Picard's MusicBrainz ids are. A file probed below it is probed again.
    public const int TagVersion = 6;

    private SqliteTransaction? _transaction;
    private int _pending;

    // Every file recorded under the folder, as it was when it was recorded.
    public Dictionary<string, RecordedFile> FilesOf(string folder)
    {
        using var command = Command(
            "SELECT path, size, modified, fingerprint, missing_since IS NOT NULL, tags, error IS NOT NULL FROM library_file WHERE folder = $folder",
            ("$folder", folder));
        using var reader = command.ExecuteReader();
        var files = new Dictionary<string, RecordedFile>(StringComparer.Ordinal);
        while (reader.Read())
        {
            files[reader.GetString(0)] = new RecordedFile(reader.GetInt64(1), reader.GetInt64(2),
                reader.IsDBNull(3) ? null : reader.GetString(3), reader.GetBoolean(4), reader.GetInt32(5), reader.GetBoolean(6));
        }
        return files;
    }

    // Recorded files with this fingerprint, anywhere in the library.
    public List<string> PathsWithFingerprint(string fingerprint)
    {
        using var command = Command("SELECT path FROM library_file WHERE fingerprint = $fingerprint",
            ("$fingerprint", fingerprint));
        using var reader = command.ExecuteReader();
        var paths = new List<string>();
        while (reader.Read())
            paths.Add(reader.GetString(0));
        return paths;
    }

    // The recorded file at `from` is now `file`: its row, and so its track id,
    // follows it. Answers what was recorded, so the caller can tell whether
    // the file also changed.
    public RecordedFile Move(string from, string folder, LibraryFile file)
    {
        RecordedFile recorded;
        using (var command = Command("SELECT size, modified, fingerprint, tags, error IS NOT NULL FROM library_file WHERE path = $path",
                   ("$path", from)))
        using (var reader = command.ExecuteReader())
        {
            reader.Read();
            recorded = new RecordedFile(reader.GetInt64(0), reader.GetInt64(1),
                reader.IsDBNull(2) ? null : reader.GetString(2), Missing: false, reader.GetInt32(3), reader.GetBoolean(4));
        }

        Write("""
            UPDATE library_file SET path = $path, folder = $folder, directory = $directory, missing_since = NULL
            WHERE path = $from
            """, ("$path", file.Path), ("$folder", folder), ("$directory", file.Directory), ("$from", from));
        Counted();
        return recorded;
    }

    // The file is where it was recorded, unchanged: shown again if it was
    // missing, and given its fingerprint if it was recorded without one.
    public void Confirm(string path, string? fingerprint)
    {
        Write("""
            UPDATE library_file SET missing_since = NULL, fingerprint = IFNULL(fingerprint, $fingerprint)
            WHERE path = $path
            """, ("$path", path), ("$fingerprint", fingerprint));
        Counted();
    }

    // Hides a file the pass did not find. Answers whether it was shown.
    public bool MarkMissing(string path, long since)
    {
        var marked = Write(
            "UPDATE library_file SET missing_since = $since WHERE path = $path AND missing_since IS NULL RETURNING 1",
            ("$path", path), ("$since", since)) is not null;
        Counted();
        return marked;
    }

    // Forgets files missing since before `cutoff`, with their tracks.
    public void Purge(long cutoff)
    {
        DeleteFiles("missing_since < $cutoff", ("$cutoff", cutoff));
        RemoveOrphans();
    }

    // `declared` is the folder's declared media type, which the classifier
    // weighs first.
    public void Store(string folder, LibraryFile file, string? fingerprint, Result<ProbedTrack, LibraryError> probed,
        MediaType? declared = null)
    {
        var path = file.Path;
        var trackId = TrackOf(path);
        string? error = null;
        if (probed.IsSuccess)
        {
            var tags = probed.Value;
            var verdict = MediaTypeClassifier.Classify(new MediaTypeEvidence(declared, null, tags.MediaKind,
                Path.GetExtension(path), tags.Genre));
            var artistId = ArtistId(tags.Artist);
            long? albumId = tags.Album is null ? null : AlbumId(tags.Album, ArtistId(tags.AlbumArtist ?? tags.Artist), tags.Year);
            (string, object?)[] values =
            [
                ("$title", tags.Title), ("$artist", artistId), ("$album", albumId), ("$disc", tags.Disc),
                ("$number", tags.Number), ("$duration", (long?)tags.Duration?.TotalMilliseconds),
                ("$genre", tags.Genre), ("$composer", tags.Composer), ("$year", tags.Year),
                ("$mediaKind", tags.MediaKind), .. LibraryCatalog.MediaTypeValues(verdict),
                .. LibraryCatalog.FormatValues(tags.Format),
                ("$mbReleaseGroup", tags.Ids?.ReleaseGroup), ("$mbRelease", tags.Ids?.Release),
                ("$mbArtist", tags.Ids?.Artist), ("$mbAlbumArtist", tags.Ids?.AlbumArtist),
            ];
            // `added` is set once, when the track is first recorded.
            trackId = trackId is { } existing
                ? Write("""
                    UPDATE track SET title = $title, artist_id = $artist, album_id = $album, disc = $disc,
                        number = $number, duration_ms = $duration, genre = $genre, composer = $composer, year = $year,
                        codec = $codec, sample_rate = $sampleRate, bit_depth = $bitDepth, channels = $channels,
                        bitrate = $bitrate, media_kind = $mediaKind, media_type = $mediaType,
                        media_type_source = $mediaTypeSource, mb_release_group = $mbReleaseGroup,
                        mb_release = $mbRelease, mb_artist = $mbArtist, mb_album_artist = $mbAlbumArtist
                    WHERE id = $id RETURNING id
                    """, [.. values, ("$id", existing)])
                : Write("""
                    INSERT INTO track (title, artist_id, album_id, disc, number, duration_ms, genre, composer, year, added,
                        codec, sample_rate, bit_depth, channels, bitrate, media_kind, media_type, media_type_source,
                        mb_release_group, mb_release, mb_artist, mb_album_artist)
                    VALUES ($title, $artist, $album, $disc, $number, $duration, $genre, $composer, $year, $added,
                        $codec, $sampleRate, $bitDepth, $channels, $bitrate, $mediaKind, $mediaType, $mediaTypeSource,
                        $mbReleaseGroup, $mbRelease, $mbArtist, $mbAlbumArtist)
                    RETURNING id
                    """, [.. values, ("$added", DateTime.UtcNow.Ticks)]);
            // What the add-ins derived was from the file as it was (phase 12).
            Write("""
                DELETE FROM track_search WHERE rowid = $id;
                INSERT INTO track_search (rowid, title, artist, album) VALUES ($id, $title, $artist, $album);
                DELETE FROM enrichment WHERE track_id = $id;
                DELETE FROM track_chapter WHERE track_id = $id;
                """, ("$id", trackId), ("$title", tags.Title), ("$artist", tags.Artist), ("$album", tags.Album));
            foreach (var (chapter, index) in (tags.Chapters ?? []).Select((c, i) => (c, i)))
            {
                Write("INSERT INTO track_chapter (track_id, idx, title, start_ms) VALUES ($id, $idx, $title, $start)",
                    ("$id", trackId), ("$idx", index), ("$title", chapter.Title), ("$start", chapter.StartMs));
            }
        }
        else
        {
            if (trackId is { } stale)
                DeleteTrack(stale);
            trackId = null;
            error = probed.Failure.Detail;
        }

        Write("""
            INSERT INTO library_file (path, folder, directory, size, modified, track_id, error, fingerprint, tags)
            VALUES ($path, $folder, $directory, $size, $modified, $track, $error, $fingerprint, $tags)
            ON CONFLICT (path) DO UPDATE SET folder = excluded.folder, directory = excluded.directory,
                size = excluded.size, modified = excluded.modified, track_id = excluded.track_id,
                error = excluded.error, fingerprint = excluded.fingerprint, missing_since = NULL, tags = excluded.tags
            """, ("$path", path), ("$folder", folder), ("$directory", file.Directory), ("$size", file.Length),
            ("$modified", file.LastWriteUtc.Ticks), ("$track", trackId), ("$error", error),
            ("$fingerprint", fingerprint), ("$tags", TagVersion));
        Counted();
    }

    // The folder's tracks classified again from what is stored (its declared
    // type changed, or the server started): no probe, ids kept. Answers how
    // many changed.
    public int Reclassify(string folder, MediaType? declared)
    {
        var changed = new List<(long Id, MediaTypeVerdict Verdict)>();
        using (var command = Command("""
                   SELECT t.id, t.media_kind, f.path, t.genre, t.media_type, t.media_type_source
                   FROM track t JOIN library_file f ON f.track_id = t.id WHERE f.folder = $folder
                   """, ("$folder", folder)))
        using (var reader = command.ExecuteReader())
        {
            while (reader.Read())
            {
                var verdict = MediaTypeClassifier.Classify(new MediaTypeEvidence(declared, null,
                    reader.IsDBNull(1) ? null : reader.GetInt32(1), Path.GetExtension(reader.GetString(2)),
                    reader.IsDBNull(3) ? null : reader.GetString(3)));
                var stored = new MediaTypeVerdict(LibraryCatalog.EnumOrNull<MediaType>(reader, 4),
                    LibraryCatalog.EnumOrNull<MediaTypeSource>(reader, 5));
                if (verdict != stored)
                    changed.Add((reader.GetInt64(0), verdict));
            }
        }
        foreach (var (id, verdict) in changed)
        {
            Write("UPDATE track SET media_type = $mediaType, media_type_source = $mediaTypeSource WHERE id = $id",
                [("$id", id), .. LibraryCatalog.MediaTypeValues(verdict)]);
            Counted();
        }
        Flush();
        return changed.Count;
    }

    // Forgets every folder the configuration no longer names. Answers how
    // many there were.
    public int Prune(IReadOnlyCollection<string> folders)
    {
        var recorded = new List<string>();
        using (var command = Command("SELECT DISTINCT folder FROM library_file"))
        using (var reader = command.ExecuteReader())
        {
            while (reader.Read())
                recorded.Add(reader.GetString(0));
        }

        var gone = recorded.Where(f => !folders.Contains(f, StringComparer.Ordinal)).ToList();
        foreach (var folder in gone)
            DeleteFiles("folder = $folder", ("$folder", folder));
        if (gone.Count > 0)
            RemoveOrphans();
        Flush();
        return gone.Count;
    }

    // Forgets one folder: removed or disabled while this server runs.
    public void Forget(string folder)
    {
        DeleteFiles("folder = $folder", ("$folder", folder));
        RemoveOrphans();
        Flush();
    }

    // Albums and artists no track names any more.
    public void RemoveOrphans() => Write("""
        DELETE FROM album WHERE id NOT IN (SELECT album_id FROM track WHERE album_id IS NOT NULL);
        DELETE FROM artist WHERE id NOT IN (
            SELECT artist_id FROM track WHERE artist_id IS NOT NULL
            UNION SELECT artist_id FROM album WHERE artist_id IS NOT NULL);
        """);

    public void Flush()
    {
        _transaction?.Commit();
        _transaction?.Dispose();
        _transaction = null;
        _pending = 0;
    }

    // Rolls back an unflushed batch. The writer can be used again afterwards.
    public void Dispose()
    {
        _transaction?.Dispose();
        _transaction = null;
        _pending = 0;
    }

    private long? TrackOf(string path) =>
        Scalar("SELECT track_id FROM library_file WHERE path = $path", ("$path", path));

    // Deletes the files matching `condition`, and their tracks. Ordered so no
    // foreign key is broken: the search rows, the tracks (which empties the
    // files' track_id and deletes their enrichment), then the files.
    private void DeleteFiles(string condition, params (string Name, object? Value)[] parameters)
    {
        var tracks = $"SELECT track_id FROM library_file WHERE {condition}";
        Write($"""
            DELETE FROM track_search WHERE rowid IN ({tracks});
            DELETE FROM track WHERE id IN ({tracks});
            DELETE FROM library_file WHERE {condition};
            """, parameters);
    }

    private void DeleteTrack(long trackId) => Write("""
        DELETE FROM track_search WHERE rowid = $id;
        DELETE FROM track WHERE id = $id;
        """, ("$id", trackId));

    private long? ArtistId(string? name) => name is null
        ? null
        : Scalar("SELECT id FROM artist WHERE name = $name", ("$name", name)) ??
          Write("INSERT INTO artist (name) VALUES ($name) RETURNING id", ("$name", name));

    private long AlbumId(string title, long? artistId, int? year) =>
        Scalar("SELECT id FROM album WHERE title = $title AND artist_id IS $artist", ("$title", title),
            ("$artist", artistId)) ??
        Write("INSERT INTO album (title, artist_id, year) VALUES ($title, $artist, $year) RETURNING id",
            ("$title", title), ("$artist", artistId), ("$year", year))!.Value;

    private void Counted()
    {
        if (++_pending >= BatchSize)
            Flush();
    }

    // Runs a write inside the batch; answers the first column of the first
    // row, if the statement returns one.
    private long? Write(string sql, params (string Name, object? Value)[] parameters)
    {
        _transaction ??= connection.BeginTransaction();
        return Scalar(sql, parameters);
    }

    private long? Scalar(string sql, params (string Name, object? Value)[] parameters)
    {
        using var command = Command(sql, parameters);
        return command.ExecuteScalar() is long value ? value : null;
    }

    private SqliteCommand Command(string sql, params (string Name, object? Value)[] parameters)
    {
        var command = connection.CreateCommand();
        command.Transaction = _transaction;
#pragma warning disable CA2100 // Constant SQL; every value is a parameter.
        command.CommandText = sql;
#pragma warning restore CA2100
        foreach (var (name, value) in parameters)
            command.Parameters.AddWithValue(name, value ?? DBNull.Value);
        return command;
    }
}
