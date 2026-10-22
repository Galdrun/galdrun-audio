using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Logging.Abstractions;
using Multiroom.Server.Abstractions;
using Multiroom.Server.Library;
using Multiroom.Server.Playback;
using Multiroom.Server.Providers;

namespace Multiroom.Server.Tests;

// A song liked as a radio played it is found among the providers, the
// library first, and kept beside the like; not while the setting is off.
public sealed class RadioLikeFinderTests : IAsyncDisposable
{
    private readonly TempDirectory _dir = new();
    private readonly FakeCatalog _local = new("local");
    private readonly FakeCatalog _stream = new("stream");
    private readonly ServiceProvider _services = new ServiceCollection().BuildServiceProvider();
    private readonly PlaybackSettings _settings = new();
    private ProviderRegistry? _registry;
    private RadioLikeFinder? _finder;

    private static CancellationToken Ct => TestContext.Current.CancellationToken;

    public async ValueTask DisposeAsync()
    {
        if (_finder is not null)
        {
            await _finder.StopAsync(CancellationToken.None);
            _finder.Dispose();
        }
        if (_registry is not null)
            await _registry.DisposeAsync();
        await _services.DisposeAsync();
        _dir.Dispose();
    }

    private async Task<LikeStore> StartAsync()
    {
        var database = new LibraryDatabase(Path.Combine(_dir.Path, "library.db"));
        Assert.True(database.Migrate().IsSuccess);
        var likes = new LikeStore(database);
        _registry = new ProviderRegistry([new("stream", _ => _stream), new("local", _ => _local)], _services,
            NullLogger<ProviderRegistry>.Instance);
        await _registry.StartAsync(Ct);
        var resolver = new SongResolver(new UnifiedSearch(_registry, TimeSpan.FromSeconds(5)), _registry, () => null);
        _finder = new RadioLikeFinder(likes, resolver, _settings, NullLogger<RadioLikeFinder>.Instance, TimeSpan.Zero);
        await _finder.StartAsync(Ct);
        return likes;
    }

    private static async Task<FoundTrack?> FoundAsync(LikeStore likes, long id)
    {
        for (var i = 0; i < 100; i++)
        {
            if (likes.Likes(null, UserStore.Everyone).Value.Single(l => l.Id == id).Found is { } found)
                return found;
            await Task.Delay(20, Ct);
        }
        return null;
    }

    [Fact]
    public async Task A_liked_radio_song_is_found_in_the_library_first_and_marked_close_when_only_its_core_matched()
    {
        var owned = _local.Add("So What", "Miles Davis", "Kind of Blue");
        _stream.Add("So What", "Miles Davis", "Kind of Blue");
        var remaster = _stream.Add("Blue in Green - Remastered 1997", "Miles Davis");
        var likes = await StartAsync();
        var changed = new List<LikedItem>();
        likes.Changed += (_, l) => changed.Add(l);

        var exact = likes.Add(RadioTrackMetadata.FromTitle("Miles Davis - So What")!).Value.Like;
        Assert.Equal(new FoundTrack(owned, false), await FoundAsync(likes, exact.Id));
        var close = likes.Add(RadioTrackMetadata.FromTitle("Miles Davis - Blue in Green")!).Value.Like;
        Assert.Equal(new FoundTrack(remaster, true), await FoundAsync(likes, close.Id));

        Assert.Empty(likes.UnfoundRadioSongs().Value);
        Assert.Contains(changed, l => l.Id == exact.Id && l.Found?.Track == owned);
        // The like itself stays the radio's song.
        Assert.Equal(RadioTrackMetadata.ProviderId, likes.Likes(null).Value.Single(l => l.Id == exact.Id).Track!.ProviderId);
    }

    [Fact]
    public async Task Nothing_is_looked_for_while_the_setting_is_off_and_all_is_when_it_is_turned_on()
    {
        var owned = _local.Add("Teardrop", "Massive Attack");
        _settings.Set(null, findRadioLikes: false);
        var likes = await StartAsync();

        var like = likes.Add(RadioTrackMetadata.FromTitle("Massive Attack - Teardrop")!).Value.Like;
        likes.Add(RadioTrackMetadata.FromTitle("Nobody - Nothing like it")!);
        await Task.Delay(200, Ct);
        Assert.Equal(0, _local.Searches);
        Assert.Equal(2, likes.UnfoundRadioSongs().Value.Count);

        _settings.Set(null, findRadioLikes: true);
        Assert.Equal(new FoundTrack(owned, false), await FoundAsync(likes, like.Id));
        Assert.Equal("Nothing like it", Assert.Single(likes.UnfoundRadioSongs().Value).Track!.Title);
    }
}
