using System.Net;
using System.Net.Http.Headers;
using System.Text.Json.Nodes;
using Xunit;

namespace Multiroom.Server.Tests;

// Phase 39: a room's name apart from its client id, and forgetting a room.
public sealed partial class ApiTests
{
    [Fact]
    public async Task Room_power_state_is_exposed_in_room_status_and_forwarded_as_live_events()
    {
        var status = JsonNode.Parse(_core.StatusBody)!;
        status["clients"]![0]!["stream"] = new JsonObject { ["ready"] = true, ["powerState"] = "on" };
        _core.StatusBody = status.ToJsonString();
        await StartAsync();
        Assert.Equal("on", (await Ok(HttpMethod.Get, "rooms/roomA"))["actual"]!["powerState"]!.GetValue<string>());
        using var stream = await EventsAsync(null);
        foreach (var power in new[] { "standby", "standbyManual", "on" })
        {
            await _core.EmitAsync($$$"""{"event":"room.powerState","client":"roomA","powerState":"{{{power}}}"}""");
            var changed = JsonNode.Parse((await stream.NextAsync(e => e.Type == "room.powerState")).Data)!;
            Assert.Equal("roomA", changed["client"]!.GetValue<string>());
            Assert.Equal(power, changed["powerState"]!.GetValue<string>());
        }
    }

    [Fact]
    public async Task Button_power_bindings_accept_gesture_keys_and_validate_their_values()
    {
        await StartAsync();
        foreach (var action in new[] { "standbyOff", "standbyOn", "toggleStandby", "deepSleep", "toggleMute" })
        {
            var reply = await Ok(HttpMethod.Patch, "rooms/roomA/inputs/power",
                $$$"""{"click":{"action":"{{{action}}}"}}""");
            Assert.Equal(action, reply["bindings"]!["click"]!["action"]!.GetValue<string>());
            Assert.Equal("client.setInputBindings", _core.All[^1].Command);
        }
        var asked = _core.All.Count;
        await Refused(HttpMethod.Patch, "rooms/roomA/inputs/power", """{"click":{"action":"toggleStandby","secret":1}}""",
            HttpStatusCode.BadRequest, "invalidRequest", "unknown field click.secret");
        await Refused(HttpMethod.Patch, "rooms/roomA/inputs/power", """{"click":{}}""",
            HttpStatusCode.BadRequest, "invalidRequest", "field click.action is required");
        await Refused(HttpMethod.Patch, "rooms/roomA/inputs/power", """{"click":{"action":"unknownPower"}}""",
            HttpStatusCode.BadRequest, "invalidRequest");
        Assert.Equal(asked, _core.All.Count);
    }

    [Fact]
    public async Task Firmware_network_and_led_controls_use_the_core_and_propagate_refusals()
    {
        _core.Answer("client.getNetwork", p => new JsonObject { ["client"] = "roomA", ["supported"] = true,
            ["network"] = JsonNode.Parse("""{"ethernet":{"speedMbps":"auto"}}""") });
        _core.Answer("client.setEthernetSpeed", p => new JsonObject { ["client"] = "roomA", ["supported"] = true,
            ["network"] = new JsonObject { ["ethernet"] = new JsonObject { ["speedMbps"] = p["speedMbps"]!.DeepClone() } },
            ["applyDelaySeconds"] = 2 });
        _core.Answer("client.getLeds", p => new JsonObject { ["client"] = "roomA", ["supported"] = true, ["off"] = false });
        _core.Answer("client.setLeds", p => new JsonObject { ["client"] = "roomA", ["supported"] = true, ["off"] = p["off"]!.DeepClone() });
        await StartAsync();
        Assert.Equal("auto", (await Ok(HttpMethod.Get, "rooms/roomA/network"))["network"]!["ethernet"]!["speedMbps"]!.GetValue<string>());
        var network = await Ok(HttpMethod.Patch, "rooms/roomA/network", """{"speedMbps":"10"}""");
        Assert.Equal("10", network["network"]!["ethernet"]!["speedMbps"]!.GetValue<string>());
        Assert.Equal(("client.setEthernetSpeed", """{"client":"roomA","speedMbps":"10"}"""), (_core.All[^1].Command, _core.All[^1].Params));
        Assert.False((await Ok(HttpMethod.Get, "rooms/roomA/leds"))["off"]!.GetValue<bool>());
        Assert.True((await Ok(HttpMethod.Patch, "rooms/roomA/leds", """{"off":true}"""))["off"]!.GetValue<bool>());
        Assert.Equal(("client.setLeds", """{"client":"roomA","off":true}"""), (_core.All[^1].Command, _core.All[^1].Params));
        await Refused(HttpMethod.Patch, "rooms/roomA/leds", """{"off":"true"}""", HttpStatusCode.BadRequest, "invalidRequest");
        await Refused(HttpMethod.Patch, "rooms/roomA/leds", "{}", HttpStatusCode.BadRequest, "invalidRequest");
        _core.Answer("client.getPower", p => new JsonObject { ["client"] = "roomA", ["supported"] = true, ["state"] = "on",
            ["standbyAfterSeconds"] = 900, ["deepSleepAfterSeconds"] = 0, ["deepSleep"] = true });
        _core.Answer("client.setPower", p => new JsonObject { ["client"] = "roomA", ["supported"] = true, ["state"] = "on",
            ["standbyAfterSeconds"] = 900, ["deepSleepAfterSeconds"] = p["deepSleepAfterSeconds"]!.DeepClone(), ["deepSleep"] = true });
        _core.Answer("client.power", p => new JsonObject { ["client"] = "roomA", ["supported"] = true, ["state"] = "standbyManual" });
        Assert.Equal(900, (await Ok(HttpMethod.Get, "rooms/roomA/power"))["standbyAfterSeconds"]!.GetValue<int>());
        Assert.Equal(3600, (await Ok(HttpMethod.Patch, "rooms/roomA/power", """{"deepSleepAfterSeconds":3600}"""))["deepSleepAfterSeconds"]!.GetValue<int>());
        Assert.Equal(("client.setPower", """{"client":"roomA","deepSleepAfterSeconds":3600}"""), (_core.All[^1].Command, _core.All[^1].Params));
        Assert.Equal("standbyManual", (await Ok(HttpMethod.Post, "rooms/roomA/power", """{"action":"standbyOn"}"""))["state"]!.GetValue<string>());
        Assert.Equal(("client.power", """{"client":"roomA","action":"standbyOn"}"""), (_core.All[^1].Command, _core.All[^1].Params));
        await Refused(HttpMethod.Patch, "rooms/roomA/power", """{"deepSleepAfterSeconds":-1}""", HttpStatusCode.BadRequest, "invalidRequest");
        await Refused(HttpMethod.Patch, "rooms/roomA/network", """{"speedMbps":"10","unknown":1}""", HttpStatusCode.BadRequest, "invalidRequest");
        _core.Refuse("client.setEthernetSpeed", "unavailable", "unsupported firmware");
        await Refused(HttpMethod.Patch, "rooms/roomA/network", """{"speedMbps":"100"}""", HttpStatusCode.ServiceUnavailable, "unavailable");
    }

    [Fact]
    public async Task A_room_is_shown_and_its_zone_named_by_its_name_and_renamed_through_the_core()
    {
        _core.StatusBody = _core.StatusBody.Replace("""{"clientId":"roomA",""", """{"clientId":"roomA","name":"Kitchen",""",
            StringComparison.Ordinal);
        await StartAsync();

        var room = await Ok(HttpMethod.Get, "rooms/roomA");
        Assert.Equal(("roomA", "Kitchen"), (room["client"]!.GetValue<string>(), room["name"]!.GetValue<string>()));
        Assert.Equal("Kitchen", (await Ok(HttpMethod.Get, "zones/living"))["name"]!.GetValue<string>());

        await Ok(HttpMethod.Patch, "rooms/roomA", """{"name":"Arbeitsraum"}""");
        Assert.Equal(("client.setName", """{"client":"roomA","name":"Arbeitsraum"}"""),
            (_core.All[^1].Command, _core.All[^1].Params));

        // Section 60's rule is checked here; nothing reaches the core.
        var asked = _core.All.Count;
        foreach (var bad in new[] { "\" Arbeitsraum\"", "\"\"", $"\"{new string('a', 65)}\"", "\"Arbeits\\nraum\"" })
            await Refused(HttpMethod.Patch, "rooms/roomA", $$"""{"name":{{bad}}}""", HttpStatusCode.BadRequest, "invalidArgument");
        Assert.Equal(asked, _core.All.Count);

        // The core refuses a room that is away or never said a name.
        _core.Refuse("client.setName", "unavailable", "roomA is not connected or has not said its name");
        await Refused(HttpMethod.Patch, "rooms/roomA", """{"name":"Wohnzimmer"}""", HttpStatusCode.ServiceUnavailable, "unavailable");

        // The name the client takes comes back as room.name, and is passed on.
        using var stream = await EventsAsync(null);
        await _core.EmitAsync("""{"event":"room.name","client":"roomA","name":"Arbeitsraum"}""");
        var renamed = JsonNode.Parse((await stream.NextAsync(e => e.Type == "room.name")).Data)!;
        Assert.Equal("Arbeitsraum", renamed["name"]!.GetValue<string>());
        _core.StatusBody = _core.StatusBody.Replace("\"name\":\"Kitchen\"", "\"name\":\"Arbeitsraum\"", StringComparison.Ordinal);
        Assert.Equal("Arbeitsraum", (await Ok(HttpMethod.Get, "rooms/roomA"))["name"]!.GetValue<string>());
        Assert.Equal("Arbeitsraum", (await Ok(HttpMethod.Get, "zones/living"))["name"]!.GetValue<string>());
    }

    [Fact]
    public async Task Mcp_takes_a_zone_by_one_of_its_rooms_names_in_any_case()
    {
        _core.StatusBody = _core.StatusBody.Replace("""{"clientId":"roomA",""", """{"clientId":"roomA","name":"Kitchen",""",
            StringComparison.Ordinal);
        await StartAsync($$""", "token": "{{McpToken}}" """);
        Http.DefaultRequestHeaders.Authorization = new AuthenticationHeaderValue("Bearer", McpToken);
        await Ok(HttpMethod.Put, "zones/living/clients", """{"clients":["roomA"]}""");

        await using var mcp = await McpAsync(McpToken);
        foreach (var name in new[] { "Kitchen", " kitchen ", "roomA" })
        {
            var zone = await Call(mcp, "set_zone_volume", new { zone = name, volume = 0.5 });
            Assert.Equal(("living", "Kitchen"), (zone["zone"]!.GetValue<string>(), zone["name"]!.GetValue<string>()));
        }
    }

    [Fact]
    public async Task A_room_without_a_name_is_shown_by_its_id()
    {
        await StartAsync();
        Assert.Equal("roomA", (await Ok(HttpMethod.Get, "rooms/roomA"))["name"]!.GetValue<string>());
        Assert.Equal("roomA", (await Ok(HttpMethod.Get, "zones/living"))["name"]!.GetValue<string>());
    }

    [Fact]
    public async Task A_room_that_is_away_is_forgotten_with_its_image_and_one_that_is_here_is_not()
    {
        _core.StatusBody = _core.StatusBody.Replace("\"clients\":[{", """
            "clients":[{"clientId":"ESP-AUDIO-A1S","connected":false,"member":false,"zone":"office",
               "settings":{"audioControl":"bitPerfectFallback","driftCorrection":true,"audioControlIsDefault":true,"driftCorrectionIsDefault":true},
               "driftCorrection":{"requested":true}},{
            """, StringComparison.Ordinal).Replace("\"zones\":[{", """
            "zones":[{"zone":"office","state":"idle","transport":"stopped","staged":[],"failedSources":[],
               "members":[],"clients":["ESP-AUDIO-A1S"],"startsSent":0,"rejoins":0},{
            """, StringComparison.Ordinal);
        await StartAsync();

        await Refused(HttpMethod.Delete, "rooms/roomA", HttpStatusCode.Conflict, "conflict");
        await Refused(HttpMethod.Delete, "rooms/nobody", HttpStatusCode.NotFound, "notFound");

        byte[] jpeg = [0xFF, 0xD8, 0xFF, 0xE0, 4, 5];
        Assert.Equal(HttpStatusCode.OK, (await SendBytes(HttpMethod.Put, "rooms/ESP-AUDIO-A1S/image", jpeg)).StatusCode);

        using var stream = await EventsAsync(null);
        Assert.Equal(HttpStatusCode.NoContent, (await SendBytes(HttpMethod.Delete, "rooms/ESP-AUDIO-A1S", null)).StatusCode);
        var forgotten = JsonNode.Parse((await stream.NextAsync(e => e.Type == "room.forgotten")).Data)!;
        Assert.Equal("ESP-AUDIO-A1S", forgotten["client"]!.GetValue<string>());

        // Out of its zone first, then gone from the core.
        var commands = _core.All.Select(r => (r.Command, r.Params)).ToList();
        var moved = commands.FindIndex(c => c == ("zone.setClients", """{"zone":"office","clients":[]}"""));
        var forgot = commands.FindIndex(c => c == ("client.forget", """{"client":"ESP-AUDIO-A1S"}"""));
        Assert.True(moved >= 0 && forgot > moved);
        await Refused(HttpMethod.Get, "rooms/ESP-AUDIO-A1S/image", HttpStatusCode.NotFound, "notFound");
    }

    // A room the server holds only as a zone's member, which the core has no
    // record of (it answers notFound), is forgotten all the same, and not
    // given a new zone.
    [Fact]
    public async Task A_room_the_core_does_not_know_is_forgotten_and_not_given_a_new_zone()
    {
        _core.StatusBody = _core.StatusBody.Replace("\"zones\":[{", """
            "zones":[{"zone":"office","state":"idle","transport":"stopped","staged":[],"failedSources":[],
               "members":[],"clients":[],"startsSent":0,"rejoins":0},{
            """, StringComparison.Ordinal);
        _core.Refuse("client.forget", "notFound", "no room ESP-AUDIO-A1S");
        await StartAsync();
        await Ok(HttpMethod.Put, "zones/office/clients", """{"clients":["ESP-AUDIO-A1S"]}""");
        await Ok(HttpMethod.Get, "rooms/ESP-AUDIO-A1S");
        // As the core answers: emptying the zone names the room it lost, and
        // prunes its record, so client.forget finds none.
        _core.Answer("zone.setClients", p => new JsonObject
        {
            ["zone"] = p["zone"]!.GetValue<string>(),
            ["added"] = new JsonArray(),
            ["removed"] = new JsonArray("ESP-AUDIO-A1S"),
        });

        Assert.Equal(HttpStatusCode.NoContent, (await SendBytes(HttpMethod.Delete, "rooms/ESP-AUDIO-A1S", null)).StatusCode);
        await Refused(HttpMethod.Get, "rooms/ESP-AUDIO-A1S", HttpStatusCode.NotFound, "notFound");
        Assert.Single(_core.All, r => r.Command == "zone.setClients" && r.Params.Contains("[\"ESP-AUDIO-A1S\"]", StringComparison.Ordinal));
    }
}
