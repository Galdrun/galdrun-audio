using System.Diagnostics;
using System.Text.Json;
using System.Text.RegularExpressions;
using Multiroom.Server.Abstractions;
using static Multiroom.Server.Providers.Covers.ServiceJson;

namespace Multiroom.Server.Providers.Covers;

// Where MusicBrainzSource asks: MusicBrainz's web service, the Cover Art
// Archive, Wikidata and Wikimedia Commons. A test points them at a fixture.
public sealed record MusicBrainzEndpoints(Uri Api, Uri CoverArt, Uri Wikidata, Uri Commons)
{
    public static MusicBrainzEndpoints Default { get; } = new(new Uri("https://musicbrainz.org/ws/2/"),
        new Uri("https://coverartarchive.org/"), new Uri("https://www.wikidata.org/"),
        new Uri("https://commons.wikimedia.org/"));
}

// The open source, and the one that knows the ids every source takes:
// MusicBrainz's metadata (CC0, no key) resolves an album and an artist to
// their ids, the Cover Art Archive holds an album's front cover, and an
// artist's Wikidata item names its picture on Wikimedia Commons. Images are
// asked for ArtworkSources.MinimumSize wide: the archive's 1200 px
// thumbnail, Commons' scaled to 1200 px (or the original, when smaller).
//
// Resolving: an album tagged with its release group (Picard) is not looked
// up; one tagged with a release only is looked up for its group and artist;
// otherwise it matches a release group whose title and credited artist fold
// to the asked ones (SongResolver.AlbumKey). Several matches are narrowed to
// those of primary type Album; anything but exactly one is no match. The
// album's artist is the match's first credited artist. An artist alone
// matches by its whole folded name (SongResolver.NameKey), only exactly one.
//
// MusicBrainz takes one request a second from a client: its requests go one
// at a time, at least `spacing` apart. The others are not limited.
#pragma warning disable CA1001 // Its SemaphoreSlim never has its wait handle read, so it holds nothing to release (as ArtworkCache's).
public sealed partial class MusicBrainzSource(HttpClient http, MusicBrainzEndpoints? endpoints = null,
    TimeSpan? spacing = null) : IArtworkSource
#pragma warning restore CA1001
{
    public const string SourceId = "musicbrainz";

    public static readonly TimeSpan DefaultSpacing = TimeSpan.FromSeconds(1.1);

    // The candidates a search answers; the best matches come first.
    private const int Candidates = 10;

    private const int Retries = 2;

    private const int PictureWidth = ArtworkSources.MinimumSize;

    private readonly MusicBrainzEndpoints _endpoints = endpoints ?? MusicBrainzEndpoints.Default;
    private readonly TimeSpan _spacing = spacing ?? DefaultSpacing;
    private readonly SemaphoreSlim _gate = new(1, 1);
    private long _last;

    public string Id => SourceId;

    // The album with its release group and artist ids filled in where they
    // can be; unchanged when nothing matches.
    public async Task<Result<AlbumQuery, ProviderError>> ResolveAlbumAsync(AlbumQuery album,
        CancellationToken cancellationToken)
    {
        album = album with
        {
            ReleaseGroupId = Mbid(album.ReleaseGroupId), ReleaseId = Mbid(album.ReleaseId), ArtistId = Mbid(album.ArtistId),
        };
        if (album.ReleaseGroupId is not null)
            return album;
        if (album.ReleaseId is { } release)
        {
            var answer = await LimitedAsync($"release/{release}?inc=release-groups+artist-credits&fmt=json",
                cancellationToken).ConfigureAwait(false);
            if (!answer.IsSuccess)
                return answer.Failure;
            using var document = answer.Value;
            if (document is null)
                return album;
            var root = document.RootElement;
            var group = root.TryGetProperty("release-group", out var g) ? Mbid(String(g, "id")) : null;
            return album with { ReleaseGroupId = group, ArtistId = album.ArtistId ?? FirstCreditedArtist(root) };
        }

        var query = $"releasegroup:{Phrase(album.Album)} AND artist:{Phrase(album.Artist)}";
        var found = await LimitedAsync($"release-group?query={Uri.EscapeDataString(query)}&fmt=json&limit={Candidates}",
            cancellationToken).ConfigureAwait(false);
        if (!found.IsSuccess)
            return found.Failure;
        var key = SongResolver.AlbumKey(album.Album, album.Artist);
        var matches = new List<(string Id, string? Type, string? Artist)>();
        using (var document = found.Value)
        {
            foreach (var group in Items(document?.RootElement ?? default, "release-groups"))
            {
                if (Mbid(String(group, "id")) is not { } id || String(group, "title") is not { } title)
                    continue;
                var credit = string.Concat(Items(group, "artist-credit")
                    .Select(c => String(c, "name") + String(c, "joinphrase")));
                if (SongResolver.AlbumKey(title, credit) == key)
                    matches.Add((id, String(group, "primary-type"), FirstCreditedArtist(group)));
            }
        }
        matches = [.. matches.DistinctBy(m => m.Id)];
        if (matches.Count > 1)
            matches = [.. matches.Where(m => m.Type == "Album")];
        return matches.Count == 1
            ? album with { ReleaseGroupId = matches[0].Id, ArtistId = album.ArtistId ?? matches[0].Artist }
            : album;
    }

    // The artist's id: the one given, else exactly one artist of that name.
    public async Task<Result<string?, ProviderError>> ResolveArtistAsync(string artist, string? artistId,
        CancellationToken cancellationToken)
    {
        if (Mbid(artistId) is { } id)
            return id;
        var query = $"artist:{Phrase(artist)}";
        var answer = await LimitedAsync($"artist?query={Uri.EscapeDataString(query)}&fmt=json&limit={Candidates}",
            cancellationToken).ConfigureAwait(false);
        if (!answer.IsSuccess)
            return answer.Failure;
        var key = SongResolver.NameKey(artist);
        List<string> matches;
        using (var document = answer.Value)
        {
            matches = [.. Items(document?.RootElement ?? default, "artists")
                .Where(a => String(a, "name") is { } name && SongResolver.NameKey(name) == key)
                .Select(a => Mbid(String(a, "id"))).OfType<string>().Distinct(StringComparer.Ordinal)];
        }
        return matches.Count == 1 ? matches[0] : (string?)null;
    }

    // Phase 40: a song named without its album (a radio song) as the album
    // it is on: a recording whose title and credited artist fold to the asked
    // ones (SongResolver.SongKey), and of its release groups an album before
    // a single or an EP, a compilation or a live one last. Null when no
    // recording matches.
    public async Task<Result<AlbumQuery?, ProviderError>> ResolveSongAsync(string title, string artist,
        CancellationToken cancellationToken)
    {
        var query = $"recording:{Phrase(title)} AND artist:{Phrase(artist)}";
        var found = await LimitedAsync($"recording?query={Uri.EscapeDataString(query)}&fmt=json&limit={Candidates}",
            cancellationToken).ConfigureAwait(false);
        if (!found.IsSuccess)
            return found.Failure;
        var key = SongResolver.SongKey(title, artist);
        var groups = new List<(AlbumQuery Album, int Rank)>();
        using (var document = found.Value)
        {
            foreach (var recording in Items(document?.RootElement ?? default, "recordings"))
            {
                var credit = string.Concat(Items(recording, "artist-credit")
                    .Select(c => String(c, "name") + String(c, "joinphrase")));
                if (String(recording, "title") is not { } name || SongResolver.SongKey(name, credit) != key)
                    continue;
                foreach (var release in Items(recording, "releases"))
                {
                    if (!release.TryGetProperty("release-group", out var group) ||
                        Mbid(String(group, "id")) is not { } id || String(group, "title") is not { } album)
                    {
                        continue;
                    }
                    var rank = (String(group, "primary-type") switch { "Album" => 0, "Single" or "EP" => 1, _ => 2 }) +
                        (Items(group, "secondary-types").Any() ? 3 : 0);
                    groups.Add((new AlbumQuery(artist, album, id, null, FirstCreditedArtist(recording)), rank));
                }
            }
        }
        return groups.OrderBy(g => g.Rank).Select(g => g.Album).FirstOrDefault();
    }

    // The release's own cover first (the edition the file is), then its group's.
    public Task<Result<IReadOnlyList<Uri>, ProviderError>> CoversAsync(AlbumQuery album,
        CancellationToken cancellationToken)
    {
        List<Uri> covers = [];
        if (Mbid(album.ReleaseId) is { } release)
            covers.Add(new Uri(_endpoints.CoverArt, $"release/{release}/front-{PictureWidth}"));
        if (Mbid(album.ReleaseGroupId) is { } group)
            covers.Add(new Uri(_endpoints.CoverArt, $"release-group/{group}/front-{PictureWidth}"));
        return Task.FromResult<Result<IReadOnlyList<Uri>, ProviderError>>(covers);
    }

    // The artist's Wikidata item, then that item's image (P18) as a Commons
    // thumbnail. None when either is missing.
    public async Task<Result<IReadOnlyList<Uri>, ProviderError>> PicturesAsync(string artistId,
        CancellationToken cancellationToken)
    {
        if (Mbid(artistId) is null)
            return Array.Empty<Uri>();
        var artist = await LimitedAsync($"artist/{artistId}?inc=url-rels&fmt=json", cancellationToken)
            .ConfigureAwait(false);
        if (!artist.IsSuccess)
            return artist.Failure;
        string? item;
        using (var document = artist.Value)
        {
            item = Items(document?.RootElement ?? default, "relations")
                .Where(r => String(r, "type") == "wikidata" && r.TryGetProperty("url", out _))
                .Select(r => String(r.GetProperty("url"), "resource"))
                .Select(u => u?[(u.LastIndexOf('/') + 1)..])
                .FirstOrDefault(q => q is not null && ItemPattern().IsMatch(q));
        }
        if (item is null)
            return Array.Empty<Uri>();

        var entity = await GetAsync(http, new Uri(_endpoints.Wikidata, $"wiki/Special:EntityData/{item}.json"),
            cancellationToken).ConfigureAwait(false);
        if (!entity.IsSuccess)
            return entity.Failure;
        string? file = null;
        using (var document = entity.Value)
        {
            // A merged item answers under the id it was merged into.
            if (document is not null && document.RootElement.TryGetProperty("entities", out var entities) &&
                entities.ValueKind == JsonValueKind.Object &&
                entities.EnumerateObject().FirstOrDefault().Value is { ValueKind: JsonValueKind.Object } found &&
                found.TryGetProperty("claims", out var claims) && claims.ValueKind == JsonValueKind.Object)
            {
                file = Items(claims, "P18")
                    .Select(c => c.TryGetProperty("mainsnak", out var snak) && snak.TryGetProperty("datavalue", out var value)
                        ? String(value, "value")
                        : null)
                    .FirstOrDefault(f => !string.IsNullOrWhiteSpace(f));
            }
        }
        return file is null
            ? Array.Empty<Uri>()
            : new[]
            {
                new Uri(_endpoints.Commons,
                    $"wiki/Special:FilePath/{Uri.EscapeDataString(file.Replace(' ', '_'))}?width={PictureWidth}"),
            };
    }

    // A request of MusicBrainz's own API, one at a time and spaced. A busy
    // MusicBrainz (503, which it answers often) is asked again Retries times,
    // two spacings more apart each time, before it is Unavailable.
    private async Task<Result<JsonDocument?, ProviderError>> LimitedAsync(string relative,
        CancellationToken cancellationToken)
    {
        await _gate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            if (_last != 0 && _spacing - Stopwatch.GetElapsedTime(_last) is { } wait && wait > TimeSpan.Zero)
                await Task.Delay(wait, cancellationToken).ConfigureAwait(false);
            var answer = await GetAsync(http, new Uri(_endpoints.Api, relative), cancellationToken).ConfigureAwait(false);
            for (var retry = 1; retry <= Retries && !answer.IsSuccess && answer.Failure.Code == ProviderError.Unavailable; retry++)
            {
                await Task.Delay(_spacing * 2 * retry, cancellationToken).ConfigureAwait(false);
                answer = await GetAsync(http, new Uri(_endpoints.Api, relative), cancellationToken).ConfigureAwait(false);
            }
            return answer;
        }
        finally
        {
            _last = Stopwatch.GetTimestamp();
            _gate.Release();
        }
    }

    private static string? FirstCreditedArtist(JsonElement credited) =>
        Items(credited, "artist-credit").FirstOrDefault() is { ValueKind: JsonValueKind.Object } first &&
        first.TryGetProperty("artist", out var artist)
            ? Mbid(String(artist, "id"))
            : null;

    // A Lucene phrase: the text quoted, its quotes and backslashes escaped.
    private static string Phrase(string text) =>
        "\"" + text.Replace("\\", "\\\\", StringComparison.Ordinal).Replace("\"", "\\\"", StringComparison.Ordinal) + "\"";

    // An id as it goes into a URL: a MusicBrainz id, else none.
    internal static string? Mbid(string? id) => id is not null && MbidPattern().IsMatch(id) ? id : null;

    [GeneratedRegex("^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$")]
    private static partial Regex MbidPattern();

    [GeneratedRegex("^Q[0-9]+$")]
    private static partial Regex ItemPattern();
}
