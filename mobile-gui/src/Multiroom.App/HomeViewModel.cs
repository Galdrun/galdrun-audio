using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using Multiroom.App.Api;

namespace Multiroom.App;

// One favourite as the home shelf shows it: artwork, title and subtitle.
public sealed record HomeItem(long Id, string Title, string Subtitle, string? Artwork)
{
    public static HomeItem From(Favorite favorite) => new(favorite.Id, favorite.Title,
        favorite.Track is { } track ? track.Artist ?? track.Album ?? "" : favorite.Station?.Genre ?? "Station",
        favorite.Item.ArtworkId);
}

// The home page's shelves. Favourites are live server data; last played and
// proposals wait for server contracts that do not exist yet, so their shelves
// say so instead of inventing content.
public sealed partial class HomeViewModel : ObservableObject
{
    private readonly Func<Func<MultiroomClient, CancellationToken, Task>, Task> execute;
    private readonly Func<string?> zone;
    public HomeViewModel(Func<Func<MultiroomClient, CancellationToken, Task>, Task> execute, Func<string?> zone)
    {
        this.execute = execute; this.zone = zone;
        Favourites.CollectionChanged += (_, _) => OnPropertyChanged(nameof(NoFavourites));
    }
    public ObservableCollection<HomeItem> Favourites { get; } = [];
    public event Action? QueueChanged;
    [ObservableProperty] private string _notice = "";
    [ObservableProperty, NotifyPropertyChangedFor(nameof(NoFavourites))] private bool _isLoading;
    public bool NoFavourites => Favourites.Count == 0 && !IsLoading;

    public void Reset()
    { Favourites.Clear(); Notice = ""; IsLoading = false; }

    public Task RefreshAsync() => execute(async (api, ct) =>
    {
        IsLoading = true;
        try
        {
            var favourites = await api.FavoritesAsync(ct);
            ct.ThrowIfCancellationRequested();
            Favourites.Clear();
            foreach (var favorite in favourites) Favourites.Add(HomeItem.From(favorite));
        }
        finally { IsLoading = false; }
    });

    [RelayCommand]
    private Task PlayAsync(HomeItem? item) => execute(async (api, ct) =>
    {
        if (item is not { } favourite) return;
        if (zone() is not { } target) throw new ArgumentException("Choose or create a zone before playing music.");
        await api.QueueAsync(target, new AddItems(Favorite: favourite.Id, Replace: true), ct);
        await api.TransportAsync(target, "play", null, ct);
        Notice = $"Playing in {target}";
        QueueChanged?.Invoke();
    });

    // A card tap queues that favourite at the end of the selected zone's queue.
    [RelayCommand]
    private Task QueueAsync(HomeItem? item) => execute(async (api, ct) =>
    {
        if (item is not { } favourite) return;
        if (zone() is not { } target) throw new ArgumentException("Choose or create a zone before adding music.");
        await api.QueueAsync(target, new AddItems(Favorite: favourite.Id), ct);
        Notice = $"Added to {target}";
        QueueChanged?.Invoke();
    });
}
