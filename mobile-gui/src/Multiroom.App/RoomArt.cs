using Avalonia.Media.Imaging;
using Avalonia.Platform;
using Multiroom.App.Api;

namespace Multiroom.App;

// A room without its own picture shows one of these, picked by its client id as the web interface does
// (server-gui roomArt), so a room looks the same in both. Rendered from server-gui/www/img/rooms/*.svg.
internal static class RoomArt
{
    private static readonly string[] Names = ["living", "office", "kitchen", "bedroom", "bathroom", "terrace"];
    private static readonly Dictionary<string, Bitmap> Loaded = [];

    public static Bitmap For(string client)
    {
        uint hash = 0;
        foreach (var rune in client.EnumerateRunes()) hash = unchecked(hash * 31 + rune.ToString()[0]); // JS charCodeAt(0)
        var name = Names[hash % (uint)Names.Length];
        if (!Loaded.TryGetValue(name, out var art))
            Loaded[name] = art = new Bitmap(AssetLoader.Open(new Uri($"avares://Multiroom.App/Assets/rooms/{name}.png")));
        return art;
    }

    // The room's own picture from the server, or null when it has none or it cannot be read.
    public static async Task<Bitmap?> PictureAsync(ArtworkCache pictures, string client)
    {
        try
        {
            if (await pictures.RoomImageAsync(client, CancellationToken.None) is not { Length: > 0 } bytes) return null;
            using var stream = new MemoryStream(bytes);
            return new Bitmap(stream);
        }
        catch (Exception e) when (e is ApiException or HttpRequestException or OperationCanceledException or IOException or ArgumentException or InvalidOperationException) { return null; }
    }
}
