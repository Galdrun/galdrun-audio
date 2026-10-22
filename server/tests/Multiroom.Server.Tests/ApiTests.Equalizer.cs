using System.Net;
using System.Text.Json.Nodes;
using Xunit;

namespace Multiroom.Server.Tests;

// Phase 41: a room's equalizer and the house's profiles.
public sealed partial class ApiTests
{
    private const string Eq =
        """{"enabled":true,"preampDb":-6,"bands":[{"type":"peak","frequencyHz":1000,"q":1.41,"gainDb":3},{"type":"highPass","frequencyHz":30,"q":0.7071,"gainDb":0}]}""";

    [Fact]
    public async Task A_room_s_equalizer_is_sent_whole_kept_as_asked_and_its_state_in_the_core_passed_on()
    {
        _core.StatusBody = _core.StatusBody.Replace("\"connectionDrops\":0}", """
            "connectionDrops":0,"equalizer":{"active":false,"clippedSamples":3}}
            """, StringComparison.Ordinal);
        await StartAsync();

        var room = await Ok(HttpMethod.Patch, "rooms/roomA", $$"""{"equalizer":{{Eq}}}""");
        Assert.Equal(("client.setEqualizer",
                """{"client":"roomA","enabled":true,"preampDb":-6,"bands":[{"type":"peak","frequencyHz":1000,"q":1.41,"gainDb":3},{"type":"highPass","frequencyHz":30,"q":0.7071,"gainDb":0}]}"""),
            (_core.All[^1].Command, _core.All[^1].Params));
        var desired = room["desired"]!["equalizer"]!;
        Assert.True(desired["enabled"]!.GetValue<bool>());
        Assert.Equal(-6, desired["preampDb"]!.GetValue<double>());
        Assert.Equal("highPass", desired["bands"]![1]!["type"]!.GetValue<string>());

        var output = room["actual"]!["plan"]!["equalizer"]!;
        Assert.False(output["active"]!.GetValue<bool>());
        Assert.Equal(3UL, output["clippedSamples"]!.GetValue<ulong>());

        // Out of the core's ranges: refused here, nothing sent.
        var asked = _core.All.Count;
        foreach (var bad in new[]
        {
            """{"enabled":true,"preampDb":13,"bands":[]}""",
            """{"enabled":true,"preampDb":0,"bands":[{"type":"peak","frequencyHz":5,"q":1}]}""",
            """{"enabled":true,"preampDb":0,"bands":[{"type":"peak","frequencyHz":100,"q":11}]}""",
            """{"enabled":true,"preampDb":0,"bands":[{"type":"notch","frequencyHz":100,"q":1}]}""",
        })
        {
            var answer = await Send(HttpMethod.Patch, "rooms/roomA", $$"""{"equalizer":{{bad}}}""");
            Assert.Equal(HttpStatusCode.BadRequest, answer.Status);
        }
        Assert.Equal(asked, _core.All.Count);
    }

    [Fact]
    public async Task Profiles_are_listed_saved_removed_and_applied_as_a_copy()
    {
        await StartAsync();

        var all = (await Ok(HttpMethod.Get, "equalizer/profiles")).AsArray();
        Assert.Equal(["Flat", "Bass Boost", "Vocal", "Night"],
            all.Select(p => p!["name"]!.GetValue<string>()));
        Assert.All(all, p => Assert.True(p!["builtIn"]!.GetValue<bool>()));
        Assert.Equal(10, all[0]!["bands"]!.AsArray().Count);

        var mine = """{"preampDb":-2,"bands":[{"type":"lowShelf","frequencyHz":120,"q":0.7,"gainDb":4}]}""";
        Assert.Equal("Desk", (await Ok(HttpMethod.Put, "equalizer/profiles/Desk", mine))["name"]!.GetValue<string>());
        await Refused(HttpMethod.Put, "equalizer/profiles/flat", mine, HttpStatusCode.Conflict, "conflict");
        await Refused(HttpMethod.Put, "equalizer/profiles/Desk",
            """{"preampDb":20,"bands":[]}""", HttpStatusCode.BadRequest, "invalidArgument");
        Assert.Equal(5, (await Ok(HttpMethod.Get, "equalizer/profiles")).AsArray().Count);

        // Applied: copied into the room and switched on.
        var room = await Ok(HttpMethod.Post, "rooms/roomA/equalizer/profile", """{"profile":"desk"}""");
        Assert.Equal(("client.setEqualizer",
                """{"client":"roomA","enabled":true,"preampDb":-2,"bands":[{"type":"lowShelf","frequencyHz":120,"q":0.7,"gainDb":4}]}"""),
            (_core.All[^1].Command, _core.All[^1].Params));
        Assert.Equal(-2, room["desired"]!["equalizer"]!["preampDb"]!.GetValue<double>());
        await Refused(HttpMethod.Post, "rooms/roomA/equalizer/profile", """{"profile":"nope"}""",
            HttpStatusCode.NotFound, "notFound");

        // Changing or removing the profile leaves the room's copy alone.
        await Ok(HttpMethod.Put, "equalizer/profiles/Desk", """{"preampDb":-8,"bands":[]}""");
        Assert.Equal(HttpStatusCode.NoContent, (await Send(HttpMethod.Delete, "equalizer/profiles/desk", null)).Status);
        await Refused(HttpMethod.Delete, "equalizer/profiles/desk", HttpStatusCode.NotFound, "notFound");
        await Refused(HttpMethod.Delete, "equalizer/profiles/Night", HttpStatusCode.Conflict, "conflict");
        var kept = (await Ok(HttpMethod.Get, "rooms/roomA"))["desired"]!["equalizer"]!;
        Assert.Equal(-2, kept["preampDb"]!.GetValue<double>());
        Assert.Equal("lowShelf", kept["bands"]![0]!["type"]!.GetValue<string>());
    }
}
