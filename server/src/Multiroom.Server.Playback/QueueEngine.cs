using System.Globalization;
using System.Collections.Concurrent;
using System.Security.Cryptography;
using Microsoft.Extensions.Logging;
using Multiroom.Server.Abstractions;
using Multiroom.Server.Playback.Core;
using Multiroom.Server.Providers;

namespace Multiroom.Server.Playback;

// The queue engine: one ZoneSession per zone, each mapping its queue onto the
// core's staged sources and transport, driven by the core's events. It is the
// only thing that stages: it resolves through the provider registry, publishes
// the stream to the media endpoint and names the URL in source.stage.
//
// Room settings other than volume and membership (audio control, drift
// correction, output latency) are CoreLink's desired state already, sent with
// CoreLink.SendAsync; the room.* events keep CoreLink.Model current. Neither
// needs a session.
public sealed partial class QueueEngine : IQueueEngine, ICoreEventSubscriber
{
    private readonly ConcurrentDictionary<string, ZoneSession> _sessions = new(StringComparer.Ordinal);

    // A sourceId names one staging and is never given twice to one zone: the
    // core answers a known one as a replay and stages nothing. The run prefix
    // keeps this process's ids apart from a previous one's in a core that
    // outlived it.
    private readonly string _run = Convert.ToHexStringLower(RandomNumberGenerator.GetBytes(4));
    private long _nextSourceId;
    private long _nextItemId;

    // The time provider is the playhead's clock; a test passes its own.
    public QueueEngine(CoreLink link, ProviderRegistry providers, IMediaPublisher media, ILogger<QueueEngine> log,
        TimeProvider? time = null)
    {
        Link = link;
        Providers = providers;
        Media = media;
        Log = log;
        Time = time ?? TimeProvider.System;
        link.Subscribe(this);
    }

    internal CoreLink Link { get; }

    internal ProviderRegistry Providers { get; }

    internal IMediaPublisher Media { get; }

    internal ILogger Log { get; }

    internal TimeProvider Time { get; }

    // A queue changed: its items, position, modes, transport or now-playing.
    // Raised under the zone's gate, so a handler must not wait on anything.
    public event EventHandler<QueueState>? QueueChanged;

    // A zone's queue was deleted; the argument is the zone.
    public event EventHandler<string>? QueueDeleted;

    public IReadOnlyList<QueueState> Queues =>
        [.. _sessions.Values.Select(s => s.State).OrderBy(s => s.Zone, StringComparer.Ordinal)];

    // Every queue as it is kept across a restart, the playhead read now.
    public IReadOnlyList<SavedQueueState> Save() =>
        [.. _sessions.Values.Select(s => s.Saved).OrderBy(s => s.Zone, StringComparer.Ordinal)];

    // At startup, before CoreLink first connects: the queues come back
    // stopped, where they were. With resume, those that were playing are
    // started again once the core is in step. Item ids go on after theirs.
    public void Restore(IReadOnlyList<SavedQueueState> queues, bool resume)
    {
        foreach (var saved in queues)
        {
            var session = new ZoneSession(saved.Zone, this);
            session.Restore(saved, resume);
            _sessions[saved.Zone] = session;
            _nextItemId = Math.Max(_nextItemId, saved.Items.Select(i => i.Id.Value).DefaultIfEmpty().Max());
        }
    }

    internal string NextSourceId() => $"{_run}-{Interlocked.Increment(ref _nextSourceId)}";

    internal QueueItemId NextItemId() => new(Interlocked.Increment(ref _nextItemId));

    internal void OnQueueChanged(QueueState state) => QueueChanged?.Invoke(this, state);

    public Result<QueueState, QueueError> GetQueue(string zone) =>
        _sessions.TryGetValue(zone, out var session) ? session.State : NoQueue(zone);

    public Task<Result<QueueState, QueueError>> AddAsync(string zone, IReadOnlyList<Track> tracks, int? index,
        CancellationToken cancellationToken) =>
        AddEntriesAsync(zone, Entries(tracks), index, cancellationToken);

    public Task<Result<QueueState, QueueError>> AddAsync(string zone, RadioStation station, int? index,
        CancellationToken cancellationToken) =>
        AddEntriesAsync(zone, [new QueueEntry(Station: station)], index, cancellationToken);

    public Task<Result<QueueState, QueueError>> AddNextAsync(string zone, IReadOnlyList<Track> tracks,
        CancellationToken cancellationToken) =>
        AddEntriesNextAsync(zone, Entries(tracks), cancellationToken);

    public Task<Result<QueueState, QueueError>> AddNextAsync(string zone, RadioStation station,
        CancellationToken cancellationToken) =>
        AddEntriesNextAsync(zone, [new QueueEntry(Station: station)], cancellationToken);

    public Task<Result<QueueState, QueueError>> ReplaceAsync(string zone, IReadOnlyList<Track> tracks,
        CancellationToken cancellationToken) =>
        ReplaceEntriesAsync(zone, Entries(tracks), cancellationToken);

    public Task<Result<QueueState, QueueError>> ReplaceAsync(string zone, RadioStation station,
        CancellationToken cancellationToken) =>
        ReplaceEntriesAsync(zone, [new QueueEntry(Station: station)], cancellationToken);

    public Task<Result<QueueState, QueueError>> AddEntriesAsync(string zone, IReadOnlyList<QueueEntry> entries,
        int? index, CancellationToken cancellationToken) =>
        Checked(entries) ?? Create(zone, s => s.AddAsync(entries, index), cancellationToken);

    public Task<Result<QueueState, QueueError>> AddEntriesNextAsync(string zone, IReadOnlyList<QueueEntry> entries,
        CancellationToken cancellationToken) =>
        Checked(entries) ?? Create(zone, s => s.AddNextAsync(entries), cancellationToken);

    public Task<Result<QueueState, QueueError>> ReplaceEntriesAsync(string zone, IReadOnlyList<QueueEntry> entries,
        CancellationToken cancellationToken) =>
        Checked(entries) ?? Create(zone, s => s.ReplaceAsync(entries), cancellationToken);

    private static QueueEntry[] Entries(IReadOnlyList<Track> tracks) => [.. tracks.Select(t => new QueueEntry(t))];

    // Exactly one of a track, a station or a group, and a group with parts.
    private static Task<Result<QueueState, QueueError>>? Checked(IReadOnlyList<QueueEntry> entries)
    {
        foreach (var entry in entries)
        {
            var kinds = (entry.Track is null ? 0 : 1) + (entry.Station is null ? 0 : 1) + (entry.Group is null ? 0 : 1);
            if (kinds != 1)
            {
                return Task.FromResult<Result<QueueState, QueueError>>(Result.Fail(QueueError.InvalidArgument,
                    "a queue entry is exactly one of a track, a station or a group"));
            }
            if (entry.Group is { Parts.Count: 0 } empty)
            {
                return Task.FromResult<Result<QueueState, QueueError>>(Result.Fail(QueueError.InvalidArgument,
                    $"group {empty.Title} has no parts"));
            }
        }
        return null;
    }

    public Task<Result<QueueState, QueueError>> RemoveAsync(string zone, QueueItemId item,
        CancellationToken cancellationToken) =>
        Existing(zone, s => s.RemoveAsync(item), cancellationToken);

    public Task<Result<QueueState, QueueError>> MoveAsync(string zone, QueueItemId item, int index,
        CancellationToken cancellationToken) =>
        Existing(zone, s => s.MoveAsync(item, index), cancellationToken);

    public Task<Result<QueueState, QueueError>> SetRepeatAsync(string zone, RepeatMode repeat,
        CancellationToken cancellationToken) =>
        Existing(zone, s => s.SetRepeatAsync(repeat), cancellationToken);

    public Task<Result<QueueState, QueueError>> SetShuffleAsync(string zone, bool shuffle,
        CancellationToken cancellationToken) =>
        Existing(zone, s => s.SetShuffleAsync(shuffle), cancellationToken);

    public Task<Result<QueueState, QueueError>> PlayAsync(string zone, QueueItemId? item,
        CancellationToken cancellationToken, bool fromStart = false) =>
        Existing(zone, s => s.PlayAsync(item, fromStart), cancellationToken);

    public Task<Result<QueueState, QueueError>> PauseAsync(string zone, CancellationToken cancellationToken) =>
        Existing(zone, s => s.PauseAsync(), cancellationToken);

    public Task<Result<QueueState, QueueError>> StopAsync(string zone, CancellationToken cancellationToken) =>
        Existing(zone, s => s.StopAsync(), cancellationToken);

    public Task<Result<QueueState, QueueError>> NextAsync(string zone, CancellationToken cancellationToken) =>
        Existing(zone, s => s.NextAsync(), cancellationToken);

    public Task<Result<QueueState, QueueError>> PreviousAsync(string zone, CancellationToken cancellationToken) =>
        Existing(zone, s => s.PreviousAsync(), cancellationToken);

    public Task<Result<QueueState, QueueError>> SeekAsync(string zone, QueueItemId item, TimeSpan offset,
        CancellationToken cancellationToken) =>
        Existing(zone, s => s.SeekAsync(item, offset), cancellationToken);

    public Task<Result<QueueState, QueueError>> SetClientsAsync(string zone, IReadOnlyList<string> clients,
        CancellationToken cancellationToken) =>
        Create(zone, s => s.SetClientsAsync(clients), cancellationToken);

    public async Task<Result<bool, QueueError>> SetVolumeAsync(string client, double? value, bool? muted,
        CancellationToken cancellationToken)
    {
        if (value is null && muted is null)
            return Result.Fail(QueueError.InvalidArgument, "a volume needs a value, a mute, or both");
        if (value is < 0.0 or > 1.0)
            return Result.Fail(QueueError.InvalidArgument,
                string.Create(CultureInfo.InvariantCulture, $"volume {value} is outside 0.0..1.0"));
        var set = await Link.SendAsync(new ClientSetVolume(client, value, muted), cancellationToken)
            .ConfigureAwait(false);
        if (set.IsSuccess)
            return set.Value.Changed;
        return Result.Fail(set.Failure.Code is CoreError.NotConnected or CoreError.ConnectionLost
            ? QueueError.Unavailable
            : QueueError.Failed, $"client.setVolume: {set.Failure}");
    }

    public async Task<Result<bool, QueueError>> DeleteAsync(string zone, CancellationToken cancellationToken)
    {
        if (!_sessions.TryGetValue(zone, out var session))
            return NoQueue(zone);
        var deleted = await session.RunAsync(session.DeleteAsync, cancellationToken).ConfigureAwait(false);
        if (!deleted.IsSuccess)
            return deleted.Failure;
        _sessions.TryRemove(zone, out _);
        QueueDeleted?.Invoke(this, zone);
        return true;
    }

    public async ValueTask HandleAsync(CoreEvent coreEvent, CancellationToken cancellationToken)
    {
        var zone = coreEvent switch
        {
            SourceNeeded e => e.Zone,
            SourceStarted e => e.Zone,
            SourceFailed e => e.Zone,
            ZoneEnded e => e.Zone,
            ZoneTransport e => e.Zone,
            _ => null,
        };
        if (zone is not null)
        {
            if (_sessions.TryGetValue(zone, out var session))
                await session.RunAsync(() => session.OnEventAsync(coreEvent), cancellationToken).ConfigureAwait(false);
            return;
        }

        // The core's own events.lost is followed by a resync and its
        // CoreSynchronised, and until then the model is behind. Events CoreLink
        // dropped on the way to its handlers left the model right.
        if (coreEvent is EventsLost { Local: true } or CoreSynchronised)
        {
            foreach (var session in _sessions.Values)
            {
                await session.RunAsync(
                    () => session.ReconcileAsync(Link.Model.Current.Zones.GetValueOrDefault(session.Zone)),
                    cancellationToken).ConfigureAwait(false);
            }
        }
    }

    private Task<Result<QueueState, QueueError>> Create(string zone,
        Func<ZoneSession, Task<Failure<QueueError>?>> operation, CancellationToken cancellationToken)
    {
        if (string.IsNullOrEmpty(zone))
            return Task.FromResult<Result<QueueState, QueueError>>(Result.Fail(QueueError.InvalidArgument, "a zone needs a name"));
        var session = _sessions.GetOrAdd(zone, z => new ZoneSession(z, this));
        return session.RunAsync(() => operation(session), cancellationToken);
    }

    private Task<Result<QueueState, QueueError>> Existing(string zone,
        Func<ZoneSession, Task<Failure<QueueError>?>> operation, CancellationToken cancellationToken) =>
        _sessions.TryGetValue(zone, out var session)
            ? session.RunAsync(() => operation(session), cancellationToken)
            : Task.FromResult<Result<QueueState, QueueError>>(NoQueue(zone));

    private static Failure<QueueError> NoQueue(string zone) =>
        Result.Fail(QueueError.NotFound, $"no queue for zone {zone}");
}
