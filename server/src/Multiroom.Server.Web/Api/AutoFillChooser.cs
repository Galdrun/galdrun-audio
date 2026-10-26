using Multiroom.Server.Abstractions;
using Multiroom.Server.Ai;
using Multiroom.Server.Library;
using Multiroom.Server.Playback;
using Multiroom.Server.Providers;
using Multiroom.Server.Providers.AddIns;

namespace Multiroom.Server.Web.Api;

// What AutoFill adds, wired here because only
// Web sees both halves. `ai` is phase 24's suggestion loop, not the agent:
// seeded with the queue's last ten tracks (those the zone's listener or the
// household liked or made a favourite first; ZoneUsers), the zone's
// request as the request, the queue excluded, one model call per round and
// nothing it can do but name songs. `recommendations` is phase 12's merge over
// the same seed. Both leave out the songs among the house's last AvoidRecent
// plays, by song and not only by track, so a song just heard from the library
// does not come back from Tidal. When the AI fails or finds nothing, nothing
// is added and the reason is said, unless Fallback fills from recommendations
// and says so: the AI's choice is never quietly replaced.
public sealed class AutoFillChooser(SongSuggestions suggestions, AddInRegistry addIns, PlayHistoryStore history,
    ZoneUsers users)
    : IAutoFillChooser
{
    public const int SeedTracks = 10;

    // Recommendations asked for, before the queue and the recent plays are
    // taken out of them.
    public const int RecommendationsAsked = 100;

    public async Task<AutoFillChoice> ChooseAsync(string zone, IReadOnlyList<Track> queue, AutoFillSettings settings,
        CancellationToken cancellationToken)
    {
        using var origin = AiOriginScope.Begin(AiOrigin.AutoFill);
        // A history that cannot be read avoids nothing: it weighs, it does not decide.
        IReadOnlyList<Track> played = settings.AvoidRecent > 0 && history.Recent(settings.AvoidRecent) is { IsSuccess: true } recent
            ? [.. recent.Value.Select(p => p.Track).Where(MediaTypeClassifier.IsMusic)]
            : [];
        var listener = users.Of(zone);
        var seed = suggestions.Pick([.. queue.TakeLast(ApiEndpoint.SeedCandidates)], SeedTracks, fromEnd: true, listener);
        if (settings.Source == AutoFillSource.Recommendations)
            return await RecommendAsync(seed, queue, played, settings.Count, cancellationToken).ConfigureAwait(false);

        var answer = await suggestions.SuggestAsync(settings.Request, seed, queue, settings.Count, null, null, null,
            cancellationToken, played, user: listener).ConfigureAwait(false);
        string reason;
        if (answer.IsSuccess)
        {
            List<Track> found = [.. answer.Value.Songs.Select(s => s.Track).OfType<Track>()];
            if (found.Count > 0)
                return new AutoFillChoice(found, AutoFillSource.Ai);
            reason = $"nothing found: {answer.Value.NotFound} song(s) named, none playable here";
        }
        else
        {
            var error = ApiError.From(answer.Failure);
            reason = $"{error.Error}: {error.Message}";
        }
        if (!settings.Fallback)
            return new AutoFillChoice([], AutoFillSource.Ai, Reason: reason);
        var fallback = await RecommendAsync(seed, queue, played, settings.Count, cancellationToken).ConfigureAwait(false);
        return fallback with
        {
            Fallback = true,
            Reason = fallback.Reason is { } also ? $"ai failed ({reason}); {also}" : $"ai failed ({reason})",
        };
    }

    private async Task<AutoFillChoice> RecommendAsync(IReadOnlyList<Track> seed, IReadOnlyList<Track> queue,
        IReadOnlyList<Track> played, int count, CancellationToken cancellationToken)
    {
        if (seed.Count == 0)
            return new AutoFillChoice([], AutoFillSource.Recommendations, Reason: "the queue holds no track to seed from");
        var answer = await addIns.RecommendAsync(seed, RecommendationsAsked, cancellationToken).ConfigureAwait(false);
        var queued = queue.Select(t => (t.ProviderId, t.ProviderItemId)).ToHashSet();
        var avoid = played.Select(t => SongResolver.SongKey(t.Title, t.Artist)).ToHashSet(StringComparer.Ordinal);
        List<Track> chosen = [.. answer.Items.Select(i => i.Track)
            .Where(t => !queued.Contains((t.ProviderId, t.ProviderItemId)) && !avoid.Contains(SongResolver.SongKey(t.Title, t.Artist)))
            .Take(count)];
        if (chosen.Count > 0)
            return new AutoFillChoice(chosen, AutoFillSource.Recommendations);
        var failed = answer.Failures.Count > 0
            ? "; " + string.Join(", ", answer.Failures.Select(f => $"{f.AddIn}: {f.Error}"))
            : "";
        return new AutoFillChoice([], AutoFillSource.Recommendations,
            Reason: $"nothing found: no recommendation that is not queued or played lately{failed}");
    }
}
