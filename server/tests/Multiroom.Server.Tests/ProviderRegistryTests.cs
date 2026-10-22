using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Logging.Abstractions;
using Multiroom.Server.Abstractions;
using Multiroom.Server.Providers;

namespace Multiroom.Server.Tests;

public sealed class ProviderRegistryTests : IAsyncDisposable
{
    private readonly TempDirectory _dir = new();
    private readonly ServiceProvider _services = new ServiceCollection().BuildServiceProvider();
    private readonly List<ProviderRegistry> _registries = [];

    public async ValueTask DisposeAsync()
    {
        foreach (var registry in _registries)
            await registry.DisposeAsync();
        await _services.DisposeAsync();
        _dir.Dispose();
    }

    private static CancellationToken Ct => TestContext.Current.CancellationToken;

    [Fact]
    public async Task The_fake_provider_registers_answers_status_and_serves_every_capability()
    {
        var fake = new FakeProvider();
        var file = _dir.Write("song.flac", "not really flac");
        var track = fake.AddTrack(file, "Blue Song");
        var station = fake.AddStation("Blue Radio", [1, 2, 3]);
        var registry = await Start(new ProviderRegistration(FakeProvider.ProviderId, _ => fake));

        Assert.Equal([(FakeProvider.ProviderId, new ProviderStatus(ProviderState.Ready, "fake catalog"))],
            await registry.GetStatusesAsync(Ct));

        var found = await registry.Get<ISearchProvider>("fake").Value.SearchAsync("blue", SearchKinds.All, 0, 10, Ct);
        Assert.Equal([track], found.Value.Tracks);
        Assert.Equal([station], found.Value.Stations);

        var browse = registry.Get<IBrowseProvider>("fake").Value;
        Assert.Equal(["tracks", "stations"], (await browse.BrowseAsync(new(null, 0, 10), Ct)).Value.Nodes.Select(n => n.NodeId));
        Assert.Equal([track], (await browse.BrowseAsync(new("tracks", 0, 10), Ct)).Value.Items.Tracks);
        Assert.Equal(ProviderError.NotFound, (await browse.BrowseAsync(new("nope", 0, 10), Ct)).Failure.Code);

        var known = (await registry.Get<IStreamResolver>("fake").Value.ResolveAsync(track, Ct)).Value;
        Assert.Equal((StreamShape.KnownLength, 15L, "audio/flac"), (known.Shape, known.Length, known.ContentType));
        await using (var stream = known.Open().Value)
            Assert.True(stream.CanSeek);

        var live = (await registry.Get<ILiveStreamResolver>("fake").Value.ResolveAsync(station, Ct)).Value;
        Assert.Equal((StreamShape.Live, (long?)null), (live.Shape, live.Length));
        await using (var stream = live.Open().Value)
            Assert.False(stream.CanSeek);

        var artwork = await registry.Get<IArtworkProvider>("fake").Value.GetArtworkAsync(track.Artwork!, Ct);
        Assert.Equal(FakeProvider.ArtworkBytes, artwork.Value.Bytes.ToArray());
    }

    [Fact]
    public async Task A_provider_that_fails_to_start_is_reported_disabled_and_asked_for_nothing()
    {
        var registry = await Start(
            new ProviderRegistration("broken", _ => throw new InvalidOperationException("no such account")),
            new ProviderRegistration(FakeProvider.ProviderId, _ => new FakeProvider()));

        var statuses = await registry.GetStatusesAsync(Ct);
        Assert.Equal(("broken", new ProviderStatus(ProviderState.Disabled, "no such account")), statuses[0]);
        Assert.Equal(ProviderState.Ready, statuses[1].Status.State);
        var get = registry.Get<IStreamResolver>("broken");
        Assert.Equal(ProviderError.Unavailable, get.Failure.Code);
        Assert.Equal("provider broken is disabled: no such account", get.Failure.Detail);
    }

    [Fact]
    public async Task A_provider_whose_id_or_capabilities_do_not_hold_is_disabled_and_disposed()
    {
        var liar = new Liar("liar", ProviderCapabilities.Stream | ProviderCapabilities.Search);
        var impostor = new Liar("someone-else", ProviderCapabilities.None);
        var registry = await Start(new ProviderRegistration("liar", _ => liar),
            new ProviderRegistration("impostor", _ => impostor));

        var statuses = await registry.GetStatusesAsync(Ct);
        Assert.Equal(new ProviderStatus(ProviderState.Disabled,
            "capabilities and interfaces disagree on Search, Stream"), statuses[0].Status);
        Assert.Equal(new ProviderStatus(ProviderState.Disabled,
            "registered as impostor but reports someone-else"), statuses[1].Status);
        Assert.True(liar.Disposed);
        Assert.True(impostor.Disposed);
    }

    [Fact]
    public async Task A_status_call_that_throws_is_reported_unavailable()
    {
        var registry = await Start(new ProviderRegistration("liar", _ => new Liar("liar", ProviderCapabilities.None)));

        Assert.Equal([("liar", new ProviderStatus(ProviderState.Unavailable, "status exploded"))],
            await registry.GetStatusesAsync(Ct));
    }

    [Fact]
    public async Task An_unknown_id_is_not_found_and_a_missing_capability_is_unavailable()
    {
        var registry = await Start(new ProviderRegistration("liar", _ => new Liar("liar", ProviderCapabilities.None)));

        Assert.Equal(ProviderError.NotFound, registry.Get<IProvider>("nope").Failure.Code);
        Assert.Equal("provider liar is no ISearchProvider", registry.Get<ISearchProvider>("liar").Failure.Detail);
        Assert.True(registry.Get<IProvider>("liar").IsSuccess);
    }

    [Fact]
    public async Task An_id_registered_twice_is_refused_by_name()
    {
        var registry = new ProviderRegistry([new("fake", _ => new FakeProvider()), new("fake", _ => new FakeProvider())],
            _services, NullLogger<ProviderRegistry>.Instance);
        _registries.Add(registry);

        var refused = await Assert.ThrowsAsync<InvalidOperationException>(() => registry.StartAsync(Ct));
        Assert.Equal("provider id fake is registered twice", refused.Message);
    }

    private async Task<ProviderRegistry> Start(params ProviderRegistration[] registrations)
    {
        var registry = new ProviderRegistry(registrations, _services, NullLogger<ProviderRegistry>.Instance);
        _registries.Add(registry);
        await registry.StartAsync(Ct);
        return registry;
    }

    // Claims what it likes, implements nothing, and its status throws.
    private sealed class Liar(string id, ProviderCapabilities capabilities) : IProvider, IDisposable
    {
        public bool Disposed { get; private set; }

        public string Id => id;

        public string DisplayName => "Liar";

        public ProviderCapabilities Capabilities => capabilities;

        public Task<ProviderStatus> GetStatusAsync(CancellationToken cancellationToken) =>
            throw new InvalidOperationException("status exploded");

        public void Dispose() => Disposed = true;
    }
}
