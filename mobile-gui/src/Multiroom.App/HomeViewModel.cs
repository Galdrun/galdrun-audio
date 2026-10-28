using System.Collections.ObjectModel;
using Avalonia.Media;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using Multiroom.App.Api;

namespace Multiroom.App;

// One cover as a home shelf shows it: artwork, title, subtitle, why it is
// proposed, and what queues it.
public sealed record HomeItem(string Title, string Subtitle, string? Artwork, AddItems Items, string? Reason = null)
{
    public static HomeItem From(Favorite favorite) => new(favorite.Title, favorite.Item switch
    {
        Track track => track.Artist ?? track.Album ?? "",
        RadioStation station => station.Genre ?? "Station",
        MediaGroup group => group.Artist ?? "Book",
        _ => ""
    }, favorite.Item.ArtworkId, new(Favorite: favorite.Id));

    // A song liked off the radio plays as the track it was found as; until
    // one is found it has nothing to play.
    public static HomeItem? From(LikedItem like) => like.Found is { } found ? Of(found.Track)
        : like.Item is Track { ProviderId: "radio-metadata" } ? null : Of(like.Item);

    public static HomeItem? From(ProposalItem proposal) => Of(proposal.Item, proposal.Reason);

    public static HomeItem From(PlayedTrack played, string zone) => Of(played.Track)! with
    { Subtitle = string.Join(" · ", new[] { played.Track.Artist, zone }.Where(s => !string.IsNullOrEmpty(s))) };

    // An album, artist or book is its provider's node, queued whole; a book's
    // reference is "provider:node".
    private static HomeItem? Of(MediaItem item, string? reason = null) => item switch
    {
        Track t => new(t.Title, t.Artist ?? t.Album ?? "", t.ArtworkId, new(Tracks: [t]), reason),
        RadioStation s => new(s.Name, s.Genre ?? "Station", s.ArtworkId, new(Station: s), reason),
        Album a => new(a.Title, a.Artist ?? "Album", a.ArtworkId, new(Node: new(a.ProviderId, a.ProviderItemId)), reason),
        Artist a => new(a.Name, "Artist", a.ArtworkId, new(Node: new(a.ProviderId, a.ProviderItemId)), reason),
        Audiobook b when b.Reference.IndexOf(':') is var at and > 0 =>
            new(b.Title, b.Author ?? "Book", b.ArtworkId, new(Node: new(b.Reference[..at], b.Reference[(at + 1)..])), reason),
        _ => null
    };
}

// One shelf: its first covers, and All of them for its "See all" grid.
// Icon is drawn as an outline before its title; Empty says how it fills
// while it has none.
public sealed partial class HomeShelf : ObservableObject
{
    public const int Size = 14;
    public HomeShelf(string title, string icon, string empty)
    {
        Title = title; Icon = Geometry.Parse(icon); _empty = empty;
        Items.CollectionChanged += (_, _) => OnPropertyChanged(nameof(IsEmpty));
    }
    public string Title { get; }
    public Geometry Icon { get; }
    public ObservableCollection<HomeItem> Items { get; } = [];
    [ObservableProperty] private IReadOnlyList<HomeItem> _all = [];
    public bool IsEmpty => Items.Count == 0;
    [ObservableProperty] private string _empty;
    public void Set(IEnumerable<HomeItem?> items)
    {
        All = items.OfType<HomeItem>().ToArray();
        Items.Clear();
        foreach (var item in All.Take(Size)) Items.Add(item);
    }
}

// The home page's shelves, as the server GUI's: what the AI proposes, the
// favourites, the likes and what was played lately (the household's).
public sealed partial class HomeViewModel : ObservableObject
{
    private readonly Func<Func<MultiroomClient, CancellationToken, Task>, Task> execute;
    private readonly Func<string?> zone;
    private readonly Func<string, string> zoneName;
    public HomeViewModel(Func<Func<MultiroomClient, CancellationToken, Task>, Task> execute, Func<string?> zone,
        Func<string, string>? zoneName = null)
    { this.execute = execute; this.zone = zone; this.zoneName = zoneName ?? (z => z); Shelves = [ForYou, Favourites, Liked, Recent]; }
    public HomeShelf ForYou { get; } = new("For you", "M 12,2 L 14.5,9.5 L 22,12 L 14.5,14.5 L 12,22 L 9.5,14.5 L 2,12 L 9.5,9.5 Z",
        "No proposals yet.");
    public HomeShelf Favourites { get; } = new("Favourites", "M 12,2 L 15,8.6 L 22,9.3 L 16.8,14 L 18.2,21 L 12,17.4 L 5.8,21 L 7.2,14 L 2,9.3 L 9,8.6 Z",
        "Save a track or station from Browse or Search to keep it close.");
    public HomeShelf Liked { get; } = new("Liked", "M 12,21 C 12,21 2,14.5 2,8.5 A 5,5 0 0 1 12,6 A 5,5 0 0 1 22,8.5 C 22,14.5 12,21 12,21 Z",
        "Tap ♥ on whatever you enjoy.");
    public HomeShelf Recent { get; } = new("Last played", "M 12,2 A 10,10 0 1 1 11.99,2 Z M 12,6 L 12,12 L 16,14",
        "What plays in your rooms shows up here.");
    public IReadOnlyList<HomeShelf> Shelves { get; }
    public event Action? QueueChanged;
    [ObservableProperty] private string _notice = "";
    [ObservableProperty] private bool _isLoading;
    // The shelf whose "See all" grid shows in place of the shelves.
    [ObservableProperty, NotifyPropertyChangedFor(nameof(IsShelfOpen))] private HomeShelf? _openShelf;
    public bool IsShelfOpen => OpenShelf is not null;

    public void Reset()
    {
        foreach (var shelf in Shelves) shelf.Set([]);
        OpenShelf = null;
        Notice = ""; IsLoading = false;
    }

    // Proposals, likes and history are optional: a server without them still shows its favourites.
    private static async Task<T?> Optional<T>(Task<T> read) where T : class
    {
        try { return await read; }
        catch (ApiException) { return null; }
    }

    public Task RefreshAsync() => execute(async (api, ct) =>
    {
        IsLoading = true;
        try
        {
            var favourites = api.FavoritesAsync(ct);
            var proposals = Optional(api.ProposalsAsync(ct));
            var likes = Optional(api.LikesAsync(ct));
            var history = Optional(api.HistoryAsync(100, ct));
            await Task.WhenAll(favourites, proposals, likes, history);
            ct.ThrowIfCancellationRequested();
            ShowProposals(await proposals);
            Favourites.Set((await favourites).Select(HomeItem.From));
            Liked.Set(((await likes) ?? []).OrderByDescending(l => l.Id).Select(HomeItem.From));
            // Newest first, each track once.
            Recent.Set(((await history)?.Items ?? []).DistinctBy(p => (p.Track.ProviderId, p.Track.ProviderItemId))
                .Select(p => HomeItem.From(p, zoneName(p.Zone))));
        }
        finally { IsLoading = false; }
    });

    // Tracks, albums, artists and books in turn, as the server GUI's "All".
    private void ShowProposals(ProposalsView? view)
    {
        ForYou.Empty = view?.Run?.State == ProposalRunState.Running ? "The AI is choosing proposals for you…"
            : view?.AiAvailable == true ? "No proposals yet." : "No proposals: connect an AI provider first.";
        IReadOnlyList<ProposalItem>[] lists = view is null ? [] : [view.Tracks, view.Albums, view.Artists, view.Audiobooks];
        var mixed = Enumerable.Range(0, lists.Select(l => l.Count).DefaultIfEmpty().Max())
            .SelectMany(i => lists.Where(l => i < l.Count).Select(l => l[i]));
        ForYou.Set(mixed.Select(HomeItem.From));
    }

    [RelayCommand] private void SeeAll(HomeShelf? shelf) => OpenShelf = shelf;
    [RelayCommand] private void CloseShelf() => OpenShelf = null;

    [RelayCommand]
    private Task PlayAsync(HomeItem? item) => execute(async (api, ct) =>
    {
        if (item is null) return;
        if (zone() is not { } target) throw new ArgumentException("Choose or create a zone before playing music.");
        await api.QueueAsync(target, item.Items with { Replace = true }, ct);
        await api.TransportAsync(target, "play", null, ct);
        Notice = $"Playing in {zoneName(target)}";
        QueueChanged?.Invoke();
    });

    // Queues the item at the end of the selected zone's queue.
    [RelayCommand]
    private Task QueueAsync(HomeItem? item) => execute(async (api, ct) =>
    {
        if (item is null) return;
        if (zone() is not { } target) throw new ArgumentException("Choose or create a zone before adding music.");
        await api.QueueAsync(target, item.Items, ct);
        Notice = $"Added to {zoneName(target)}";
        QueueChanged?.Invoke();
    });
}
