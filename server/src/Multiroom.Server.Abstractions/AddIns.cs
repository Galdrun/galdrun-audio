using System.Text.Json;

namespace Multiroom.Server.Abstractions;

// The add-in surface: where derived data and
// "more like this" plug in without touching the rest. An add-in is a class
// the host registers (AddAddIn), one line per add-in; nothing is loaded
// dynamically. One class may be both kinds.
//
// The contract for every implementation, AI or not:
// - It runs out of process, or on bounded resources of its own.
// - It is never on a search or playback path: only the recommendation
//   endpoint and the enrichment task call it.
// - Each call is bounded by its registration's timeout. A call that does not
//   answer in time is abandoned, and at most a few calls of one add-in are
//   ever unanswered at once; past that it is not asked.
// - A failure is "no suggestions" or "no data", never the caller's error.
// - What it costs is on status: calls, failures, timeouts and time spent.
public interface IAddIn
{
    // "library": the name of everything it answers, on status and in answers.
    string Id { get; }

    string DisplayName { get; }
}

// "More like this": ranked provider-scoped tracks for a seed (a track, a
// playlist's tracks, a queue), each with one line saying why. The seed's own
// tracks are left out of the answer by the caller.
public interface IRecommendationPlugin : IAddIn
{
    Task<Result<IReadOnlyList<Recommendation>, AddInError>> RecommendAsync(IReadOnlyList<Track> seed, int limit,
        CancellationToken cancellationToken);
}

// Derived data for one library track (mood, tempo, an embedding: whatever it
// computes), as one JSON value. It is kept per track and add-in, and asked
// for again only when the track's file changes.
public interface IEnrichmentPlugin : IAddIn
{
    Task<Result<JsonElement, AddInError>> EnrichAsync(Track track, CancellationToken cancellationToken);
}

// Score is the add-in's own, higher is closer; only its order among the
// add-in's answers means anything. Reason is one line: "same artist Miles
// Davis, genre Jazz".
public sealed record Recommendation(Track Track, double Score, string Reason);

public enum AddInError
{
    // The seed or the track is unknown to it.
    NotFound,

    // A seed or a limit it cannot take.
    InvalidArgument,

    // Disabled, busy with calls that have not answered, or cannot answer now.
    Unavailable,

    // Anything else, an exception included.
    Failed,

    // No answer within its registration's timeout.
    Timeout,
}
