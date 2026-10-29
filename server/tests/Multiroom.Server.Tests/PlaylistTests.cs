using System.Text.Json.Nodes;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Logging.Abstractions;
using Multiroom.Server.Abstractions;
using Multiroom.Server.Library;
using Multiroom.Server.Playback;
using Multiroom.Server.Playback.Core;
using Multiroom.Server.Providers;
using Multiroom.Server.Providers.Local;

namespace Multiroom.Server.Tests;

// Favourites and playlists: the store over a real library database, and the
// player queueing them through the queue engine against FakeCore, with the
// local provider over a folder the indexer has read.
public sealed class PlaylistTests : IAsyncDisposable
{
    private const string Zone = "living";
    private static readonly TimeSpan Patience = TimeSpan.FromSeconds(10);

    private readonly TempDirectory _dir = new();
    private readonly string _music;
    private readonly LibraryDatabase _database;
    private readonly LibraryIndexer _indexer;
    private readonly PlaylistStore _store;
    private readonly FakeCore _core = new();
    private readonly QueueEngineTests.FakeMedia _media = new();
    private readonly QueueEngineTests.Synchronised _synchronised = new();
    private readonly ServiceProvider _services = new ServiceCollection().BuildServiceProvider();
    private readonly ProviderRegistry _providers;
    private readonly CoreLink _link;
    private readonly QueueEngine _engine;
    private readonly PlaylistPlayer _player;

    public PlaylistTests()
    {
        _music = Directory.CreateDirectory(Path.Combine(_dir.Path, "music")).FullName;
        _database = new LibraryDatabase(Path.Combine(_dir.Path, "library.db"));
        var folders = LibraryTests.Registry(
            new LibrarySettings([new LibraryFolder(_music, LibraryFolderType.Local)], TimeSpan.FromMinutes(5)), _database,
            _dir.Path);
        _indexer = new LibraryIndexer(folders, _database, NullLogger<LibraryIndexer>.Instance);
        _store = new PlaylistStore(_database);
        _providers = new ProviderRegistry(
            [new ProviderRegistration(LocalFilesProvider.ProviderId, _ => new LocalFilesProvider(folders,
                new LibraryCatalog(_database, LocalFilesProvider.ProviderId), _indexer, Path.Combine(_dir.Path, "artwork")))],
            _services, NullLogger<ProviderRegistry>.Instance);
        _link = new CoreLink(new CoreLinkOptions("127.0.0.1", _core.Port), [_synchronised],
            NullLogger<CoreLink>.Instance);
        _engine = new QueueEngine(_link, _providers, _media, NullLogger<QueueEngine>.Instance);
        _player = new PlaylistPlayer(_store, _engine, _providers);
        Assert.True(_database.Migrate().IsSuccess);
    }

    private static CancellationToken Ct => TestContext.Current.CancellationToken;

    public async ValueTask DisposeAsync()
    {
        await _link.StopAsync(CancellationToken.None);
        _link.Dispose();
        await _core.DisposeAsync();
        await _providers.DisposeAsync();
        await _services.DisposeAsync();
        await _indexer.StopAsync(CancellationToken.None);
        _indexer.Dispose();
        _dir.Dispose();
    }

    [Fact]
    public void Playlists_are_created_renamed_reordered_and_deleted()
    {
        var evening = Ok(_store.CreatePlaylist(" Evening "));
        var morning = Ok(_store.CreatePlaylist("Morning"));
        var party = Ok(_store.CreatePlaylist("Party"));
        Assert.Equal("Evening", evening.Name);
        Assert.Equal(0, evening.Count);
        Assert.Equal([("Evening", 0), ("Morning", 1), ("Party", 2)], Order());

        Assert.Equal(0, Ok(_store.MovePlaylist(party.Id, 0)).Position);
        Assert.Equal([("Party", 0), ("Evening", 1), ("Morning", 2)], Order());
        Assert.Equal(2, Ok(_store.MovePlaylist(evening.Id, 2)).Position);
        Assert.Equal([("Party", 0), ("Morning", 1), ("Evening", 2)], Order());

        Assert.Equal("Night", Ok(_store.RenamePlaylist(evening.Id, "Night")).Name);
        Assert.True(Ok(_store.DeletePlaylist(party.Id)));
        Assert.Equal([("Morning", 0), ("Night", 1)], Order());

        Assert.Equal(LibraryError.InvalidArgument, _store.CreatePlaylist("  ").Failure.Code);
        Assert.Equal(LibraryError.InvalidArgument, _store.RenamePlaylist(morning.Id, "").Failure.Code);
        Assert.Equal(LibraryError.InvalidArgument, _store.MovePlaylist(morning.Id, 2).Failure.Code);
        Assert.Equal("no playlist " + party.Id, _store.DeletePlaylist(party.Id).Failure.Detail);
        Assert.Equal(LibraryError.NotFound, _store.RenamePlaylist(party.Id, "Again").Failure.Code);
        Assert.Equal(LibraryError.NotFound, _store.MovePlaylist(party.Id, 0).Failure.Code);
        Assert.Equal(LibraryError.NotFound, _store.Entries(party.Id).Failure.Code);
    }

    [Fact]
    public void Entries_are_added_moved_and_removed_and_a_track_may_be_there_twice()
    {
        var mix = Ok(_store.CreatePlaylist("Mix"));
        var other = Ok(_store.CreatePlaylist("Other"));

        Ok(_store.AddEntries(mix.Id, [Snapshot("A"), Snapshot("C")], null));
        Assert.Equal(["A", "B", "C"], Titles(Ok(_store.AddEntries(mix.Id, [Snapshot("B")], 1))));
        var entries = Ok(_store.AddEntries(mix.Id, [Snapshot("A")], null));
        Assert.Equal(["A", "B", "C", "A"], Titles(entries));
        Assert.NotEqual(entries[0].Id, entries[3].Id);
        Assert.Equal(Snapshot("A"), entries[0].Track);

        Assert.Equal(["A", "A", "B", "C"], Titles(entries = Ok(_store.MoveEntry(mix.Id, entries[3].Id, 1))));
        Assert.Equal(["A", "B", "C", "A"], Titles(entries = Ok(_store.MoveEntry(mix.Id, entries[0].Id, 3))));
        Assert.Equal(["A", "C", "A"], Titles(Ok(_store.RemoveEntry(mix.Id, entries[1].Id))));
        Assert.Equal("0,1,2", Scalar($"SELECT group_concat(position) FROM (SELECT position FROM playlist_item WHERE playlist_id = {mix.Id} ORDER BY position)"));
        Assert.Equal(3, Ok(_store.Playlists())[0].Count);

        Assert.Equal(LibraryError.InvalidArgument, _store.AddEntries(mix.Id, [], null).Failure.Code);
        Assert.Equal(LibraryError.InvalidArgument, _store.AddEntries(mix.Id, [Snapshot("X")], 4).Failure.Code);
        Assert.Equal(LibraryError.InvalidArgument,
            _store.AddEntries(mix.Id, [new Track("", "x", "Nameless")], null).Failure.Code);
        Assert.Equal(LibraryError.InvalidArgument, _store.MoveEntry(mix.Id, entries[0].Id, 3).Failure.Code);
        Assert.Equal($"no entry {entries[0].Id} in playlist {other.Id}",
            _store.RemoveEntry(other.Id, entries[0].Id).Failure.Detail);
        Assert.Equal(["A", "C", "A"], Titles(Ok(_store.Entries(mix.Id))));

        Assert.True(Ok(_store.DeletePlaylist(mix.Id)));
        Assert.Equal("0", Scalar("SELECT COUNT(*) FROM playlist_item"));
        Assert.Equal(0, Ok(_store.Playlists()).Single().Position);
    }

    [Fact]
    public void A_favourite_is_added_once_and_removed()
    {
        var track = Snapshot("Song");
        var first = Ok(_store.AddFavorite(track));
        Assert.Equal(first, Ok(_store.AddFavorite(track with { Title = "Renamed" })));
        var station = Ok(_store.AddFavorite(new RadioStation("fake", "id-Song", "Jazz FM")));

        Assert.Equal(["Song", "Jazz FM"], Ok(_store.Favorites()).Select(f => f.Title));
        Assert.Equal(track, first.Track);
        Assert.Equal(station, Ok(_store.GetFavorite(station.Id)));

        Assert.True(Ok(_store.RemoveFavorite(first.Id)));
        Assert.Equal(LibraryError.NotFound, _store.RemoveFavorite(first.Id).Failure.Code);
        Assert.Equal(LibraryError.NotFound, _store.GetFavorite(first.Id).Failure.Code);
        Assert.Equal(["Jazz FM"], Ok(_store.Favorites()).Select(f => f.Title));
    }

    [Fact]
    public async Task A_playlist_of_local_tracks_enqueues_and_plays_end_to_end()
    {
        var (one, two) = await IndexedAsync();
        var playlist = Ok(_store.CreatePlaylist("Local"));
        Ok(_store.AddEntries(playlist.Id, [one, two], null));
        await StartCoreAsync();

        var enqueued = Ok(await _player.EnqueuePlaylistAsync(Zone, playlist.Id, QueuePlacement.End, Ct));
        Assert.Empty(enqueued.Refused);
        // A snapshot keeps no artwork: a library track's is its own reference.
        Assert.Equal([one with { Artwork = new("local", one.ProviderItemId) }, two with { Artwork = new("local", two.ProviderItemId) }],
            enqueued.Queue.Items.Select(i => i.Track));

        Ok(await _engine.SetClientsAsync(Zone, ["roomA"], Ct));
        Ok(await _engine.PlayAsync(Zone, null, Ct));
        var first = Staged()[0];
        Assert.Equal(("local:" + one.ProviderItemId, "One"), (first.TrackId, first.Title));
        Assert.Equal(new FileInfo(Path.Combine(_music, "one.wav")).Length, _media[first.MediaUri].Length);

        await EmitAsync(
            $$"""{"event":"zone.sourceNeeded","zone":"living","afterSourceId":"{{first.SourceId}}"}""",
            $$"""{"event":"zone.sourceStarted","zone":"living","sourceId":"{{first.SourceId}}","timelineNs":0,"serverTimeNs":1,"startOffsetNs":0}""");
        await Until(() => Staged().Count == 2);
        var second = Staged()[1];
        Assert.Equal(("local:" + two.ProviderItemId, "Two"), (second.TrackId, second.Title));

        await EmitAsync(
            $$"""{"event":"zone.sourceNeeded","zone":"living","afterSourceId":"{{second.SourceId}}"}""",
            $$"""{"event":"zone.sourceStarted","zone":"living","sourceId":"{{second.SourceId}}","timelineNs":1,"serverTimeNs":2,"startOffsetNs":0}""",
            $$"""{"event":"zone.ended","zone":"living","afterSourceId":"{{second.SourceId}}"}""");
        await Until(() => _engine.GetQueue(Zone).Value.Current is null);
        Assert.Equal(2, Staged().Count);
        Assert.Empty(_media.Live);
    }

    [Fact]
    public async Task An_entry_of_a_dead_provider_lists_from_its_snapshot_and_is_refused_by_name()
    {
        var (one, two) = await IndexedAsync();
        var gone = new Track("tidal", "123", "Vanished", "Somebody", "Somewhere", TimeSpan.FromSeconds(200));
        var playlist = Ok(_store.CreatePlaylist("Mixed"));
        Ok(_store.AddEntries(playlist.Id, [one, gone], null));
        var favourite = Ok(_store.AddFavorite(gone));
        var station = Ok(_store.AddFavorite(new RadioStation("webradio", "fip", "FIP")));
        var local = Ok(_store.AddFavorite(two));
        await StartCoreAsync();

        Assert.Equal(gone, Ok(_store.Entries(playlist.Id))[1].Track);

        var enqueued = Ok(await _player.EnqueuePlaylistAsync(Zone, playlist.Id, QueuePlacement.End, Ct));
        Assert.Equal(["One"], enqueued.Queue.Items.Select(i => i.Title));
        var refusal = Assert.Single(enqueued.Refused);
        Assert.Equal((gone, "NotFound: no provider tidal"), (refusal.Entry.Track, refusal.Reason));

        var refused = await _player.EnqueueFavoriteAsync(Zone, favourite.Id, QueuePlacement.End, Ct);
        Assert.Equal(QueueError.Unavailable, refused.Failure.Code);
        Assert.Equal($"favourite {favourite.Id} (Vanished): NotFound: no provider tidal", refused.Failure.Detail);
        Assert.Equal("favourite " + station.Id + " (FIP): NotFound: no provider webradio",
            (await _player.EnqueueFavoriteAsync(Zone, station.Id, QueuePlacement.Next, Ct)).Failure.Detail);

        var dead = Ok(_store.CreatePlaylist("Dead"));
        Ok(_store.AddEntries(dead.Id, [gone], null));
        Assert.Equal("no entry of playlist " + dead.Id + " can play: Vanished (NotFound: no provider tidal)",
            (await _player.EnqueuePlaylistAsync(Zone, dead.Id, QueuePlacement.End, Ct)).Failure.Detail);
        Assert.Equal(QueueError.NotFound,
            (await _player.EnqueuePlaylistAsync(Zone, Ok(_store.CreatePlaylist("Empty")).Id, QueuePlacement.End, Ct)).Failure.Code);
        Assert.Equal(QueueError.NotFound, (await _player.EnqueuePlaylistAsync(Zone, 999, QueuePlacement.End, Ct)).Failure.Code);

        // Next is after the current item; nothing is current yet, so it is first.
        Assert.Equal(["Two", "One"],
            Ok(await _player.EnqueueFavoriteAsync(Zone, local.Id, QueuePlacement.Next, Ct)).Items.Select(i => i.Title));
        Assert.Equal(["Two", "One", "Two"],
            Ok(await _player.EnqueueFavoriteAsync(Zone, local.Id, QueuePlacement.End, Ct)).Items.Select(i => i.Title));
        // An index puts it before the item there (a drop onto a queue row).
        Assert.Equal(["Two", "Two", "One", "Two"],
            Ok(await _player.EnqueueFavoriteAsync(Zone, local.Id, QueuePlacement.End, Ct, index: 1)).Items.Select(i => i.Title));
    }

    [Fact]
    public async Task A_node_of_audiobook_tracks_queues_as_one_group_and_music_as_items()
    {
        var (book, music) = await BooksAsync();
        await StartCoreAsync();

        var album = Ok(await _player.EnqueueNodeAsync(Zone, "local", music, QueuePlacement.End, Ct));
        Assert.Equal((2, false), (album.Queued, album.Grouped));
        Assert.Equal(["One", "Two"], album.Queue.Items.Select(i => i.Title));

        var grouped = Ok(await _player.EnqueueNodeAsync(Zone, "local", book, QueuePlacement.End, Ct));
        Assert.Equal((3, true), (grouped.Queued, grouped.Grouped));
        var item = grouped.Queue.Items[^1];
        Assert.Equal(("The Book", "Narrator", MediaType.Audiobook, "local:" + book),
            (item.Title, item.Group!.Artist, item.Group.Type, item.Group.Source));
        Assert.Equal(["Part One", "Part Two", "Part Three"], item.Group.Parts.Select(p => p.Track.Title));

        // A book ripped as two discs is one book node, and one group.
        var books = (await new LibraryCatalog(_database, "local").BrowseAsync(new BrowseRequest("audiobooks"), Ct)).Value;
        var node = books.Nodes.Single(n => n.Title == "Two Discs");
        var whole = Ok(await _player.EnqueueNodeAsync(Zone, "local", node.NodeId, QueuePlacement.End, Ct));
        Assert.Equal(["Part One", "Part Two", "Part Three", "Part Four"],
            whole.Queue.Items[^1].Group!.Parts.Select(p => p.Track.Title));

        // Asked for single items, a book's parts go in one by one.
        var single = Ok(await _player.EnqueueNodeAsync(Zone, "local", book, QueuePlacement.Replace, Ct, group: false));
        Assert.Equal(["Part One", "Part Two", "Part Three"], single.Queue.Items.Select(i => i.Title));
        Assert.All(single.Queue.Items, i => Assert.Null(i.Group));
    }

    [Fact]
    public async Task A_playlist_keeps_a_group_entry_through_a_restart_and_queues_it_as_one_refusing_a_part_by_name()
    {
        var (book, _) = await BooksAsync();
        await StartCoreAsync();
        var expanded = Ok(await _player.ExpandAsync("local", book, group: true, Ct));
        var gone = new QueuePart(new Track("tidal", "123", "Lost Part", MediaType: MediaType.Audiobook));
        var group = expanded.Group! with { Parts = [.. expanded.Group.Parts, gone] };
        var playlist = Ok(_store.CreatePlaylist("Evening"));
        Ok(_store.AddEntries(playlist.Id, [new Track("local", "track:999", "Song")], null));
        Ok(_store.AddGroup(playlist.Id, group, 0));

        var again = Ok(new PlaylistStore(_database).Entries(playlist.Id));
        Assert.Equal(["The Book", "Song"], again.Select(e => e.Title));
        var kept = again[0].Group!;
        Assert.Equal(("local:" + book, MediaType.Audiobook, 4), (kept.Source, kept.Type, kept.Parts.Count));
        Assert.Equal(["Part One", "Part Two", "Part Three", "Lost Part"], kept.Parts.Select(p => p.Track.Title));
        Assert.Equal(MediaType.Audiobook, kept.Parts[0].Track.MediaType);

        var enqueued = Ok(await _player.EnqueuePlaylistAsync(Zone, playlist.Id, QueuePlacement.End, Ct));
        Assert.Equal(["The Book", "Song"], enqueued.Queue.Items.Select(i => i.Title));
        Assert.Equal(3, enqueued.Queue.Items[0].Group!.Parts.Count);
        var refusal = Assert.Single(enqueued.Refused);
        Assert.Equal("part Lost Part: NotFound: no provider tidal", refusal.Reason);

        Ok(_store.RemoveEntry(playlist.Id, again[0].Id));
        Assert.Equal(0L, long.Parse(Scalar("SELECT COUNT(*) FROM playlist_item_part"), System.Globalization.CultureInfo.InvariantCulture));
    }

    [Fact]
    public void A_station_is_kept_as_an_entry_beside_tracks_and_books_and_saved_from_a_queue()
    {
        var station = new RadioStation("webradio", "fip", "FIP", "Jazz");
        var playlist = Ok(_store.CreatePlaylist("Mixed"));
        Ok(_store.AddEntries(playlist.Id, [new Track("local", "track:1", "Song")], null));
        var entries = Ok(_store.AddStation(playlist.Id, station, 0));
        Assert.Equal(["FIP", "Song"], Titles(entries));
        // Kept as a favourite station is: its identity and its name.
        Assert.Equal(new RadioStation("webradio", "fip", "FIP"), Ok(new PlaylistStore(_database).Entries(playlist.Id))[0].Station);
        Assert.Equal(new RadioStation("webradio", "fip", "FIP"), entries[0].Item);

        Assert.Equal(["Song", "FIP"], Titles(Ok(_store.MoveEntry(playlist.Id, entries[0].Id, 1))));
        Assert.Equal(["Song"], Titles(Ok(_store.RemoveEntry(playlist.Id, entries[0].Id))));
        Assert.Equal(LibraryError.InvalidArgument, _store.AddStation(playlist.Id, station with { ProviderItemId = "" }, null).Failure.Code);
        Assert.Equal(LibraryError.NotFound, _store.AddStation(999, station, null).Failure.Code);
        Assert.Equal(LibraryError.InvalidArgument, _store.AddStation(playlist.Id, station, 5).Failure.Code);

        QueueItem[] queue = [new(new(1), new Track("local", "track:2", "Other"), null), new(new(2), null, station)];
        var saved = Ok(_store.SaveAs("Saved", queue, overwrite: false));
        Assert.Equal(["Other", "FIP"], Titles(Ok(_store.Entries(saved.Id))));
        Assert.Equal(LibraryError.InvalidArgument, _store.SaveAs("Empty", [], overwrite: false).Failure.Code);
    }

    [Fact]
    public async Task With_books_not_grouped_a_node_a_playlists_book_and_a_favourite_book_go_in_part_by_part()
    {
        var (book, _) = await BooksAsync();
        await StartCoreAsync();
        var group = Ok(await _player.ExpandAsync("local", book, group: true, Ct)).Group!;
        var playlist = Ok(_store.CreatePlaylist("Evening"));
        Ok(_store.AddGroup(playlist.Id, group, null));
        var favourite = Ok(_store.AddFavorite(group));
        _player.Settings.Set(groupBooks: false);

        string[] parts = ["Part One", "Part Two", "Part Three"];
        var node = Ok(await _player.EnqueueNodeAsync(Zone, "local", book, QueuePlacement.Replace, Ct));
        Assert.Equal(parts, node.Queue.Items.Select(i => i.Title));
        Assert.False(node.Grouped);
        Assert.Equal(parts, Ok(await _player.EnqueuePlaylistAsync(Zone, playlist.Id, QueuePlacement.Replace, Ct))
            .Queue.Items.Select(i => i.Title));
        Assert.Equal(parts, Ok(await _player.EnqueueFavoriteAsync(Zone, favourite.Id, QueuePlacement.Replace, Ct))
            .Items.Select(i => i.Title));

        // A caller that asks wins over the house.
        var asked = Ok(await _player.EnqueueNodeAsync(Zone, "local", book, QueuePlacement.Replace, Ct, group: true));
        Assert.Equal("The Book", Assert.Single(asked.Queue.Items).Title);
    }

    [Fact]
    public async Task A_book_is_a_favourite_once_with_its_parts_and_queues_as_one_group()
    {
        var (book, _) = await BooksAsync();
        await StartCoreAsync();
        var group = Ok(await _player.ExpandAsync("local", book, group: true, Ct)).Group!;
        var favourite = Ok(_store.AddFavorite(group));
        Assert.Equal(favourite.Id, Ok(_store.AddFavorite(group)).Id);
        Assert.Single(Ok(_store.Favorites()));

        var kept = Ok(new PlaylistStore(_database).GetFavorite(favourite.Id));
        Assert.Equal(("The Book", "local:" + book, MediaType.Audiobook), (kept.Title, kept.Group!.Source, kept.Group.Type));
        Assert.Equal(["Part One", "Part Two", "Part Three"], kept.Group.Parts.Select(p => p.Track.Title));

        var queued = Ok(await _player.EnqueueFavoriteAsync(Zone, favourite.Id, QueuePlacement.End, Ct));
        var item = Assert.Single(queued.Items);
        Assert.Equal(("The Book", 3), (item.Title, item.Group!.Parts.Count));

        // A part that cannot play refuses the favourite by name.
        var lost = Ok(_store.AddFavorite(group with
        {
            Source = "local:folder:elsewhere",
            Parts = [.. group.Parts, new QueuePart(new Track("tidal", "1", "Lost Part"))],
        }));
        var refused = await _player.EnqueueFavoriteAsync(Zone, lost.Id, QueuePlacement.End, Ct);
        Assert.Equal(QueueError.Unavailable, refused.Failure.Code);
        Assert.EndsWith("part Lost Part: NotFound: no provider tidal", refused.Failure.Detail, StringComparison.Ordinal);

        Assert.True(Ok(_store.RemoveFavorite(favourite.Id)));
        Assert.True(Ok(_store.RemoveFavorite(lost.Id)));
        Assert.Equal("0", Scalar("SELECT COUNT(*) FROM favorite_part"));
        Assert.Equal(LibraryError.InvalidArgument, _store.AddFavorite(group with { Source = null }).Failure.Code);
    }

    // --- helpers ------------------------------------------------------------

    // A book (an album of three audiobook files), a second book on two discs
    // and an album of music; answers the first book's and the music's nodes.
    private async Task<(string Book, string Music)> BooksAsync()
    {
        LibraryTests.WriteTrack(Path.Combine(_music, "one.wav"), "One", "Aurora", "Days", 1);
        LibraryTests.WriteTrack(Path.Combine(_music, "two.wav"), "Two", "Aurora", "Days", 2);
        foreach (var (file, title, number) in new[] { ("b1.wav", "Part One", 1), ("b2.wav", "Part Two", 2), ("b3.wav", "Part Three", 3) })
            LibraryTests.WriteTrack(Path.Combine(_music, file), title, "Narrator", "The Book", number, "Audiobook");
        LibraryTests.WriteTrack(Path.Combine(_music, "c1.wav"), "Part One", "Reader", "Two Discs CD 1", 1, "Hörbuch");
        LibraryTests.WriteTrack(Path.Combine(_music, "c2.wav"), "Part Two", "Reader", "Two Discs CD 1", 2, "Hörbuch");
        LibraryTests.WriteTrack(Path.Combine(_music, "c3.wav"), "Part Three", "Reader", "Two Discs CD 2", 1, "Hörbuch");
        LibraryTests.WriteTrack(Path.Combine(_music, "c4.wav"), "Part Four", "Reader", "Two Discs CD 2", 2, "Hörbuch");
        await _indexer.StartAsync(Ct);
        await Until(() => _indexer.Status.Passes == 1);
        var albums = (await new LibraryCatalog(_database, "local").BrowseAsync(new BrowseRequest("albums", 0, 100), Ct)).Value;
        return (albums.Nodes.Single(n => n.Title == "The Book").NodeId, albums.Nodes.Single(n => n.Title == "Days").NodeId);
    }

    private static Track Snapshot(string title) =>
        new("fake", "id-" + title, title, "Artist", "Album", TimeSpan.FromSeconds(90),
            Format: new AudioFormat("FLAC", 96000, 24, 2));

    private static List<string> Titles(IReadOnlyList<PlaylistEntry> entries) => [.. entries.Select(e => e.Title)];

    private List<(string, int)> Order() => [.. Ok(_store.Playlists()).Select(p => (p.Name, p.Position))];

    // Two tracks in the library, as the local provider answers them.
    private async Task<(Track One, Track Two)> IndexedAsync()
    {
        LibraryTests.WriteTrack(Path.Combine(_music, "one.wav"), "One", "Aurora", "Days", 1);
        LibraryTests.WriteTrack(Path.Combine(_music, "two.wav"), "Two", "Aurora", "Days", 2);
        await _indexer.StartAsync(Ct);
        await Until(() => _indexer.Status.Passes == 1);
        var album = new LibraryCatalog(_database, LocalFilesProvider.ProviderId);
        var tracks = (await album.SearchAsync("days", SearchKinds.All, 0, 10, Ct)).Value.Tracks.OrderBy(t => t.Title).ToList();
        Assert.Equal(["One", "Two"], tracks.Select(t => t.Title));
        return (tracks[0], tracks[1]);
    }

    private async Task StartCoreAsync()
    {
        await _providers.StartAsync(Ct);
        await _link.StartAsync(Ct);
        await _synchronised.Next.Task.WaitAsync(Patience, Ct);
    }

    private sealed record Stage(string SourceId, Uri MediaUri, string? TrackId, string Title);

    private List<Stage> Staged() =>
        [.. _core.All.Where(r => r.Command == "source.stage").Select(r => JsonNode.Parse(r.Params)!).Select(p =>
            new Stage(p["sourceId"]!.GetValue<string>(), new Uri(p["mediaUri"]!.GetValue<string>()),
                p["trackId"]?.GetValue<string>(), p["title"]!.GetValue<string>()))];

    private Task EmitAsync(params string[] lines) => _core.EmitAsync(lines);

    private string Scalar(string sql)
    {
        using var connection = _database.Open();
        using var command = connection.CreateCommand();
        command.CommandText = sql;
        return Convert.ToString(command.ExecuteScalar(), System.Globalization.CultureInfo.InvariantCulture)!;
    }

    private static T Ok<T>(Result<T, LibraryError> result)
    {
        Assert.True(result.IsSuccess, result.ToString());
        return result.Value;
    }

    private static T Ok<T>(Result<T, QueueError> result)
    {
        Assert.True(result.IsSuccess, result.ToString());
        return result.Value;
    }

    private static async Task Until(Func<bool> condition)
    {
        using var timeout = new CancellationTokenSource(Patience);
        while (!condition())
            await Task.Delay(5, timeout.Token);
    }
}
