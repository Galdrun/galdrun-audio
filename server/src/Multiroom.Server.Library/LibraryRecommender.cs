using Multiroom.Server.Abstractions;

namespace Multiroom.Server.Library;

// The one built-in "more like this" add-in (phase 12): metadata similarity
// over the library, LibraryCatalog.SimilarAsync. Boring and bounded on
// purpose: it is the proof that the add-in surface works.
public sealed class LibraryRecommender(LibraryCatalog catalog) : IRecommendationPlugin
{
    public const string AddInId = "library";

    public string Id => AddInId;

    public string DisplayName => "Library similarity";

    public async Task<Result<IReadOnlyList<Recommendation>, AddInError>> RecommendAsync(IReadOnlyList<Track> seed,
        int limit, CancellationToken cancellationToken)
    {
        var similar = await catalog.SimilarAsync(seed, limit, cancellationToken).ConfigureAwait(false);
        if (!similar.IsSuccess)
            return Result.Fail(AddInError.Failed, similar.Failure.Detail);
        List<Recommendation> found = [.. similar.Value];
        return found;
    }
}
