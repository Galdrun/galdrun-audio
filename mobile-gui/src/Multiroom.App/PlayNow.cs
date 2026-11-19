using System.Text.Json;
using Multiroom.App.Api;

namespace Multiroom.App;

// "Play now", as the app's settings say (Preferences.PlayNowKeepsQueue): the items replace the zone's queue,
// or they go in right after what plays (where "Add as next" puts them) and play at once, the rest staying.
internal static class PlayNow
{
    // Start is the track of the items to begin with (a track picked from a list); otherwise the first added.
    public static async Task<JsonElement?> PlayAsync(MultiroomClient api, string zone, AddItems items, bool keepQueue,
        Track? start, CancellationToken ct)
    {
        var answer = await api.QueueAsync(zone, items with { Replace = !keepQueue, Next = keepQueue }, ct);
        long? first = null;
        if ((keepQueue || start is not null) && answer is { } json)
        {
            QueueState? queue;
            try { queue = json.Deserialize<QueueState>(MultiroomClient.Json); }
            catch (JsonException) { queue = null; }
            if (queue is { Items: { } list })
            {
                // Added next: right after the current item, or first when none is current (the server's rule).
                var at = keepQueue && queue.Current is { } current && list.ToList().FindIndex(i => i.Id == current) is var index and >= 0 ? index + 1 : 0;
                var added = list.Skip(at).ToArray();
                first = start is { } s
                    ? added.FirstOrDefault(q => q.Track is { } t && t.ProviderId == s.ProviderId && t.ProviderItemId == s.ProviderItemId)?.Id
                    : null;
                if (first is null && keepQueue) first = added.FirstOrDefault()?.Id;
            }
        }
        await api.TransportAsync(zone, "play", first, ct);
        return answer;
    }
}
