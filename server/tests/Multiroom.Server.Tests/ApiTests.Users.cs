using System.Net;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace Multiroom.Server.Tests;

// Users (phase 36): a like, a favourite or a play may be one person's; an
// empty user is the household.
public sealed partial class ApiTests
{
    [Fact]
    public async Task A_users_likes_and_favourites_are_read_with_the_households_and_left_to_it_when_removed()
    {
        await StartAsync();
        using var stream = await EventsAsync(null);
        var a = _fake.AddTrack(_dir.Write("a.flac", "a"), "A");
        var b = _fake.AddTrack(_dir.Write("b.flac", "b"), "B");

        Assert.Equal("Alice", (await Send(HttpMethod.Post, "users", """{"name":" Alice "}""")).Body!["name"]!.GetValue<string>());
        await Refused(HttpMethod.Post, "users", """{"name":"alice"}""", HttpStatusCode.Conflict, "alreadyExists");
        await Refused(HttpMethod.Post, "users", """{"name":"  "}""", HttpStatusCode.BadRequest, "invalidArgument");
        await Refused(HttpMethod.Post, "users", """{"name":"*"}""", HttpStatusCode.BadRequest, "invalidArgument");
        Assert.Equal(["Alice"], (await Ok(HttpMethod.Get, "users")).AsArray().Select(u => u!["name"]!.GetValue<string>()));

        // The household and Alice like the same track: two rows.
        var house = (await Ok(HttpMethod.Post, "likes", $$"""{"track":{{Json(a)}}}"""))["id"]!.GetValue<long>();
        var hers = await Ok(HttpMethod.Post, "likes", $$"""{"track":{{Json(a)}},"user":"alice"}""");
        Assert.NotEqual(house, hers["id"]!.GetValue<long>());
        Assert.Equal("Alice", hers["user"]!.GetValue<string>());
        Assert.Equal("Alice", JsonNode.Parse((await stream.NextAsync(e => e.Type == "like.changed" &&
            e.Data.Contains("Alice", StringComparison.Ordinal))).Data)!["user"]!.GetValue<string>());
        await Ok(HttpMethod.Post, "likes", $$"""{"track":{{Json(b)}},"user":"Alice"}""");
        await Refused(HttpMethod.Post, "likes", $$"""{"track":{{Json(b)}},"user":"bob"}""", HttpStatusCode.BadRequest,
            "invalidArgument", "no user bob");

        static (string, string)[] Likes(JsonNode likes) =>
            [.. likes.AsArray().Select(l => (l!["item"]!["title"]!.GetValue<string>(), l["user"]!.GetValue<string>()))];
        Assert.Equal([("A", "")], Likes(await Ok(HttpMethod.Get, "likes")));
        Assert.Equal([("A", "Alice"), ("B", "Alice")], Likes(await Ok(HttpMethod.Get, "likes?user=alice")));
        Assert.Equal(3, (await Ok(HttpMethod.Get, "likes?user=*")).AsArray().Count);
        await Refused(HttpMethod.Get, "likes?user=bob", HttpStatusCode.BadRequest, "invalidArgument", "no user bob");
        var status = await Ok(HttpMethod.Post, "items/status",
            $$"""{"items":[{"kind":"track","providerId":"fake","providerItemId":"{{b.ProviderItemId}}"}],"user":"Alice"}""");
        Assert.True(status[0]!["liked"]!.GetValue<bool>());
        Assert.False((await Ok(HttpMethod.Post, "items/status",
            $$"""{"items":[{"kind":"track","providerId":"fake","providerItemId":"{{b.ProviderItemId}}"}]}"""))[0]!["liked"]!
            .GetValue<bool>());

        var favorite = await Ok(HttpMethod.Post, "favorites", $$"""{"track":{{Json(b)}},"user":"Alice"}""");
        Assert.Equal("Alice", JsonNode.Parse((await stream.NextAsync(e => e.Type == "favorite.changed")).Data)!
            ["user"]!.GetValue<string>());
        Assert.Empty((await Ok(HttpMethod.Get, "favorites")).AsArray());
        Assert.Equal("Alice", (await Ok(HttpMethod.Get, "favorites?user=Alice"))[0]!["user"]!.GetValue<string>());

        // only=true: hers alone, without the household's.
        Assert.Equal([("A", "Alice"), ("B", "Alice")], Likes(await Ok(HttpMethod.Get, "likes?user=Alice&only=true")));
        Assert.Equal([("A", "")], Likes(await Ok(HttpMethod.Get, "likes?only=true")));
        await Refused(HttpMethod.Get, "likes?user=Alice&only=yes", HttpStatusCode.BadRequest, "invalidRequest");

        // Removed: her rows are the household's, the shared track left once.
        Assert.Equal(HttpStatusCode.NoContent, (await Send(HttpMethod.Delete, "users/ALICE")).Status);
        Assert.Equal([("A", ""), ("B", "")], Likes(await Ok(HttpMethod.Get, "likes?user=*")));
        Assert.Equal(favorite["id"]!.GetValue<long>(), (await Ok(HttpMethod.Get, "favorites"))[0]!["id"]!.GetValue<long>());
        await Refused(HttpMethod.Delete, "users/Alice", HttpStatusCode.NotFound, "notFound");
        Assert.Empty((await Ok(HttpMethod.Get, "users")).AsArray());

        // Removed with data=delete: her rows go with her.
        await Ok(HttpMethod.Post, "users", """{"name":"Bob"}""");
        await Ok(HttpMethod.Post, "likes", $$"""{"track":{{Json(a)}},"user":"Bob"}""");
        await Refused(HttpMethod.Delete, "users/Bob?data=maybe", HttpStatusCode.BadRequest, "invalidRequest");
        Assert.Equal(HttpStatusCode.NoContent, (await Send(HttpMethod.Delete, "users/Bob?data=delete")).Status);
        Assert.Equal([("A", ""), ("B", "")], Likes(await Ok(HttpMethod.Get, "likes?user=*")));
    }

    [Fact]
    public async Task A_rooms_user_records_their_plays_is_kept_across_a_restart_and_is_answered_over_mcp()
    {
        await StartAsync();
        var a = _fake.AddTrack(_dir.Write("a.flac", "a"), "Song A");
        await Ok(HttpMethod.Post, "users", """{"name":"Alice"}""");
        await Ok(HttpMethod.Put, "zones/living/clients", """{"clients":["roomA"]}""");
        using (var stream = await EventsAsync(null))
        {
            Assert.Equal("Alice", (await Ok(HttpMethod.Patch, "rooms/roomA", """{"user":"alice"}"""))["user"]!.GetValue<string>());
            var room = JsonNode.Parse((await stream.NextAsync(e => e.Type == "room.userChanged")).Data)!;
            Assert.Equal(("roomA", "Alice"), (room["client"]!.GetValue<string>(), room["user"]!.GetValue<string>()));
            var changed = JsonNode.Parse((await stream.NextAsync(e => e.Type == "zone.userChanged")).Data)!;
            Assert.Equal(("living", "Alice"), (changed["zone"]!.GetValue<string>(), changed["user"]!.GetValue<string>()));
        }
        Assert.Equal("Alice", (await Ok(HttpMethod.Get, "zones/living"))["user"]!.GetValue<string>());
        await Refused(HttpMethod.Patch, "rooms/roomA", """{"user":"nobody"}""", HttpStatusCode.BadRequest, "invalidArgument",
            "no user nobody");
        await Refused(HttpMethod.Patch, "zones/living", """{"user":"Alice"}""", HttpStatusCode.BadRequest, "invalidRequest");

        await Ok(HttpMethod.Post, "zones/living/queue/items", $$"""{"tracks":[{{Json(a)}}]}""");
        await Ok(HttpMethod.Post, "zones/living/play");
        await HeardAsync("Song A");
        await Until(async () => (await Ok(HttpMethod.Get, "history?user=*"))["total"]!.GetValue<int>() == 1);
        Assert.Equal(0, (await Ok(HttpMethod.Get, "history"))["total"]!.GetValue<int>());
        var hers = await Ok(HttpMethod.Get, "history?user=Alice");
        Assert.Equal(("Song A", "Alice"), (hers["items"]![0]!["track"]!["title"]!.GetValue<string>(),
            hers["items"]![0]!["user"]!.GetValue<string>()));

        await using (var mcp = await McpAsync(await NamedClient("users", "read", "modify")))
        {
            Assert.Equal("Alice", (await Call(mcp, "list_users"))[0]!["name"]!.GetValue<string>());
            Assert.Equal(1, (await Call(mcp, "get_history", new { user = "Alice" }))["total"]!.GetValue<int>());
            await Call(mcp, "like", new { track = JsonSerializer.SerializeToNode(a, TestJson), user = "Alice" });
            Assert.True(JsonNode.DeepEquals(await Ok(HttpMethod.Get, "likes?user=Alice"),
                (await Call(mcp, "list_likes", new { user = "Alice" }))["items"]));
        }

        // A second room with another listener: the zone is the household's;
        // with the same listener, hers again.
        await Ok(HttpMethod.Put, "zones/living/clients", """{"clients":["roomA","roomB"]}""");
        Assert.Equal("", (await Ok(HttpMethod.Get, "zones/living"))["user"]!.GetValue<string>());
        await Ok(HttpMethod.Patch, "rooms/roomB", """{"user":"Alice"}""");
        Assert.Equal("Alice", (await Ok(HttpMethod.Get, "zones/living"))["user"]!.GetValue<string>());

        // The rooms' users are kept in playback.json.
        _http!.Dispose();
        await _host!.StopAsync(Ct);
        _host.Dispose();
        await StartAsync();
        Assert.Equal("Alice", (await Ok(HttpMethod.Get, "rooms/roomA"))["user"]!.GetValue<string>());
        Assert.Equal("Alice", (await Ok(HttpMethod.Get, "zones/living"))["user"]!.GetValue<string>());
        await Ok(HttpMethod.Patch, "rooms/roomB", """{"user":""}""");
        Assert.Equal("", (await Ok(HttpMethod.Get, "zones/living"))["user"]!.GetValue<string>());
    }
}
