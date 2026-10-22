using System.Globalization;
using System.Text;
using System.Text.RegularExpressions;
using Multiroom.Server.Abstractions;

namespace Multiroom.Server.Providers;

public enum SongMatchKind
{
    // The same title and artist, nothing left over.
    Exact,

    // The same song under a title with a suffix, a primary artist only, or a
    // version not asked for when no plain one was found.
    Close,
}

// One song as named and what was found for it: a track and how well it
// matched, or none and why, in one line.
public sealed record ResolvedSong(SongSuggestion Suggestion, Track? Track = null, SongMatchKind? Match = null,
    string? Reason = null);

// Phase 38: an album or an artist as named and what was found for it, or why
// none was.
public sealed record ResolvedAlbum(string Title, string? Artist, Album? Album = null, string? Reason = null);

public sealed record ResolvedArtist(string Name, Artist? Artist = null, string? Reason = null);

// The songs in the order asked, how many were found, and the providers that
// did not answer a search.
public sealed record ResolvedSongs(IReadOnlyList<ResolvedSong> Songs, int Found, int NotFound,
    IReadOnlyList<SearchFailure> Failures);

// Finds songs named by title and artist among the providers that stream
// tracks. It knows no AI: whoever named the
// songs, this decides only whether the house can play them.
//
// Each song is searched as "artist title" through unified search, at most
// Parallelism at a time, each provider bounded by unified search's timeout.
// Matching is written out and boring: title and artist compared folded (case,
// accents, punctuation), the title's suffixes ("- Remastered 2011", "(Radio
// Edit)", "feat. X") set aside, the artist's primary name enough. A version
// not asked for (live, remix, karaoke, ...) is not taken while a plain one
// exists; the album decides between equals. The first provider in the user's
// order with a match wins, so the library plays before a streaming service.
// A track excluded by the caller, or already found for an earlier song, is
// not answered again.
public sealed partial class SongResolver(UnifiedSearch search, ProviderRegistry providers,
    Func<IReadOnlyList<string>?> preference)
{
    public const int MaxSongs = 50;
    public const int Parallelism = 4;

    // The local library's id: first in the default order.
    public const string LibraryProviderId = "local";

    private const int CandidatesPerProvider = 20;

    private static readonly HashSet<string> VersionWords = new(StringComparer.Ordinal)
    {
        "live", "remix", "rmx", "karaoke", "cover", "instrumental", "acoustic", "demo", "unplugged",
    };

    // The providers asked, in order: the user's, else the library and then
    // every other provider that streams tracks, in registration order. A
    // provider that does not stream tracks now is left out.
    public IReadOnlyList<string> ProviderOrder()
    {
        var ids = preference() ?? [.. providers.Ids.OrderBy(id => id == LibraryProviderId ? 0 : 1)];
        return [.. ids.Where(id => providers.Get<IStreamResolver>(id).IsSuccess)];
    }

    // `avoid` (phase 26): songs, by SongKey, that are not answered from any
    // provider: what the house played lately.
    public async Task<Result<ResolvedSongs, ProviderError>> ResolveAsync(IReadOnlyList<SongSuggestion> songs,
        IReadOnlyCollection<Track> exclude, CancellationToken cancellationToken, IReadOnlySet<string>? avoid = null)
    {
        if (songs.Count is 0 or > MaxSongs)
            return Result.Fail(ProviderError.InvalidArgument, $"songs: 1 to {MaxSongs}, was {songs.Count}");
        if (songs.Select((s, i) => (s, i)).FirstOrDefault(p => string.IsNullOrWhiteSpace(p.s.Title)) is { s: not null } blank)
            return Result.Fail(ProviderError.InvalidArgument, $"songs.{blank.i}.title is empty");

        var order = ProviderOrder();
        var answers = new UnifiedSearchResult?[songs.Count];
        if (order.Count > 0)
        {
            await Parallel.ForEachAsync(Enumerable.Range(0, songs.Count),
                new ParallelOptions { MaxDegreeOfParallelism = Parallelism, CancellationToken = cancellationToken },
                async (i, token) => answers[i] = await search.SearchAsync(QueryOf(songs[i]), SearchKinds.Tracks, 0,
                    CandidatesPerProvider, order, token).ConfigureAwait(false)).ConfigureAwait(false);
        }

        var taken = exclude.Select(Key).ToHashSet();
        var resolved = new List<ResolvedSong>(songs.Count);
        foreach (var (song, answer) in songs.Zip(answers))
        {
            var found = avoid?.Contains(SongKey(song.Title, song.Artist)) == true
                ? new ResolvedSong(song, Reason: "the caller asked to avoid this song")
                : Choose(song, (answer?.Items.Tracks ?? [])
                    // A song never resolves to an audiobook chapter of the same title (phase 30).
                    .Where(t => MediaTypeClassifier.IsMusic(t) && avoid?.Contains(SongKey(t.Title, t.Artist)) != true).ToList(),
                    order, taken);
            if (found.Track is { } track)
                taken.Add(Key(track));
            resolved.Add(found);
        }
        var failures = answers.SelectMany(a => a?.Failures ?? []).DistinctBy(f => f.Provider).ToList();
        var count = resolved.Count(r => r.Track is not null);
        return new ResolvedSongs(resolved, count, resolved.Count - count, failures);
    }

    // Phase 38: albums named by title and artist, found by the same rules as
    // songs (title without suffixes, primary artist), in the same provider
    // order. Neither an excluded album (provider, item) nor one whose
    // AlbumKey is avoided is answered; each album at most once.
    public async Task<Result<(IReadOnlyList<ResolvedAlbum> Albums, IReadOnlyList<SearchFailure> Failures), ProviderError>>
        ResolveAlbumsAsync(IReadOnlyList<(string Title, string? Artist)> albums,
            IReadOnlySet<(string ProviderId, string ProviderItemId)> exclude, IReadOnlySet<string> avoid,
            CancellationToken cancellationToken)
    {
        if (albums.Count is 0 or > MaxSongs)
            return Result.Fail(ProviderError.InvalidArgument, $"albums: 1 to {MaxSongs}, was {albums.Count}");
        var (answers, failures) = await SearchEachAsync(albums.Select(a => QueryOf(new SongSuggestion(a.Title, a.Artist))).ToList(),
            SearchKinds.Albums, cancellationToken).ConfigureAwait(false);
        var taken = new HashSet<(string, string)>(exclude);
        var order = ProviderOrder();
        var resolved = new List<ResolvedAlbum>(albums.Count);
        foreach (var ((title, artist), answer) in albums.Zip(answers))
        {
            var wanted = Title.Of(title).Core;
            var found = (answer?.Items.Albums ?? [])
                .Where(a => Title.Of(a.Title).Core == wanted && ArtistMatches(artist, a.Artist) &&
                    !taken.Contains((a.ProviderId, a.ProviderItemId)) && !avoid.Contains(AlbumKey(a.Title, a.Artist)))
                .OrderBy(a => Rank(order, a.ProviderId))
                .FirstOrDefault();
            if (found is not null)
                taken.Add((found.ProviderId, found.ProviderItemId));
            resolved.Add(new ResolvedAlbum(title, artist, found,
                found is null ? "no album with this title and artist was found, or it is to be avoided" : null));
        }
        return (resolved, failures);
    }

    // Phase 38: artists by name, the folded primary name compared.
    public async Task<Result<(IReadOnlyList<ResolvedArtist> Artists, IReadOnlyList<SearchFailure> Failures), ProviderError>>
        ResolveArtistsAsync(IReadOnlyList<string> names, IReadOnlySet<(string ProviderId, string ProviderItemId)> exclude,
            IReadOnlySet<string> avoid, CancellationToken cancellationToken)
    {
        if (names.Count is 0 or > MaxSongs)
            return Result.Fail(ProviderError.InvalidArgument, $"artists: 1 to {MaxSongs}, was {names.Count}");
        var (answers, failures) = await SearchEachAsync(names, SearchKinds.Artists, cancellationToken).ConfigureAwait(false);
        var taken = new HashSet<(string, string)>(exclude);
        var order = ProviderOrder();
        var resolved = new List<ResolvedArtist>(names.Count);
        foreach (var (name, answer) in names.Zip(answers))
        {
            var wanted = ArtistKey(name);
            var found = (answer?.Items.Artists ?? [])
                .Where(a => ArtistKey(a.Name) == wanted && !taken.Contains((a.ProviderId, a.ProviderItemId)) &&
                    !avoid.Contains(ArtistKey(a.Name)))
                .OrderBy(a => Rank(order, a.ProviderId))
                .FirstOrDefault();
            if (found is not null)
                taken.Add((found.ProviderId, found.ProviderItemId));
            resolved.Add(new ResolvedArtist(name, found,
                found is null ? "no artist of this name was found, or it is to be avoided" : null));
        }
        return (resolved, failures);
    }

    // One album whatever its provider: the song key's folding of its title and artist.
    public static string AlbumKey(string title, string? artist) => SongKey(title, artist);

    // One artist whatever its provider: the folded primary name.
    public static string ArtistKey(string name) => PrimaryArtist(name);

    // One artist's whole name, folded: "Simon & Garfunkel" is not "Simon".
    public static string NameKey(string name) => FoldArtist(name);

    private async Task<(UnifiedSearchResult?[] Answers, IReadOnlyList<SearchFailure> Failures)> SearchEachAsync(
        IReadOnlyList<string> queries, SearchKinds kinds, CancellationToken cancellationToken)
    {
        var order = ProviderOrder();
        var answers = new UnifiedSearchResult?[queries.Count];
        if (order.Count > 0)
        {
            await Parallel.ForEachAsync(Enumerable.Range(0, queries.Count),
                new ParallelOptions { MaxDegreeOfParallelism = Parallelism, CancellationToken = cancellationToken },
                async (i, token) => answers[i] = await search.SearchAsync(queries[i], kinds, 0, CandidatesPerProvider,
                    order, token).ConfigureAwait(false)).ConfigureAwait(false);
        }
        return (answers, answers.SelectMany(a => a?.Failures ?? []).DistinctBy(f => f.Provider).ToList());
    }

    private static int Rank(IReadOnlyList<string> order, string provider)
    {
        var index = order.ToList().IndexOf(provider);
        return index < 0 ? int.MaxValue : index;
    }

    private static (string, string) Key(Track track) => (track.ProviderId, track.ProviderItemId);

    // One song whatever its provider and version: the folded title without its
    // suffixes and the folded primary artist, by the matching rules above.
    public static string SongKey(string title, string? artist) =>
        $"{Title.Of(title).Core}|{(artist is null ? "" : PrimaryArtist(artist))}";

    // The primary artist and the title without its suffixes: what a provider
    // has to find, without words it may not hold.
    // As named, not folded: each provider matches case and accents its own way.
    private static string QueryOf(SongSuggestion song)
    {
        var title = Title.Of(song.Title).Named;
        return song.Artist is { } artist ? $"{ArtistSeparator().Split(artist, 2)[0].Trim()} {title}" : title;
    }

    private static ResolvedSong Choose(SongSuggestion song, IReadOnlyList<Track> tracks, IReadOnlyList<string> order,
        HashSet<(string, string)> taken)
    {
        var wanted = Title.Of(song.Title);
        var matches = tracks
            .Select((track, index) => (Track: track, Index: index, Title: Title.Of(track.Title)))
            .Where(c => c.Title.Core == wanted.Core && ArtistMatches(song.Artist, c.Track.Artist))
            .ToList();
        if (matches.Count == 0)
            return new ResolvedSong(song, Reason: "no track with this title and artist was found");
        var free = matches.Where(m => !taken.Contains(Key(m.Track))).ToList();
        if (free.Count == 0)
            return new ResolvedSong(song, Reason: "every match is excluded or was found for an earlier song");

        // A plain version from the first provider that has one, else any
        // version from the first provider that has one.
        var plain = free.Where(m => !m.Title.Versions.Except(wanted.Versions).Any()).ToList();
        var pool = plain.Count > 0 ? plain : free;
        var provider = order.First(id => pool.Any(m => m.Track.ProviderId == id));
        var album = song.Album is { } a ? Fold(a) : null;
        var best = pool.Where(m => m.Track.ProviderId == provider)
            .OrderBy(m => album is null || m.Track.Album is not { } other || Fold(other) != album ? 1 : 0)
            .ThenBy(m => Fold(m.Track.Title) == Fold(song.Title) ? 0 : 1)
            .ThenBy(m => m.Index)
            .First();
        return Describe(song, best.Track, wanted, best.Title, plain.Count == 0);
    }

    private static ResolvedSong Describe(SongSuggestion song, Track track, Title wanted, Title found, bool versioned)
    {
        var differences = new List<string>();
        if (Fold(track.Title) != Fold(song.Title))
            differences.Add($"titled \"{track.Title}\"");
        if (song.Artist is null)
            differences.Add("no artist was named");
        else if (FoldArtist(song.Artist) != FoldArtist(track.Artist!))
            differences.Add($"by {track.Artist}");
        if (versioned)
        {
            var versions = string.Join(", ", found.Versions.Except(wanted.Versions).Order(StringComparer.Ordinal));
            differences.Add($"a {versions} version; no plain one was found");
        }
        var sameAlbum = song.Album is { } album && track.Album is { } other && Fold(album) == Fold(other);
        if (differences.Count == 0)
        {
            return new ResolvedSong(song, track, SongMatchKind.Exact,
                $"same title and artist{(sameAlbum ? ", same album" : "")} on {track.ProviderId}");
        }
        return new ResolvedSong(song, track, SongMatchKind.Close,
            $"{string.Join("; ", differences)}{(sameAlbum ? "; same album" : "")} on {track.ProviderId}");
    }

    private static bool ArtistMatches(string? wanted, string? candidate)
    {
        if (wanted is null)
            return true;
        if (candidate is null)
            return false;
        return FoldArtist(wanted) == FoldArtist(candidate) || PrimaryArtist(wanted) == PrimaryArtist(candidate);
    }

    // "The Beatles" and "Beatles" are one artist; "&" is "and".
    private static string FoldArtist(string artist) => Names.Key(artist);

    // The name before the first "feat.", "&", ",", "with", "x" or "/".
    private static string PrimaryArtist(string artist) => FoldArtist(ArtistSeparator().Split(artist, 2)[0]);

    // Lower case, no accents, every run of anything but letters and digits one space.
    private static string Fold(string text) => Names.Fold(text);

    [GeneratedRegex(@"\s*(?:,|;|&|/|\bfeat\.?|\bft\.|\bfeaturing\b|\bwith\b|\bx\b)\s*", RegexOptions.IgnoreCase)]
    private static partial Regex ArtistSeparator();

    // "(…)", "[…]" and " - …" at the end of a title, and "feat. …".
    [GeneratedRegex(@"\s*(?:\(([^()]*)\)|\[([^\[\]]*)\]|\s[-–—]\s+(.*)|\s(?:feat\.?|ft\.|featuring)\s+(.*))\s*$", RegexOptions.IgnoreCase)]
    private static partial Regex TitleSuffix();

    // A title without its suffixes, as named and folded, and the versions its
    // suffixes name.
    private sealed record Title(string Named, string Core, IReadOnlySet<string> Versions)
    {
        public static Title Of(string title)
        {
            var versions = new HashSet<string>(StringComparer.Ordinal);
            var rest = title.Trim();
            while (TitleSuffix().Match(rest) is { Success: true, Index: > 0 } suffix)
            {
                var text = suffix.Groups.Values.Skip(1).FirstOrDefault(g => g.Success)?.Value ?? "";
                var isFeature = suffix.Groups[4].Success;
                if (!isFeature)
                    versions.UnionWith(Fold(text).Split(' ').Where(VersionWords.Contains));
                rest = rest[..suffix.Index];
            }
            return new Title(rest.Trim(), Fold(rest), versions);
        }
    }
}
