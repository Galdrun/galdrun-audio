using System.IO.Pipelines;
using Multiroom.Server.Abstractions;
using Multiroom.Server.Providers;

namespace Multiroom.Server.Tests;

// The whole provider SPI over an in-memory catalog: tracks are files served
// with their length, stations are byte arrays served live. Its root browses
// to two nodes, "tracks" and "stations"; every artwork reference answers the
// same bytes. A search finds a title holding the text, or every word of it
// in a track's artist and title.
internal sealed class FakeProvider : IProvider, ISearchProvider, IBrowseProvider, IStreamResolver,
    ILiveStreamResolver, IArtworkProvider, IStationTitles
{
    public const string ProviderId = "fake";

    public static readonly byte[] ArtworkBytes = [0x89, 0x50, 0x4E, 0x47];

    private readonly Dictionary<string, (Track Track, string File)> _tracks = new(StringComparer.Ordinal);
    private readonly Dictionary<string, (RadioStation Station, byte[] Data)> _stations =
        new(StringComparer.Ordinal);

    public string Id => ProviderId;

    public string DisplayName => "Fake";
    public string? RadioTitle { get; set; }
    public string? CurrentTitle(string stationId) => _stations.ContainsKey(stationId) ? RadioTitle : null;

    public ProviderCapabilities Capabilities => ProviderCapabilities.Search | ProviderCapabilities.Browse |
        ProviderCapabilities.Stream | ProviderCapabilities.LiveStream | ProviderCapabilities.Artwork;

    public ProviderStatus Status { get; set; } = new(ProviderState.Ready, "fake catalog");

    public Track AddTrack(string file, string title)
    {
        var track = new Track(ProviderId, $"t{_tracks.Count}", title, "Fake Artist",
            Artwork: new ArtworkRef(ProviderId, "cover"));
        _tracks.Add(track.ProviderItemId, (track, file));
        return track;
    }

    public RadioStation AddStation(string name, byte[] data)
    {
        var station = new RadioStation(ProviderId, $"s{_stations.Count}", name);
        _stations.Add(station.ProviderItemId, (station, data));
        return station;
    }

    public Task<ProviderStatus> GetStatusAsync(CancellationToken cancellationToken) => Task.FromResult(Status);

    public Task<Result<CatalogItems, ProviderError>> SearchAsync(string text, SearchKinds kinds, int offset, int limit,
        CancellationToken cancellationToken)
    {
        var tracks = _tracks.Values.Select(t => t.Track).Where(_ => kinds.HasFlag(SearchKinds.Tracks))
            .Where(t => t.Title.Contains(text, StringComparison.OrdinalIgnoreCase) || ByWords(text, t))
            .Skip(offset).Take(limit).ToList();
        var stations = _stations.Values.Select(s => s.Station).Where(_ => kinds.HasFlag(SearchKinds.Stations))
            .Where(s => s.Name.Contains(text, StringComparison.OrdinalIgnoreCase)).Skip(offset).Take(limit).ToList();
        return Task.FromResult<Result<CatalogItems, ProviderError>>(new CatalogItems(tracks, [], [], stations, []));
    }

    // "artist title", as the song resolver asks: every word in the track's artist or title.
    private static bool ByWords(string text, Track track) =>
        text.Split(' ', StringSplitOptions.RemoveEmptyEntries).All(w =>
            $"{track.Artist} {track.Title}".Split(' ').Contains(w, StringComparer.OrdinalIgnoreCase));

    public Task<Result<BrowsePage, ProviderError>> BrowseAsync(BrowseRequest request, CancellationToken cancellationToken)
    {
        var (nodeId, offset, limit) = (request.NodeId, request.Offset, request.Limit);
        Result<BrowsePage, ProviderError> page = nodeId switch
        {
            null => new BrowsePage([new(ProviderId, "tracks", "Tracks"), new(ProviderId, "stations", "Stations")],
                CatalogItems.Empty),
            "tracks" => new BrowsePage([],
                CatalogItems.Empty with { Tracks = _tracks.Values.Select(t => t.Track).Skip(offset).Take(limit).ToList() }),
            "stations" => new BrowsePage([],
                CatalogItems.Empty with { Stations = _stations.Values.Select(s => s.Station).Skip(offset).Take(limit).ToList() }),
            _ => Result.Fail(ProviderError.NotFound, $"no node {nodeId}"),
        };
        return Task.FromResult(page);
    }

    // What a track's bytes turn out to be on resolving, by its item id.
    public Dictionary<string, AudioFormat> Granted { get; } = new(StringComparer.Ordinal);

    public Task<Result<ResolvedStream, ProviderError>> ResolveAsync(Track track, CancellationToken cancellationToken)
    {
        if (!_tracks.TryGetValue(track.ProviderItemId, out var entry))
            return Task.FromResult<Result<ResolvedStream, ProviderError>>(
                Result.Fail(ProviderError.NotFound, $"no track {track.ProviderItemId}"));
        if (!Granted.TryGetValue(track.ProviderItemId, out var format))
            return Task.FromResult(ResolvedStream.FromFile(entry.File));
        var file = entry.File;
        return Task.FromResult<Result<ResolvedStream, ProviderError>>(ResolvedStream.KnownLength(new FileInfo(file).Length,
            "audio/mp4", () => File.OpenRead(file), format: format));
    }

    public Task<Result<ResolvedStream, ProviderError>> ResolveAsync(RadioStation station,
        CancellationToken cancellationToken)
    {
        Result<ResolvedStream, ProviderError> resolved = _stations.TryGetValue(station.ProviderItemId, out var entry)
            ? ResolvedStream.Live("audio/mpeg", () => Relay(entry.Data))
            : Result.Fail(ProviderError.NotFound, $"no station {station.ProviderItemId}");
        return Task.FromResult(resolved);
    }

    public Task<Result<Artwork, ProviderError>> GetArtworkAsync(ArtworkRef reference,
        CancellationToken cancellationToken) =>
        Task.FromResult<Result<Artwork, ProviderError>>(new Artwork("image/png", ArtworkBytes));

    // An endless relay's shape: a reader that cannot seek. It holds all of the
    // data unread.
    private static Result<Stream, ProviderError> Relay(byte[] data)
    {
        var pipe = new Pipe(new PipeOptions(pauseWriterThreshold: 0));
        pipe.Writer.WriteAsync(data).AsTask().GetAwaiter().GetResult();
        pipe.Writer.Complete();
        return pipe.Reader.AsStream();
    }
}
