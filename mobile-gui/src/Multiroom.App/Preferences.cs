using System.Text.Json;

namespace Multiroom.App;

// Only presentation preferences. Bearer tokens never enter this type or file.
// The server id is the mDNS instance name of the chosen server; the address
// is the last one that worked there, the fallback when no announcement
// answers on the next start.
public sealed record Preferences(string Address, string? ActiveZone = null, string? ServerId = null)
{
    public static string DefaultPath => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), Branding.Name, "Controller", "settings.json");
    public static async Task<Preferences?> LoadAsync(string path, CancellationToken ct)
    {
        if (!File.Exists(path)) return null;
        using var stream = File.OpenRead(path);
        return await JsonSerializer.DeserializeAsync<Preferences>(stream, cancellationToken: ct);
    }
    public async Task SaveAsync(string path, CancellationToken ct)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        var temporary = path + "." + Guid.NewGuid().ToString("N") + ".tmp";
        try
        {
            await File.WriteAllTextAsync(temporary, JsonSerializer.Serialize(this), ct);
            File.Move(temporary, path, overwrite: true);
        }
        finally { if (File.Exists(temporary)) File.Delete(temporary); }
    }
}
