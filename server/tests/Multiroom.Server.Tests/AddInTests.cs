using System.Diagnostics;
using System.Text.Json;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Logging.Abstractions;
using Multiroom.Server.Abstractions;
using Multiroom.Server.Library;
using Multiroom.Server.Providers.AddIns;

namespace Multiroom.Server.Tests;

// The add-in surface (phase 12): the built-in recommender over a library the
// tests tag, the registry's bounds, and enrichment on its own task.
public sealed class AddInTests : IAsyncDisposable
{
    private readonly TempDirectory _dir = new();
    private readonly LibraryDatabase _database;
    private readonly LibraryCatalog _catalog;
    private readonly List<IAsyncDisposable> _stops = [];
    private LibraryIndexer? _indexer;

    public AddInTests()
    {
        _database = new LibraryDatabase(Path.Combine(_dir.Path, "library.db"));
        _catalog = new LibraryCatalog(_database, "local");
    }

    private static CancellationToken Ct => TestContext.Current.CancellationToken;

    public async ValueTask DisposeAsync()
    {
        foreach (var stop in Enumerable.Reverse(_stops))
            await stop.DisposeAsync();
        if (_indexer is not null)
        {
            await _indexer.StopAsync(CancellationToken.None);
            _indexer.Dispose();
        }
        _dir.Dispose();
    }

    [Fact]
    public async Task The_library_recommender_ranks_by_what_a_track_shares_with_the_seed_and_says_why_in_one_line()
    {
        await SeedAsync();
        var playlists = new PlaylistStore(_database);
        var mix = playlists.CreatePlaylist("Mix").Value.Id;
        playlists.AddEntries(mix, [await TrackAsync("so what"), await TrackAsync("noise")], null);
        var recommender = new LibraryRecommender(_catalog);

        var similar = (await recommender.RecommendAsync([await TrackAsync("so what")], 10, Ct)).Value;

        Assert.Equal(
        [
            ("Blue in Green", 6.0, "same artist Miles Davis, genre Jazz, era 1959"),
            ("Freddie Freeloader", 6.0, "same artist Miles Davis, genre Jazz, era 1959"),
            ("Naima", 3.0, "genre Jazz, era 1960"),
            ("Peace Piece", 3.0, "genre Jazz, era 1958"),
            ("Noise", 2.0, "in a playlist with the seed"),
        ], similar.Select(r => (r.Track.Title, r.Score, r.Reason)));
        Assert.All(similar, r => Assert.Equal("local", r.Track.ProviderId));

        // A composer counts; the limit cuts.
        var blue = (await recommender.RecommendAsync([await TrackAsync("blue in green")], 3, Ct)).Value;
        Assert.Equal(["Freddie Freeloader", "So What", "Peace Piece"], blue.Select(r => r.Track.Title));
        Assert.Equal("genre Jazz, composer Bill Evans, era 1958", blue[2].Reason);

        // A seed of another provider counts by its artist's name.
        var foreign = (await recommender.RecommendAsync(
            [new Track("tidal", "track:9", "Giant Steps", "john coltrane")], 10, Ct)).Value;
        Assert.Equal(("Naima", "same artist John Coltrane"), (foreign[0].Track.Title, foreign[0].Reason));

        Assert.Empty((await recommender.RecommendAsync([new Track("tidal", "track:1", "Unknown")], 10, Ct)).Value);
        Assert.Empty((await recommender.RecommendAsync([], 10, Ct)).Value);
    }

    [Fact]
    public async Task An_add_in_that_never_answers_costs_its_own_timeout_and_is_not_asked_past_its_bound()
    {
        await SeedAsync();
        var registry = await StartRegistryAsync(
            new AddInRegistration(LibraryRecommender.AddInId, _ => new LibraryRecommender(_catalog), TimeSpan.FromSeconds(5)),
            new AddInRegistration("sleepy", _ => new Sleepy("sleepy"), TimeSpan.FromMilliseconds(200)),
            new AddInRegistration("broken", _ => new Throwing(), TimeSpan.FromSeconds(5)));
        var seed = await TrackAsync("so what");

        var clock = Stopwatch.StartNew();
        var answer = await registry.RecommendAsync([seed], 10, Ct);
        clock.Stop();

        Assert.True(clock.Elapsed < TimeSpan.FromSeconds(2), $"answered in {clock.Elapsed}");
        Assert.Equal("Blue in Green", answer.Items[0].Track.Title);
        var source = Assert.Single(answer.Items[0].Sources);
        Assert.Equal((LibraryRecommender.AddInId, 1.0), (source.AddIn, source.Score));
        Assert.Equal(0.5, answer.Items.Single(i => i.Track.Title == "Naima").Score);
        Assert.Equal([new RecommendationCount(LibraryRecommender.AddInId, 4)], answer.Sources);
        Assert.Equal(
        [
            new AddInFailure("sleepy", "timeout", "no answer within 200 ms"),
            new AddInFailure("broken", "failed", "the model is not loaded"),
        ], answer.Failures);

        // The abandoned call is still out; three more fill the bound, and the
        // next is refused without waiting.
        for (var i = 0; i < AddInRegistry.MaxInFlight - 1; i++)
            await registry.RecommendAsync([seed], 10, Ct);
        clock.Restart();
        answer = await registry.RecommendAsync([seed], 10, Ct);
        Assert.True(clock.Elapsed < TimeSpan.FromMilliseconds(150), $"waited {clock.Elapsed}");
        Assert.Contains(new AddInFailure("sleepy", "unavailable", "sleepy is busy: 4 calls have not answered"),
            answer.Failures);

        var sleepy = registry.Status.Single(s => s.Id == "sleepy");
        Assert.Equal((AddInRegistry.MaxInFlight + 1L, 4L, 1L, 4L), (sleepy.Calls, sleepy.Timeouts, sleepy.Busy, sleepy.InFlight));
        Assert.True(sleepy.WaitedMs >= 4 * 200);
        var library = registry.Status.Single(s => s.Id == LibraryRecommender.AddInId);
        Assert.Equal((AddInState.Ready, 5L, 0L, 0L), (library.State, library.Calls, library.Timeouts, library.InFlight));
        Assert.Equal(["recommendation"], library.Kinds);
        Assert.Equal(5L, registry.Status.Single(s => s.Id == "broken").Failures);
    }

    [Fact]
    public async Task Recommendations_neither_seed_from_nor_answer_an_audiobook()
    {
        var echo = new Echo();
        var registry = await StartRegistryAsync(new AddInRegistration("echo", _ => echo, TimeSpan.FromSeconds(5)));
        var song = new Track("x", "seed", "Seed Song");
        var answer = await registry.RecommendAsync(
            [song, new Track("x", "book", "Seed Chapter", MediaType: MediaType.Audiobook)], 10, Ct);

        Assert.Equal([song], echo.Seed);
        Assert.Equal(["A Song"], answer.Items.Select(i => i.Track.Title));
    }

    [Fact]
    public async Task An_add_in_that_fails_to_start_is_disabled_with_its_reason_and_an_id_registered_twice_refuses()
    {
        var registry = await StartRegistryAsync(
            new AddInRegistration("throws", _ => throw new InvalidOperationException("no GPU"), TimeSpan.FromSeconds(1)),
            new AddInRegistration("renamed", _ => new Sleepy("other"), TimeSpan.FromSeconds(1)),
            new AddInRegistration("neither", _ => new Neither(), TimeSpan.FromSeconds(1)));

        Assert.Equal(
        [
            ("throws", AddInState.Disabled, "no GPU"),
            ("renamed", AddInState.Disabled, "registered as renamed but reports other"),
            ("neither", AddInState.Disabled, "neither a recommendation nor an enrichment add-in"),
        ], registry.Status.Select(s => (s.Id, s.State, s.Detail)));
        var answer = await registry.RecommendAsync([new Track("local", "track:1", "X")], 10, Ct);
        Assert.Empty(answer.Items);
        Assert.Empty(answer.Failures);

        await using var twice = new AddInRegistry(
            [new AddInRegistration("a", _ => new Sleepy("a"), TimeSpan.FromSeconds(1)),
             new AddInRegistration("a", _ => new Sleepy("a"), TimeSpan.FromSeconds(1))],
            new ServiceCollection().BuildServiceProvider(), NullLogger<AddInRegistry>.Instance);
        var refused = await Assert.ThrowsAsync<InvalidOperationException>(() => twice.StartAsync(Ct));
        Assert.Equal("add-in id a is registered twice", refused.Message);
    }

    [Fact]
    public async Task Enrichment_asks_each_track_once_on_its_own_task_records_refusals_and_asks_again_when_the_file_changes()
    {
        var music = await SeedAsync();
        var registry = await StartRegistryAsync(
            new AddInRegistration("tempo", _ => new Tempo(), TimeSpan.FromSeconds(5)),
            new AddInRegistration("sleepy", _ => new Sleepy("sleepy"), TimeSpan.FromMilliseconds(200)));
        var store = new EnrichmentStore(_database, _catalog);
        var runner = new EnrichmentRunner(registry, store, _indexer!, NullLogger<EnrichmentRunner>.Instance);
        await runner.StartAsync(Ct);
        _stops.Add(new Stopping(runner));

        await Until(() => store.Counts("tempo").Value.Pending == 0, "every track enriched");
        Assert.Equal(new EnrichmentCounts(6, 1, 0), store.Counts("tempo").Value);
        var soWhat = (await TrackAsync("so what")).ProviderItemId;
        Assert.Equal("So What", store.Get("tempo", soWhat).Value.GetProperty("title").GetString());
        var noise = (await TrackAsync("noise")).ProviderItemId;
        Assert.Equal($"tempo could not enrich {noise}: NotFound: no tempo in noise", store.Get("tempo", noise).Failure.Detail);
        Assert.Equal(LibraryError.NotFound, store.Get("tempo", "track:999").Failure.Code);

        // The add-in that never answers held up nothing, and rests.
        await Until(() => registry.Status.Single(s => s.Id == "sleepy").Timeouts == 1, "the sleepy add-in's timeout");
        var sleepy = registry.Status.Single(s => s.Id == "sleepy");
        Assert.Equal((1L, 1L), (sleepy.Calls, sleepy.Timeouts));
        Assert.Equal(new EnrichmentCounts(0, 0, 7), store.Counts("sleepy").Value);

        // A changed file is a new question; its track id stays.
        var staging = Path.Combine(_dir.Path, "staging.wav");
        LibraryTests.WriteTrack(staging, "So What (take 2)", "Miles Davis", "Kind of Blue", 1, "Jazz", year: 1959);
        File.Move(staging, Path.Combine(music, "so-what.wav"), overwrite: true);
        await Until(() => store.Get("tempo", soWhat) is { IsSuccess: true } data &&
            data.Value.GetProperty("title").GetString() == "So What (take 2)", "the changed track enriched again");
    }

    // Seven tagged tracks in one watched folder, indexed.
    private async Task<string> SeedAsync()
    {
        var music = Directory.CreateDirectory(Path.Combine(_dir.Path, "music")).FullName;
        LibraryTests.WriteTrack(Path.Combine(music, "so-what.wav"), "So What", "Miles Davis", "Kind of Blue", 1, "Jazz", year: 1959);
        LibraryTests.WriteTrack(Path.Combine(music, "freddie.wav"), "Freddie Freeloader", "Miles Davis", "Kind of Blue", 2, "Jazz", year: 1959);
        LibraryTests.WriteTrack(Path.Combine(music, "blue.wav"), "Blue in Green", "Miles Davis", "Kind of Blue", 3, "Jazz", "Bill Evans", 1959);
        LibraryTests.WriteTrack(Path.Combine(music, "peace.wav"), "Peace Piece", "Bill Evans", "Everybody Digs", 1, "Jazz", "Bill Evans", 1958);
        LibraryTests.WriteTrack(Path.Combine(music, "naima.wav"), "Naima", "John Coltrane", "Giant Steps", 7, "Jazz", year: 1960);
        LibraryTests.WriteTrack(Path.Combine(music, "runaway.wav"), "Runaway", "Aurora", "All My Demons", 1, "Pop", year: 2015);
        LibraryTests.WriteTrack(Path.Combine(music, "noise.wav"), "Noise", "Static", "Hiss", 1, "Rock", year: 1990);
        _indexer = new LibraryIndexer(
            LibraryTests.Registry(new LibrarySettings([new LibraryFolder(music, LibraryFolderType.Local)], TimeSpan.FromMinutes(5)),
                _database, _dir.Path), _database, NullLogger<LibraryIndexer>.Instance);
        await _indexer.StartAsync(Ct);
        await Until(() => _indexer.Status.Passes == 1, "the first pass");
        return music;
    }

    private async Task<Track> TrackAsync(string text) =>
        Assert.Single((await _catalog.SearchAsync(text, SearchKinds.All, 0, 10, Ct)).Value.Tracks);

    private async Task<AddInRegistry> StartRegistryAsync(params AddInRegistration[] registrations)
    {
        var registry = new AddInRegistry(registrations, new ServiceCollection().BuildServiceProvider(),
            NullLogger<AddInRegistry>.Instance);
        await registry.StartAsync(Ct);
        _stops.Add(registry);
        return registry;
    }

    private static async Task Until(Func<bool> condition, string what)
    {
        var deadline = DateTime.UtcNow + TimeSpan.FromSeconds(20);
        while (!condition())
        {
            if (DateTime.UtcNow > deadline)
                Assert.Fail($"timed out waiting for {what}");
            await Task.Delay(20, Ct);
        }
    }

    private sealed class Stopping(EnrichmentRunner runner) : IAsyncDisposable
    {
        public async ValueTask DisposeAsync()
        {
            await runner.StopAsync(CancellationToken.None);
            runner.Dispose();
        }
    }

    // Both kinds; never answers, and ignores its cancellation.
    private sealed class Sleepy(string id) : IRecommendationPlugin, IEnrichmentPlugin
    {
        public string Id => id;

        public string DisplayName => "Sleepy";

        public Task<Result<IReadOnlyList<Recommendation>, AddInError>> RecommendAsync(IReadOnlyList<Track> seed,
            int limit, CancellationToken cancellationToken) =>
            new TaskCompletionSource<Result<IReadOnlyList<Recommendation>, AddInError>>().Task;

        public Task<Result<JsonElement, AddInError>> EnrichAsync(Track track, CancellationToken cancellationToken) =>
            new TaskCompletionSource<Result<JsonElement, AddInError>>().Task;
    }

    // Answers a song and a book, and remembers the seed it was given.
    private sealed class Echo : IRecommendationPlugin
    {
        public IReadOnlyList<Track> Seed { get; private set; } = [];

        public string Id => "echo";

        public string DisplayName => "Echo";

        public Task<Result<IReadOnlyList<Recommendation>, AddInError>> RecommendAsync(IReadOnlyList<Track> seed,
            int limit, CancellationToken cancellationToken)
        {
            Seed = seed;
            return Task.FromResult<Result<IReadOnlyList<Recommendation>, AddInError>>(new Recommendation[]
            {
                new(new Track("x", "song", "A Song"), 1, "echo"),
                new(new Track("x", "chapter", "A Chapter", MediaType: MediaType.Audiobook), 2, "echo"),
            });
        }
    }

    private sealed class Throwing : IRecommendationPlugin
    {
        public string Id => "broken";

        public string DisplayName => "Broken";

        public Task<Result<IReadOnlyList<Recommendation>, AddInError>> RecommendAsync(IReadOnlyList<Track> seed,
            int limit, CancellationToken cancellationToken) =>
            throw new InvalidOperationException("the model is not loaded");
    }

    private sealed class Neither : IAddIn
    {
        public string Id => "neither";

        public string DisplayName => "Neither";
    }

    // The title back as data; "Noise" is refused.
    private sealed class Tempo : IEnrichmentPlugin
    {
        public string Id => "tempo";

        public string DisplayName => "Tempo";

        public Task<Result<JsonElement, AddInError>> EnrichAsync(Track track, CancellationToken cancellationToken) =>
            Task.FromResult<Result<JsonElement, AddInError>>(track.Title == "Noise"
                ? Result.Fail(AddInError.NotFound, "no tempo in noise")
                : JsonSerializer.SerializeToElement(new { title = track.Title }));
    }
}
