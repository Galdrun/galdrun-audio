using Multiroom.Server.Abstractions;

namespace Multiroom.Server.Providers.Covers;

// A service on the net that holds album covers and artist pictures by their
// MusicBrainz ids: the artwork add-in's sources, asked in order (fanart.tv,
// then MusicBrainz's Cover Art Archive and Wikidata). The ids are Picard's
// tags, else what MusicBrainzSource resolved by name; a source never
// searches. A new source is a class here, a line in ArtworkAddIn.Sources and
// a key in ArtworkOptions.
//
// It answers URLs to try in order, best first, none when it holds nothing.
// Not holding is no failure; a failure is the service's (Unavailable: asked
// again later). It keeps its service's rate limit itself, and asks for
// images of at least ArtworkSources.MinimumSize pixels where it can.
public interface IArtworkSource
{
    string Id { get; }

    // The album's front covers. Its ids are resolved: a release group or a
    // release, or the album is not asked about.
    Task<Result<IReadOnlyList<Uri>, ProviderError>> CoversAsync(AlbumQuery album, CancellationToken cancellationToken);

    Task<Result<IReadOnlyList<Uri>, ProviderError>> PicturesAsync(string artistId, CancellationToken cancellationToken);
}

// An album as the library knows it: its artist (the album artist), its title,
// and the MusicBrainz ids of its tags or resolved, each null when unknown.
public sealed record AlbumQuery(string Artist, string Album, string? ReleaseGroupId = null, string? ReleaseId = null,
    string? ArtistId = null)
{
    public bool Identified => ReleaseGroupId is not null || ReleaseId is not null;
}

public static class ArtworkSources
{
    // What a source asks for: wide enough for a full-screen now playing.
    public const int MinimumSize = 1200;
}
