using System.Collections.Concurrent;
using System.Threading.Channels;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;
using Multiroom.Server.Abstractions;
using Multiroom.Server.Library;

namespace Multiroom.Server.Playback;

// Writes the play history (phase 26): a track is played when it becomes
// audible, which is a now-playing with a source this zone has not announced
// before. Stations are not recorded; a group once per queue item. A track resumed after a restart of the
// core or of this server is a new source, and counts again.
//
// QueueChanged is raised under the zone's gate, so the handler only queues
// the play; the database is written on this service's own task.
public sealed partial class PlayHistoryRecorder : BackgroundService
{
    // Far more than a house plays between two writes; a burst beyond it drops
    // its oldest plays rather than wait.
    private const int Capacity = 256;

    private readonly PlayHistoryStore _store;
    private readonly ILogger _log;
    private readonly TimeProvider _time;
    // The zone's last announced source, and its queue item.
    private readonly ConcurrentDictionary<string, (string Source, QueueItemId Item)> _announced =
        new(StringComparer.Ordinal);
    private readonly ZoneUsers? _users;
    private readonly Channel<(string Zone, Track Track, DateTimeOffset At, string User)> _plays =
        Channel.CreateBounded<(string, Track, DateTimeOffset, string)>(
            new BoundedChannelOptions(Capacity) { FullMode = BoundedChannelFullMode.DropOldest, SingleReader = true });

    public PlayHistoryRecorder(QueueEngine queues, PlayHistoryStore store, ILogger<PlayHistoryRecorder> log,
        TimeProvider? time = null, ZoneUsers? users = null)
    {
        _store = store;
        _log = log;
        _time = time ?? TimeProvider.System;
        _users = users;
        queues.QueueChanged += (_, state) => Note(state);
        queues.QueueDeleted += (sender, zone) => _announced.TryRemove(zone, out _);
    }

    private void Note(QueueState state)
    {
        if (state.NowPlaying is not { } playing)
            return;
        var known = _announced.TryGetValue(state.Zone, out var last);
        if (known && last.Source == playing.SourceId)
            return;
        _announced[state.Zone] = (playing.SourceId, playing.Item);
        var item = state.Items.FirstOrDefault(i => i.Id == playing.Item);
        // A group is one play (phase 31): its first part heard in that queue
        // item, recorded as that part; its next parts are no new play.
        if (item?.Group is { } group)
        {
            if (!(known && last.Item == playing.Item) && playing.Part is { } part && part < group.Parts.Count)
                _plays.Writer.TryWrite((state.Zone, group.Parts[part].Track, _time.GetUtcNow(), UserOf(state.Zone)));
        }
        else if (item?.Track is { } track)
        {
            _plays.Writer.TryWrite((state.Zone, track, _time.GetUtcNow(), UserOf(state.Zone)));
        }
    }

    // Taken as the track becomes audible (phase 36), not when it is written.
    private string UserOf(string zone) => _users?.Of(zone) ?? UserStore.Household;

    protected override async Task ExecuteAsync(CancellationToken stoppingToken)
    {
        try
        {
            await foreach (var (zone, track, at, user) in _plays.Reader.ReadAllAsync(stoppingToken).ConfigureAwait(false))
            {
                var recorded = _store.Record(zone, track, at, user);
                if (!recorded.IsSuccess)
                    LogUnrecorded(_log, zone, recorded.Failure.Detail);
            }
        }
        catch (OperationCanceledException) when (stoppingToken.IsCancellationRequested)
        {
            // Stopping.
        }
    }

    [LoggerMessage(Level = LogLevel.Warning, Message = "zone {Zone}: a play was not recorded in the history: {Reason}")]
    private static partial void LogUnrecorded(ILogger log, string zone, string reason);
}
