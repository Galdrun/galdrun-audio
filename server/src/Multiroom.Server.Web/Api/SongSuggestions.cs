using Multiroom.Server.Abstractions;
using Multiroom.Server.Ai;
using Multiroom.Server.Ai.Suggestions;
using Multiroom.Server.Library;
using Multiroom.Server.Providers;

namespace Multiroom.Server.Web.Api;

// A suggestion answered: every song named, found or not, in the AI's order
// (at most `count` found), the rounds it took, what it cost, and the
// providers that did not answer a search. Sources: the pages a web lookup
// cited (phase 27), as plain text; none without one.
public sealed record SuggestionAnswer(
    IReadOnlyList<ResolvedSong> Songs,
    int Found,
    int NotFound,
    int Rounds,
    AiUsage Usage,
    IReadOnlyList<SearchFailure> Failures,
    IReadOnlyList<AiSource> Sources);

// What suggestions have cost since the server started, on status.
public sealed record SuggestionStats(long Requests, long Rounds, long SongsAsked, long Found, long NotFound,
    long WebLookups);

// The loop that joins the two halves of phase 24: the
// suggester names songs, the resolver finds them, and neither knows the
// other. Twice the count is asked (at most 50); when fewer than the count are
// found, the model is asked again, told what was not found and what was
// already chosen, at most AiSettings.SuggestionRounds rounds in all. What was
// found by then is the answer. A provider that fails in the first round is
// the answer's failure; in a later one, what was found so far stands. Seed
// songs the household liked or made a favourite are marked, so the model
// weights them more. With webLookup whenAvailable and a provider that has a
// search of its own, one lookup turn comes first (phase 27, two steps by the
// owner's choice) and its notes go with every round.
public sealed class SongSuggestions(SongSuggester suggester, SongResolver resolver, AiProviderRegistry ai,
    LikeStore likes, PlaylistStore playlists)
{
    public const int MaxCount = SongSuggester.MaxCount;

    private long _requests;
    private long _rounds;
    private long _asked;
    private long _found;
    private long _notFound;
    private long _lookups;

    public SuggestionStats Stats => new(Interlocked.Read(ref _requests), Interlocked.Read(ref _rounds),
        Interlocked.Read(ref _asked), Interlocked.Read(ref _found), Interlocked.Read(ref _notFound),
        Interlocked.Read(ref _lookups));

    // Songs played lately that are named to the model as "do not suggest";
    // the rest of them are only kept out by the resolver (phase 26).
    public const int MaxAvoidNamed = 50;

    // `exclude` is never answered (what is already queued); the seed is not
    // either. `avoidSongs` (AutoFill's recent plays, newest first): their
    // songs are not answered from any provider. `avoidKeys` (phase 38): more
    // songs not to answer, by SongResolver.SongKey; `maxRounds` in place of
    // the settings' rounds.
    public async Task<Result<SuggestionAnswer, AiError>> SuggestAsync(string request, IReadOnlyList<Track> seed,
        IReadOnlyList<Track> exclude, int count, string? provider, string? model, AiReasoning? reasoning,
        CancellationToken cancellationToken, IReadOnlyList<Track>? avoidSongs = null, IReadOnlySet<string>? avoidKeys = null,
        int? maxRounds = null)
    {
        if (string.IsNullOrWhiteSpace(request))
            return Result.Fail(AiError.InvalidArgument, "request is empty");
        if (count is < 1 or > MaxCount)
            return Result.Fail(AiError.InvalidArgument, $"count: 1 to {MaxCount}, was {count}");
        var chosen = ai.Choose(provider, model);
        if (!chosen.IsSuccess)
            return chosen.Failure;
        Interlocked.Increment(ref _requests);

        // A lookup that fails marks nothing: the marks weight, they do not decide.
        var preferred = Preferred(seed);
        List<SeedSong> seedSongs = [.. seed.Select(t => new SeedSong(SongSuggestion.Of(t), preferred.Has(t)))];
        List<Track> excluded = [.. seed, .. exclude];
        var answered = new List<ResolvedSong>();
        var avoid = new List<SongSuggestion>((avoidSongs ?? [])
            .DistinctBy(t => SongResolver.SongKey(t.Title, t.Artist)).Take(MaxAvoidNamed).Select(SongSuggestion.Of));
        HashSet<string>? avoided = avoidSongs is { Count: > 0 } || avoidKeys is { Count: > 0 }
            ? [.. (avoidSongs ?? []).Select(t => SongResolver.SongKey(t.Title, t.Artist)), .. avoidKeys ?? new HashSet<string>()]
            : null;
        var failures = new List<SearchFailure>();
        long input = 0, output = 0, thought = 0, searched = 0;
        var level = reasoning ?? ai.Settings.ReasoningLevels.Suggestions;
        // Phase 27: the web lookup, once, before the rounds. A lookup that
        // fails is the answer's failure, as a first round's is.
        string? notes = null;
        IReadOnlyList<AiSource> sources = [];
        if (ai.Settings.Lookup == AiWebLookup.WhenAvailable && ai.Supports(chosen.Value.Provider, AiCapability.WebSearch))
        {
            Interlocked.Increment(ref _lookups);
            var lookup = await suggester.LookupAsync(new SongRequest(request, seedSongs, [.. avoid],
                Math.Min(2 * count, MaxCount), chosen.Value.Provider, chosen.Value.Model, level), cancellationToken)
                .ConfigureAwait(false);
            if (!lookup.IsSuccess)
                return lookup.Failure;
            (notes, sources) = (lookup.Value.Notes, lookup.Value.Sources);
            input += lookup.Value.Usage.InputTokens;
            output += lookup.Value.Usage.OutputTokens;
            thought += lookup.Value.Usage.ReasoningTokens;
            searched += lookup.Value.Usage.WebSearches;
        }
        var found = 0;
        var rounds = 0;
        while (found < count && rounds < (maxRounds ?? ai.Settings.SuggestionRounds))
        {
            rounds++;
            Interlocked.Increment(ref _rounds);
            var asked = Math.Min(2 * (count - found), MaxCount);
            var suggested = await suggester.SuggestAsync(new SongRequest(request, seedSongs, [.. avoid], asked,
                chosen.Value.Provider, chosen.Value.Model, level, notes), cancellationToken).ConfigureAwait(false);
            if (!suggested.IsSuccess)
            {
                if (rounds == 1)
                    return suggested.Failure;
                break;
            }
            input += suggested.Value.Usage.InputTokens;
            output += suggested.Value.Usage.OutputTokens;
            thought += suggested.Value.Usage.ReasoningTokens;
            var songs = suggested.Value.Songs;
            Interlocked.Add(ref _asked, songs.Count);
            if (songs.Count == 0)
                continue;
            var resolved = await resolver.ResolveAsync(songs, excluded, cancellationToken, avoided).ConfigureAwait(false);
            if (!resolved.IsSuccess)
                return Result.Fail(AiError.InvalidAnswer, resolved.Failure.Detail);
            failures.AddRange(resolved.Value.Failures.Where(f => failures.All(g => g.Provider != f.Provider)));
            foreach (var song in resolved.Value.Songs)
            {
                avoid.Add(song.Suggestion);
                if (song.Track is { } track)
                {
                    // Found beyond the count: not answered, but not suggested again either.
                    if (found == count)
                        continue;
                    found++;
                    excluded.Add(track);
                }
                answered.Add(song);
            }
        }
        var notFound = answered.Count - found;
        Interlocked.Add(ref _found, found);
        Interlocked.Add(ref _notFound, notFound);
        return new SuggestionAnswer(answered, found, notFound, rounds, new AiUsage(input, output, thought, searched), failures,
            sources);
    }

    // The liked or favourite tracks among these; none when the lookup fails.
    public HashSet<(string ProviderId, string ProviderItemId)> Preferred(IEnumerable<Track> tracks) =>
        PreferredTracks.Of(likes, playlists, tracks) is { IsSuccess: true } found ? found.Value : [];

    // At most `limit` of `tracks` as a seed, in their order: the preferred
    // ones first, then those nearest the end (a queue's latest) or the start
    // (a playlist's first).
    public IReadOnlyList<Track> Pick(IReadOnlyList<Track> tracks, int limit, bool fromEnd)
    {
        if (tracks.Count <= limit)
            return tracks;
        var preferred = Preferred(tracks);
        return [.. tracks.Select((t, i) => (Track: t, Index: i))
            .OrderBy(p => preferred.Has(p.Track) ? 0 : 1)
            .ThenBy(p => fromEnd ? -p.Index : p.Index)
            .Take(limit)
            .OrderBy(p => p.Index)
            .Select(p => p.Track)];
    }
}