using Multiroom.Server.Abstractions;
using Multiroom.Server.Providers;

namespace Multiroom.Server.Tests;

// A provider that holds a small catalogue of tracks by title, artist and
// album, and searches it as a service does: every word of the query in the
// track's title, artist or album, case, accents and punctuation aside. Its tracks
// stream (from File, when one is set), so the song resolver may choose
// them. Two of them stand in for the library and a streaming service.
internal sealed class FakeCatalog(string id) : IProvider, ISearchProvider, IStreamResolver
{
    private readonly List<Track> _tracks = [];

    public string Id => id;

    public string DisplayName => id;

    public ProviderCapabilities Capabilities => ProviderCapabilities.Search | ProviderCapabilities.Stream;

    public string? File { get; set; }

    public int Searches { get; private set; }

    public Track Add(string title, string artist, string? album = null, MediaType? type = null)
    {
        var track = new Track(id, $"{id}-{_tracks.Count}", title, artist, album, MediaType: type);
        _tracks.Add(track);
        return track;
    }

    public Task<ProviderStatus> GetStatusAsync(CancellationToken cancellationToken) =>
        Task.FromResult(new ProviderStatus(ProviderState.Ready));

    public Task<Result<CatalogItems, ProviderError>> SearchAsync(string text, SearchKinds kinds, int offset, int limit,
        CancellationToken cancellationToken)
    {
        Searches++;
        var words = Words(text);
        var tracks = _tracks.Where(_ => kinds.HasFlag(SearchKinds.Tracks))
            .Where(t => words.IsSubsetOf(Words($"{t.Title} {t.Artist} {t.Album}")))
            .Skip(offset).Take(limit).ToList();
        // Its albums and artists are its tracks' (phase 38's proposals find them).
        var albums = _tracks.Where(t => t.Album is not null).Select(t => (Title: t.Album!, t.Artist))
            .Distinct().Select((a, i) => new Album(id, $"{id}-album-{i}", a.Title, a.Artist))
            .Where(a => kinds.HasFlag(SearchKinds.Albums) && words.IsSubsetOf(Words($"{a.Title} {a.Artist}")))
            .Skip(offset).Take(limit).ToList();
        var artists = _tracks.Select(t => t.Artist!).Distinct().Select((name, i) => new Artist(id, $"{id}-artist-{i}", name))
            .Where(a => kinds.HasFlag(SearchKinds.Artists) && words.IsSubsetOf(Words(a.Name)))
            .Skip(offset).Take(limit).ToList();
        return Task.FromResult<Result<CatalogItems, ProviderError>>(CatalogItems.Empty with
        {
            Tracks = tracks,
            Albums = albums,
            Artists = artists,
        });
    }

    public Task<Result<ResolvedStream, ProviderError>> ResolveAsync(Track track, CancellationToken cancellationToken) =>
        Task.FromResult(File is not null && _tracks.Contains(track)
            ? ResolvedStream.FromFile(File)
            : Result.Fail(ProviderError.NotFound, $"no track {track.ProviderItemId}"));

    private static HashSet<string> Words(string text) =>
        [.. new string([.. text.ToLowerInvariant().Normalize(System.Text.NormalizationForm.FormD)
                .Where(c => System.Globalization.CharUnicodeInfo.GetUnicodeCategory(c) != System.Globalization.UnicodeCategory.NonSpacingMark)
                .Select(c => char.IsLetterOrDigit(c) ? c : ' ')])
            .Split(' ', StringSplitOptions.RemoveEmptyEntries)];
}
