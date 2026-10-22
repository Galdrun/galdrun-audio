namespace Multiroom.Server.Abstractions;

// A song by name, as a model names it or a seed is described to one
// Never an id, a path or a URL. Shared by the
// suggester (Ai), which asks for these, and the resolver (Providers), which
// finds them, so neither knows the other.
public sealed record SongSuggestion(string Title, string? Artist = null, string? Album = null, int? Year = null)
{
    public static SongSuggestion Of(Track track) => new(track.Title, track.Artist, track.Album);
}
