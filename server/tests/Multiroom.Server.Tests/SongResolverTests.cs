using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Logging.Abstractions;
using Multiroom.Server.Abstractions;
using Multiroom.Server.Providers;

namespace Multiroom.Server.Tests;

// The song resolver (phase 24): songs named by title and artist found among
// the providers that stream tracks, matched by written-out rules, the user's
// provider order deciding where a song in both comes from.
public sealed class SongResolverTests : IAsyncDisposable
{
    private readonly FakeCatalog _local = new("local");
    private readonly FakeCatalog _stream = new("stream");
    private readonly ServiceProvider _services = new ServiceCollection().BuildServiceProvider();
    private ProviderRegistry? _registry;
    private IReadOnlyList<string>? _preference;

    private static CancellationToken Ct => TestContext.Current.CancellationToken;

    public async ValueTask DisposeAsync()
    {
        if (_registry is not null)
            await _registry.DisposeAsync();
        await _services.DisposeAsync();
    }

    private async Task<SongResolver> StartAsync()
    {
        _registry = new ProviderRegistry(
            [new("stream", _ => _stream), new("local", _ => _local), new("fake", _ => new FakeProvider())],
            _services, NullLogger<ProviderRegistry>.Instance);
        await _registry.StartAsync(Ct);
        return new SongResolver(new UnifiedSearch(_registry, TimeSpan.FromSeconds(5)), _registry, () => _preference);
    }

    private static async Task<ResolvedSongs> Resolve(SongResolver resolver, params SongSuggestion[] songs) =>
        (await resolver.ResolveAsync(songs, [], Ct)).Value;

    [Fact]
    public async Task A_song_never_resolves_to_an_audiobook_chapter_of_the_same_title()
    {
        _local.Add("The Hobbit", "Rob Inglis", "The Hobbit", MediaType.Audiobook);
        var song = _stream.Add("The Hobbit", "Rob Inglis", "Songs of Middle-earth");
        _local.Add("Riddles", "Rob Inglis", "The Hobbit", MediaType.Audiobook);
        var resolver = await StartAsync();

        var found = await Resolve(resolver, new SongSuggestion("The Hobbit", "Rob Inglis"),
            new SongSuggestion("Riddles", "Rob Inglis"));
        Assert.Equal(song, found.Songs[0].Track);
        Assert.Null(found.Songs[1].Track);
        Assert.Equal((1, 1), (found.Found, found.NotFound));
    }

    [Fact]
    public async Task The_library_comes_first_by_default_and_the_users_order_decides_otherwise()
    {
        var owned = _local.Add("So What", "Miles Davis", "Kind of Blue");
        var streamed = _stream.Add("So What", "Miles Davis", "Kind of Blue");
        var resolver = await StartAsync();

        Assert.Equal(["local", "stream", "fake"], resolver.ProviderOrder());
        var found = await Resolve(resolver, new SongSuggestion("So What", "Miles Davis", "Kind of Blue", 1959));
        Assert.Equal((1, 0), (found.Found, found.NotFound));
        Assert.Equal(owned, found.Songs[0].Track);
        Assert.Equal(SongMatchKind.Exact, found.Songs[0].Match);
        Assert.Equal("same title and artist, same album on local", found.Songs[0].Reason);

        _preference = ["stream", "local"];
        Assert.Equal(streamed, (await Resolve(resolver, new SongSuggestion("so what", "MILES DAVIS"))).Songs[0].Track);
    }

    // Phase 26: a song to avoid is not answered from any provider, in any
    // version, while another song by the same artist is.
    [Fact]
    public async Task A_song_to_avoid_is_not_answered_from_any_provider_or_version()
    {
        var heard = _local.Add("Here Comes the Sun", "The Beatles");
        _stream.Add("Here Comes the Sun - Remastered 2009", "Beatles");
        var other = _stream.Add("Something", "The Beatles");
        var resolver = await StartAsync();

        HashSet<string> avoid = [SongResolver.SongKey(heard.Title, heard.Artist)];
        Assert.Equal(SongResolver.SongKey("Here Comes The Sun (Live)", "The Beatles feat. Someone"), avoid.Single());
        var found = (await resolver.ResolveAsync(
            [new SongSuggestion("Here Comes the Sun", "The Beatles"), new SongSuggestion("Something", "Beatles")], [], Ct, avoid)).Value;
        Assert.Equal([null, other], found.Songs.Select(s => s.Track));
        Assert.Equal("the caller asked to avoid this song", found.Songs[0].Reason);
    }

    [Fact]
    public async Task A_suffix_and_a_featured_artist_match_close()
    {
        var remaster = _stream.Add("Here Comes the Sun - Remastered 2009", "The Beatles");
        var featured = _stream.Add("Empire State of Mind (feat. Alicia Keys)", "JAY-Z, Alicia Keys");
        var accented = _stream.Add("Joga", "Björk");
        var resolver = await StartAsync();

        var found = await Resolve(resolver,
            new SongSuggestion("Here Comes The Sun", "Beatles"),
            new SongSuggestion("Empire State of Mind", "Jay-Z"),
            new SongSuggestion("Jóga", "Bjork"));
        Assert.Equal([remaster, featured, accented], found.Songs.Select(s => s.Track));
        Assert.Equal(SongMatchKind.Close, found.Songs[0].Match);
        Assert.Equal("titled \"Here Comes the Sun - Remastered 2009\" on stream", found.Songs[0].Reason);
        Assert.Contains("by JAY-Z, Alicia Keys", found.Songs[1].Reason, StringComparison.Ordinal);
    }

    [Fact]
    public async Task A_version_not_asked_for_is_taken_only_without_a_plain_one()
    {
        var live = _stream.Add("Hotel California (Live)", "Eagles", "Hell Freezes Over");
        var studio = _stream.Add("Hotel California", "Eagles", "Hotel California");
        var onlyLive = _stream.Add("Desperado - Live at the Forum", "Eagles");
        var resolver = await StartAsync();

        var found = await Resolve(resolver, new SongSuggestion("Hotel California", "Eagles"),
            new SongSuggestion("Desperado", "Eagles"));
        Assert.Equal(studio, found.Songs[0].Track);
        Assert.Equal(onlyLive, found.Songs[1].Track);
        Assert.Equal(SongMatchKind.Close, found.Songs[1].Match);
        Assert.Contains("a live version; no plain one was found", found.Songs[1].Reason, StringComparison.Ordinal);

        // Asked for, it is the plain answer.
        Assert.Equal(live, (await Resolve(resolver, new SongSuggestion("Hotel California (Live)", "Eagles"))).Songs[0].Track);
    }

    [Fact]
    public async Task Another_artists_song_of_the_same_title_is_not_taken_and_a_track_is_answered_once()
    {
        _local.Add("Yesterday", "Boyz II Men");
        var song = _local.Add("Blackbird", "The Beatles", "The White Album");
        var other = _local.Add("Blackbird", "The Beatles", "Love");
        var resolver = await StartAsync();

        var found = await Resolve(resolver, new SongSuggestion("Yesterday", "The Beatles"),
            new SongSuggestion("Blackbird", "The Beatles", "Love"), new SongSuggestion("Blackbird", "Beatles"),
            new SongSuggestion("Blackbird", "Beatles"));
        Assert.Null(found.Songs[0].Track);
        Assert.Equal("no track with this title and artist was found", found.Songs[0].Reason);
        Assert.Equal(other, found.Songs[1].Track);
        Assert.Equal(song, found.Songs[2].Track);
        Assert.Null(found.Songs[3].Track);
        Assert.Equal((2, 2), (found.Found, found.NotFound));

        var excluded = (await resolver.ResolveAsync([new SongSuggestion("Blackbird", "The Beatles", "Love")], [song, other], Ct)).Value;
        Assert.Equal("every match is excluded or was found for an earlier song", excluded.Songs[0].Reason);

        Assert.False((await resolver.ResolveAsync([], [], Ct)).IsSuccess);
        Assert.False((await resolver.ResolveAsync([new SongSuggestion(" ", "x")], [], Ct)).IsSuccess);
    }
}
