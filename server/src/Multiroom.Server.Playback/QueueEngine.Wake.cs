using System.Collections.Concurrent;
using Microsoft.Extensions.Logging;
using Multiroom.Server.Abstractions;
using Multiroom.Server.Playback.Core;

namespace Multiroom.Server.Playback;

// A play wakes the zone's rooms in deep sleep: the core sends each one Wake-on-LAN packet. A room that was still
// going to sleep then (connected, or its network not yet listening for the packet) misses it, and so does a
// packet lost on the way; nothing else would wake it. So for WakeRetryWindow after a play, each room of the zone
// that is away is woken again every WakeRetryInterval, until every room is back or the zone no longer plays.
// Only after a play, as the core's own packet: a room put to sleep while its zone plays on stays asleep until
// the next play.
public sealed partial class QueueEngine
{
    public static readonly TimeSpan WakeRetryInterval = TimeSpan.FromSeconds(5);
    public static readonly TimeSpan WakeRetryWindow = TimeSpan.FromSeconds(90);

    private readonly ConcurrentDictionary<string, CancellationTokenSource> _wakeRetries = new(StringComparer.Ordinal);

    // A play of `zone` was accepted: (re)starts its retries.
    internal void RetryWakes(string zone)
    {
        var retries = new CancellationTokenSource(WakeRetryWindow, Time);
        if (_wakeRetries.TryRemove(zone, out var previous))
            previous.Cancel();
        _wakeRetries[zone] = retries;
        _ = WakeLoopAsync(zone, retries);
    }

    private async Task WakeLoopAsync(string zone, CancellationTokenSource retries)
    {
        try
        {
            while (true)
            {
                await Task.Delay(WakeRetryInterval, Time, retries.Token).ConfigureAwait(false);
                if (GetQueue(zone) is not { IsSuccess: true } queue ||
                    queue.Value.Transport is not (QueueTransport.Starting or QueueTransport.Playing))
                    return;
                var rooms = Link.Model.Current.Rooms;
                var members = Link.Desired.MembersOf(zone);
                // Back: connected and not on its way to sleep.
                if (members.All(c => rooms.GetValueOrDefault(c) is { Connected: true } room && room.PowerState != "sleep"))
                    return;
                var desired = Link.Desired.Rooms;
                foreach (var client in members)
                {
                    var room = rooms.GetValueOrDefault(client);
                    if (room is { Connected: true })
                        continue;
                    var mac = room is { PowerState: "sleep", WakeOnLan: { } announced } ? announced
                        : desired.GetValueOrDefault(client)?.WakeOnLan;
                    if (mac is null)
                        continue;
                    var woken = await Link.SendAsync(new ClientWake(client, mac), retries.Token).ConfigureAwait(false);
                    if (woken.IsSuccess)
                        LogWakeRetried(Log, zone, client);
                    else
                        LogWakeRetryFailed(Log, zone, client, woken.Failure);
                }
            }
        }
        catch (OperationCanceledException) when (retries.IsCancellationRequested)
        {
        }
        finally
        {
            _wakeRetries.TryRemove(new KeyValuePair<string, CancellationTokenSource>(zone, retries));
            retries.Dispose();
        }
    }

    [LoggerMessage(Level = LogLevel.Information, Message = "zone {Zone}: woke {Client} again, still away after the play")]
    private static partial void LogWakeRetried(ILogger log, string zone, string client);

    [LoggerMessage(Level = LogLevel.Debug, Message = "zone {Zone}: could not wake {Client} again: {Reason}")]
    private static partial void LogWakeRetryFailed(ILogger log, string zone, string client, Failure<CoreError> reason);
}
