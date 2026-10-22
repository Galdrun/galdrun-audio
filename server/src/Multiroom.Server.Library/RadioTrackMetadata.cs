using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using Multiroom.Server.Abstractions;

namespace Multiroom.Server.Library;

// A radio song has metadata, but no playable provider track id. Keep a
// station-independent identity and the metadata snapshot in the existing liked table.
public sealed record TrackMetadata(string Title, string? Artist = null, string? Album = null);

public static class RadioTrackMetadata
{
    // The provider id of such a track: no provider, so never playable as is.
    public const string ProviderId = "radio-metadata";

    public static Track? FromTitle(string? streamTitle)
    {
        if (string.IsNullOrWhiteSpace(streamTitle)) return null;
        var parts = streamTitle.Split(" - ", 2, StringSplitOptions.TrimEntries);
        return FromMetadata(parts.Length == 2 && parts.All(p => p.Length > 0)
            ? new(parts[1], parts[0]) : new(streamTitle));
    }

    public static Track? FromMetadata(TrackMetadata metadata)
    {
        if (string.IsNullOrWhiteSpace(metadata.Title)) return null;
        static string? Trim(string? value) => string.IsNullOrWhiteSpace(value) ? null : value.Trim();
        var title = metadata.Title.Trim(); var artist = Trim(metadata.Artist); var album = Trim(metadata.Album);
        var identity = JsonSerializer.Serialize(new[] { title.ToUpperInvariant(), artist?.ToUpperInvariant(), album?.ToUpperInvariant() });
        var id = Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(identity)));
        return new Track(ProviderId, id, title, artist, album);
    }
}
