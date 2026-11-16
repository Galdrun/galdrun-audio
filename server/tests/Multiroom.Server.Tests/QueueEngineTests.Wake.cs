using Multiroom.Server.Abstractions;
using Multiroom.Server.Playback;

namespace Multiroom.Server.Tests;

public sealed partial class QueueEngineTests
{
    private int Wakes() => _core.All.Count(r => r.Command == "client.wake" &&
        r.Params == """{"client":"roomA","wakeOnLan":"02:00:00:00:00:01"}""");

    // One retry interval, once the retries wait on it (their window's timer and the interval's).
    private async Task NextWakeRetryAsync()
    {
        await Until(() => _time.Pending == 2);
        _time.Advance(QueueEngine.WakeRetryInterval);
    }

    private async Task SleepingZoneAsync(bool connected)
    {
        var tracks = await StartAsync("A");
        Ok(await _engine.AddAsync(Zone, tracks, null, Ct));
        Ok(await _engine.SetClientsAsync(Zone, ["roomA"], Ct));
        await EmitAsync("""{"event":"room.connected","client":"roomA","deviceName":"d","reconnect":false}""",
            """{"event":"room.powerState","client":"roomA","powerState":"sleep","wakeOnLan":"02:00:00:00:00:01"}""");
        if (!connected)
            await EmitAsync("""{"event":"room.disconnected","client":"roomA","reason":"test"}""");
        await Until(() => _link.Model.Current.Rooms.GetValueOrDefault("roomA") is { PowerState: "sleep" } room &&
            room.Connected == connected);
    }

    [Fact]
    public async Task A_room_going_to_sleep_as_the_zone_plays_is_woken_again_until_it_is_back()
    {
        // On its way to deep sleep as the play comes: still connected, so the core's one packet is not sent.
        await SleepingZoneAsync(connected: true);
        Ok(await _engine.PlayAsync(Zone, null, Ct));
        await NextWakeRetryAsync();
        await EmitAsync("""{"event":"room.disconnected","client":"roomA","reason":"test"}""");
        await Until(() => _link.Model.Current.Rooms["roomA"] is { Connected: false });
        Assert.Equal(0, Wakes());

        // Asleep: woken at each interval.
        await NextWakeRetryAsync();
        await Until(() => Wakes() == 1);
        await NextWakeRetryAsync();
        await Until(() => Wakes() == 2);

        // Back: the retries end.
        await EmitAsync("""{"event":"room.connected","client":"roomA","deviceName":"d","reconnect":true}""");
        await Until(() => _link.Model.Current.Rooms["roomA"] is { Connected: true, PowerState: null });
        await NextWakeRetryAsync();
        await Until(() => _time.Pending == 0);
        Assert.Equal(2, Wakes());
    }

    [Fact]
    public async Task Wake_retries_end_with_their_window()
    {
        await SleepingZoneAsync(connected: false);
        Ok(await _engine.PlayAsync(Zone, null, Ct));

        // One at each interval before the window closes; then none: a room that never comes back is let be.
        var expected = (int)(QueueEngine.WakeRetryWindow / QueueEngine.WakeRetryInterval) - 1;
        for (var i = 1; i <= expected; i++)
        {
            await NextWakeRetryAsync();
            await Until(() => Wakes() == i);
        }
        await NextWakeRetryAsync();
        await Until(() => _time.Pending == 0);
        Assert.Equal(expected, Wakes());
    }

    [Fact]
    public async Task Wake_retries_end_when_the_zone_stops()
    {
        await SleepingZoneAsync(connected: false);
        Ok(await _engine.PlayAsync(Zone, null, Ct));
        await NextWakeRetryAsync();
        await Until(() => Wakes() == 1);
        Ok(await _engine.StopAsync(Zone, Ct));
        await EmitAsync("""{"event":"zone.transport","zone":"living","state":"stopped","timelineNs":0}""");
        await Until(() => Queue().Transport == QueueTransport.Stopped);
        await NextWakeRetryAsync();
        await Until(() => _time.Pending == 0);
        Assert.Equal(1, Wakes());
    }
}
