using System.Globalization;
using System.Text.Json;
using Multiroom.Server.Abstractions;

namespace Multiroom.Server.Providers.Tidal;

// Tidal's media browser (phase 14): an online service is endless and curated,
// so its pages are the service's own, in sections, each paged on its own, and
// nothing is counted, sorted or lettered unless Tidal does it.
//
// The root: For you, New releases, My collection, Mixes, Genres and Moods.
// Nodes:
// - "collection": the account's favourite playlists, albums, artists and
//   tracks, and its own playlists, as sections; each "collection:<part>" is
//   paged, and the favourites sort by "added" (newest first) or "name".
// - "page:<path>": one of Tidal's pages (pages/for_you, ...), its modules read
//   as sections; a module's rest is "data:<path>", its page links "page:<path>".
//   Both paths come from Tidal's own answers and are checked before they are
//   asked for: only "pages/..." with plain characters, never a URL.
// - "artist:<id>": top tracks, albums, singles and EPs, and similar artists,
//   as sections; each "artist:<id>/<part>" is paged.
// - "album:<id>", "playlist:<uuid>", "mix:<id>": their tracks, with a header.
// Albums, playlists, mixes, artists (their top tracks) and the collection's
// tracks can be queued as a whole: ExpandAsync pages through them.
public sealed partial class TidalProvider
{
    public const string CollectionNode = "collection";
    public const string PagePrefix = "page:";
    public const string DataPrefix = "data:";
    public const string MixPrefix = "mix:";

    private static readonly (string Id, string Title)[] Roots =
    [
        (PagePrefix + "pages/for_you", "For you"),
        (PagePrefix + "pages/explore_new_music", "New releases"),
        (CollectionNode, "My collection"),
        (PagePrefix + "pages/my_collection_my_mixes", "Mixes"),
        (PagePrefix + "pages/genre_page", "Genres"),
        (PagePrefix + "pages/moods", "Moods"),
    ];

    // The collection's parts: its node, title, path under users/<id>/, and
    // whether Tidal sorts it.
    private static readonly (string Part, string Title, string Path, bool Sorted)[] CollectionParts =
    [
        ("playlists", "Playlists", "favorites/playlists", true),
        ("albums", "Albums", "favorites/albums", true),
        ("artists", "Artists", "favorites/artists", true),
        ("tracks", "Tracks", "favorites/tracks", true),
        ("own", "Your playlists", "playlists", false),
    ];

    private static readonly (string Part, string Title, string Path, (string, string)[] Query)[] ArtistParts =
    [
        ("toptracks", "Top tracks", "toptracks", []),
        ("albums", "Albums", "albums", []),
        ("singles", "Singles & EPs", "albums", [("filter", "EPSANDSINGLES")]),
        ("similar", "Similar artists", "similar", []),
    ];

    private static readonly string[] CollectionSorts = ["added", "name"];

    // How many tracks one expansion asks for at a time.
    private const int ExpandPage = 100;

    public async Task<Result<BrowsePage, ProviderError>> BrowseAsync(BrowseRequest request,
        CancellationToken cancellationToken)
    {
        var nodeId = request.NodeId;
        if (request.Filter is not null || request.Letter is not null)
            return Result.Fail(ProviderError.InvalidArgument, $"{nodeId ?? "the root"}: Tidal lists take no filter or letter");
        var sortable = nodeId is not null && Collection(nodeId) is { Sorted: true };
        if (request.Sort is { } sort && (!sortable || !CollectionSorts.Contains(sort, StringComparer.Ordinal)))
        {
            return Result.Fail(ProviderError.InvalidArgument, sortable
                ? $"{nodeId} sorts by {string.Join(", ", CollectionSorts)}, not {sort}"
                : $"{nodeId ?? "the root"} takes no sort");
        }

        if (nodeId is null)
        {
            return new BrowsePage([.. Roots.Skip(request.Offset).Take(request.Limit)
                .Select(r => Node(r.Id, r.Title, BrowseKind.Root))], CatalogItems.Empty, Total: Roots.Length);
        }
        if (nodeId == CollectionNode)
            return await CollectionAsync(request.Limit, cancellationToken).ConfigureAwait(false);
        if (Collection(nodeId) is { } part)
            return await CollectionPartAsync(part, request, cancellationToken).ConfigureAwait(false);
        if (nodeId.StartsWith(PagePrefix, StringComparison.Ordinal) && TidalPath(nodeId[PagePrefix.Length..]) is { } page)
            return await PageAsync(nodeId, page, cancellationToken).ConfigureAwait(false);
        if (nodeId.StartsWith(DataPrefix, StringComparison.Ordinal) && TidalPath(nodeId[DataPrefix.Length..]) is { } data)
            return await DataAsync(nodeId, data, request, cancellationToken).ConfigureAwait(false);
        if (ArtistPart(nodeId) is { } artistPart)
            return await ArtistPartAsync(nodeId, artistPart.Artist, artistPart.Part, request, cancellationToken).ConfigureAwait(false);
        if (IdOf(nodeId, ArtistPrefix) is { } artist)
            return await ArtistAsync(nodeId, artist, request, cancellationToken).ConfigureAwait(false);
        if (IdOf(nodeId, AlbumPrefix) is { } album)
        {
            return await TracksWithHeaderAsync($"albums/{album}", $"albums/{album}/tracks", nodeId, request,
                AlbumNode, cancellationToken).ConfigureAwait(false);
        }
        if (nodeId.StartsWith(PlaylistPrefix, StringComparison.Ordinal) && IsUuid(nodeId[PlaylistPrefix.Length..]))
        {
            var uuid = nodeId[PlaylistPrefix.Length..];
            return await TracksWithHeaderAsync($"playlists/{uuid}", $"playlists/{uuid}/tracks", nodeId, request,
                PlaylistNode, cancellationToken).ConfigureAwait(false);
        }
        if (MixOf(nodeId) is { } mix)
        {
            var items = await Get($"mixes/{mix}/items", request.Offset, request.Limit, nodeId, cancellationToken)
                .ConfigureAwait(false);
            return items.IsSuccess
                ? new BrowsePage([], CatalogItems.Empty with { Tracks = Tracks(items.Value) },
                    Node(nodeId, "Mix", BrowseKind.Playlist, playable: true), Total(items.Value))
                : items.Failure;
        }
        return Result.Fail(ProviderError.NotFound, $"no node {nodeId}");
    }

    public async Task<Result<ExpandedContainer, ProviderError>> ExpandAsync(string nodeId, int limit,
        CancellationToken cancellationToken)
    {
        string? path = null;
        (string, string)[] query = [];
        if (IdOf(nodeId, AlbumPrefix) is { } album)
            path = $"albums/{album}/tracks";
        else if (nodeId.StartsWith(PlaylistPrefix, StringComparison.Ordinal) && IsUuid(nodeId[PlaylistPrefix.Length..]))
            path = $"playlists/{nodeId[PlaylistPrefix.Length..]}/tracks";
        else if (MixOf(nodeId) is { } mix)
            path = $"mixes/{mix}/items";
        else if ((IdOf(nodeId, ArtistPrefix) ?? (ArtistPart(nodeId) is { Part: "toptracks" } top ? top.Artist : null)) is { } artist)
            path = $"artists/{artist}/toptracks";
        else if (nodeId == $"{CollectionNode}/tracks")
            path = $"users/{_session.Token?.UserId ?? 0}/favorites/tracks";

        if (path is null)
        {
            return nodeId == CollectionNode || Collection(nodeId) is not null || ArtistPart(nodeId) is not null ||
                nodeId.StartsWith(PagePrefix, StringComparison.Ordinal) || nodeId.StartsWith(DataPrefix, StringComparison.Ordinal)
                ? Result.Fail(ProviderError.InvalidArgument, $"{nodeId} is opened, not queued")
                : Result.Fail(ProviderError.NotFound, $"no node {nodeId}");
        }
        if (path.StartsWith("users/0/", StringComparison.Ordinal))
            return Result.Fail(ProviderError.Unavailable, $"{nodeId}: Tidal is not authorized; {_session.Waiting}");

        var expanded = await ExpandPagesAsync(path, query, nodeId, limit, cancellationToken).ConfigureAwait(false);
        // Tidal's app API flags no audiobook: an album queued whole is one
        // when its track titles are chapters (phase 31), so it goes in as
        // one book.
        if (!expanded.IsSuccess || IdOf(nodeId, AlbumPrefix) is null || expanded.Value.Tracks.Count == 0)
            return expanded;
        var (tracks, refused, truncated, _) = expanded.Value;
        var all = tracks.Concat(refused.Select(r => r.Track)).ToList();
        if (!MediaTypeClassifier.LooksSpoken([.. all.Select(t => t.Title)]))
            return expanded;
        var spoken = MediaTypeClassifier.Classify(new MediaTypeEvidence(Provider: MediaType.Audiobook));
        Track Spoken(Track t) => t with { MediaType = spoken.Type, MediaTypeSource = spoken.Source };
        return new ExpandedContainer([.. tracks.Select(Spoken)], [.. refused.Select(r => r with { Track = Spoken(r.Track) })],
            truncated);
    }

    private async Task<Result<ExpandedContainer, ProviderError>> ExpandPagesAsync(string path, (string, string)[] query,
        string nodeId, int limit, CancellationToken cancellationToken)
    {
        var tracks = new List<Track>();
        var refused = new List<RefusedTrack>();
        var offset = 0;
        while (true)
        {
            var page = await _session.GetAsync(path, [.. query, .. Page(offset, ExpandPage)], nodeId, cancellationToken)
                .ConfigureAwait(false);
            if (!page.IsSuccess)
                return page.Failure;
            var items = page.Value.Items().Select(Unwrap).ToList();
            foreach (var item in items)
            {
                if (ToTrack(item) is not { } track)
                    continue;
                if (tracks.Count == limit)
                    return new ExpandedContainer(tracks, refused, Truncated: true);
                if (Unstreamable(item) is { } reason)
                    refused.Add(new RefusedTrack(track, reason));
                else
                    tracks.Add(track);
            }
            offset += items.Count;
            if (items.Count < ExpandPage || (Total(page.Value) is { } total && offset >= total))
                return new ExpandedContainer(tracks, refused, Truncated: false);
        }
    }

    // First pages of each part of the collection.
    private async Task<Result<BrowsePage, ProviderError>> CollectionAsync(int limit, CancellationToken cancellationToken)
    {
        if (_session.Token is not { } token)
            return Result.Fail(ProviderError.Unavailable, $"{CollectionNode}: Tidal is not authorized; {_session.Waiting}");
        var asked = CollectionParts.Select(p => _session.GetAsync($"users/{token.UserId}/{p.Path}",
            [.. Page(0, limit), .. p.Sorted ? SortQuery("added") : []], p.Title, cancellationToken)).ToList();
        var answers = await Task.WhenAll(asked).ConfigureAwait(false);
        var sections = new List<BrowseSection>();
        for (var i = 0; i < answers.Length; i++)
        {
            if (!answers[i].IsSuccess)
                return answers[i].Failure;
            var part = CollectionParts[i];
            sections.Add(Section(part.Title, answers[i].Value, Node($"{CollectionNode}/{part.Part}", part.Title,
                BrowseKind.Section, playable: part.Part == "tracks")));
        }
        return new BrowsePage([], CatalogItems.Empty, Node(CollectionNode, "My collection", BrowseKind.Root),
            Sections: sections);
    }

    private async Task<Result<BrowsePage, ProviderError>> CollectionPartAsync(
        (string Part, string Title, string Path, bool Sorted) part, BrowseRequest request,
        CancellationToken cancellationToken)
    {
        if (_session.Token is not { } token)
            return Result.Fail(ProviderError.Unavailable, $"{request.NodeId}: Tidal is not authorized; {_session.Waiting}");
        var sort = part.Sorted ? request.Sort ?? "added" : null;
        var answer = await _session.GetAsync($"users/{token.UserId}/{part.Path}",
            [.. Page(request.Offset, request.Limit), .. sort is null ? [] : SortQuery(sort)], part.Title,
            cancellationToken).ConfigureAwait(false);
        if (!answer.IsSuccess)
            return answer.Failure;
        return List(answer.Value, Node(request.NodeId!, part.Title, BrowseKind.Section, playable: part.Part == "tracks")) with
        {
            Sorts = part.Sorted ? CollectionSorts : null,
            Sort = sort,
        };
    }

    // A page of Tidal's own, module by module. What a module holds is read
    // from its type; modules the browser has no use for (promotions, videos)
    // are left out.
    private async Task<Result<BrowsePage, ProviderError>> PageAsync(string nodeId, (string Path, (string, string)[] Query) page,
        CancellationToken cancellationToken)
    {
        var answer = await _session.GetAsync(page.Path, [.. page.Query, .. PageQuery], nodeId, cancellationToken)
            .ConfigureAwait(false);
        if (!answer.IsSuccess)
            return answer.Failure;
        var sections = new List<BrowseSection>();
        foreach (var module in answer.Value.Items("rows").SelectMany(r => r.Items("modules")))
        {
            var title = module.Text("title") ?? "";
            switch (module.Text("type"))
            {
                case "ALBUM_LIST" or "ARTIST_LIST" or "PLAYLIST_LIST" or "MIX_LIST" or "TRACK_LIST"
                    when module.Get("pagedList") is { } list:
                    var more = list.Text("dataApiPath") is { } rest && TidalPath(rest) is not null
                        ? Node(DataPrefix + rest, title, BrowseKind.Section, playable: false)
                        : null;
                    sections.Add(Section(title, list, more));
                    break;
                case "PAGE_LINKS" or "PAGE_LINKS_CLOUD" when module.Get("pagedList") is { } links:
                    sections.Add(new BrowseSection(title, [.. links.Items().Select(PageLink).OfType<BrowseNode>()],
                        CatalogItems.Empty, Total: Total(links)));
                    break;
            }
        }
        return new BrowsePage([], CatalogItems.Empty,
            Node(nodeId, answer.Value.Text("title") ?? Roots.FirstOrDefault(r => r.Id == nodeId).Title ?? "Tidal", BrowseKind.Root),
            Sections: sections);
    }

    // The rest of a page's module, paged.
    private async Task<Result<BrowsePage, ProviderError>> DataAsync(string nodeId, (string Path, (string, string)[] Query) data,
        BrowseRequest request, CancellationToken cancellationToken)
    {
        var answer = await _session.GetAsync(data.Path,
            [.. data.Query.Where(q => q.Item1 is not ("offset" or "limit")), .. PageQuery, .. Page(request.Offset, request.Limit)],
            nodeId, cancellationToken).ConfigureAwait(false);
        return answer.IsSuccess ? List(answer.Value, Node(nodeId, "More", BrowseKind.Section)) : answer.Failure;
    }

    private async Task<Result<BrowsePage, ProviderError>> ArtistAsync(string nodeId, long artist, BrowseRequest request,
        CancellationToken cancellationToken)
    {
        if (request.Offset != 0)
            return Result.Fail(ProviderError.InvalidArgument, $"{nodeId} is a page of sections; each pages on its own");
        var header = _session.GetAsync($"artists/{artist}", [], nodeId, cancellationToken);
        var parts = ArtistParts.Select(p => _session.GetAsync($"artists/{artist}/{p.Path}", [.. p.Query, .. Page(0, request.Limit)],
            $"{nodeId} {p.Title}", cancellationToken)).ToList();
        var answers = await Task.WhenAll([header, .. parts]).ConfigureAwait(false);
        // Not FirstOrDefault: a default Result reads as a failure.
        if (Array.FindIndex(answers, a => !a.IsSuccess) is var failed and >= 0)
            return answers[failed].Failure;

        var sections = new List<BrowseSection>();
        for (var i = 0; i < ArtistParts.Length; i++)
        {
            var part = ArtistParts[i];
            sections.Add(Section(part.Title, answers[i + 1].Value, Node($"{nodeId}/{part.Part}", part.Title,
                BrowseKind.Section, playable: part.Part == "toptracks")));
        }
        return new BrowsePage([], CatalogItems.Empty, ArtistNode(answers[0].Value) ?? Node(nodeId, "Artist", BrowseKind.Artist),
            Sections: sections);
    }

    private async Task<Result<BrowsePage, ProviderError>> ArtistPartAsync(string nodeId, long artist, string partName,
        BrowseRequest request, CancellationToken cancellationToken)
    {
        var part = ArtistParts.First(p => p.Part == partName);
        var answer = await _session.GetAsync($"artists/{artist}/{part.Path}",
            [.. part.Query, .. Page(request.Offset, request.Limit)], nodeId, cancellationToken).ConfigureAwait(false);
        return answer.IsSuccess
            ? List(answer.Value, Node(nodeId, part.Title, BrowseKind.Section, playable: part.Part == "toptracks"))
            : answer.Failure;
    }

    // An album's or a playlist's tracks, under its own node as the header.
    private async Task<Result<BrowsePage, ProviderError>> TracksWithHeaderAsync(string headerPath, string tracksPath,
        string nodeId, BrowseRequest request, Func<JsonElement, BrowseNode?> toHeader, CancellationToken cancellationToken)
    {
        var header = _session.GetAsync(headerPath, [], nodeId, cancellationToken);
        var tracks = Get(tracksPath, request.Offset, request.Limit, nodeId, cancellationToken);
        var answers = await Task.WhenAll(header, tracks).ConfigureAwait(false);
        // Not FirstOrDefault: a default Result reads as a failure.
        if (Array.FindIndex(answers, a => !a.IsSuccess) is var failed and >= 0)
            return answers[failed].Failure;
        return new BrowsePage([], CatalogItems.Empty with { Tracks = Tracks(answers[1].Value) }, toHeader(answers[0].Value),
            Total(answers[1].Value));
    }

    // A section of whatever the list holds: containers as nodes, tracks as items.
    private static BrowseSection Section(string title, JsonElement list, BrowseNode? more)
    {
        var items = list.Items().Select(Unwrap).ToList();
        var nodes = items.Select(ContainerOf).OfType<BrowseNode>().ToList();
        var layout = nodes.Count > 0 && nodes.All(n => n.Kind is BrowseKind.Album or BrowseKind.Playlist)
            ? BrowseLayout.Grid
            : BrowseLayout.List;
        return new BrowseSection(title, nodes, CatalogItems.Empty with { Tracks = [.. items.Select(ToTrack).OfType<Track>()] },
            more, Total(list), layout);
    }

    private static BrowsePage List(JsonElement list, BrowseNode header)
    {
        var section = Section(header.Title, list, null);
        return new BrowsePage(section.Nodes, section.Items, header, section.Total, Layout: section.Layout);
    }

    private static List<Track> Tracks(JsonElement list) => [.. list.Items().Select(Unwrap).Select(ToTrack).OfType<Track>()];

    // Favourites and playlist entries are wrapped: {"created": ..., "item": {...}}.
    private static JsonElement Unwrap(JsonElement item) => item.Get("item") ?? item;

    // An item of a mixed list, told by the fields each kind has.
    private static BrowseNode? ContainerOf(JsonElement json)
    {
        if (json.Text("uuid") is not null)
            return PlaylistNode(json);
        if (json.Text("mixType") is not null)
            return MixNode(json);
        if (json.Get("trackNumber") is not null || (json.Get("album") is not null && json.Get("duration") is not null))
            return null;
        if (json.Get("numberOfTracks") is not null || json.Get("releaseDate") is not null)
            return AlbumNode(json);
        if (json.Text("name") is not null && json.Text("title") is null)
            return ArtistNode(json);
        return null;
    }

    private static BrowseNode? AlbumNode(JsonElement json) => ToAlbum(json) is { } album
        ? new BrowseNode(ProviderId, album.ProviderItemId, album.Title, album.Artwork, BrowseKind.Album,
            Join(album.Artist, album.Year?.ToString(CultureInfo.InvariantCulture),
                json.Number("numberOfTracks") is { } n ? Count(n, "track") : null),
            Playable: true)
        : null;

    private static BrowseNode? ArtistNode(JsonElement json) => ToArtist(json) is { } artist
        ? new BrowseNode(ProviderId, artist.ProviderItemId, artist.Name, artist.Artwork, BrowseKind.Artist, Playable: true)
        : null;

    private static BrowseNode? PlaylistNode(JsonElement json) =>
        json.Text("uuid") is { } uuid && IsUuid(uuid) && json.Text("title") is { } title
            ? new BrowseNode(ProviderId, PlaylistPrefix + uuid, title,
                ArtworkOf(json.Text("squareImage") ?? json.Text("image")), BrowseKind.Playlist,
                json.Number("numberOfTracks") is { } n ? Count(n, "track") : null, Playable: true)
            : null;

    // A mix's images are URLs, not image ids: it goes without artwork.
    private static BrowseNode? MixNode(JsonElement json) =>
        json.Text("id") is { } id && IsMixId(id) && json.Text("title") is { } title
            ? new BrowseNode(ProviderId, MixPrefix + id, title, null, BrowseKind.Playlist, json.Text("subTitle"),
                Playable: true)
            : null;

    private static BrowseNode? PageLink(JsonElement json) =>
        json.Text("apiPath") is { } path && TidalPath(path) is not null && json.Text("title") is { } title
            ? new BrowseNode(ProviderId, PagePrefix + path, title, ArtworkOf(json.Text("imageId")), BrowseKind.Genre,
                Unplayable: "a page is opened, not queued")
            : null;

    private static BrowseNode Node(string id, string title, BrowseKind kind, bool playable = false) =>
        new(ProviderId, id, title, Kind: kind, Playable: playable,
            Unplayable: playable ? null : "a list is opened, not queued");

    // Why a track Tidal lists cannot be played: it says so itself.
    private static string? Unstreamable(JsonElement track) =>
        track.Get("streamReady") is { ValueKind: JsonValueKind.False } || track.Get("allowStreaming") is { ValueKind: JsonValueKind.False }
            ? "Tidal does not stream it to this account"
            : null;

    private static int? Total(JsonElement list) => list.Number("totalNumberOfItems") is { } total ? (int)total : null;

    private static (string, string)[] SortQuery(string sort) => sort == "name"
        ? [("order", "NAME"), ("orderDirection", "ASC")]
        : [("order", "DATE"), ("orderDirection", "DESC")];

    // What Tidal's pages take besides the country.
    private static readonly (string, string)[] PageQuery = [("deviceType", "BROWSER"), ("locale", "en_US")];

    private static (string Part, string Title, string Path, bool Sorted)? Collection(string nodeId) =>
        nodeId.StartsWith(CollectionNode + "/", StringComparison.Ordinal) &&
        CollectionParts.FirstOrDefault(p => p.Part == nodeId[(CollectionNode.Length + 1)..]) is { Part: not null } part
            ? part
            : null;

    private static (long Artist, string Part)? ArtistPart(string nodeId)
    {
        var slash = nodeId.IndexOf('/', StringComparison.Ordinal);
        if (slash < 0 || IdOf(nodeId[..slash], ArtistPrefix) is not { } artist)
            return null;
        var part = nodeId[(slash + 1)..];
        return ArtistParts.Any(p => p.Part == part) ? (artist, part) : null;
    }

    private static string? MixOf(string nodeId) =>
        nodeId.StartsWith(MixPrefix, StringComparison.Ordinal) && IsMixId(nodeId[MixPrefix.Length..])
            ? nodeId[MixPrefix.Length..]
            : null;

    // Tidal's mix ids: hex digits.
    private static bool IsMixId(string text) => text.Length is >= 8 and <= 64 && text.All(char.IsAsciiHexDigit);

    // A path Tidal named in its own answer, split from its query: only
    // "pages/..." of letters, digits, '_', '-' and '/', and a plain query.
    // Anything else, a URL above all, is never asked for.
    private static (string Path, (string, string)[] Query)? TidalPath(string text)
    {
        var question = text.IndexOf('?', StringComparison.Ordinal);
        var path = question < 0 ? text : text[..question];
        if (!path.StartsWith("pages/", StringComparison.Ordinal) || path.Length > 200 ||
            path.Split('/').Any(s => s.Length == 0 || s == "..") ||
            !path.All(c => char.IsAsciiLetterOrDigit(c) || c is '_' or '-' or '/'))
            return null;
        var query = new List<(string, string)>();
        if (question >= 0)
        {
            foreach (var pair in text[(question + 1)..].Split('&', StringSplitOptions.RemoveEmptyEntries))
            {
                var equals = pair.IndexOf('=', StringComparison.Ordinal);
                var key = equals < 0 ? pair : pair[..equals];
                if (key.Length == 0 || !key.All(char.IsAsciiLetterOrDigit))
                    return null;
                query.Add((key, equals < 0 ? "" : Uri.UnescapeDataString(pair[(equals + 1)..])));
            }
        }
        return (path, [.. query]);
    }

    private static string Count(long count, string what) =>
        $"{count.ToString(CultureInfo.InvariantCulture)} {what}{(count == 1 ? "" : "s")}";

    private static string? Join(params string?[] parts)
    {
        var shown = parts.Where(p => !string.IsNullOrEmpty(p)).ToList();
        return shown.Count == 0 ? null : string.Join(" · ", shown);
    }
}
