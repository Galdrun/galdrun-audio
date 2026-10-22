using Multiroom.Server.Abstractions;
using Multiroom.Server.Library;

namespace Multiroom.Server.Providers;

// Which of some tracks the house keeps, whatever their provider: in a saved
// playlist, in a zone's queue, a favourite, or liked, by anyone. Unified
// search answers those first. A store that cannot be read keeps nothing:
// the search is only ordered as before.
public sealed class KeptTracks(PlaylistStore playlists, LikeStore likes, IQueueEngine queues)
{
    public IReadOnlySet<(string ProviderId, string ProviderItemId)> Of(IReadOnlyList<Track> tracks)
    {
        var kept = new HashSet<(string, string)>();
        if (tracks.Count == 0)
            return kept;

        if (PreferredTracks.Of(likes, playlists, tracks) is { IsSuccess: true } preferred)
            kept.UnionWith(preferred.Value);
        foreach (var chunk in tracks.Select(t => new ItemRef(LikeKind.Track, t.ProviderId, t.ProviderItemId))
                     .Distinct().Chunk(LikeStore.MaxLookup))
        {
            if (playlists.InPlaylists(chunk) is { IsSuccess: true } listed)
                kept.UnionWith(listed.Value);
        }

        var asked = tracks.Select(t => (t.ProviderId, t.ProviderItemId)).ToHashSet();
        foreach (var item in queues.Queues.SelectMany(q => q.Items))
        {
            var tracksOf = item.Group is { } group ? group.Parts.Select(p => (Track?)p.Track) : new[] { item.Track };
            foreach (var track in tracksOf)
            {
                if (track is not null && asked.Contains((track.ProviderId, track.ProviderItemId)))
                    kept.Add((track.ProviderId, track.ProviderItemId));
            }
        }
        return kept;
    }
}
