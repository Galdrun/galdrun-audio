using System.Globalization;
using System.Text.Json;
using System.Text.RegularExpressions;
using Microsoft.Data.Sqlite;
using Multiroom.Server.Abstractions;

namespace Multiroom.Server.Library;

// A track as the artwork add-in looks it up (ArtworkSubjectOf). AlbumArtist
// is the album's artist: its album artist, else its artist. The Mb* are the
// MusicBrainz ids of its tags (Picard's), or null.
public sealed record ArtworkSubject(string Path, string? Album, string? AlbumArtist, string? Artist,
    string? MbReleaseGroup = null, string? MbRelease = null, string? MbArtist = null, string? MbAlbumArtist = null);

// What an artwork reference names (ArtworkNamesOf); any part may be null.
public sealed record ArtworkNames(string? Album, string? AlbumArtist, string? Artist);

// Search, browse and expansion over the library, as the provider named
// ProviderId answers them. Each call reads on a connection of its own, beside
// the indexer's writes (WAL).
//
// The library is complete and ours, so its lists are counted, sorted,
// filtered and indexed by letter (phase 14). The root holds "artists",
// "album-artists", "albums", "genres", "composers", "years", "recent" and
// "folders". Ids: a track is "track:<id>", an album "album:<id>", an artist
// "artist:<id>" (a page of two sections, its albums "artist-albums:<id>" and
// its tracks "artist-tracks:<id>"), a genre "genre:<name>", a composer
// "composer:<name>", a year "year:<n>" and a folder "folder:<path>". A
// track's id follows its file when the file moves (the indexer), so it is
// what a saved playlist holds; PathOf finds the file. Albums and tracks carry
// artwork by the album's (or a loose track's own) id, which the local
// provider reads from the files; artists by their own id, a picture the
// artwork add-in found.
//
// A file the indexer no longer finds is hidden until it is found again or
// forgotten, and so are albums, artists, genres, composers and years with no
// track left to show.
public sealed partial class LibraryCatalog(LibraryDatabase database, string providerId)
{
    // The provider the library answers as (LocalFilesProvider.ProviderId).
    public const string LocalProvider = "local";

    public const string TrackPrefix = "track:";
    public const string AlbumPrefix = "album:";
    private const string ArtistPrefix = "artist:";
    private const string ArtistAlbumsPrefix = "artist-albums:";
    private const string ArtistTracksPrefix = "artist-tracks:";
    private const string GenrePrefix = "genre:";
    private const string ComposerPrefix = "composer:";
    private const string YearPrefix = "year:";
    private const string FolderPrefix = "folder:";
    private const string BookPrefix = "book:";

    private const string NotQueued = "a list is opened, not queued";

    private static readonly string[] AlbumSorts = ["title", "artist", "year", "added"];
    private static readonly string[] NameSort = ["name"];

    // The most tracks "more like this" scores.
    private const int MaxCandidates = 5000;

    // What each trait a track shares with the seed weighs. A playlist counts
    // once per playlist, at most MaxPlaylistsCounted; an era is a year within
    // EraYears of a seed track's.
    private const int ArtistWeight = 3;
    private const int GenreWeight = 2;
    private const int ComposerWeight = 2;
    private const int PlaylistWeight = 2;
    private const int MaxPlaylistsCounted = 3;
    private const int EraWeight = 1;
    private const int EraYears = 5;

    // The tracks with a file to show.
    internal const string Shown = "JOIN library_file f ON f.track_id = t.id AND f.missing_since IS NULL";

    // Read by TrackOf, in this order.
    internal const string TrackColumns = $"""
        SELECT t.id, t.title, ar.name, al.title, t.duration_ms, t.album_id,
               t.codec, t.sample_rate, t.bit_depth, t.channels, t.bitrate, t.media_type, t.media_type_source
        FROM track t
        {Shown}
        LEFT JOIN artist ar ON ar.id = t.artist_id
        LEFT JOIN album al ON al.id = t.album_id
        """;

    // An album with what its shown tracks add up to. WHERE ... GROUP BY al.id follows.
    private const string AlbumColumns = $"""
        SELECT al.id, al.title, ar.name, al.year, COUNT(t.id), SUM(t.duration_ms), MAX(t.added)
        FROM album al
        LEFT JOIN artist ar ON ar.id = al.artist_id
        JOIN track t ON t.album_id = al.id
        {Shown}
        """;

    private const string Paging = " LIMIT $limit OFFSET $offset";

    private static readonly (string Id, string Title, BrowseLayout Layout)[] RootLists =
    [
        ("artists", "Artists", BrowseLayout.List),
        ("album-artists", "Album artists", BrowseLayout.List),
        ("albums", "Albums", BrowseLayout.Grid),
        ("genres", "Genres", BrowseLayout.List),
        ("composers", "Composers", BrowseLayout.List),
        ("years", "Years", BrowseLayout.List),
        ("recent", "Recently added", BrowseLayout.Grid),
        ("folders", "Folders", BrowseLayout.List),
        ("audiobooks", "Audiobooks", BrowseLayout.Grid),
    ];

    // A book ripped as one album per disc: the title before a disc suffix
    // (" CD 2", " - Disc 2", " (2/5)", "[CD 2]"), and the disc it names.
    [GeneratedRegex(@"^(?<title>.*?)[\s,:\-–]*[\(\[]?\s*(?:(?:cd|disc|disk)\s*(?<disc>\d+)(?:\s*(?:/|of)\s*\d+)?|(?<disc>\d+)\s*/\s*\d+)\s*[\)\]]?\s*$",
        RegexOptions.IgnoreCase | RegexOptions.CultureInvariant)]
    private static partial Regex DiscSuffix();

    // Whether the track `t` is one the house keeps: in a playlist (or a
    // book's parts there), a favourite (or its parts), or liked, by anyone.
    internal const string Kept = """
        (EXISTS (SELECT 1 FROM playlist_item k WHERE k.provider_id = $provider AND k.provider_item_id = 'track:' || t.id)
         OR EXISTS (SELECT 1 FROM playlist_item_part k WHERE k.provider_id = $provider AND k.provider_item_id = 'track:' || t.id)
         OR EXISTS (SELECT 1 FROM favorite k WHERE k.kind = 'track' AND k.provider_id = $provider AND k.provider_item_id = 'track:' || t.id)
         OR EXISTS (SELECT 1 FROM favorite_part k WHERE k.provider_id = $provider AND k.provider_item_id = 'track:' || t.id)
         OR EXISTS (SELECT 1 FROM liked k WHERE k.kind = 'track' AND k.provider_id = $provider AND k.provider_item_id = 'track:' || t.id))
        """;

    // Every word a prefix, all words required, over title, artist and album.
    // Tracks the house keeps (Kept) come first, then by relevance; albums
    // match on their title or artist, and artists on their name. The library
    // holds no playlists or stations.
    public Task<Result<CatalogItems, ProviderError>> SearchAsync(string text, SearchKinds kinds, int offset,
        int limit, CancellationToken cancellationToken)
    {
        var match = MatchExpression(text);
        if (match is null || (kinds & (SearchKinds.Tracks | SearchKinds.Albums | SearchKinds.Artists)) == 0)
            return Task.FromResult<Result<CatalogItems, ProviderError>>(CatalogItems.Empty);

        return Read<CatalogItems>(connection => new CatalogItems(
            !kinds.HasFlag(SearchKinds.Tracks) ? [] : Tracks(connection, TrackColumns + """

                JOIN track_search ON track_search.rowid = t.id
                WHERE track_search MATCH $match
                ORDER BY
                """ + Kept + " DESC, track_search.rank", offset, limit, ("$match", match), ("$provider", providerId)),
            !kinds.HasFlag(SearchKinds.Albums) ? [] : [.. AlbumRows(connection, """
                WHERE al.id IN (SELECT t.album_id FROM track_search JOIN track t ON t.id = track_search.rowid
                                WHERE track_search MATCH $match)
                """, ("$match", $"{{album artist}} : ({match})"))
                .OrderBy(a => a.Title, BrowseIndex.Order).Skip(offset).Take(limit).Select(ToAlbum)],
            !kinds.HasFlag(SearchKinds.Artists) ? [] : Rows(connection, """
                SELECT ar.id, ar.name FROM artist ar
                WHERE ar.id IN (SELECT t.artist_id FROM track_search JOIN track t ON t.id = track_search.rowid
                                JOIN library_file f ON f.track_id = t.id AND f.missing_since IS NULL
                                WHERE track_search MATCH $match)
                ORDER BY ar.name
                """ + Paging, r => new Artist(providerId, ArtistPrefix + r.GetInt64(0), r.GetString(1),
                    new ArtworkRef(providerId, ArtistPrefix + r.GetInt64(0))),
                ("$match", $"artist : ({match})"), ("$offset", offset), ("$limit", limit)),
            [], []));
    }

    public Task<Result<BrowsePage, ProviderError>> BrowseAsync(BrowseRequest request,
        CancellationToken cancellationToken) => Read(connection => Browse(connection, request));

    // A playable node's tracks in order, at most `limit`, with the chapter
    // marks of its audiobook tracks (phase 32). Nothing is refused: a file
    // that went away is found at stage time, like any other.
    public Task<Result<ExpandedContainer, ProviderError>> ExpandAsync(string nodeId, int limit,
        CancellationToken cancellationToken) => Read<ExpandedContainer>(connection =>
    {
        var query = TracksOf(connection, nodeId);
        if (!query.IsSuccess)
            return query.Failure;
        var (where, order, parameters) = query.Value;
        var tracks = Tracks(connection, TrackColumns + where + order, 0, limit + 1, parameters);
        List<Track> kept = [.. tracks.Take(limit)];
        return new ExpandedContainer(kept, [], tracks.Count > limit,
            ChaptersOf(connection, [.. kept.Where(t => t.MediaType == MediaType.Audiobook)]));
    });

    // Each track's chapters, ends filled in: the next one's start, or the
    // track's length for the last. Tracks without chapters are left out.
    private static Dictionary<string, IReadOnlyList<Chapter>>? ChaptersOf(SqliteConnection connection,
        IReadOnlyList<Track> tracks)
    {
        var chapters = new Dictionary<string, IReadOnlyList<Chapter>>(StringComparer.Ordinal);
        foreach (var track in tracks)
        {
            if (Id(track.ProviderItemId, TrackPrefix) is not { } id)
                continue;
            var marks = Rows(connection, "SELECT title, start_ms FROM track_chapter WHERE track_id = $id ORDER BY idx",
                r => (Title: r.GetString(0), Start: r.GetInt64(1)), ("$id", id));
            if (marks.Count < 2)
                continue;
            var length = (long?)track.Duration?.TotalMilliseconds;
            chapters[track.ProviderItemId] = [.. marks.Select((m, i) =>
                new Chapter(m.Title, m.Start, i + 1 < marks.Count ? marks[i + 1].Start : length))];
        }
        return chapters.Count == 0 ? null : chapters;
    }

    // The file of a track this catalog answered, while it is where the
    // indexer last found it.
    public Result<string, ProviderError> PathOf(string trackItemId)
    {
        if (Id(trackItemId, TrackPrefix) is not { } id)
            return Result.Fail(ProviderError.NotFound, $"{trackItemId} is no library track");
        return FileOf("SELECT path FROM library_file WHERE track_id = $id AND missing_since IS NULL", id, trackItemId);
    }

    // The file whose picture shows an album ("album:<id>": its first track in
    // order) or a loose track ("track:<id>"), and its write time as the
    // indexer last saw it, so a cached picture is told from a changed one.
    public Result<(string Path, long Modified), ProviderError> ArtworkFileOf(string reference)
    {
        var album = Id(reference, AlbumPrefix);
        var track = Id(reference, TrackPrefix);
        if (album is null && track is null)
            return Result.Fail(ProviderError.NotFound, $"no artwork {reference}");
        try
        {
            using var connection = database.Open();
            var files = Rows(connection, album is not null
                ? """
                  SELECT f.path, f.modified FROM track t JOIN library_file f ON f.track_id = t.id AND f.missing_since IS NULL
                  WHERE t.album_id = $id ORDER BY t.disc, t.number, f.path LIMIT 1
                  """
                : "SELECT path, modified FROM library_file WHERE track_id = $id AND missing_since IS NULL",
                r => (r.GetString(0), r.GetInt64(1)), ("$id", album ?? track!.Value));
            return files.Count == 0 ? Result.Fail(ProviderError.NotFound, $"no file for {reference}") : files[0];
        }
        catch (SqliteException e)
        {
            return Result.Fail(ProviderError.Failed, e.Message);
        }
    }

    // The names an artwork reference stands for, which the stored art is kept
    // by: an artist's ("artist:<id>": Artist only), an album's ("album:<id>":
    // its title, and its album artist, else its artist, as both artists) or
    // a loose track's ("track:<id>": its artist).
    public Result<ArtworkNames, ProviderError> ArtworkNamesOf(string reference)
    {
        var (sql, id) = Id(reference, ArtistPrefix) is { } artist
            ? ("SELECT NULL, NULL, name FROM artist WHERE id = $id", artist)
            : Id(reference, AlbumPrefix) is { } album
                ? ("SELECT al.title, ar.name, ar.name FROM album al LEFT JOIN artist ar ON ar.id = al.artist_id WHERE al.id = $id",
                    album)
                : Id(reference, TrackPrefix) is { } track
                    ? ("""
                       SELECT al.title, aa.name, ar.name FROM track t
                       LEFT JOIN album al ON al.id = t.album_id
                       LEFT JOIN artist aa ON aa.id = al.artist_id
                       LEFT JOIN artist ar ON ar.id = t.artist_id
                       WHERE t.id = $id
                       """, track)
                    : ((string?)null, 0L);
        if (sql is null)
            return Result.Fail(ProviderError.NotFound, $"no artwork {reference}");
        try
        {
            using var connection = database.Open();
            var names = Rows(connection, sql,
                r => new ArtworkNames(StringOrNull(r, 0), StringOrNull(r, 1), StringOrNull(r, 2)), ("$id", id));
            return names.Count == 0 ? Result.Fail(ProviderError.NotFound, $"no artwork {reference}") : names[0];
        }
        catch (SqliteException e)
        {
            return Result.Fail(ProviderError.Failed, e.Message);
        }
    }

    // What the artwork add-in looks up for a track: its file, while it is
    // where the indexer last found it, its album's title and artist, its own
    // artist, and the MusicBrainz ids its tags carry (Picard's).
    public Result<ArtworkSubject, ProviderError> ArtworkSubjectOf(string trackItemId)
    {
        if (Id(trackItemId, TrackPrefix) is not { } id)
            return Result.Fail(ProviderError.NotFound, $"{trackItemId} is no library track");
        try
        {
            using var connection = database.Open();
            var subjects = Rows(connection, """
                SELECT f.path, al.title, aa.name, ar.name,
                       t.mb_release_group, t.mb_release, t.mb_artist, t.mb_album_artist
                FROM track t
                JOIN library_file f ON f.track_id = t.id AND f.missing_since IS NULL
                LEFT JOIN album al ON al.id = t.album_id
                LEFT JOIN artist aa ON aa.id = al.artist_id
                LEFT JOIN artist ar ON ar.id = t.artist_id
                WHERE t.id = $id
                """, r => new ArtworkSubject(r.GetString(0), StringOrNull(r, 1), StringOrNull(r, 2), StringOrNull(r, 3),
                    StringOrNull(r, 4), StringOrNull(r, 5), StringOrNull(r, 6), StringOrNull(r, 7)),
                ("$id", id));
            return subjects.Count == 0 ? Result.Fail(ProviderError.NotFound, $"no file for {trackItemId}") : subjects[0];
        }
        catch (SqliteException e)
        {
            return Result.Fail(ProviderError.Failed, e.Message);
        }
    }

    // "More like this" over the library (phase 12): its tracks that share an
    // artist (as artist or album artist), a genre, a composer or a playlist
    // with a seed track, scored by what they share; a year within EraYears of
    // a seed track's adds to that, but is not enough alone. A seed track of
    // another provider counts by its artist's name. The seed's own library
    // tracks are left out. Ranked by score, then title; each with one line
    // saying why.
    public Task<Result<IReadOnlyList<Recommendation>, ProviderError>> SimilarAsync(IReadOnlyList<Track> seed,
        int limit, CancellationToken cancellationToken) =>
        Read<IReadOnlyList<Recommendation>>(connection => Similar(connection, [.. seed], limit));

    private List<Recommendation> Similar(SqliteConnection connection, List<Track> seed, int limit)
    {
        if (seed.Count == 0)
            return [];
        var own = seed.Where(t => t.ProviderId == providerId).Select(t => Id(t.ProviderItemId, TrackPrefix))
            .OfType<long>().ToHashSet();
        var traits = Rows(connection, $"""
            SELECT t.artist_id, al.artist_id, t.genre, t.composer, t.year FROM track t
            LEFT JOIN album al ON al.id = t.album_id
            WHERE t.id IN ({In("$s", own.Count)})
            """, r => (Artist: LongOrNull(r, 0), AlbumArtist: LongOrNull(r, 1), Genre: StringOrNull(r, 2),
                Composer: StringOrNull(r, 3), Year: LongOrNull(r, 4)), Values("$s", own));
        var names = seed.Where(t => t.ProviderId != providerId && t.Artist is not null).Select(t => t.Artist!)
            .Distinct(StringComparer.OrdinalIgnoreCase).ToList();
        var artists = traits.SelectMany(t => new[] { t.Artist, t.AlbumArtist }).OfType<long>()
            .Concat(Rows(connection, $"SELECT id FROM artist WHERE name IN ({In("$n", names.Count)})",
                r => r.GetInt64(0), Values("$n", names)))
            .ToHashSet();
        var genres = traits.Select(t => t.Genre).OfType<string>().ToHashSet(StringComparer.OrdinalIgnoreCase);
        var composers = traits.Select(t => t.Composer).OfType<string>().ToHashSet(StringComparer.OrdinalIgnoreCase);
        var years = traits.Select(t => t.Year).OfType<long>().ToList();

        // How many playlists each library track shares with a seed track.
        var together = new Dictionary<long, long>();
        var pairs = string.Join(", ", seed.Select((_, i) => $"($p{Invariant(i)}, $i{Invariant(i)})"));
        foreach (var (item, count) in Rows(connection, $"""
                     SELECT other.provider_item_id, COUNT(DISTINCT other.playlist_id) FROM playlist_item seed
                     JOIN playlist_item other ON other.playlist_id = seed.playlist_id AND other.provider_id = $provider
                     WHERE (seed.provider_id, seed.provider_item_id) IN (VALUES {pairs})
                     GROUP BY other.provider_item_id
                     """, r => (r.GetString(0), r.GetInt64(1)),
                     [("$provider", providerId), .. seed.SelectMany((t, i) => new (string, object)[]
                         { ("$p" + Invariant(i), t.ProviderId), ("$i" + Invariant(i), t.ProviderItemId) })]))
        {
            if (Id(item, TrackPrefix) is { } id && !own.Contains(id))
                together[id] = count;
        }

        var candidates = Rows(connection, $"""
            SELECT t.id, t.title, ar.name, al.title, t.duration_ms, t.album_id, t.codec, t.sample_rate,
                   t.bit_depth, t.channels, t.bitrate, t.media_type, t.media_type_source,
                   t.artist_id, al.artist_id, alar.name, t.genre, t.composer, t.year
            FROM track t
            {Shown}
            LEFT JOIN artist ar ON ar.id = t.artist_id
            LEFT JOIN album al ON al.id = t.album_id
            LEFT JOIN artist alar ON alar.id = al.artist_id
            WHERE (t.media_type IS NULL OR t.media_type = 'music')
              AND (t.artist_id IN ({In("$a", artists.Count)}) OR al.artist_id IN ({In("$a", artists.Count)})
               OR t.genre IN ({In("$g", genres.Count)}) OR t.composer IN ({In("$c", composers.Count)})
               OR t.id IN ({In("$t", together.Count)}))
            LIMIT {Invariant(MaxCandidates)}
            """, r => Score(r, artists, genres, composers, years, together),
            [.. Values("$a", artists), .. Values("$g", genres), .. Values("$c", composers), .. Values("$t", together.Keys)]);
        return [.. candidates.Where(c => !own.Contains(c.Id))
            .OrderByDescending(c => c.Recommendation.Score)
            .ThenBy(c => c.Recommendation.Track.Title, BrowseIndex.Order)
            .ThenBy(c => c.Id)
            .Take(limit)
            .Select(c => c.Recommendation)];
    }

    // A candidate row of Similar: what it shares, weighed and said.
    private (long Id, Recommendation Recommendation) Score(SqliteDataReader r, HashSet<long> artists,
        HashSet<string> genres, HashSet<string> composers, List<long> years, Dictionary<long, long> together)
    {
        var track = TrackOf(r);
        var id = r.GetInt64(0);
        var score = 0;
        var why = new List<string>();
        if (LongOrNull(r, 13) is { } artist && artists.Contains(artist))
        {
            score += ArtistWeight;
            why.Add($"same artist {track.Artist}");
        }
        else if (LongOrNull(r, 14) is { } albumArtist && artists.Contains(albumArtist))
        {
            score += ArtistWeight;
            why.Add($"same album artist {StringOrNull(r, 15)}");
        }
        if (StringOrNull(r, 16) is { } genre && genres.Contains(genre))
        {
            score += GenreWeight;
            why.Add($"genre {genre}");
        }
        if (StringOrNull(r, 17) is { } composer && composers.Contains(composer))
        {
            score += ComposerWeight;
            why.Add($"composer {composer}");
        }
        if (together.TryGetValue(id, out var playlists))
        {
            score += PlaylistWeight * (int)Math.Min(playlists, MaxPlaylistsCounted);
            why.Add(playlists == 1 ? "in a playlist with the seed" : $"in {Invariant(playlists)} playlists with the seed");
        }
        if (LongOrNull(r, 18) is { } year && years.Any(y => Math.Abs(y - year) <= EraYears))
        {
            score += EraWeight;
            why.Add($"era {Invariant(year)}");
        }
        return (id, new Recommendation(track, score, string.Join(", ", why)));
    }

    // "$p0, $p1, ..." for `count` values; empty for none, which SQLite takes
    // as an IN list that matches nothing.
    private static string In(string prefix, int count) =>
        string.Join(", ", Enumerable.Range(0, count).Select(i => prefix + Invariant(i)));

    private static (string, object)[] Values<T>(string prefix, IEnumerable<T> values)
        where T : notnull =>
        [.. values.Select((v, i) => (prefix + Invariant(i), (object)v))];

    private static long? LongOrNull(SqliteDataReader reader, int column) =>
        reader.IsDBNull(column) ? null : reader.GetInt64(column);

    // Null when the text holds no word to search for.
    internal static string? MatchExpression(string text)
    {
        var words = text.Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries)
            .Where(w => w.Any(char.IsLetterOrDigit))
            .Select(w => "\"" + w.Replace("\"", "\"\"", StringComparison.Ordinal) + "\"*")
            .ToList();
        return words.Count == 0 ? null : string.Join(' ', words);
    }

    private Result<BrowsePage, ProviderError> Browse(SqliteConnection connection, BrowseRequest r)
    {
        switch (r.NodeId)
        {
            case null:
                return Plain(r, () => new BrowsePage(
                    [.. RootLists.Skip(r.Offset).Take(r.Limit).Select(l => Node(l.Id, l.Title, BrowseKind.Folder))],
                    CatalogItems.Empty, Total: RootLists.Length));
            case "artists":
                return Names(r, List("artists", "Artists"), ArtistNodes(connection, """
                    WHERE ar.id IN (SELECT t.artist_id FROM track t JOIN library_file f ON f.track_id = t.id AND f.missing_since IS NULL
                                    UNION SELECT al.artist_id FROM album al JOIN track t ON t.album_id = al.id
                                    JOIN library_file f ON f.track_id = t.id AND f.missing_since IS NULL)
                    """));
            case "album-artists":
                return Names(r, List("album-artists", "Album artists"), ArtistNodes(connection, """
                    WHERE ar.id IN (SELECT al.artist_id FROM album al JOIN track t ON t.album_id = al.id
                                    JOIN library_file f ON f.track_id = t.id AND f.missing_since IS NULL)
                    """));
            case "albums":
                return Albums(r, List("albums", "Albums"), AlbumRows(connection, ""), AlbumSorts);
            case "recent":
                return Plain(r, () => Page(r, List("recent", "Recently added"),
                    [.. AlbumRows(connection, "").OrderByDescending(a => a.Added).ThenByDescending(a => a.Id).Select(AlbumNode)],
                    BrowseLayout.Grid));
            case "genres":
                return Names(r, List("genres", "Genres"), Rows(connection, $"""
                    SELECT MIN(t.genre COLLATE BINARY), COUNT(DISTINCT t.album_id) FROM track t {Shown}
                    WHERE t.genre IS NOT NULL GROUP BY t.genre
                    """, g => Node(GenrePrefix + g.GetString(0), g.GetString(0), BrowseKind.Genre,
                        Count(g.GetInt64(1), "album"), playable: true)));
            case "composers":
                return Names(r, List("composers", "Composers"), Rows(connection, $"""
                    SELECT MIN(t.composer COLLATE BINARY), COUNT(*) FROM track t {Shown}
                    WHERE t.composer IS NOT NULL GROUP BY t.composer
                    """, c => Node(ComposerPrefix + c.GetString(0), c.GetString(0), BrowseKind.Composer,
                        Count(c.GetInt64(1), "track"), playable: true)));
            case "years":
                return Plain(r, () => Page(r, List("years", "Years"), Rows(connection, $"""
                    SELECT t.year, COUNT(DISTINCT t.album_id) FROM track t {Shown}
                    WHERE t.year IS NOT NULL GROUP BY t.year ORDER BY t.year DESC
                    """, y => Node(YearPrefix + Invariant(y.GetInt64(0)), Invariant(y.GetInt64(0)), BrowseKind.Year,
                        Count(y.GetInt64(1), "album"), playable: true)), BrowseLayout.List));
            case "audiobooks":
                return Plain(r, () => Page(r, List("audiobooks", "Audiobooks"),
                    [.. Books(connection).Select(b => BookNode(b))], BrowseLayout.Grid));
            case "folders":
                return Plain(r, () => Page(r, List("folders", "Folders"),
                    [.. Strings(connection, "SELECT DISTINCT folder FROM library_file WHERE missing_since IS NULL ORDER BY folder")
                        .Select(f => Node(FolderPrefix + f, f, BrowseKind.Folder, playable: true))],
                    BrowseLayout.List));
        }

        var nodeId = r.NodeId;
        if (Id(nodeId, ArtistPrefix) is { } artist)
            return Artist(connection, r, artist);
        if (Id(nodeId, ArtistAlbumsPrefix) is { } artistAlbums)
        {
            return ArtistNode(connection, artistAlbums) is not { } of
                ? Result.Fail(ProviderError.NotFound, $"no artist {nodeId}")
                : Plain(r, () => Page(r, of with { NodeId = nodeId, Title = $"Albums of {of.Title}", Playable = false, Unplayable = NotQueued },
                    [.. ArtistAlbums(connection, artistAlbums).Select(AlbumNode)], BrowseLayout.Grid));
        }
        if (Id(nodeId, ArtistTracksPrefix) is { } artistTracks)
        {
            return ArtistNode(connection, artistTracks) is not { } of
                ? Result.Fail(ProviderError.NotFound, $"no artist {nodeId}")
                : TrackPage(connection, r, of with { NodeId = nodeId, Title = $"Tracks of {of.Title}" }, TracksOf(connection, nodeId).Value);
        }
        if (Id(nodeId, AlbumPrefix) is { } album)
        {
            var rows = AlbumRows(connection, "WHERE al.id = $v", ("$v", album));
            return rows.Count == 0
                ? Result.Fail(ProviderError.NotFound, $"no album {nodeId}")
                : TrackPage(connection, r, AlbumHeader(rows[0]), TracksOf(connection, nodeId).Value);
        }
        if (Id(nodeId, BookPrefix) is { } bookId)
        {
            return Books(connection).FirstOrDefault(b => b.Id == bookId) is not { } book
                ? Result.Fail(ProviderError.NotFound, $"no book {nodeId}")
                : TrackPage(connection, r, BookNode(book, header: true), TracksOf(connection, nodeId).Value);
        }
        if (Named(nodeId, GenrePrefix) is { } genre)
        {
            var rows = AlbumRows(connection, "WHERE t.genre = $v", ("$v", genre));
            return rows.Count == 0
                ? Result.Fail(ProviderError.NotFound, $"no genre {genre}")
                : Albums(r, Node(nodeId, genre, BrowseKind.Genre, Count(rows.Count, "album"), playable: true), rows,
                    ["title", "artist", "year"]);
        }
        if (Named(nodeId, ComposerPrefix) is { } composer)
        {
            var query = TracksOf(connection, nodeId).Value;
            return Count(connection, query) == 0
                ? Result.Fail(ProviderError.NotFound, $"no composer {composer}")
                : TrackPage(connection, r, Node(nodeId, composer, BrowseKind.Composer, playable: true), query);
        }
        if (Id(nodeId, YearPrefix) is { } year)
        {
            var rows = AlbumRows(connection, "WHERE t.year = $v", ("$v", year));
            return rows.Count == 0
                ? Result.Fail(ProviderError.NotFound, $"no year {year}")
                : Albums(r, Node(nodeId, Invariant(year), BrowseKind.Year, Count(rows.Count, "album"), playable: true),
                    rows, ["title", "artist"]);
        }
        if (nodeId.StartsWith(FolderPrefix, StringComparison.Ordinal))
            return Plain(r, () => Folder(connection, nodeId[FolderPrefix.Length..], r.Offset, r.Limit));

        return Result.Fail(ProviderError.NotFound, $"no node {nodeId}");
    }

    // Two sections, each a first page: the artist's albums (as album artist
    // or on a track) and its tracks. Each opens on its own for the rest.
    private Result<BrowsePage, ProviderError> Artist(SqliteConnection connection, BrowseRequest r, long id)
    {
        if (ArtistNode(connection, id) is not { } artist)
            return Result.Fail(ProviderError.NotFound, $"no artist {r.NodeId}");
        if (Refuse(r, [], false, false) is { } refused)
            return refused;
        if (r.Offset != 0)
            return Result.Fail(ProviderError.InvalidArgument, $"{r.NodeId} is a page of sections; each pages on its own");

        var albums = ArtistAlbums(connection, id);
        var tracksQuery = TracksOf(connection, ArtistTracksPrefix + Invariant(id)).Value;
        var tracks = Tracks(connection, TrackColumns + tracksQuery.Where + tracksQuery.Order, 0, r.Limit, tracksQuery.Parameters);
        return new BrowsePage([], CatalogItems.Empty, artist, Sections:
        [
            new BrowseSection("Albums", [.. albums.Take(r.Limit).Select(AlbumNode)], CatalogItems.Empty,
                Node(ArtistAlbumsPrefix + Invariant(id), "Albums", BrowseKind.Section), albums.Count, BrowseLayout.Grid),
            new BrowseSection("Tracks", [], CatalogItems.Empty with { Tracks = tracks },
                Node(ArtistTracksPrefix + Invariant(id), "Tracks", BrowseKind.Section, playable: true),
                (int)Count(connection, tracksQuery)),
        ]);
    }

    // A list of names (artists, genres, composers): by name, filtered, by letter.
    private static Result<BrowsePage, ProviderError> Names(BrowseRequest r, BrowseNode header, List<BrowseNode> nodes)
    {
        if (Refuse(r, NameSort, true, true) is { } refused)
            return refused;
        var ordered = nodes.OrderBy(n => n.Title, BrowseIndex.Order).ThenBy(n => n.NodeId, StringComparer.Ordinal)
            .Where(n => r.Filter is null || BrowseIndex.Matches(r.Filter, n.Title)).ToList();
        return Lettered(r, header, ordered, [.. ordered.Select(n => n.Title)], NameSort, "name", BrowseLayout.List);
    }

    // A grid of albums in one of `sorts`, title first; filtered by title or
    // artist; by letter when sorted by title or artist.
    private Result<BrowsePage, ProviderError> Albums(BrowseRequest r, BrowseNode header, List<AlbumRow> rows,
        string[] sorts)
    {
        var lettered = r.Sort is null or "title" or "artist";
        if (Refuse(r, sorts, true, lettered) is { } refused)
            return refused;
        var sort = r.Sort ?? "title";
        var filtered = rows.Where(a => r.Filter is null || BrowseIndex.Matches(r.Filter, a.Title, a.Artist));
        var ordered = (sort switch
        {
            "artist" => filtered.OrderBy(a => a.Artist ?? "", BrowseIndex.Order).ThenBy(a => a.Year).ThenBy(a => a.Title, BrowseIndex.Order),
            "year" => filtered.OrderByDescending(a => a.Year ?? int.MinValue).ThenBy(a => a.Title, BrowseIndex.Order),
            "added" => filtered.OrderByDescending(a => a.Added ?? long.MinValue).ThenBy(a => a.Title, BrowseIndex.Order),
            _ => filtered.OrderBy(a => a.Title, BrowseIndex.Order).ThenBy(a => a.Artist ?? "", BrowseIndex.Order),
        }).ThenBy(a => a.Id).ToList();
        var keys = lettered ? ordered.Select(a => sort == "artist" ? a.Artist ?? "" : a.Title).ToList() : null;
        return Lettered(r, header, [.. ordered.Select(AlbumNode)], keys, sorts, sort, BrowseLayout.Grid);
    }

    private static BrowsePage Lettered(BrowseRequest r, BrowseNode header, List<BrowseNode> ordered,
        List<string>? keys, IReadOnlyList<string> sorts, string sort, BrowseLayout layout)
    {
        var letters = keys is null ? null : BrowseIndex.Of(keys);
        var offset = r.Letter is { } letter ? BrowseIndex.OffsetOf(letters!, letter, ordered.Count) : r.Offset;
        return new BrowsePage([.. ordered.Skip(offset).Take(r.Limit)], CatalogItems.Empty, header, ordered.Count, null,
            sorts, sort, Filterable: true, letters, layout);
    }

    private static BrowsePage Page(BrowseRequest r, BrowseNode header, List<BrowseNode> nodes, BrowseLayout layout) =>
        new([.. nodes.Skip(r.Offset).Take(r.Limit)], CatalogItems.Empty, header, nodes.Count, Layout: layout);

    private Result<BrowsePage, ProviderError> TrackPage(SqliteConnection connection, BrowseRequest r, BrowseNode header,
        (string Where, string Order, (string, object)[] Parameters) query)
    {
        if (Refuse(r, [], false, false) is { } refused)
            return refused;
        return new BrowsePage([], CatalogItems.Empty with
        {
            Tracks = Tracks(connection, TrackColumns + query.Where + query.Order, r.Offset, r.Limit, query.Parameters),
        }, header, (int)Count(connection, query));
    }

    // What a playable node queues, as a WHERE and an ORDER over TrackColumns.
    private static Result<(string Where, string Order, (string, object)[] Parameters), ProviderError> TracksOf(
        SqliteConnection connection, string nodeId)
    {
        const string ByAlbum = " ORDER BY al.title COLLATE NOCASE, t.album_id, t.disc, t.number, t.title";
        if (Id(nodeId, AlbumPrefix) is { } album)
            return (" WHERE t.album_id = $v", " ORDER BY t.disc, t.number, t.title, f.path", [("$v", album)]);
        if ((Id(nodeId, ArtistPrefix) ?? Id(nodeId, ArtistTracksPrefix)) is { } artist)
        {
            return Exists(connection, "SELECT 1 FROM artist WHERE id = $id", artist)
                ? (" WHERE t.artist_id = $v", ByAlbum, [("$v", artist)])
                : Result.Fail(ProviderError.NotFound, $"no artist {nodeId}");
        }
        if (Id(nodeId, BookPrefix) is { } bookId)
        {
            if (Books(connection).FirstOrDefault(b => b.Id == bookId) is not { } book)
                return Result.Fail(ProviderError.NotFound, $"no book {nodeId}");
            // Disc by disc as the albums are ordered, then by the tags.
            var albums = book.Albums.Select((a, i) => ($"$b{Invariant(i)}", (object)a)).ToArray();
            return ($" WHERE t.album_id IN ({string.Join(", ", albums.Select(a => a.Item1))})",
                $" ORDER BY CASE t.album_id {string.Join(" ", albums.Select((a, i) => $"WHEN {a.Item1} THEN {Invariant(i)}"))} END, t.disc, t.number, t.title, f.path",
                albums);
        }
        if (Named(nodeId, GenrePrefix) is { } genre)
            return (" WHERE t.genre = $v", ByAlbum, [("$v", genre)]);
        if (Named(nodeId, ComposerPrefix) is { } composer)
            return (" WHERE t.composer = $v", ByAlbum, [("$v", composer)]);
        if (Id(nodeId, YearPrefix) is { } year)
            return (" WHERE t.year = $v", ByAlbum, [("$v", year)]);
        if (nodeId.StartsWith(FolderPrefix, StringComparison.Ordinal))
        {
            var directory = nodeId[FolderPrefix.Length..];
            return (" WHERE (f.directory = $v OR substr(f.directory, 1, length($prefix)) = $prefix)", " ORDER BY f.path",
                [("$v", directory), ("$prefix", WithSeparator(directory))]);
        }
        if (nodeId is "artists" or "album-artists" or "albums" or "genres" or "composers" or "years" or "recent"
            or "folders" or "audiobooks" || Id(nodeId, ArtistAlbumsPrefix) is not null)
            return Result.Fail(ProviderError.InvalidArgument, $"{nodeId}: {NotQueued}");
        return Result.Fail(ProviderError.NotFound, $"no node {nodeId}");
    }

    // Its subfolders that hold indexed tracks, and the tracks directly in it.
    private Result<BrowsePage, ProviderError> Folder(SqliteConnection connection, string directory, int offset,
        int limit)
    {
        var prefix = WithSeparator(directory);
        var separator = prefix[^1];
        var below = Strings(connection, """
            SELECT DISTINCT directory FROM library_file
            WHERE track_id IS NOT NULL AND missing_since IS NULL AND substr(directory, 1, length($prefix)) = $prefix
            """, ("$prefix", prefix));
        var subfolders = below.Select(d => d[prefix.Length..].Split(separator)[0])
            .Distinct(StringComparer.Ordinal)
            .Order(StringComparer.OrdinalIgnoreCase)
            .ToList();
        var tracks = Tracks(connection, TrackColumns + " WHERE f.directory = $directory ORDER BY f.path",
            offset, limit, ("$directory", directory));
        if (subfolders.Count == 0 && tracks.Count == 0 &&
            !Exists(connection, "SELECT 1 FROM library_file WHERE directory = $id", directory))
            return Result.Fail(ProviderError.NotFound, $"no folder {directory}");

        var name = directory.TrimEnd(separator);
        return new BrowsePage(
            [.. subfolders.Skip(offset).Take(limit).Select(n => Node(FolderPrefix + prefix + n, n, BrowseKind.Folder, playable: true))],
            CatalogItems.Empty with { Tracks = tracks },
            Node(FolderPrefix + directory, name[(name.LastIndexOf(separator) + 1)..], BrowseKind.Folder, playable: true));
    }

    // A folder on a share (smb://...) is '/' separated, whatever this OS uses.
    private static string WithSeparator(string directory)
    {
        var separator = Smb.SmbLocation.IsUri(directory) ? '/' : Path.DirectorySeparatorChar;
        return directory.EndsWith(separator) ? directory : directory + separator;
    }

    // A book: the albums of audiobook tracks that share an album artist and a
    // title up to a disc suffix, in disc order; its id is its first album's.
    private sealed record Book(long Id, string Title, string? Artist, int? Year, IReadOnlyList<long> Albums,
        long Tracks, long? DurationMs);

    private static List<Book> Books(SqliteConnection connection) =>
        [.. AlbumRows(connection, "WHERE t.media_type = 'audiobook'")
            .Select(a => (Album: a, Title: BookTitle(a.Title)))
            .GroupBy(a => (a.Album.Artist?.ToUpperInvariant(), a.Title.Title.ToUpperInvariant()))
            .Select(g =>
            {
                var discs = g.OrderBy(a => a.Title.Disc ?? 0).ThenBy(a => a.Album.Title, BrowseIndex.Order)
                    .ThenBy(a => a.Album.Id).ToList();
                var first = discs[0].Album;
                return new Book(discs.Min(a => a.Album.Id), discs[0].Title.Title, first.Artist,
                    discs.Min(a => a.Album.Year), [.. discs.Select(a => a.Album.Id)], discs.Sum(a => a.Album.Tracks),
                    discs.All(a => a.Album.DurationMs is not null) ? discs.Sum(a => a.Album.DurationMs) : null);
            })
            .OrderBy(b => b.Title, BrowseIndex.Order).ThenBy(b => b.Artist ?? "", BrowseIndex.Order).ThenBy(b => b.Id)];

    // An album's title without its disc suffix, and the disc it names.
    public static (string Title, int? Disc) BookTitle(string album)
    {
        var match = DiscSuffix().Match(album);
        return match.Success && match.Groups["title"].Value.Trim() is { Length: > 0 } title
            ? (title, int.Parse(match.Groups["disc"].Value, CultureInfo.InvariantCulture))
            : (album, null);
    }

    private BrowseNode BookNode(Book b, bool header = false) => Node(BookPrefix + Invariant(b.Id), b.Title,
        BrowseKind.Album, header
            ? Join(b.Artist, b.Year is { } y ? Invariant(y) : null, Count(b.Tracks, "track"),
                b.DurationMs is { } ms ? Length(TimeSpan.FromMilliseconds(ms)) : null)
            : Join(b.Artist, b.Year is { } year ? Invariant(year) : null),
        playable: true, artwork: AlbumPrefix + Invariant(b.Albums[0]));

    private sealed record AlbumRow(long Id, string Title, string? Artist, int? Year, long Tracks, long? DurationMs, long? Added);

    private static List<AlbumRow> AlbumRows(SqliteConnection connection, string where,
        params (string Name, object Value)[] parameters) =>
        Rows(connection, AlbumColumns + "\n" + where + " GROUP BY al.id", r => new AlbumRow(r.GetInt64(0),
            r.GetString(1), StringOrNull(r, 2), r.IsDBNull(3) ? null : r.GetInt32(3), r.GetInt64(4),
            r.IsDBNull(5) ? null : r.GetInt64(5), r.IsDBNull(6) ? null : r.GetInt64(6)), parameters);

    private static List<AlbumRow> ArtistAlbums(SqliteConnection connection, long artist) =>
        [.. AlbumRows(connection, "WHERE al.artist_id = $v OR t.artist_id = $v", ("$v", artist))
            .OrderBy(a => a.Year ?? int.MaxValue).ThenBy(a => a.Title, BrowseIndex.Order).ThenBy(a => a.Id)];

    private BrowseNode AlbumNode(AlbumRow a) => Node(AlbumPrefix + Invariant(a.Id), a.Title, BrowseKind.Album,
        Join(a.Artist, a.Year is { } y ? Invariant(y) : null), playable: true, artwork: AlbumPrefix + Invariant(a.Id));

    // An album page's header: artist, year, tracks and their length.
    private BrowseNode AlbumHeader(AlbumRow a) => AlbumNode(a) with
    {
        Subtitle = Join(a.Artist, a.Year is { } y ? Invariant(y) : null, Count(a.Tracks, "track"),
            a.DurationMs is { } ms ? Length(TimeSpan.FromMilliseconds(ms)) : null),
    };

    private Album ToAlbum(AlbumRow a) => new(providerId, AlbumPrefix + Invariant(a.Id), a.Title, a.Artist, a.Year,
        new ArtworkRef(providerId, AlbumPrefix + Invariant(a.Id)));

    private List<BrowseNode> ArtistNodes(SqliteConnection connection, string where) =>
        Rows(connection, "SELECT ar.id, ar.name FROM artist ar " + where,
            r => Node(ArtistPrefix + Invariant(r.GetInt64(0)), r.GetString(1), BrowseKind.Artist, playable: true,
                artwork: ArtistPrefix + Invariant(r.GetInt64(0))));

    private BrowseNode? ArtistNode(SqliteConnection connection, long id) =>
        Rows(connection, "SELECT name FROM artist WHERE id = $id",
            r => Node(ArtistPrefix + Invariant(id), r.GetString(0), BrowseKind.Artist, playable: true,
                artwork: ArtistPrefix + Invariant(id)), ("$id", id))
            .FirstOrDefault();

    private BrowseNode List(string id, string title) =>
        Node(id, title, BrowseKind.Folder);

    private BrowseNode Node(string id, string title, BrowseKind kind, string? subtitle = null, bool playable = false,
        string? artwork = null) =>
        new(providerId, id, title, artwork is null ? null : new ArtworkRef(providerId, artwork), kind, subtitle,
            playable, playable ? null : NotQueued);

    // A node that takes no option.
    private static Result<BrowsePage, ProviderError> Plain(BrowseRequest r, Func<Result<BrowsePage, ProviderError>> page) =>
        Refuse(r, [], false, false) is { } refused ? refused : page();

    // An option the node does not take is refused, never ignored (rule 5).
    private static Failure<ProviderError>? Refuse(BrowseRequest r, string[] sorts, bool filterable,
        bool lettered)
    {
        var node = r.NodeId ?? "the root";
        if (r.Sort is { } sort && !sorts.Contains(sort, StringComparer.Ordinal))
        {
            return Result.Fail(ProviderError.InvalidArgument, sorts.Length == 0
                ? $"{node} takes no sort"
                : $"{node} sorts by {string.Join(", ", sorts)}, not {sort}");
        }
        if (r.Filter is not null && !filterable)
            return Result.Fail(ProviderError.InvalidArgument, $"{node} takes no filter");
        if (r.Letter is { } letter)
        {
            if (!lettered)
                return Result.Fail(ProviderError.InvalidArgument, $"{node} has no letter index in this order");
            if (!BrowseIndex.IsLetter(letter))
                return Result.Fail(ProviderError.InvalidArgument, $"letter: one letter or #, was {letter}");
            if (r.Offset != 0)
                return Result.Fail(ProviderError.InvalidArgument, "a letter is where the page starts: it takes no offset");
        }
        return null;
    }

    private static long Count(SqliteConnection connection, (string Where, string Order, (string, object)[] Parameters) query) =>
        Rows(connection, $"SELECT COUNT(*) FROM track t {Shown} LEFT JOIN album al ON al.id = t.album_id" + query.Where,
            r => r.GetInt64(0), query.Parameters)[0];

    private static string Count(long count, string what) =>
        $"{Invariant(count)} {what}{(count == 1 ? "" : "s")}";

    private static string Length(TimeSpan length) => length.TotalHours >= 1
        ? length.ToString(@"h\:mm\:ss", CultureInfo.InvariantCulture)
        : length.ToString(@"m\:ss", CultureInfo.InvariantCulture);

    private static string? Join(params string?[] parts)
    {
        var shown = parts.Where(p => !string.IsNullOrEmpty(p)).ToList();
        return shown.Count == 0 ? null : string.Join(" · ", shown);
    }

    private static string Invariant(long value) => value.ToString(CultureInfo.InvariantCulture);

    internal static long? Id(string nodeId, string prefix) =>
        nodeId.StartsWith(prefix, StringComparison.Ordinal) && long.TryParse(nodeId.AsSpan(prefix.Length),
            NumberStyles.None, CultureInfo.InvariantCulture, out var id)
            ? id
            : null;

    private static string? Named(string nodeId, string prefix) =>
        nodeId.StartsWith(prefix, StringComparison.Ordinal) && nodeId.Length > prefix.Length ? nodeId[prefix.Length..] : null;

    private Result<string, ProviderError> FileOf(string sql, long id, string what)
    {
        try
        {
            using var connection = database.Open();
            var paths = Strings(connection, sql, ("$id", id));
            return paths.Count == 0
                ? Result.Fail(ProviderError.NotFound, $"no file for {what}")
                : paths[0];
        }
        catch (SqliteException e)
        {
            return Result.Fail(ProviderError.Failed, e.Message);
        }
    }

    private List<Track> Tracks(SqliteConnection connection, string sql, int offset, int limit,
        params (string, object)[] parameters) =>
        Rows(connection, sql + Paging, TrackOf, [.. parameters, ("$offset", offset), ("$limit", limit)]);

    // A row that starts with TrackColumns' thirteen.
    internal Track TrackOf(SqliteDataReader r) => new(providerId, TrackPrefix + r.GetInt64(0), r.GetString(1),
        StringOrNull(r, 2), StringOrNull(r, 3), r.IsDBNull(4) ? null : TimeSpan.FromMilliseconds(r.GetInt64(4)),
        new ArtworkRef(providerId, r.IsDBNull(5) ? TrackPrefix + r.GetInt64(0) : AlbumPrefix + r.GetInt64(5)),
        FormatOf(r, 6), EnumOrNull<MediaType>(r, 11), EnumOrNull<MediaTypeSource>(r, 12));

    // A media type or its source as the database keeps it: its camel-case
    // name, null when unknown.
    internal static T? EnumOrNull<T>(SqliteDataReader r, int column)
        where T : struct, Enum =>
        r.IsDBNull(column) ? null : Enum.Parse<T>(r.GetString(column), ignoreCase: true);

    internal static string? NameOf<T>(T? value)
        where T : struct, Enum =>
        value is { } v ? JsonNamingPolicy.CamelCase.ConvertName(v.ToString()) : null;

    // The parameters $mediaType and $mediaTypeSource.
    internal static (string, object?)[] MediaTypeValues(MediaTypeVerdict verdict) =>
        [("$mediaType", NameOf(verdict.Type)), ("$mediaTypeSource", NameOf(verdict.Source))];

    // codec, sample_rate, bit_depth, channels, bitrate from `first`; null
    // when all five are.
    internal static AudioFormat? FormatOf(SqliteDataReader r, int first)
    {
        int? Int(int column) => r.IsDBNull(column) ? null : r.GetInt32(column);
        var format = new AudioFormat(StringOrNull(r, first), Int(first + 1), Int(first + 2), Int(first + 3),
            Int(first + 4));
        return format == new AudioFormat() ? null : format;
    }

    // The parameters $codec, $sampleRate, $bitDepth, $channels and $bitrate.
    internal static (string, object?)[] FormatValues(AudioFormat? format) =>
    [
        ("$codec", format?.Codec), ("$sampleRate", format?.SampleRate), ("$bitDepth", format?.BitDepth),
        ("$channels", format?.Channels), ("$bitrate", format?.Bitrate),
    ];

    private static List<string> Strings(SqliteConnection connection, string sql, params (string, object)[] parameters) =>
        Rows(connection, sql, r => r.GetString(0), parameters);

    private static bool Exists(SqliteConnection connection, string sql, object id) =>
        Rows(connection, sql, _ => true, ("$id", id)).Count > 0;

    private static List<T> Rows<T>(SqliteConnection connection, string sql, Func<SqliteDataReader, T> map,
        params (string Name, object Value)[] parameters)
    {
        using var command = connection.CreateCommand();
#pragma warning disable CA2100 // Constant SQL; every value is a parameter.
        command.CommandText = sql;
#pragma warning restore CA2100
        foreach (var (name, value) in parameters)
            command.Parameters.AddWithValue(name, value);
        using var reader = command.ExecuteReader();
        var rows = new List<T>();
        while (reader.Read())
            rows.Add(map(reader));
        return rows;
    }

    private static string? StringOrNull(SqliteDataReader reader, int column) =>
        reader.IsDBNull(column) ? null : reader.GetString(column);

    private Task<Result<T, ProviderError>> Read<T>(Func<SqliteConnection, Result<T, ProviderError>> read)
    {
        try
        {
            using var connection = database.Open();
            return Task.FromResult(read(connection));
        }
        catch (SqliteException e)
        {
            return Task.FromResult<Result<T, ProviderError>>(Result.Fail(ProviderError.Failed, e.Message));
        }
    }
}
