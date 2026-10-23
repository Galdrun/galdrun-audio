using System.Buffers;
using System.Collections.Concurrent;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Text.Encodings.Web;
using System.Text.Json;
using System.Text.Json.Serialization;
using System.Threading.Channels;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;
using Multiroom.Server.Abstractions;

namespace Multiroom.Server.Playback.Core;

public sealed record CoreLinkOptions(string Host, int Port);

// The client for the core's control API on 4713: one persistent TCP
// connection, one compact JSON object per line each way. Requests carry
// `command`, `params` and `id`; answers are matched by `id`; a line whose
// key is `event` is an event.
//
// On every connect: events.subscribe, one `status` into the model, then the
// desired state re-asserted -- only the commands the diff between the two
// needs. The core's events.lost gets the same resync. A dropped connection is
// retried with a backoff, for as long as the server runs.
//
// The reader never waits on anyone. Answers and events are applied to the
// model on the reader, in wire order; events then go to the handlers through
// a bounded queue on a task of their own. A handler that stops costs that
// queue its oldest events, counted, and an EventsLost -- never a stalled
// reader.
public sealed partial class CoreLink : BackgroundService
{
    // The core's own bound for a subscriber's queue.
    public const int EventQueueCapacity = 256;

    private static readonly TimeSpan FirstBackoff = TimeSpan.FromMilliseconds(250);
    private static readonly TimeSpan LastBackoff = TimeSpan.FromSeconds(5);

    // How far past the playhead a zone's volume change lands: time for the
    // command to reach every room before that instant plays.
    private const ulong ZoneChangeLeadNs = 250_000_000;

    internal static readonly JsonSerializerOptions Json = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull,
        RespectNullableAnnotations = true,
        RespectRequiredConstructorParameters = true,
        // Raw UTF-8 on the wire, not \u escapes: a title is read by the
        // core's own parser and printed in its log.
        Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping,
        Converters =
        {
            new AudioControlJsonConverter(),
            new JsonStringEnumConverter(JsonNamingPolicy.CamelCase, allowIntegerValues: false),
        },
    };

    private static readonly Dictionary<string, Type> EventTypes = new(StringComparer.Ordinal)
    {
        ["zone.sourceNeeded"] = typeof(SourceNeeded),
        ["zone.sourceStarted"] = typeof(SourceStarted),
        ["zone.sourceFailed"] = typeof(SourceFailed),
        ["zone.ended"] = typeof(ZoneEnded),
        ["zone.transport"] = typeof(ZoneTransport),
        ["room.connected"] = typeof(RoomConnected),
        ["room.disconnected"] = typeof(RoomDisconnected),
        ["room.joined"] = typeof(RoomJoined),
        ["room.left"] = typeof(RoomLeft),
        ["room.notReady"] = typeof(RoomNotReady),
        ["room.fellBehind"] = typeof(RoomFellBehind),
        ["room.leftBehind"] = typeof(RoomLeftBehind),
        ["room.unstable"] = typeof(RoomUnstable),
        ["room.stable"] = typeof(RoomStable),
        ["room.input"] = typeof(RoomInput),
        ["room.capabilities"] = typeof(RoomCapabilities),
        ["room.audioDevices"] = typeof(RoomAudioDevices),
        ["room.bufferConfig"] = typeof(RoomBufferConfig),
        ["room.volumeControl"] = typeof(RoomVolumeControl),
        ["room.powerState"] = typeof(RoomPowerState),
        ["room.name"] = typeof(RoomName),
        ["room.forgotten"] = typeof(RoomForgotten),
        ["events.lost"] = typeof(EventsLost),
    };

    private readonly CoreLinkOptions _options;
    private readonly CoreDiscovery? _discovery;
    private volatile string _host;
    private volatile int _port;
    private volatile CancellationTokenSource? _connectionStop;
    private readonly Channel<bool> _wake =
        Channel.CreateBounded<bool>(new BoundedChannelOptions(1) { FullMode = BoundedChannelFullMode.DropWrite });
    private readonly Lock _subscribeLock = new();
    private volatile ICoreEventSubscriber[] _handlers;
    private readonly ILogger _log;
    private readonly Channel<CoreEvent> _events;
    private readonly Channel<bool> _resyncRequests =
        Channel.CreateBounded<bool>(new BoundedChannelOptions(1) { FullMode = BoundedChannelFullMode.DropWrite });
    private readonly ConcurrentDictionary<long, Pending> _pending = new();
    private readonly SemaphoreSlim _writeLock = new(1, 1);
    private volatile Connection? _connection;
    private Task? _dispatcher;
    private long _nextId;
    private long _lostSinceDelivery;
    private long _droppedEvents;
    private long _connections;

    // With a discovery, a selected core is connected to wherever its beacon
    // last came from, and a new selection drops the connection to take effect.
    public CoreLink(CoreLinkOptions options, IEnumerable<ICoreEventSubscriber> handlers, ILogger<CoreLink> log,
        CoreDiscovery? discovery = null, TimeProvider? time = null)
    {
        _options = options;
        _discovery = discovery;
        _host = options.Host;
        _port = options.Port;
        if (discovery is not null)
            discovery.SelectionChanged += DropConnection;
        Desired = new DesiredState();
        Model = new CoreModel(time);
        _handlers = [.. handlers];
        _log = log;
        _events = Channel.CreateBounded<CoreEvent>(
            new BoundedChannelOptions(EventQueueCapacity)
            {
                FullMode = BoundedChannelFullMode.DropOldest,
                SingleReader = true,
                SingleWriter = false,
            },
            _ =>
            {
                Interlocked.Increment(ref _lostSinceDelivery);
                Interlocked.Increment(ref _droppedEvents);
            });
    }

    // Its clock dates what this server notes itself: an episode heard of, a
    // room's underruns (phase 34).
    public CoreModel Model { get; }

    public DesiredState Desired { get; }

    public bool IsConnected => _connection is not null;

    // Where the last connect went: the selected core's beacon address, or the
    // configured one.
    public string Host => _host;

    public int Port => _port;

    public string ConfiguredHost => _options.Host;

    public int ConfiguredPort => _options.Port;

    // The event queue's status: what it holds (bounded by
    // EventQueueCapacity) and how many events it has dropped since startup.
    public int QueuedEvents => _events.Reader.Count;

    public long DroppedEvents => Interlocked.Read(ref _droppedEvents);

    public long Connections => Interlocked.Read(ref _connections);

    // The core's version from the last `status` handshake; null before one.
    public string? CoreVersion { get; private set; }

    // A handler that sends through this link, and so cannot be one of the
    // constructor's: the container could build neither first. It receives
    // every event delivered after the call.
    public void Subscribe(ICoreEventSubscriber handler)
    {
        lock (_subscribeLock)
            _handlers = [.. _handlers, handler];
    }

    // Sends one command and waits for its answer. A desired-state command
    // (zones, memberships, room settings, volume) is recorded in Desired when
    // the core accepts it, or when there is no connection to send it on, so
    // the next connect asserts it. A core error comes back with the core's own
    // code and message.
    //
    // A room's volume and mute are recorded as the room's own and sent as
    // their effective values (DesiredState.Effective). A
    // membership change or a zone deletion also sends what it changes in the
    // rooms' effective settings: a correction switched on before the
    // membership that needs it, so both rooms are admitted on one line, and
    // everything else after it.
    public async Task<Result<TReply, CoreError>> SendAsync<TReply>(CoreCommand<TReply> command,
        CancellationToken cancellationToken)
    {
        var answer = command switch
        {
            ZoneSetClients or ZoneDelete => await SendMembershipAsync(command, cancellationToken).ConfigureAwait(false),
            ClientSetVolume c => await SendRecordedAsync(command, c with
            {
                Value = c.Value is null ? null : Effective(command).Volume,
                Muted = c.Muted is null ? null : Effective(command).Muted,
            }, cancellationToken).ConfigureAwait(false),
            _ => await SendRecordedAsync(command, command, cancellationToken).ConfigureAwait(false),
        };
        if (answer.Reply is null)
            return answer.Failure;
        return (TReply)answer.Reply;
    }

    // Phase 15: a zone's own volume and mute, recorded, then each member sent
    // its effective pair. A volume sends values only and a mute mutes only, so
    // unmuting restores each level without a jump. While the zone plays every
    // member gets one shared applyAtTimelineNs, so the rooms change together.
    // The first refusal is answered after every member was asked; a room the
    // core cannot reach now is put right by the next re-assert.
    public async Task<Result<bool, CoreError>> SetZoneLevelAsync(string zone, double? volume, bool? muted,
        CancellationToken cancellationToken)
    {
        var before = Desired.MembersOf(zone).ToDictionary(c => c, Desired.Effective, StringComparer.Ordinal);
        Desired.SetZoneLevel(zone, volume, muted);
        // A replay changes no member's pair and sends nothing.
        var changed = before.Keys.Where(c => Desired.Effective(c) is var now &&
            ((volume is not null && now.Volume != before[c].Volume) || (muted is not null && now.Muted != before[c].Muted)))
            .ToList();
        if (changed.Count == 0)
            return true;
        ulong? at = null;
        if (Model.Current.Zones.GetValueOrDefault(zone)?.Transport == TransportState.Playing &&
            (await SendRawAsync(new GetZones(), cancellationToken).ConfigureAwait(false)).Reply is ZonesReply zones &&
            zones.Zones.FirstOrDefault(z => z.Zone == zone)?.TimelineNs is { } playhead)
        {
            at = playhead + ZoneChangeLeadNs;
        }

        Failure<CoreError>? refused = null;
        foreach (var client in changed)
        {
            var effective = Desired.Effective(client);
            var command = new ClientSetVolume(client,
                volume is null || effective.Volume == before[client].Volume ? null : effective.Volume,
                muted is null || effective.Muted == before[client].Muted ? null : effective.Muted, at);
            var answer = await SendRawAsync(command, cancellationToken).ConfigureAwait(false);
            // A member not on the zone's line yet takes no position.
            if (answer.Reply is null && at is not null && answer.Failure.Code == CoreError.Unavailable)
                answer = await SendRawAsync(command with { ApplyAtTimelineNs = null }, cancellationToken).ConfigureAwait(false);
            if (answer.Reply is null)
                refused ??= answer.Failure;
        }
        if (refused is { } failure)
            return failure;
        return true;
    }

    // A room's level at full volume, recorded, then its effective volume sent
    // when that changed. With no core connected it is recorded all the same.
    public async Task<Result<bool, CoreError>> SetMaxVolumeDbAsync(string client, double maxVolumeDb,
        CancellationToken cancellationToken)
    {
        var before = Desired.Effective(client).Volume;
        Desired.SetMaxVolumeDb(client, maxVolumeDb);
        if (Desired.Effective(client).Volume is not { } now || now == before)
            return true;
        var answer = await SendRawAsync(new ClientSetVolume(client, now, null), cancellationToken).ConfigureAwait(false);
        if (answer.Reply is null)
            return answer.Failure;
        return true;
    }

    private EffectiveRoom Effective(CoreCommand command) =>
        Desired.With(command, Model.Current).Effective(command switch
        {
            ClientSetVolume c => c.Client,
            _ => throw new ArgumentException(command.Name, nameof(command)),
        });

    // Sends `wire`, and records `intent` when the core accepted it or could not
    // be asked.
    private async Task<Answer> SendRecordedAsync(CoreCommand intent, CoreCommand wire,
        CancellationToken cancellationToken)
    {
        var model = Model.Current;
        var answer = await SendRawAsync(wire, cancellationToken).ConfigureAwait(false);
        if (answer.Reply is not null || answer.Failure.Code is CoreError.ConnectionLost or CoreError.NotConnected)
            Desired.Record(intent, model);
        return answer;
    }

    private async Task<Answer> SendMembershipAsync(CoreCommand command, CancellationToken cancellationToken)
    {
        var after = Desired.With(command, Model.Current);
        var clients = Desired.Clients.Union(after.Clients, StringComparer.Ordinal).ToList();
        var before = clients.ToDictionary(c => c, Desired.Effective, StringComparer.Ordinal);
        // Grouped before the membership names another room, so a room is
        // corrected from the moment it shares its zone.
        foreach (var client in clients.Where(c => after.Effective(c).Grouped &&
                     GroupedToSend(c, true, before[c].Grouped)))
        {
            await SendRawAsync(new ClientSetGrouped(client, true), cancellationToken).ConfigureAwait(false);
        }

        var answer = await SendRecordedAsync(command, command, cancellationToken).ConfigureAwait(false);
        if (answer.Reply is null)
            return answer;

        foreach (var client in clients)
        {
            var now = Desired.Effective(client);
            var was = before[client];
            if (!now.Grouped && GroupedToSend(client, false, was.Grouped))
                await SendRawAsync(new ClientSetGrouped(client, false), cancellationToken).ConfigureAwait(false);
            // A value this server sent and no longer asks for goes back to
            // what nothing asked: full volume, not muted.
            var volume = now.Volume ?? (was.Volume is null ? null : 1.0);
            var muted = now.Muted ?? (was.Muted is null ? null : false);
            if (volume == was.Volume)
                volume = null;
            if (muted == was.Muted)
                muted = null;
            if (volume is not null || muted is not null)
                await SendRawAsync(new ClientSetVolume(client, volume, muted), cancellationToken).ConfigureAwait(false);
        }
        return answer;
    }

    // A zone stopped, paused or played out: a room left alone in it while it
    // played is no longer grouped (phase 16).
    private async Task ReleasePendingStopAsync(CoreEvent coreEvent, CancellationToken cancellationToken)
    {
        var zone = coreEvent switch
        {
            ZoneTransport { State: TransportState.Stopped or TransportState.Paused } e => e.Zone,
            ZoneEnded e => e.Zone,
            _ => null,
        };
        if (zone is null)
            return;
        foreach (var client in Desired.ReleasePendingStop(zone))
            await SendRawAsync(new ClientSetGrouped(client, false), cancellationToken).ConfigureAwait(false);
    }

    // A room's grouping goes to the core when it changed, or when the core
    // reported holding another value.
    private bool GroupedToSend(string client, bool grouped, bool was) =>
        grouped != was || Model.Current.Rooms.GetValueOrDefault(client)?.Settings is { } settings && settings.Grouped != grouped;

    // Sends one command as it is and records nothing.
    private async Task<Answer> SendRawAsync(CoreCommand command, CancellationToken cancellationToken)
    {
        var connection = _connection;
        if (connection is null)
        {
            return new Answer(null, Result.Fail(CoreError.NotConnected,
                $"{command.Name}: not connected to the core at {_host}:{_port}"));
        }

        var id = Interlocked.Increment(ref _nextId);
        var pending = new Pending(command, new TaskCompletionSource<Answer>(TaskCreationOptions.RunContinuationsAsynchronously));
        _pending[id] = pending;
        Answer answer;
        try
        {
            var line = Encode(command, id);
            await _writeLock.WaitAsync(cancellationToken).ConfigureAwait(false);
            try
            {
                // Not cancelled half-way: half a line would break the framing
                // for every request after it.
                await connection.Stream.WriteAsync(line, CancellationToken.None).ConfigureAwait(false);
            }
            finally
            {
                _writeLock.Release();
            }
            answer = await pending.Completion.Task.WaitAsync(cancellationToken).ConfigureAwait(false);
        }
        catch (Exception e) when (e is IOException or ObjectDisposedException)
        {
            answer = new Answer(null, Result.Fail(CoreError.ConnectionLost, $"{command.Name}: {e.Message}"));
        }
        finally
        {
            _pending.TryRemove(id, out _);
        }
        return answer;
    }

    protected override async Task ExecuteAsync(CancellationToken stoppingToken)
    {
        _dispatcher = DispatchAsync(stoppingToken);

        var backoff = FirstBackoff;
        var reconnect = false;
        var unreachableLogged = false;
        string? waitingFor = null;
        string? warnedHost = null;
        try
        {
            while (!stoppingToken.IsCancellationRequested)
            {
                // Resolved on every attempt: a selection, or the selected
                // core's address, may have changed since the last one.
                if (_discovery?.SelectedCoreId is { } selected)
                {
                    if (_discovery.Find(selected) is not { } core)
                    {
                        // Once per core: waiting for a beacon is ordinary at
                        // startup, and the connect below is never tried
                        // against a core that is not the chosen one.
                        if (waitingFor != selected)
                            LogWaitingForCore(_log, selected);
                        waitingFor = selected;
                        await PauseAsync(LastBackoff, stoppingToken).ConfigureAwait(false);
                        continue;
                    }
                    (_host, _port) = (core.Host, core.ControlPort);
                }
                else
                {
                    (_host, _port) = (_options.Host, _options.Port);
                }
                waitingFor = null;
                if (!IsLoopback(_host) && warnedHost != _host)
                    LogNotLoopback(_log, _host);
                warnedHost = _host;

                var client = new TcpClient { NoDelay = true };
                try
                {
                    await client.ConnectAsync(_host, _port, stoppingToken).ConfigureAwait(false);
                }
                catch (SocketException e)
                {
                    client.Dispose();
                    // Once at warn: a core that is not up yet is ordinary.
                    if (!unreachableLogged)
                        LogUnreachable(_log, _host, _port, e.Message);
                    else
                        LogStillUnreachable(_log, _host, _port, e.Message);
                    unreachableLogged = true;
                    await PauseAsync(backoff, stoppingToken).ConfigureAwait(false);
                    backoff = TimeSpan.FromTicks(Math.Min(backoff.Ticks * 2, LastBackoff.Ticks));
                    continue;
                }
                catch (OperationCanceledException)
                {
                    client.Dispose();
                    throw;
                }

                unreachableLogged = false;
                backoff = FirstBackoff;
                await RunConnectionAsync(client, reconnect, stoppingToken).ConfigureAwait(false);
                reconnect = true;
                if (!stoppingToken.IsCancellationRequested)
                {
                    LogDisconnected(_log, _host, _port);
                    await PauseAsync(backoff, stoppingToken).ConfigureAwait(false);
                }
            }
        }
        catch (OperationCanceledException) when (stoppingToken.IsCancellationRequested)
        {
        }
    }

    private async Task RunConnectionAsync(TcpClient client, bool reconnect, CancellationToken stoppingToken)
    {
        using var connectionStop = CancellationTokenSource.CreateLinkedTokenSource(stoppingToken);
        var connection = new Connection(client, client.GetStream());
        while (_resyncRequests.Reader.TryRead(out _))
        {
            // A resync asked on the connection before this one: this connect
            // reads status anyway.
        }

        var reader = ReadAsync(connection, connectionStop.Token);
        _connection = connection;
        _connectionStop = connectionStop;
        Interlocked.Increment(ref _connections);
        LogConnected(_log, _host, _port);
        try
        {
            if (!await SynchroniseAsync(subscribe: true, reconnect, connectionStop.Token).ConfigureAwait(false))
                return;
            while (true)
            {
                var resync = _resyncRequests.Reader.ReadAsync(connectionStop.Token).AsTask();
                if (await Task.WhenAny(reader, resync).ConfigureAwait(false) == reader)
                    return;
                LogResync(_log);
                if (!await SynchroniseAsync(subscribe: false, reconnected: false, connectionStop.Token)
                        .ConfigureAwait(false))
                {
                    return;
                }
            }
        }
        catch (OperationCanceledException) when (connectionStop.IsCancellationRequested)
        {
        }
        finally
        {
            _connection = null;
            _connectionStop = null;
            await connectionStop.CancelAsync().ConfigureAwait(false);
            client.Dispose();
            await reader.ConfigureAwait(false);
            foreach (var (id, pending) in _pending)
            {
                if (_pending.TryRemove(id, out _))
                {
                    pending.Completion.TrySetResult(new Answer(null,
                        Result.Fail(CoreError.ConnectionLost, $"{pending.Command.Name}: the connection closed")));
                }
            }
        }
    }

    // Status into the model, then the desired state's difference from it.
    // False when the connection cannot be used and must be dropped.
    private async Task<bool> SynchroniseAsync(bool subscribe, bool reconnected, CancellationToken cancellationToken)
    {
        if (subscribe)
        {
            var subscribed = await SendAsync(new EventsSubscribe(), cancellationToken).ConfigureAwait(false);
            if (!subscribed.IsSuccess)
            {
                LogSyncFailed(_log, subscribed.Failure.ToString());
                return false;
            }
        }

        var status = await SendAsync(new GetStatus(), cancellationToken).ConfigureAwait(false);
        if (!status.IsSuccess)
        {
            LogSyncFailed(_log, status.Failure.ToString());
            return false;
        }

        var commands = Desired.Diff(Model.Current);
        foreach (var command in commands)
        {
            var answer = await SendRawAsync(command, cancellationToken).ConfigureAwait(false);
            if (answer.Reply is not null)
                continue;
            LogReassertFailed(_log, command.Name, answer.Failure.ToString());
            if (answer.Failure.Code is CoreError.ConnectionLost)
                return false;
        }

        var model = Model.Current;
        var server = status.Value.Server;
        CoreVersion = server.Version;
        var uptime = TimeSpan.FromMilliseconds(server.UptimeMs);
        LogSynchronised(_log, server.Version, uptime, model.Zones.Count, model.Rooms.Count, commands.Count);
        _events.Writer.TryWrite(new CoreSynchronised(reconnected));
        return true;
    }

    private async Task ReadAsync(Connection connection, CancellationToken cancellationToken)
    {
        using var reader = new StreamReader(connection.Stream, Encoding.UTF8, detectEncodingFromByteOrderMarks: false,
            leaveOpen: true);
        try
        {
            while (await reader.ReadLineAsync(cancellationToken).ConfigureAwait(false) is { } line)
                OnLine(line);
        }
        catch (Exception e) when (e is IOException or ObjectDisposedException or OperationCanceledException)
        {
            // The connection is over; RunConnectionAsync takes it from here.
        }
    }

    private void OnLine(string line)
    {
        JsonDocument document;
        try
        {
            document = JsonDocument.Parse(line);
        }
        catch (JsonException e)
        {
            LogUnreadableLine(_log, e.Message);
            return;
        }

        using (document)
        {
            var root = document.RootElement;
            if (root.ValueKind != JsonValueKind.Object)
                LogUnreadableLine(_log, "not a JSON object");
            else if (root.TryGetProperty("event", out var name))
                OnEvent(name.GetString() ?? "", root);
            else
                OnAnswer(root);
        }
    }

    private void OnEvent(string name, JsonElement root)
    {
        if (!EventTypes.TryGetValue(name, out var type))
        {
            // The core may grow events before this server knows them.
            LogUnknownEvent(_log, name);
            return;
        }

        CoreEvent coreEvent;
        try
        {
            coreEvent = (CoreEvent)root.Deserialize(type, Json)!;
        }
        catch (JsonException e)
        {
            LogUnreadableLine(_log, $"{name}: {e.Message}");
            return;
        }

        LogEvent(_log, name);
        Model.Apply(coreEvent);
        RememberRooms();
        if (coreEvent is EventsLost)
            _resyncRequests.Writer.TryWrite(true);
        _events.Writer.TryWrite(coreEvent);
    }

    // A room's name outlives its connection and the core (phase 39): what the
    // core said last is kept in the desired state, and so in playback.json.
    // So is a deep sleep's wake address, until the room is back.
    private void RememberRooms()
    {
        foreach (var room in Model.Current.Rooms.Values)
        {
            if (room.Name is { } name)
                Desired.RememberName(room.Client, name);
            if (room is { PowerState: "sleep", WakeOnLan: { } mac })
                Desired.RememberWake(room.Client, mac);
            else if (room.Connected)
                Desired.RememberWake(room.Client, null);
        }
    }

    private void OnAnswer(JsonElement root)
    {
        if (!root.TryGetProperty("id", out var idElement) || !idElement.TryGetInt64(out var id) ||
            !_pending.TryGetValue(id, out var pending))
        {
            LogUnmatchedAnswer(_log, root.GetRawText());
            return;
        }

        var command = pending.Command;
        Answer answer;
        if (root.TryGetProperty("ok", out var ok) && ok.ValueKind == JsonValueKind.True)
        {
            try
            {
                var reply = root.Deserialize(command.ReplyType, Json)!;
                Model.Apply(command, reply);
                RememberRooms();
                answer = new Answer(reply, default);
            }
            catch (JsonException e)
            {
                answer = new Answer(null, Result.Fail(CoreError.MalformedAnswer, $"{command.Name}: {e.Message}"));
            }
        }
        else
        {
            var code = root.TryGetProperty("error", out var e) && e.ValueKind == JsonValueKind.String
                ? e.GetString()!
                : "";
            var message = root.TryGetProperty("message", out var m) && m.ValueKind == JsonValueKind.String
                ? m.GetString()!
                : "";
            answer = new Answer(null, ParseCoreCode(code) is { } parsed
                ? Result.Fail(parsed, message)
                : Result.Fail(CoreError.MalformedAnswer, $"{command.Name}: error code \"{code}\": {message}"));
        }
        pending.Completion.TrySetResult(answer);
    }

    // The core's seven codes, spelled as it spells them.
    private static CoreError? ParseCoreCode(string code)
    {
        if (code.Length == 0)
            return null;
        var name = char.ToUpperInvariant(code[0]) + code[1..];
        return Enum.TryParse<CoreError>(name, ignoreCase: false, out var parsed) &&
               parsed <= CoreError.Unavailable && char.IsLower(code[0])
            ? parsed
            : null;
    }

    private async Task DispatchAsync(CancellationToken cancellationToken)
    {
        try
        {
            await foreach (var coreEvent in _events.Reader.ReadAllAsync(cancellationToken).ConfigureAwait(false))
            {
                var lost = Interlocked.Exchange(ref _lostSinceDelivery, 0);
                if (lost > 0)
                {
                    LogEventsDropped(_log, lost);
                    await DeliverAsync(new EventsLost((ulong)lost, Local: true), cancellationToken).ConfigureAwait(false);
                }
                await ReleasePendingStopAsync(coreEvent, cancellationToken).ConfigureAwait(false);
                await DeliverAsync(coreEvent, cancellationToken).ConfigureAwait(false);
            }
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
        }
    }

    private async Task DeliverAsync(CoreEvent coreEvent, CancellationToken cancellationToken)
    {
        foreach (var handler in _handlers)
        {
            try
            {
                await handler.HandleAsync(coreEvent, cancellationToken).ConfigureAwait(false);
            }
            catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
            {
                throw;
            }
#pragma warning disable CA1031 // A handler is another module: its failure is logged at this edge and the pump goes on.
            catch (Exception e)
#pragma warning restore CA1031
            {
                LogHandlerFailed(_log, e, handler.GetType().Name, coreEvent.GetType().Name);
            }
        }
    }

    private static ReadOnlyMemory<byte> Encode(CoreCommand command, long id)
    {
        var buffer = new ArrayBufferWriter<byte>();
        using (var writer = new Utf8JsonWriter(buffer, new JsonWriterOptions { Encoder = Json.Encoder }))
        {
            writer.WriteStartObject();
            writer.WriteString("command", command.Name);
            writer.WritePropertyName("params");
            JsonSerializer.Serialize(writer, command, command.GetType(), Json);
            writer.WriteNumber("id", id);
            writer.WriteEndObject();
        }
        buffer.Write("\n"u8);
        return buffer.WrittenMemory;
    }

    // A new core selection takes effect on the next connect. A connection that
    // is ending by itself at the same moment has nothing left to cancel.
    private void DropConnection()
    {
        try
        {
            _connectionStop?.Cancel();
        }
        catch (ObjectDisposedException)
        {
        }
        _wake.Writer.TryWrite(true);
    }

    // A backoff that a new core selection cuts short, so the switch is not
    // held up by the retries against the core it replaces.
    private async Task PauseAsync(TimeSpan delay, CancellationToken stoppingToken)
    {
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(stoppingToken);
        timeout.CancelAfter(delay);
        try
        {
            await _wake.Reader.ReadAsync(timeout.Token).ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (!stoppingToken.IsCancellationRequested)
        {
        }
    }

    private static bool IsLoopback(string host) =>
        host.Equals("localhost", StringComparison.OrdinalIgnoreCase) ||
        (IPAddress.TryParse(host, out var address) && IPAddress.IsLoopback(address));

    public override void Dispose()
    {
        _writeLock.Dispose();
        base.Dispose();
    }

    private sealed record Connection(TcpClient Client, NetworkStream Stream);

    // Reply is null for a failure.
    private readonly record struct Answer(object? Reply, Failure<CoreError> Failure);

    private sealed record Pending(CoreCommand Command, TaskCompletionSource<Answer> Completion);

    [LoggerMessage(Level = LogLevel.Warning,
        Message = "the core at {Host} is not loopback: the core's control API has no authentication, and widening its bind address exposes it to the network")]
    private static partial void LogNotLoopback(ILogger log, string host);

    [LoggerMessage(Level = LogLevel.Warning, Message = "the core at {Host}:{Port} is not reachable ({Reason}); retrying")]
    private static partial void LogUnreachable(ILogger log, string host, int port, string reason);

    [LoggerMessage(Level = LogLevel.Debug, Message = "the core at {Host}:{Port} is still not reachable ({Reason})")]
    private static partial void LogStillUnreachable(ILogger log, string host, int port, string reason);

    [LoggerMessage(Level = LogLevel.Information, Message = "waiting for the selected core {CoreId} to announce itself")]
    private static partial void LogWaitingForCore(ILogger log, string coreId);

    [LoggerMessage(Level = LogLevel.Information, Message = "connected to the core at {Host}:{Port}")]
    private static partial void LogConnected(ILogger log, string host, int port);

    [LoggerMessage(Level = LogLevel.Warning, Message = "lost the connection to the core at {Host}:{Port}; reconnecting")]
    private static partial void LogDisconnected(ILogger log, string host, int port);

    [LoggerMessage(Level = LogLevel.Information,
        Message = "synchronised with the core {Version}, up {Uptime}: {Zones} zone(s), {Rooms} room(s), {Commands} command(s) re-asserted")]
    private static partial void LogSynchronised(ILogger log, string version, TimeSpan uptime, int zones, int rooms,
        int commands);

    [LoggerMessage(Level = LogLevel.Warning, Message = "the core lost events for this server; reading status again")]
    private static partial void LogResync(ILogger log);

    [LoggerMessage(Level = LogLevel.Error, Message = "could not synchronise with the core: {Failure}; reconnecting")]
    private static partial void LogSyncFailed(ILogger log, string failure);

    [LoggerMessage(Level = LogLevel.Warning, Message = "re-asserting {Command} failed: {Failure}")]
    private static partial void LogReassertFailed(ILogger log, string command, string failure);

    [LoggerMessage(Level = LogLevel.Warning, Message = "unreadable line from the core: {Reason}")]
    private static partial void LogUnreadableLine(ILogger log, string reason);

    [LoggerMessage(Level = LogLevel.Warning, Message = "an answer from the core matches no request: {Line}")]
    private static partial void LogUnmatchedAnswer(ILogger log, string line);

    [LoggerMessage(Level = LogLevel.Debug, Message = "unknown core event {Name} ignored")]
    private static partial void LogUnknownEvent(ILogger log, string name);

    [LoggerMessage(Level = LogLevel.Debug, Message = "core event {Name}")]
    private static partial void LogEvent(ILogger log, string name);

    [LoggerMessage(Level = LogLevel.Warning, Message = "{Count} core event(s) dropped: a handler is not keeping up")]
    private static partial void LogEventsDropped(ILogger log, long count);

    [LoggerMessage(Level = LogLevel.Error, Message = "core event handler {Handler} failed on {Event}")]
    private static partial void LogHandlerFailed(ILogger log, Exception exception, string handler, string @event);
}
