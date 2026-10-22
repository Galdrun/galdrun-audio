using System.Threading.Channels;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;
using Multiroom.Server.Abstractions;
using Multiroom.Server.Playback.Core;

namespace Multiroom.Server.Playback;

// What a button or encoder on a room does (MATP section 57): the core
// announces room.input, and this runs it on the path the API's own requests
// take -- the zone's queue for the transport, the desired state for volume --
// so the room, the UI and a restart all see one truth. Nothing is applied at
// the room; a volume change reaches it as the ordinary SET_VOLUME.
//
// A volume step is computed from the desired volume, never the measured one,
// and the inputs are run one at a time in the order they came, so a knob
// turned quickly cannot race itself.
//
// Run on its own task, not CoreLink's dispatch task: a queue operation can
// wait for the core, and the core's answers must never queue behind it.
public sealed partial class InputActions : BackgroundService, ICoreEventSubscriber
{
    // A hand does not press faster than this drains; a burst beyond it is
    // noise, and the oldest of it is what goes.
    private const int Capacity = 64;

    private readonly CoreLink _link;
    private readonly IQueueEngine _queues;
    private readonly ILogger _log;
    private readonly Channel<RoomInput> _inputs = Channel.CreateBounded<RoomInput>(
        new BoundedChannelOptions(Capacity) { FullMode = BoundedChannelFullMode.DropOldest, SingleReader = true });

    public InputActions(CoreLink link, IQueueEngine queues, ILogger<InputActions> log)
    {
        _link = link;
        _queues = queues;
        _log = log;
        link.Subscribe(this);
    }

    // A zone's own volume or mute was changed by an input; the argument is the
    // zone. The API's event stream says so the way it does for its own PATCH.
    public event EventHandler<string>? ZoneLevelChanged;

    public ValueTask HandleAsync(CoreEvent coreEvent, CancellationToken cancellationToken)
    {
        if (coreEvent is RoomInput input)
            _inputs.Writer.TryWrite(input);
        return ValueTask.CompletedTask;
    }

    // Runs one input. Public so a test can drive it without the loop.
    public async Task RunAsync(RoomInput input, CancellationToken cancellationToken)
    {
        LogInput(_log, input.Client, input.InputId, input.Gesture, input.Action, input.Count);
        var zone = input.Zone ?? _link.Desired.ZoneOf(input.Client);
        var scope = input.Scope ?? VolumeScope.Room;
        if (zone is null && (IsTransport(input.Action) || scope == VolumeScope.Zone))
        {
            LogNoZone(_log, input.Client, input.Action);
            return;
        }

        string? failure = input.Action switch
        {
            InputAction.Play => Failed(await _queues.PlayAsync(zone!, null, cancellationToken).ConfigureAwait(false)),
            InputAction.Pause => Failed(await _queues.PauseAsync(zone!, cancellationToken).ConfigureAwait(false)),
            InputAction.TogglePause => Failed(IsRunning(zone!)
                ? await _queues.PauseAsync(zone!, cancellationToken).ConfigureAwait(false)
                : await _queues.PlayAsync(zone!, null, cancellationToken).ConfigureAwait(false)),
            InputAction.Stop => Failed(await _queues.StopAsync(zone!, cancellationToken).ConfigureAwait(false)),
            InputAction.Next => Failed(await _queues.NextAsync(zone!, cancellationToken).ConfigureAwait(false)),
            InputAction.Previous => Failed(await _queues.PreviousAsync(zone!, cancellationToken).ConfigureAwait(false)),
            InputAction.VolumeUp or InputAction.VolumeDown or InputAction.SetVolume =>
                await SetVolumeAsync(input, zone, scope, cancellationToken).ConfigureAwait(false),
            InputAction.ToggleMute => await ToggleMuteAsync(input.Client, zone, scope, cancellationToken)
                .ConfigureAwait(false),
            _ => null,
        };
        if (failure is not null)
            LogRefused(_log, input.Client, input.Action, failure);
    }

    protected override async Task ExecuteAsync(CancellationToken stoppingToken)
    {
        try
        {
            await foreach (var input in _inputs.Reader.ReadAllAsync(stoppingToken).ConfigureAwait(false))
                await RunAsync(input, stoppingToken).ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (stoppingToken.IsCancellationRequested)
        {
            // Stopping.
        }
    }

    private static bool IsTransport(InputAction action) =>
        action is InputAction.Play or InputAction.Pause or InputAction.TogglePause or InputAction.Stop
            or InputAction.Next or InputAction.Previous;

    private bool IsRunning(string zone) =>
        _queues.GetQueue(zone) is { IsSuccess: true } queue &&
        queue.Value.Transport is QueueTransport.Playing or QueueTransport.Starting;

    private async Task<string?> SetVolumeAsync(RoomInput input, string? zone, VolumeScope scope,
        CancellationToken cancellationToken)
    {
        var current = scope == VolumeScope.Zone
            ? (_link.Desired.ZoneLevels.GetValueOrDefault(zone!) ?? new DesiredZoneLevel()).Volume
            : _link.Desired.Rooms.GetValueOrDefault(input.Client)?.Volume ?? 1.0;
        var delta = (input.Step ?? 0.0) * input.Count;
        var target = Math.Clamp(input.Action switch
        {
            InputAction.VolumeUp => current + delta,
            InputAction.VolumeDown => current - delta,
            _ => input.Value ?? current,
        }, 0.0, 1.0);
        // Rounded so a hundred 0.01 steps land on 1.0 and not on 0.9999999.
        target = Math.Round(target, 4);
        if (target == current)
            return null;

        if (scope == VolumeScope.Room)
            return Failed(await _queues.SetVolumeAsync(input.Client, target, null, cancellationToken).ConfigureAwait(false));
        return await SetZoneLevelAsync(zone!, target, null, cancellationToken).ConfigureAwait(false);
    }

    private async Task<string?> ToggleMuteAsync(string client, string? zone, VolumeScope scope,
        CancellationToken cancellationToken)
    {
        if (scope == VolumeScope.Zone)
        {
            var muted = (_link.Desired.ZoneLevels.GetValueOrDefault(zone!) ?? new DesiredZoneLevel()).Muted;
            return await SetZoneLevelAsync(zone!, null, !muted, cancellationToken).ConfigureAwait(false);
        }
        var roomMuted = _link.Desired.Rooms.GetValueOrDefault(client)?.Muted ?? false;
        return Failed(await _queues.SetVolumeAsync(client, null, !roomMuted, cancellationToken).ConfigureAwait(false));
    }

    private async Task<string?> SetZoneLevelAsync(string zone, double? volume, bool? muted,
        CancellationToken cancellationToken)
    {
        var set = await _link.SetZoneLevelAsync(zone, volume, muted, cancellationToken).ConfigureAwait(false);
        // Recorded either way: a core that is away is told when it is back.
        ZoneLevelChanged?.Invoke(this, zone);
        return set.IsSuccess ? null : $"{set.Failure.Code}: {set.Failure.Detail}";
    }

    private static string? Failed<T>(Result<T, QueueError> result) =>
        result.IsSuccess ? null : $"{result.Failure.Code}: {result.Failure.Detail}";

    [LoggerMessage(Level = LogLevel.Debug, Message = "{Client}: input {InputId} {Gesture} x{Count}: {Action}")]
    private static partial void LogInput(ILogger log, string client, string inputId, string gesture,
        InputAction action, uint count);

    [LoggerMessage(Level = LogLevel.Information, Message = "{Client}: {Action} ignored, the room is in no zone")]
    private static partial void LogNoZone(ILogger log, string client, InputAction action);

    [LoggerMessage(Level = LogLevel.Information, Message = "{Client}: {Action} from an input was refused: {Reason}")]
    private static partial void LogRefused(ILogger log, string client, InputAction action, string reason);
}
