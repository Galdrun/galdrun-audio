namespace Multiroom.Server.Abstractions;

// The provider SPI. A
// provider implements IProvider plus the capability interfaces its
// Capabilities names, and nothing else: it never sees the core, a zone or a
// room. It answers a query or a track with data, and fails with a Result,
// never an exception.
public interface IProvider
{
    // "local", "tidal", "webradio": the ProviderId of everything it answers.
    string Id { get; }

    string DisplayName { get; }

    // Exactly the capability interfaces this provider implements. The registry
    // disables a provider whose flags and interfaces disagree.
    ProviderCapabilities Capabilities { get; }

    Task<ProviderStatus> GetStatusAsync(CancellationToken cancellationToken);
}

[Flags]
public enum ProviderCapabilities
{
    None = 0,
    Search = 1,
    Browse = 2,
    Stream = 4,
    LiveStream = 8,
    Artwork = 16,
    Expand = 32,
}

public enum ProviderState
{
    Ready,

    // Started, but cannot answer now: a folder unreachable, a service down.
    Unavailable,

    // Started, but waiting for someone to authorize it. The detail names the
    // URL to open; the provider never blocks on a browser.
    AuthRequired,

    // Failed to start, so the registry holds no instance. Only the registry
    // reports it; a running provider never does.
    Disabled,
}

public sealed record ProviderStatus(ProviderState State, string Detail = "");

// The SPI's one error enum: what any provider can fail with, and what the
// registry answers for a provider it cannot reach.
public enum ProviderError
{
    // No such item, or no such provider.
    NotFound,

    // A resolved stream is not what it was when it was resolved: it vanished
    // or changed. One URL must keep its bytes (the media contract), so it is
    // gone rather than served different.
    Gone,

    // The provider is disabled, lacks the capability, or cannot answer now.
    Unavailable,

    // Anything else: I/O, the provider's own protocol, a provider bug.
    Failed,

    // An option a node does not take: a sort, a filter, a letter.
    InvalidArgument,
}

public interface ISearchProvider
{
    // The kinds asked for (tracks, albums, artists, playlists, stations)
    // matching the text, each list paged by offset and limit.
    Task<Result<CatalogItems, ProviderError>> SearchAsync(string text, SearchKinds kinds, int offset, int limit,
        CancellationToken cancellationToken);
}

public interface IBrowseProvider
{
    // The provider's tree. A null node is its root; otherwise a BrowseNode's
    // NodeId, or an Album's or Artist's ProviderItemId, which opens as a node
    // too. An option the node does not take is InvalidArgument.
    Task<Result<BrowsePage, ProviderError>> BrowseAsync(BrowseRequest request, CancellationToken cancellationToken);
}

// A playable container (BrowseNode.Playable) to the tracks it queues: an
// album, a playlist, a folder and what is below it, an artist's top tracks.
// At most `limit` tracks; the rest is reported, not queued.
public interface IContainerExpander
{
    Task<Result<ExpandedContainer, ProviderError>> ExpandAsync(string nodeId, int limit,
        CancellationToken cancellationToken);
}

// A track to its bytes, at stage time: authorization, token refresh and the
// first bytes all happen here, never when the core's GET arrives (the contract,
// Pace and time). The answer is KnownLength.
public interface IStreamResolver
{
    Task<Result<ResolvedStream, ProviderError>> ResolveAsync(Track track, CancellationToken cancellationToken);
}

// A station to an endless relay, connected at stage time. The answer is Live.
public interface ILiveStreamResolver
{
    Task<Result<ResolvedStream, ProviderError>> ResolveAsync(RadioStation station,
        CancellationToken cancellationToken);
}

// Artwork for the outward API. The core never asks for it.
public interface IArtworkProvider
{
    Task<Result<Artwork, ProviderError>> GetArtworkAsync(ArtworkRef reference, CancellationToken cancellationToken);
}
