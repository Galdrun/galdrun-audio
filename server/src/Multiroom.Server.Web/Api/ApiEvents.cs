using System.Text;
using System.Text.Json;
using System.Threading.Channels;
using Microsoft.AspNetCore.Http;
using Multiroom.Server.Abstractions;
using Multiroom.Server.Ai;
using Multiroom.Server.Library;
using Multiroom.Server.Playback;
using Multiroom.Server.Playback.Core;
using Multiroom.Server.Providers;
using Multiroom.Server.Providers.Covers;

namespace Multiroom.Server.Web.Api;

// What the event stream holds: subscribers, the events queued for them, and
// the events dropped because a subscriber did not read.
public sealed record ApiEventsStatus(int Subscribers, int Held, long Dropped);

// /api/v1/events: one ordered stream of the server's events per connection,
// as server-sent events. The core's zone and room events under their own
// names (`zone.sourceStarted`, `room.joined`, `room.input`, ...), then `core.synchronised`
// and `core.eventsLost` (the model is fresh, or was behind: read again),
// `queue.changed`, `queue.deleted`, `zone.volumeChanged`, `indexer.progress`,
// `library.folderChanged`, `zone.autoFillChanged` and `queue.autoFilled`
// (phase 26), `favorite.changed` and `zone.userChanged` (phase 36), `ai.request`
// (every call sent to an AI vendor, its prompt included) and, every
// second, `system.metrics` and `rooms.sync` (SystemMonitor).
//
// Every event has an id, counting from 1. A subscriber's queue is bounded;
// one that stops reading loses its oldest events, and the next thing it reads
// is `events.lost` with how many -- the core's own rule, kept on this side.
// Publishing never waits on a subscriber. The last events are kept, so a
// subscriber that reconnects with Last-Event-ID gets what it missed, or
// `events.lost` for what is no longer kept. A stream that stays quiet sends
// a `: ping` comment every 15 s: a connection that died while idle becomes
// visible that way, and a comment carries no id, so reconnects are unaffected.
public sealed class ApiEvents : ICoreEventSubscriber, IDisposable
{
    public const int SubscriberCapacity = 256;
    public const int ReplayCapacity = 256;

    private static readonly TimeSpan IndexerInterval = TimeSpan.FromSeconds(1);
    private static readonly TimeSpan HeartbeatInterval = TimeSpan.FromSeconds(15);

    // An SSE comment, ignored by every reader: it keeps the connection
    // visibly alive without touching `Last-Event-ID`.
    private static readonly byte[] Ping = ": ping\n\n"u8.ToArray();

    private readonly Lock _lock = new();
    private readonly Queue<ApiEvent> _recent = new();
    private readonly List<Subscriber> _subscribers = [];
    private readonly QueueEngine _queues;
    private readonly CoreLink _link;
    private readonly InputActions _inputs;
    private readonly LibraryIndexer _indexer;
    private readonly ProviderRegistry _providers;
    private readonly House _house;
    private readonly LikeStore _likes;
    private readonly PlaylistStore _playlists;
    private readonly ZoneUsers _users;
    private readonly AiProviderRegistry _ai;
    private readonly Dictionary<string, string?> _radioTitles = new(StringComparer.Ordinal);
    private readonly Timer _indexerWatch;
    private readonly Lock _indexerLock = new();
    private IndexerStatus? _lastIndexer;
    private long _lastId;
    private long _dropped;

    public ApiEvents(CoreLink link, QueueEngine queues, LibraryIndexer indexer, InputActions inputs, ProviderRegistry providers,
        House house, LikeStore likes, AutoFill autoFill, PlaylistStore playlists, ZoneUsers users, AiProviderRegistry ai,
        ArtworkService pictures)
    {
        _ai = ai;
        _house = house;
        _likes = likes;
        _playlists = playlists;
        _users = users;
        _queues = queues;
        _link = link;
        _inputs = inputs;
        _indexer = indexer;
        _providers = providers;
        link.Subscribe(this);
        queues.QueueChanged += OnQueueChanged;
        queues.QueueDeleted += OnQueueDeleted;
        inputs.ZoneLevelChanged += OnZoneLevelChanged;
        house.ZoneLevelChanged += OnZoneLevelChanged;
        likes.Added += OnLikeAdded;
        likes.Removed += OnLikeRemoved;
        likes.Changed += OnLikeAdded;
        playlists.FavoriteAdded += OnFavoriteAdded;
        playlists.FavoriteRemoved += OnFavoriteRemoved;
        users.Changed += OnZoneUserChanged;
        indexer.FolderChanged += OnFolderChanged;
        // Whoever changed it: the REST API or a tool (phase 26).
        autoFill.Changed += (_, zone) => Publish("zone.autoFillChanged", new { zone, autoFill = autoFill.View(zone) });
        // And as a fill starts and ends, so a page can show it working.
        autoFill.FillingChanged += (_, zone) => Publish("zone.autoFillChanged", new { zone, autoFill = autoFill.View(zone) });
        autoFill.Filled += (_, result) => Publish("queue.autoFilled", result);
        ai.Called += OnAiCalled;
        // Phase 40: a picture found for an item a list answered.
        pictures.Changed += (_, id) => Publish("artwork.changed", new { id });
        // The indexer counts per file; its progress is sampled, not forwarded.
        _indexerWatch = new Timer(_ => WatchIndexer(), null, TimeSpan.Zero, IndexerInterval);
    }

    public ApiEventsStatus Status
    {
        get
        {
            lock (_lock)
            {
                return new ApiEventsStatus(_subscribers.Count, _subscribers.Sum(s => s.Queue.Reader.Count),
                    Interlocked.Read(ref _dropped));
            }
        }
    }

    // Serialized once, here; each subscriber is handed the same bytes.
    public void Publish(string type, object data)
    {
        var json = JsonSerializer.SerializeToUtf8Bytes(data, data.GetType(), ApiJson.Options);
        lock (_lock)
        {
            var published = new ApiEvent(++_lastId, type, json);
            _recent.Enqueue(published);
            if (_recent.Count > ReplayCapacity)
                _recent.Dequeue();
            foreach (var subscriber in _subscribers)
                subscriber.Queue.Writer.TryWrite(published);
        }
    }

    // On CoreLink's dispatch task: only a TryWrite per subscriber.
    public ValueTask HandleAsync(CoreEvent coreEvent, CancellationToken cancellationToken)
    {
        var type = coreEvent switch
        {
            SourceNeeded => "zone.sourceNeeded",
            SourceStarted => "zone.sourceStarted",
            SourceFailed => "zone.sourceFailed",
            ZoneEnded => "zone.ended",
            ZoneTransport => "zone.transport",
            RoomConnected => "room.connected",
            RoomDisconnected => "room.disconnected",
            RoomActive => "room.active",
            RoomJoined => "room.joined",
            RoomLeft => "room.left",
            RoomNotReady => "room.notReady",
            RoomFellBehind => "room.fellBehind",
            RoomLeftBehind => "room.leftBehind",
            RoomUnstable => "room.unstable",
            RoomStable => "room.stable",
            RoomInput => "room.input",
            RoomCapabilities => "room.capabilities",
            RoomAudioDevices => "room.audioDevices",
            RoomBufferConfig => "room.bufferConfig",
            RoomVolumeControl => "room.volumeControl",
            RoomPowerState => "room.powerState",
            RoomName => "room.name",
            // RoomForgotten is not passed on: DELETE rooms/{client} publishes
            // room.forgotten itself, core or no core.
            EventsLost => "core.eventsLost",
            CoreSynchronised => "core.synchronised",
            _ => null,
        };
        if (type is not null)
            Publish(type, coreEvent);
        return ValueTask.CompletedTask;
    }

    // Streams to one connection until it closes. `lastEventId` replays the
    // kept events after it.
    public async Task StreamAsync(HttpContext context, long? lastEventId)
    {
        var subscriber = new Subscriber(this);
        lock (_lock)
        {
            if (lastEventId is { } last)
            {
                // An id past the newest is from before this server restarted:
                // what was missed cannot be counted.
                if (last > _lastId)
                {
                    subscriber.Unknown = true;
                    last = 0;
                }
                var replay = _recent.Where(e => e.Id > last).ToList();
                subscriber.Lost = _lastId - last - replay.Count;
                foreach (var missed in replay)
                    subscriber.Queue.Writer.TryWrite(missed);
            }
            _subscribers.Add(subscriber);
        }

        var response = context.Response;
        var aborted = context.RequestAborted;
        try
        {
            response.ContentType = "text/event-stream";
            response.Headers.CacheControl = "no-cache";
            await response.StartAsync(aborted).ConfigureAwait(false);
            await WriteLostAsync(subscriber, response.Body, aborted).ConfigureAwait(false);
            await response.Body.FlushAsync(aborted).ConfigureAwait(false);

            var reader = subscriber.Queue.Reader;
            while (true)
            {
                // The wait is bounded by the heartbeat: a stream that nothing
                // is written to says it is there, so a connection that died
                // while idle is noticed instead of lingering half-open.
                using var wait = CancellationTokenSource.CreateLinkedTokenSource(aborted);
                wait.CancelAfter(HeartbeatInterval);
                bool ready;
                try
                {
                    ready = await reader.WaitToReadAsync(wait.Token).ConfigureAwait(false);
                }
                catch (OperationCanceledException) when (!aborted.IsCancellationRequested)
                {
                    await response.Body.WriteAsync(Ping, aborted).ConfigureAwait(false);
                    await response.Body.FlushAsync(aborted).ConfigureAwait(false);
                    continue;
                }
                if (!ready)
                    break;
                while (reader.TryRead(out var next))
                {
                    await WriteLostAsync(subscriber, response.Body, aborted).ConfigureAwait(false);
                    await response.Body.WriteAsync(Frame(next.Id, next.Type, next.Json), aborted).ConfigureAwait(false);
                }
                await response.Body.FlushAsync(aborted).ConfigureAwait(false);
            }
        }
        catch (Exception e) when (e is OperationCanceledException or IOException)
        {
            // The subscriber went away.
        }
        finally
        {
            lock (_lock)
                _subscribers.Remove(subscriber);
        }
    }

    public void Dispose()
    {
        _queues.QueueChanged -= OnQueueChanged;
        _queues.QueueDeleted -= OnQueueDeleted;
        _inputs.ZoneLevelChanged -= OnZoneLevelChanged;
        _house.ZoneLevelChanged -= OnZoneLevelChanged;
        _likes.Added -= OnLikeAdded;
        _likes.Removed -= OnLikeRemoved;
        _likes.Changed -= OnLikeAdded;
        _playlists.FavoriteAdded -= OnFavoriteAdded;
        _playlists.FavoriteRemoved -= OnFavoriteRemoved;
        _users.Changed -= OnZoneUserChanged;
        _indexer.FolderChanged -= OnFolderChanged;
        _ai.Called -= OnAiCalled;
        _indexerWatch.Dispose();
    }

    private void OnQueueChanged(object? sender, Abstractions.QueueState state) => Publish("queue.changed", state);

    private void OnQueueDeleted(object? sender, string zone) => Publish("queue.deleted", new { zone });

    // A text longer than this is cut: a tool's result may be 32 000
    // characters, repeated in every later turn, and the last events are kept.
    private const int AiTextLimit = 8000;

    private void OnAiCalled(object? sender, AiCall call)
    {
        static string? Cut(string? text) => text is { Length: > AiTextLimit } ? text[..AiTextLimit] + "…" : text;
        static object Calls(IReadOnlyList<AiToolCall>? calls) =>
            calls?.Select(c => new { c.Id, c.Name, Arguments = Cut(c.Arguments.GetRawText()) }).ToArray() ?? [];
        var r = call.Request;
        Publish("ai.request", new
        {
            call.Provider,
            r.Model,
            Ok = call.Failure is null,
            Ms = (long)call.Elapsed.TotalMilliseconds,
            Error = call.Failure?.Code,
            Message = call.Failure?.Detail,
            Request = new
            {
                System = Cut(r.System),
                Messages = r.Messages.Select(m => new { m.Role, Text = Cut(m.Text), ToolCalls = Calls(m.ToolCalls), m.ToolCallId, m.IsError }),
                Tools = r.Tools?.Select(t => t.Name) ?? [],
                r.RequiredTool,
                r.MaxTokens,
                r.Reasoning,
                r.WebSearch,
            },
            Answer = call.Answer is { } a
                ? new { Text = Cut(a.Text), ToolCalls = Calls(a.ToolCalls), a.StopReason, a.Usage, a.Sources }
                : null,
        });
    }

    // The shape the zone PATCH publishes, for a knob that turned a zone.
    private void OnZoneLevelChanged(object? sender, string zone)
    {
        var level = _link.Desired.ZoneLevels.GetValueOrDefault(zone) ?? new DesiredZoneLevel();
        Publish("zone.volumeChanged", new { zone, volume = level.Volume, muted = level.Muted });
    }

    // like.changed: the new like, or its id once removed; either with its
    // user (phase 36). favorite.changed the same.
    private void OnLikeAdded(object? sender, LikedItem like) => Publish("like.changed", like);

    private void OnLikeRemoved(object? sender, LikedItem like) =>
        Publish("like.changed", new { id = like.Id, removed = true, user = like.User });

    private void OnFavoriteAdded(object? sender, Favorite favorite) => Publish("favorite.changed", favorite);

    private void OnFavoriteRemoved(object? sender, Favorite favorite) =>
        Publish("favorite.changed", new { id = favorite.Id, removed = true, user = favorite.User });

    private void OnZoneUserChanged(object? sender, string zone) =>
        Publish("zone.userChanged", new { zone, user = _users.Of(zone) });

    // The folder as GET library/folders answers it, or its id once removed.
    private void OnFolderChanged(object? sender, string id) =>
        Publish("library.folderChanged", (object?)_indexer.Folder(id) ?? new { id, removed = true });

    private void WatchIndexer()
    {
        lock (_indexerLock)
        {
            WatchRadioTitles();
            var status = _indexer.Status;
            if (status == _lastIndexer)
                return;
            _lastIndexer = status;
            Publish("indexer.progress", status);
        }
    }

    private void WatchRadioTitles()
    {
        var zones = _queues.Queues;
        foreach (var queue in zones)
        {
            var station = queue.Items.FirstOrDefault(i => i.Id == queue.Current)?.Station;
            string? title = null;
            if (station is not null && queue.Transport != Abstractions.QueueTransport.Stopped)
            {
                var provider = _providers.Get<IStationTitles>(station.ProviderId);
                if (provider.IsSuccess) title = provider.Value.CurrentTitle(station.ProviderItemId);
            }
            if (_radioTitles.GetValueOrDefault(queue.Zone) != title)
                Publish("zone.metadataChanged", new { zone = queue.Zone, streamTitle = title });
            _radioTitles[queue.Zone] = title;
        }
        foreach (var removed in _radioTitles.Keys.Where(zone => !zones.Any(q => q.Zone == zone)).ToArray())
            _radioTitles.Remove(removed);
    }

    // `events.lost` carries no id, so a reconnect's Last-Event-ID still names
    // the last event read. Its count is null when it cannot be known.
    private static async Task WriteLostAsync(Subscriber subscriber, Stream body, CancellationToken cancellationToken)
    {
        var lost = Interlocked.Exchange(ref subscriber.Lost, 0);
        var unknown = subscriber.Unknown;
        subscriber.Unknown = false;
        if (lost <= 0 && !unknown)
            return;
        var json = JsonSerializer.SerializeToUtf8Bytes(new { count = unknown ? (long?)null : lost }, ApiJson.Options);
        await body.WriteAsync(Frame(null, "events.lost", json), cancellationToken).ConfigureAwait(false);
    }

    private static byte[] Frame(long? id, string type, byte[] json)
    {
        var head = id is { } n ? $"id: {n}\nevent: {type}\ndata: " : $"event: {type}\ndata: ";
        return [.. Encoding.UTF8.GetBytes(head), .. json, (byte)'\n', (byte)'\n'];
    }

    private sealed record ApiEvent(long Id, string Type, byte[] Json);

    private sealed class Subscriber
    {
        public long Lost;
        public bool Unknown;

        public Subscriber(ApiEvents hub)
        {
            Queue = Channel.CreateBounded<ApiEvent>(
                new BoundedChannelOptions(SubscriberCapacity)
                {
                    FullMode = BoundedChannelFullMode.DropOldest,
                    SingleReader = true,
                },
                _ =>
                {
                    Interlocked.Increment(ref Lost);
                    Interlocked.Increment(ref hub._dropped);
                });
        }

        public Channel<ApiEvent> Queue { get; }
    }
}
