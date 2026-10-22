using System.Net;
using Multiroom.Server.Abstractions;

namespace Multiroom.Server.Tests;

// Where the library's missing art is looked for, set through the API: the
// switches and fanart.tv's key, which is never answered. The host tests
// turn MusicBrainz off in the config, so nothing here reaches the net.
public sealed partial class ApiTests
{
    [Fact]
    public async Task Artwork_sources_are_switched_and_fanart_tvs_key_set_and_removed_without_it_ever_being_answered()
    {
        await StartAsync();

        var initial = await Ok(HttpMethod.Get, "artwork");
        Assert.Equal((false, "config"), (initial["musicBrainz"]!["enabled"]!.GetValue<bool>(),
            initial["musicBrainz"]!["origin"]!.GetValue<string>()));
        Assert.Equal((true, false, "none", false), (initial["fanartTv"]!["enabled"]!.GetValue<bool>(),
            initial["fanartTv"]!["hasKey"]!.GetValue<bool>(), initial["fanartTv"]!["keyOrigin"]!.GetValue<string>(),
            initial["fanartTv"]!["active"]!.GetValue<bool>()));
        Assert.NotNull(initial["progress"]!["pending"]);

        var keyed = await Ok(HttpMethod.Put, "artwork/fanarttv/key", """{"key":" fanart-secret "}""");
        Assert.Equal((true, "api", true), (keyed["fanartTv"]!["hasKey"]!.GetValue<bool>(),
            keyed["fanartTv"]!["keyOrigin"]!.GetValue<string>(), keyed["fanartTv"]!["active"]!.GetValue<bool>()));
        Assert.DoesNotContain("fanart-secret", keyed.ToJsonString(), StringComparison.Ordinal);
        Assert.DoesNotContain("fanart-secret", (await Ok(HttpMethod.Get, "artwork")).ToJsonString(), StringComparison.Ordinal);
        Assert.Contains("fanart-secret",
            await File.ReadAllTextAsync(Path.Combine(_dir.Path, "data", "artwork", "settings.json"), Ct), StringComparison.Ordinal);

        var off = await Ok(HttpMethod.Patch, "artwork", """{"fanartTv":false,"musicBrainz":false}""");
        Assert.Equal((false, true, false), (off["fanartTv"]!["enabled"]!.GetValue<bool>(),
            off["fanartTv"]!["hasKey"]!.GetValue<bool>(), off["fanartTv"]!["active"]!.GetValue<bool>()));
        Assert.Equal("api", off["musicBrainz"]!["origin"]!.GetValue<string>());

        var removed = await Ok(HttpMethod.Delete, "artwork/fanarttv/key");
        Assert.Equal((false, "none"), (removed["fanartTv"]!["hasKey"]!.GetValue<bool>(),
            removed["fanartTv"]!["keyOrigin"]!.GetValue<string>()));

        await Refused(HttpMethod.Patch, "artwork", "{}", HttpStatusCode.BadRequest, "invalidRequest");
        await Refused(HttpMethod.Patch, "artwork", """{"discogs":true}""", HttpStatusCode.BadRequest, "invalidRequest");
        await Refused(HttpMethod.Put, "artwork/fanarttv/key", """{"key":" "}""", HttpStatusCode.BadRequest, "invalidArgument");
        Assert.DoesNotContain("fanart-secret", await ReadLog(),
            StringComparison.Ordinal);
    }

    // Phase 40: every item a list answers names its picture by artworkId,
    // and GET artwork/{id} answers it from the store, else from its provider's
    // artwork (kept for good), else notFound.
    [Fact]
    public async Task An_answered_item_names_its_picture_by_artwork_id_kept_from_its_provider()
    {
        await StartAsync();
        var track = _fake.AddTrack(_dir.Write("a.flac", "a"), "Blue in Green");
        var found = (await Ok(HttpMethod.Get, "search?q=blue"))["items"]!["tracks"]![0]!;
        var id = Artworks.Of(track)!;
        Assert.Equal(id, found["artworkId"]!.GetValue<string>());
        Assert.Null(found["artwork"]);

        using (var picture = await SendBytes(HttpMethod.Get, $"artwork/{id}", null))
        {
            Assert.Equal((HttpStatusCode.OK, "image/png"), (picture.StatusCode, picture.Content.Headers.ContentType!.MediaType));
            Assert.Equal(FakeProvider.ArtworkBytes, await picture.Content.ReadAsByteArrayAsync(Ct));
        }
        Assert.True(File.Exists(Path.Combine(_dir.Path, "data", "local", "art", "songs", id.Split(':')[1] + ".png")));

        // A queue item and a favourite are their item, with its kind.
        var queued = await Ok(HttpMethod.Post, "zones/living/queue/items", $$"""{"tracks":[{{Json(track)}}]}""");
        var item = queued["items"]![0]!["item"]!;
        Assert.Equal(("track", id), (item["kind"]!.GetValue<string>(), item["artworkId"]!.GetValue<string>()));
        // Sent back as it was answered, kind and artworkId included.
        var favourite = await Ok(HttpMethod.Post, "favorites", $$"""{"track":{{item.ToJsonString()}}}""");
        Assert.Equal(("track", null), (favourite["item"]!["kind"]!.GetValue<string>(), favourite["track"]));

        await Refused(HttpMethod.Get, $"artwork/album:{new string('0', 32)}", HttpStatusCode.NotFound, "notFound");
        await Refused(HttpMethod.Get, "artwork/nothing", HttpStatusCode.NotFound, "notFound");
    }

    [Fact]
    public async Task A_radio_song_like_gets_its_picture_from_a_track_found_by_name_and_says_so()
    {
        await StartAsync();
        using var stream = await EventsAsync(null);
        // A title no other test uses: artwork hints are process-wide, so a song
        // another test showed first would already have a picture to fetch.
        _fake.AddTrack(_dir.Write("a.flac", "a"), "Flamenco Sketches");
        var like = await Ok(HttpMethod.Post, "likes", """{"metadata":{"title":"Flamenco Sketches","artist":"Fake Artist"}}""");
        var id = like["item"]!["artworkId"]!.GetValue<string>();
        Assert.Equal(Artworks.SongId("Flamenco Sketches", "Fake Artist"), id);

        // Nothing to fetch it from yet: looked for by name, and announced.
        await Refused(HttpMethod.Get, $"artwork/{id}", HttpStatusCode.NotFound, "notFound");
        await stream.NextAsync(e => e.Type == "artwork.changed" && e.Data.Contains(id, StringComparison.Ordinal));
        using var picture = await SendBytes(HttpMethod.Get, $"artwork/{id}", null);
        Assert.Equal(HttpStatusCode.OK, picture.StatusCode);
        Assert.Equal(FakeProvider.ArtworkBytes, await picture.Content.ReadAsByteArrayAsync(Ct));
    }
    // A kept item's picture is stored without being shown: when it is liked,
    // and, missing, at the next start. The store says what it holds.
    [Fact]
    public async Task A_liked_items_picture_is_stored_unasked_and_again_at_start_when_missing()
    {
        await StartAsync();
        var track = _fake.AddTrack(_dir.Write("a.flac", "a"), "Blue in Green");
        var id = Artworks.Of(track)!;
        var file = Path.Combine(_dir.Path, "data", "local", "art", "songs", id.Split(':')[1] + ".png");
        using (var stream = await EventsAsync(null))
        {
            await Ok(HttpMethod.Post, "likes", $$"""{"track":{{Json(track)}}}""");
            await stream.NextAsync(e => e.Type == "artwork.changed" && e.Data.Contains(id, StringComparison.Ordinal));
        }
        Assert.True(File.Exists(file));
        var store = (await Ok(HttpMethod.Get, "artwork"))["store"]!;
        Assert.Equal((1, 0, 0), (store["images"]!.GetValue<int>(), store["pending"]!.GetValue<int>(),
            store["notFound"]!.GetValue<int>()));

        _http!.Dispose();
        await _host!.StopAsync(Ct);
        _host.Dispose();
        File.Delete(file);
        await StartAsync();
        await Until(async () => File.Exists(file) && (await Ok(HttpMethod.Get, "artwork"))["store"]!["pending"]!.GetValue<int>() == 0);
    }
}
