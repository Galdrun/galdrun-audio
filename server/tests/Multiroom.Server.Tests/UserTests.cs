using Multiroom.Server.Abstractions;
using Multiroom.Server.Library;

namespace Multiroom.Server.Tests;

// Users in the library's stores (phase 36): what a database from before
// them holds is the household's, and a user's rows are read with it.
public sealed class UserTests : IDisposable
{
    private readonly TempDirectory _dir = new();
    private readonly LibraryDatabase _database;

    public UserTests() => _database = new LibraryDatabase(Path.Combine(_dir.Path, "library.db"));

    public void Dispose() => _dir.Dispose();

    // The schema as the last server before users left it, with one like, one
    // favourite and one play.
    private void CreateAtSchema(int version, string rows)
    {
        using var connection = _database.Open();
        var assembly = typeof(LibraryDatabase).Assembly;
        var sql = string.Concat(assembly.GetManifestResourceNames()
            .Where(n => n.StartsWith("migrations/", StringComparison.Ordinal))
            .Order(StringComparer.Ordinal)
            .Where(n => int.Parse(n["migrations/".Length..][..4], System.Globalization.CultureInfo.InvariantCulture) <= version)
            .Select(n =>
            {
                using var reader = new StreamReader(assembly.GetManifestResourceStream(n)!);
                return reader.ReadToEnd() + "\n";
            }));
        using var command = connection.CreateCommand();
        command.CommandText = "PRAGMA foreign_keys = OFF;\n" + sql + $$"""
            CREATE TABLE schema_version (version INTEGER NOT NULL);
            INSERT INTO schema_version VALUES ({{version}});
            {{rows}}
            """;
        command.ExecuteNonQuery();
    }

    [Fact]
    public void A_database_from_before_users_opens_with_every_mark_and_play_the_households()
    {
        CreateAtSchema(17, """
            INSERT INTO liked (kind, provider_id, provider_item_id, snapshot, added)
                VALUES ('track', 'fake', 't1', '{"providerId":"fake","providerItemId":"t1","title":"One"}', 1);
            INSERT INTO liked (kind, provider_id, provider_item_id, snapshot, added)
                VALUES ('album', 'fake', 'a1', '{"providerId":"fake","providerItemId":"a1","title":"Album"}', 2);
            INSERT INTO favorite (kind, provider_id, provider_item_id, title, added) VALUES ('track', 'fake', 't1', 'One', 1);
            INSERT INTO play_history (zone, provider_id, provider_item_id, snapshot, played)
                VALUES ('living', 'fake', 't1', '{"providerId":"fake","providerItemId":"t1","title":"One"}', 1);
            """);

        Assert.True(_database.Migrate().IsSuccess);

        var likes = new LikeStore(_database);
        var like = Assert.Single(likes.Likes(LikeKind.Track).Value);
        Assert.Equal(("One", ""), (like.Track!.Title, like.User));
        // Phase 37: the album like is asked once whether it is a book.
        var album = Assert.Single(likes.UncheckedAlbums().Value);
        Assert.False(likes.Checked(album.Id, null).Value);
        Assert.Empty(likes.UncheckedAlbums().Value);
        Assert.Equal("Album", Assert.Single(likes.Likes(LikeKind.Album).Value).Album!.Title);
        Assert.Equal("", Assert.Single(new PlaylistStore(_database).Favorites().Value).User);
        Assert.Equal("", Assert.Single(new PlayHistoryStore(_database).Page(null, 0, 10).Value.Items).User);
    }

    [Fact]
    public void A_play_is_recorded_as_its_users_and_a_removed_users_as_the_households()
    {
        Assert.True(_database.Migrate().IsSuccess);
        var users = new UserStore(_database);
        var history = new PlayHistoryStore(_database);
        Assert.True(users.Add("Alice").IsSuccess);
        var track = new Track("fake", "t1", "One");

        Assert.Equal("Alice", history.Record("living", track, DateTimeOffset.UtcNow, "alice").Value.User);
        Assert.Equal("", history.Record("living", track, DateTimeOffset.UtcNow, "Bob").Value.User);
        Assert.Equal(1, history.Page(null, 0, 10, "Alice").Value.Items.Count(p => p.User == "Alice"));
        Assert.Equal(2, history.Page(null, 0, 10, UserStore.Everyone).Value.Total);

        Assert.True(users.Remove("Alice").Value);
        Assert.All(history.Page(null, 0, 10).Value.Items, p => Assert.Equal("", p.User));
        Assert.Equal(LibraryError.InvalidArgument, history.Page(null, 0, 10, "Alice").Failure.Code);
    }
}
