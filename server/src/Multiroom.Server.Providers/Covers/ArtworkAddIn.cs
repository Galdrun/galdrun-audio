using System.Reflection;
using System.Text.Json;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Logging.Abstractions;
using Multiroom.Server.Abstractions;
using Multiroom.Server.Library;
using Multiroom.Server.Providers.AddIns;

namespace Multiroom.Server.Providers.Covers;

// The artwork config (artwork.*): whether MusicBrainz is asked (by default),
// and fanart.tv's project key, which turns fanart.tv on. A class, so no
// ToString prints the key.
public sealed class ArtworkSettings(bool musicBrainz, string? fanartTvKey)
{
    public static ArtworkSettings Default { get; } = new(true, null);

    public bool MusicBrainz { get; } = musicBrainz;

    public string? FanartTvKey { get; } = fanartTvKey;
}

public static class ArtworkServiceCollectionExtensions
{
    // The artwork add-in, always: it keeps the library's own covers; the net
    // is asked as ArtworkSettingsStore says now (`settings` the config's,
    // <directory>/settings.json what the API set), through the one
    // ArtworkLookup the artwork service shares. A change forgets what was not
    // found, so it is asked again. The local provider (AddLocalFiles) must be
    // registered too: it serves the ArtStore this add-in fills. A new source
    // is one line in Sources.
    public static IServiceCollection AddArtwork(this IServiceCollection services, ArtworkSettings settings,
        string directory) =>
        services
            .AddSingleton(new ArtworkSettingsStore(Path.Combine(directory, "settings.json"), settings))
            .AddSingleton(s =>
            {
                var current = s.GetRequiredService<ArtworkSettingsStore>();
                var http = ArtworkAddIn.Client();
                var lookup = new ArtworkLookup(s.GetRequiredService<ArtStore>(), Sources(current, http), http,
                    s.GetRequiredService<ILoggerFactory>().CreateLogger<ArtworkLookup>());
                var enrichment = s.GetRequiredService<EnrichmentStore>();
                current.Changed += (_, _) =>
                {
                    lookup.ForgetMissing();
                    enrichment.Forget(ArtworkAddIn.AddInId, ["cover", "picture"], "notFound");
                };
                return lookup;
            })
            .AddAddIn(ArtworkAddIn.AddInId, s => new ArtworkAddIn(
                new LibraryCatalog(s.GetRequiredService<LibraryDatabase>(), Local.LocalFilesProvider.ProviderId),
                s.GetRequiredService<LibraryFolders>(), s.GetRequiredService<ArtStore>(),
                s.GetRequiredService<ArtworkLookup>(), s.GetRequiredService<ILoggerFactory>().CreateLogger<ArtworkAddIn>()),
                ArtworkAddIn.Timeout);

    // The sources the settings name now, best first: fanart.tv (switched on,
    // with a key), then MusicBrainz, which also resolves.
    private static Func<ArtworkSourceSet> Sources(ArtworkSettingsStore settings, HttpClient http)
    {
        var musicBrainz = new MusicBrainzSource(http);
        var fanart = (Key: (string?)null, Source: (FanartTvSource?)null);
        var gate = new Lock();
        return () =>
        {
            var key = settings.FanartTvKey;
            FanartTvSource? fanartTv = null;
            if (key is not null)
            {
                lock (gate)
                {
                    if (fanart.Key != key)
                        fanart = (key, new FanartTvSource(http, key));
                    fanartTv = fanart.Source;
                }
            }
            var resolver = settings.MusicBrainz ? musicBrainz : null;
            List<IArtworkSource> sources = [];
            if (fanartTv is not null)
                sources.Add(fanartTv);
            if (resolver is not null)
                sources.Add(resolver);
            return new ArtworkSourceSet(resolver, sources);
        };
    }
}

// The resolver (MusicBrainz, null when off) and the sources, best first.
public sealed record ArtworkSourceSet(MusicBrainzSource? Resolver, IReadOnlyList<IArtworkSource> Sources);

// Keeps the library's art in the artwork store (ArtStore), so serving a
// cover never reads the NAS, and fills in from the net what the library
// lacks. An enrichment add-in, so it runs on EnrichmentRunner's task and is
// asked once per track, again when the track's file changes.
//
// An album's cover is the track's own picture (FolderFiles.ReadPicture:
// embedded, else beside it), copied into the store. Without one, the net is
// asked through ArtworkLookup, with the MusicBrainz ids of the track's tags
// (Picard's) when it has them. A picture is kept for the album's artist and
// the track's artist the same way, by their ids. Nothing is ever written to
// the library's folders.
public sealed partial class ArtworkAddIn(LibraryCatalog catalog, LibraryFolders folders, ArtStore store,
    ArtworkLookup lookup, ILogger? log = null) : IEnrichmentPlugin, IDisposable
{
    // Sources that do not change and a lookup of its own (tests).
    public ArtworkAddIn(LibraryCatalog catalog, LibraryFolders folders, ArtStore store, MusicBrainzSource? resolver,
        IReadOnlyList<IArtworkSource> sources, HttpClient? http = null)
        : this(catalog, folders, store, new ArtworkLookup(store, () => new ArtworkSourceSet(resolver, sources), http))
    {
        _ownsLookup = true;
    }

    public const string AddInId = "artwork";

    // A track may take a few rate-limited lookups and two downloads.
    public static readonly TimeSpan Timeout = TimeSpan.FromMinutes(1);

    private static readonly TimeSpan RequestTimeout = TimeSpan.FromSeconds(20);

    private readonly ILogger _log = log ?? NullLogger.Instance;
    private readonly bool _ownsLookup;

    public string Id => AddInId;

    public string DisplayName => lookup.Current.Sources is { Count: > 0 } sources
        ? "Artwork (" + string.Join(", ", sources.Select(s => s.Id)) + ")"
        : "Artwork (the library's own)";

    // Forgets what was not found, so it is asked again (the settings changed).
    public void ForgetMissing() => lookup.ForgetMissing();

    // The services ask for a client that names itself.
    public static HttpClient Client()
    {
        var client = new HttpClient { Timeout = RequestTimeout };
        var version = typeof(ArtworkAddIn).Assembly.GetCustomAttribute<AssemblyInformationalVersionAttribute>()
            ?.InformationalVersion.Split('+')[0] ?? "0";
        client.DefaultRequestHeaders.UserAgent.ParseAdd($"{Branding.ServerName}/{version}");
        return client;
    }

    // Answers what it did. cover: "kept" (the files' own, copied), "present"
    // (kept already), "added" (found on the net), "notFound" or "none"
    // (nothing to look up); picture the same, but never "kept".
    public async Task<Result<JsonElement, AddInError>> EnrichAsync(Track track, CancellationToken cancellationToken)
    {
        var subject = catalog.ArtworkSubjectOf(track.ProviderItemId);
        if (!subject.IsSuccess)
            return Result.Fail(AddInError.NotFound, subject.Failure.Detail);
        var s = subject.Value;

        var cover = "none";
        var albumArtistId = s.MbAlbumArtist;
        if (s.Album is { } album && s.AlbumArtist is { } albumArtist && ArtStore.Named(album, albumArtist))
        {
            var found = await CoverAsync(s, album, albumArtist, cancellationToken).ConfigureAwait(false);
            if (!found.IsSuccess)
                return found.Failure;
            (cover, albumArtistId) = found.Value;
        }

        var picture = "none";
        foreach (var (name, id) in new[] { (s.AlbumArtist, albumArtistId), (s.Artist, s.MbArtist) }
            .Where(a => a.Item1 is { } n && ArtStore.Named(n)).DistinctBy(a => SongResolver.NameKey(a.Item1!)))
        {
            var found = await lookup.PictureAsync(name!, id, cancellationToken).ConfigureAwait(false);
            if (!found.IsSuccess)
                return found.Failure;
            if (name == (s.Artist ?? s.AlbumArtist))
                picture = found.Value;
        }
        return JsonSerializer.SerializeToElement(new { cover, picture });
    }

    public void Dispose()
    {
        if (_ownsLookup)
            lookup.Dispose();
    }

    // What was done, and the album artist's id as it is known now.
    private async Task<Result<(string Cover, string? ArtistId), AddInError>> CoverAsync(ArtworkSubject subject,
        string album, string albumArtist, CancellationToken cancellationToken)
    {
        if (folders.FilesFor(subject.Path) is not { } files)
            return Result.Fail(AddInError.NotFound, $"{subject.Path} is in no library folder");
        var own = await Task.Run(() => files.ReadPicture(subject.Path), cancellationToken).ConfigureAwait(false);
        if (own.IsSuccess)
        {
            if (store.HoldsAlbum(album, albumArtist, own.Value))
                return ("present", subject.MbAlbumArtist);
            var kept = ArtworkLookup.Keep(() => store.KeepAlbum(album, albumArtist, own.Value), $"the cover of {album}");
            if (!kept.IsSuccess)
                return kept.Failure;
            LogCoverKept(_log, album, albumArtist);
            return ("kept", subject.MbAlbumArtist);
        }
        return await lookup.CoverAsync(
            new AlbumQuery(albumArtist, album, subject.MbReleaseGroup, subject.MbRelease, subject.MbAlbumArtist),
            cancellationToken).ConfigureAwait(false);
    }

    [LoggerMessage(Level = LogLevel.Information, Message = "artwork: the cover of {Album} by {Artist} kept from its files")]
    private static partial void LogCoverKept(ILogger log, string album, string artist);
}
