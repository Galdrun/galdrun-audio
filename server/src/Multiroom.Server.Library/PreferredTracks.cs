using Multiroom.Server.Abstractions;

namespace Multiroom.Server.Library;

// Which of some tracks the household prefers: liked or a favourite, one mark
// either way (a track both liked and a favourite counts once), by anyone in
// the house (phase 36: a person's mark is the house's here too). The song
// suggestions weight these, and the agent's
// queue and playlist tools name them (phase 25).
public static class PreferredTracks
{
    // The (provider, item) of each preferred track among `tracks`, asked
    // LikeStore.MaxLookup at a time.
    public static Result<HashSet<(string ProviderId, string ProviderItemId)>, LibraryError> Of(LikeStore likes,
        PlaylistStore playlists, IEnumerable<Track> tracks)
    {
        var preferred = new HashSet<(string, string)>();
        foreach (var chunk in tracks.Select(t => new ItemRef(LikeKind.Track, t.ProviderId, t.ProviderItemId))
                     .Distinct().Chunk(LikeStore.MaxLookup))
        {
            var liked = likes.Liked(chunk, UserStore.Everyone);
            if (!liked.IsSuccess)
                return liked.Failure;
            var favorited = playlists.Favorited(chunk, UserStore.Everyone);
            if (!favorited.IsSuccess)
                return favorited.Failure;
            preferred.UnionWith(liked.Value.Keys.Concat(favorited.Value.Keys).Select(i => (i.ProviderId, i.ProviderItemId)));
        }
        return preferred;
    }

    public static bool Has(this HashSet<(string ProviderId, string ProviderItemId)> preferred, Track track) =>
        preferred.Contains((track.ProviderId, track.ProviderItemId));
}
