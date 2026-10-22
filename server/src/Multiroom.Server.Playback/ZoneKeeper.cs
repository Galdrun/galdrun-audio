using System.Security.Cryptography;
using System.Threading.Channels;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;
using Multiroom.Server.Abstractions;
using Multiroom.Server.Playback.Core;

namespace Multiroom.Server.Playback;

// Zones are never made by hand: every room the core knows is in exactly one
// zone, and no zone is left without a room. A room in none, asked or held by
// the core, is given a zone of its own, under a generated id; a zone whose last room moved away is deleted
// with its queue. The name a caller sees is the rooms' (House.ZoneOf), so the
// id never changes and a playing zone plays on as rooms join it.
//
// Run on its own task, not CoreLink's dispatch task: the events only ask for
// a pass, and a pass waits for the core.
public sealed partial class ZoneKeeper : BackgroundService, ICoreEventSubscriber
{
    private readonly CoreLink _link;
    private readonly IQueueEngine _queues;
    private readonly ILogger _log;
    private readonly SemaphoreSlim _gate = new(1, 1);
    private readonly Channel<bool> _passes =
        Channel.CreateBounded<bool>(new BoundedChannelOptions(1) { FullMode = BoundedChannelFullMode.DropWrite });

    public ZoneKeeper(CoreLink link, IQueueEngine queues, ILogger<ZoneKeeper> log)
    {
        _link = link;
        _queues = queues;
        _log = log;
        link.Subscribe(this);
    }

    public ValueTask HandleAsync(CoreEvent coreEvent, CancellationToken cancellationToken)
    {
        if (coreEvent is CoreSynchronised or RoomConnected or EventsLost { Local: true })
            _passes.Writer.TryWrite(true);
        return ValueTask.CompletedTask;
    }

    // Moves rooms into a zone that exists: the full list, as the core takes
    // it. A room dropped from the list gets a zone of its own again, at the
    // level of the zone it left, and a zone the move emptied is deleted.
    public async Task<Result<QueueState, QueueError>> SetClientsAsync(string zone, IReadOnlyList<string> clients,
        CancellationToken cancellationToken)
    {
        await _gate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            if (!Exists(zone))
                return Result.Fail(QueueError.NotFound, $"no zone {zone}");
            await TakeOutLockedAsync(zone, _link.Desired.MembersOf(zone).Except(clients, StringComparer.Ordinal).ToList(),
                cancellationToken).ConfigureAwait(false);
            var set = await _queues.SetClientsAsync(zone, clients, cancellationToken).ConfigureAwait(false);
            await PassLockedAsync(cancellationToken).ConfigureAwait(false);
            return set.IsSuccess ? _queues.GetQueue(zone) : set;
        }
        finally
        {
            _gate.Release();
        }
    }

    // Deletes a zone and its queue; its rooms get zones of their own again.
    public async Task<Result<bool, QueueError>> DeleteAsync(string zone, CancellationToken cancellationToken)
    {
        await _gate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            var queued = _queues.GetQueue(zone).IsSuccess;
            if (!queued && !Exists(zone))
                return Result.Fail(QueueError.NotFound, $"no zone {zone}");
            if (queued)
            {
                var deleted = await _queues.DeleteAsync(zone, cancellationToken).ConfigureAwait(false);
                if (!deleted.IsSuccess)
                    return deleted;
            }
            if (Exists(zone))
                await DeleteZoneAsync(zone, cancellationToken).ConfigureAwait(false);
            await PassLockedAsync(cancellationToken).ConfigureAwait(false);
            return true;
        }
        finally
        {
            _gate.Release();
        }
    }

    // Forgets a room that is not connected (phase 39): out of its zone, its
    // own settings and kept name gone, and the core told. A zone it leaves
    // empty is deleted with its queue, as after any move.
    public async Task<Result<bool, CoreError>> ForgetAsync(string client, CancellationToken cancellationToken)
    {
        await _gate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            var model = _link.Model.Current;
            var room = model.Rooms.GetValueOrDefault(client);
            if (room is null && !_link.Desired.Clients.Contains(client))
                return Result.Fail(CoreError.NotFound, $"no room {client}");
            if (room is { Connected: true })
                return Result.Fail(CoreError.Conflict, $"{client} is connected: only a room that is away can be forgotten");

            if ((_link.Desired.ZoneOf(client) ?? room?.Zone) is { } zone)
            {
                var others = _link.Desired.MembersOf(zone).Where(c => c != client).ToList();
                var moved = await _queues.SetClientsAsync(zone, others, cancellationToken).ConfigureAwait(false);
                if (!moved.IsSuccess)
                    LogFailed(_log, zone, moved.Failure.ToString());
            }
            // A core that does not know the room, or no core, has nothing to
            // forget; the table is what outlives both.
            var forgot = await _link.SendAsync(new ClientForget(client), cancellationToken).ConfigureAwait(false);
            if (!forgot.IsSuccess && forgot.Failure.Code is not (CoreError.NotFound or CoreError.NotConnected))
                return forgot.Failure;
            // The model can hold a room the core has no record of (a zone's
            // member that never connected): it goes too, or the pass below
            // would give it a new zone at once.
            if (!forgot.IsSuccess)
                _link.Model.Apply(new ClientForget(client), new ClientForgotten(client));
            _link.Desired.Forget(client);
            LogForgotten(_log, client);
            await PassLockedAsync(cancellationToken).ConfigureAwait(false);
            return true;
        }
        finally
        {
            _gate.Release();
        }
    }

    // One pass, for a test that does not run the loop.
    public async Task PassAsync(CancellationToken cancellationToken)
    {
        await _gate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            await PassLockedAsync(cancellationToken).ConfigureAwait(false);
        }
        finally
        {
            _gate.Release();
        }
    }

    protected override async Task ExecuteAsync(CancellationToken stoppingToken)
    {
        try
        {
            await foreach (var _ in _passes.Reader.ReadAllAsync(stoppingToken).ConfigureAwait(false))
                await PassAsync(stoppingToken).ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (stoppingToken.IsCancellationRequested)
        {
            // Stopping.
        }
    }

    public override void Dispose()
    {
        _gate.Dispose();
        base.Dispose();
    }

    // Each room into a zone of its own that already has `zone`'s volume and
    // mute, before `zone` lets it go: its effective level never changes, so
    // it is not sent full volume in between (nor anything at all).
    private async Task TakeOutLockedAsync(string zone, List<string> leaving, CancellationToken cancellationToken)
    {
        if (leaving.Count == 0)
            return;
        var level = _link.Desired.ZoneLevels.GetValueOrDefault(zone);
        foreach (var client in leaving)
        {
            var own = NewZone();
            if (level is not null)
                _link.Desired.SetZoneLevel(own, level.Volume, level.Muted);
            LogCreated(_log, own, client);
            var set = await _queues.SetClientsAsync(own, [client], cancellationToken).ConfigureAwait(false);
            if (!set.IsSuccess)
                LogFailed(_log, own, set.Failure.ToString());
        }
    }

    private bool Exists(string zone) =>
        _link.Desired.Zones.Any(z => z.Zone == zone) || _link.Model.Current.Zones.ContainsKey(zone);

    // Empty zones first, so a room they held is free; then a zone for every
    // room in none. A zone with no membership asked is empty when the core
    // holds none either. A failure is logged and left to the next pass.
    private async Task PassLockedAsync(CancellationToken cancellationToken)
    {
        var model = _link.Model.Current;
        foreach (var (zone, clients) in _link.Desired.Zones)
        {
            if ((clients ?? model.Zones.GetValueOrDefault(zone)?.Clients ?? []).Count > 0)
                continue;
            LogEmpty(_log, zone);
            if (_queues.GetQueue(zone).IsSuccess)
            {
                var deleted = await _queues.DeleteAsync(zone, cancellationToken).ConfigureAwait(false);
                if (!deleted.IsSuccess)
                    LogFailed(_log, zone, deleted.Failure.ToString());
            }
            if (Exists(zone))
                await DeleteZoneAsync(zone, cancellationToken).ConfigureAwait(false);
        }

        model = _link.Model.Current;
        var held = model.Zones.Values.SelectMany(z => z.Clients).ToHashSet(StringComparer.Ordinal);
        foreach (var client in model.Rooms.Keys.Where(c => _link.Desired.ZoneOf(c) is null && !held.Contains(c)).ToList())
        {
            var zone = NewZone();
            LogCreated(_log, zone, client);
            var set = await _queues.SetClientsAsync(zone, [client], cancellationToken).ConfigureAwait(false);
            if (!set.IsSuccess)
                LogFailed(_log, zone, set.Failure.ToString());
        }
    }

    // A zone the core does not have is as deleted as it gets: the table lets
    // it go too.
    private async Task DeleteZoneAsync(string zone, CancellationToken cancellationToken)
    {
        var deleted = await _link.SendAsync(new ZoneDelete(zone), cancellationToken).ConfigureAwait(false);
        if (deleted.IsSuccess)
            return;
        if (deleted.Failure.Code == CoreError.NotFound)
            _link.Desired.Record(new ZoneDelete(zone));
        else
            LogFailed(_log, zone, deleted.Failure.ToString());
    }

    private string NewZone()
    {
        string zone;
        do
            zone = $"zone-{Convert.ToHexStringLower(RandomNumberGenerator.GetBytes(4))}";
        while (Exists(zone) || _queues.GetQueue(zone).IsSuccess);
        return zone;
    }

    [LoggerMessage(Level = LogLevel.Information, Message = "zone {Zone} created for room {Client}")]
    private static partial void LogCreated(ILogger log, string zone, string client);

    [LoggerMessage(Level = LogLevel.Information, Message = "room {Client} forgotten")]
    private static partial void LogForgotten(ILogger log, string client);

    [LoggerMessage(Level = LogLevel.Information, Message = "zone {Zone} has no room left; deleting it")]
    private static partial void LogEmpty(ILogger log, string zone);

    [LoggerMessage(Level = LogLevel.Warning, Message = "zone {Zone}: {Failure}; the next pass tries again")]
    private static partial void LogFailed(ILogger log, string zone, string failure);
}
