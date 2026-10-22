using Multiroom.Server.Library;

namespace Multiroom.Server.Tests;

public sealed class RadioTrackMetadataTests
{
    [Fact]
    public void Radio_metadata_is_saved_without_a_local_track_and_survives_reopening()
    {
        using var dir = new TempDirectory();
        var database = new LibraryDatabase(Path.Combine(dir.Path, "library.db"));
        Assert.True(database.Migrate().IsSuccess);
        var track = RadioTrackMetadata.FromTitle(" Artist - Song ")!;
        Assert.Equal(("Song", "Artist", (string?)null), (track.Title, track.Artist, track.Album));
        Assert.Equal(track.ProviderItemId, RadioTrackMetadata.FromMetadata(new("song", "artist"))!.ProviderItemId);
        Assert.NotEqual(track.ProviderItemId, RadioTrackMetadata.FromTitle("Artist - Other song")!.ProviderItemId);
        var store = new LikeStore(database);
        var added = store.Add(track);
        Assert.True(added.IsSuccess); Assert.True(added.Value.Added);
        var reopened = new LikeStore(new LibraryDatabase(database.Path)).Likes(null);
        Assert.True(reopened.IsSuccess); Assert.Equal(track, Assert.Single(reopened.Value).Track);
        Assert.Null(RadioTrackMetadata.FromTitle(" "));
        Assert.Null(RadioTrackMetadata.FromMetadata(new(" ", "Artist")));
        var unknownArtist = RadioTrackMetadata.FromTitle("Song without artist")!;
        Assert.Equal("Song without artist", unknownArtist.Title); Assert.Null(unknownArtist.Artist); Assert.Null(unknownArtist.Album);
    }
}
