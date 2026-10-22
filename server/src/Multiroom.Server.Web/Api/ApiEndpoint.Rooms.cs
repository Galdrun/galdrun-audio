using System.Globalization;
using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Http;
using Microsoft.Extensions.Logging;
using Multiroom.Server.Abstractions;
using Multiroom.Server.Library;
using Multiroom.Server.Playback;
using Multiroom.Server.Playback.Core;

namespace Multiroom.Server.Web.Api;

public sealed partial class ApiEndpoint
{
    private void MapRooms(WebApplication app)
    {
        app.MapGet("/api/v1/rooms", (Delegate)(async (HttpContext c) =>
            Json(await house.RoomsAsync(c.RequestAborted).ConfigureAwait(false))));
        app.MapGet("/api/v1/rooms/{client}", async (string client, HttpContext c) =>
            Answer(await house.RoomAsync(client, c.RequestAborted).ConfigureAwait(false)));
        app.MapMethods("/api/v1/rooms/{client}", [HttpMethods.Patch], PatchRoomAsync);
        app.MapGet("/api/v1/rooms/{client}/network", async (string client, HttpContext c) =>
            Answer(await link.SendAsync(new ClientGetNetwork(client), c.RequestAborted).ConfigureAwait(false)));
        app.MapMethods("/api/v1/rooms/{client}/network", [HttpMethods.Patch],
            (Delegate)((string client, HttpContext c) =>
                WithBody<EthernetSpeedRequest, ClientNetworkSettings, CoreError>(c,
                    b => link.SendAsync(new ClientSetEthernetSpeed(client, b.SpeedMbps), c.RequestAborted))));
        app.MapGet("/api/v1/rooms/{client}/leds", async (string client, HttpContext c) =>
            Answer(await link.SendAsync(new ClientGetLeds(client), c.RequestAborted).ConfigureAwait(false)));
        app.MapMethods("/api/v1/rooms/{client}/leds", [HttpMethods.Patch],
            (Delegate)((string client, HttpContext c) =>
                WithBody<LedModeRequest, ClientLedSettings, CoreError>(c,
                    b => link.SendAsync(new ClientSetLeds(client, b.Off), c.RequestAborted))));
        // The room's idle delays in seconds, and standbyOn, standbyOff,
        // toggleStandby or deepSleep sent to it. Reached through the client's
        // beacon like the LEDs, so not while it is asleep: /wake is for that.
        app.MapGet("/api/v1/rooms/{client}/power", async (string client, HttpContext c) =>
            Answer(await link.SendAsync(new ClientGetPower(client), c.RequestAborted).ConfigureAwait(false)));
        app.MapMethods("/api/v1/rooms/{client}/power", [HttpMethods.Patch],
            (Delegate)((string client, HttpContext c) =>
                WithBody<PowerSettingsRequest, ClientPowerSettings, CoreError>(c,
                    b => link.SendAsync(new ClientSetPower(client, b.StandbyAfterSeconds, b.DeepSleepAfterSeconds), c.RequestAborted))));
        app.MapMethods("/api/v1/rooms/{client}/power", [HttpMethods.Post],
            (Delegate)((string client, HttpContext c) =>
                WithBody<PowerActionRequest, ClientPowerSettings, CoreError>(c,
                    b => link.SendAsync(new ClientPower(client, b.Action), c.RequestAborted))));
        // Phase 39: a room that is not connected, gone for good -- a dead
        // device, or one that came back under another id. 409 conflict while
        // it is connected. Its image goes with it.
        app.MapDelete("/api/v1/rooms/{client}", ForgetRoomAsync);
        app.MapPost("/api/v1/rooms/{client}/rejoin", async (string client, HttpContext c) =>
            Answer(await link.SendAsync(new ClientRejoin(client), c.RequestAborted).ConfigureAwait(false)));
        // A room asleep with powerState "sleep" (Connector 2 on Ethernet); 409
        // for any other state.
        app.MapPost("/api/v1/rooms/{client}/wake", async (string client, HttpContext c) =>
            Answer(await link.SendAsync(new ClientWake(client), c.RequestAborted).ConfigureAwait(false)));
        // Phase 41: a profile copied into the room and switched on, and the
        // house's profiles.
        app.MapPost("/api/v1/rooms/{client}/equalizer/profile", ApplyEqualizerProfileAsync);
        app.MapGet("/api/v1/equalizer/profiles", ListEqualizerProfiles);
        app.MapPut("/api/v1/equalizer/profiles/{name}", SaveEqualizerProfileAsync);
        app.MapDelete("/api/v1/equalizer/profiles/{name}", RemoveEqualizerProfile);
        app.MapPost("/api/v1/rooms/{client}/rescan", async (string client, HttpContext c) =>
            Answer(await link.SendAsync(new ClientRescanAudio(client), c.RequestAborted).ConfigureAwait(false)));
        // A room's button or encoder (MATP section 57): the body is the
        // gestures to rebind, e.g. {"doubleClick":{"action":"next"}}; a gesture
        // it leaves out keeps what it had. The client keeps its bindings, so a
        // room that is not connected cannot be asked.
        app.MapMethods("/api/v1/rooms/{client}/inputs/{inputId}", [HttpMethods.Patch],
            (Delegate)((string client, string inputId, HttpContext c) =>
                WithBody<Dictionary<string, CoreInputBinding>, InputBindingsSet, CoreError>(c,
                    b => link.SendAsync(new ClientSetInputBindings(client, inputId, b), c.RequestAborted))));
        // Every client the core hears on its network, a room or not, and
        // pointing one at the core or at nothing -- always through the core.
        app.MapGet("/api/v1/clients/discovered", (Delegate)(async (HttpContext c) =>
            Answer(await link.SendAsync(new GetDiscoveredClients(), c.RequestAborted).ConfigureAwait(false))));
        app.MapPost("/api/v1/clients/{client}/attach", AttachAsync);
        app.MapPost("/api/v1/clients/{client}/detach", async (string client, HttpContext c) =>
            Answer(await link.SendAsync(new ClientDetach(client), c.RequestAborted).ConfigureAwait(false)));
        // The core's audio section, and its timings changed on the running
        // core: kept in the core's own config file, not here.
        app.MapGet("/api/v1/core/audio", (Delegate)(async (HttpContext c) =>
            Answer(await link.SendAsync(new GetAudioSettings(), c.RequestAborted).ConfigureAwait(false))));
        app.MapMethods("/api/v1/core/audio", [HttpMethods.Patch], (Delegate)((HttpContext c) =>
            WithBody<CoreAudioPatch, AudioSettingsReply, CoreError>(c, p =>
                p is { MinimumStartBufferMs: null, ClientReadyTimeoutMs: null, StartDelayMs: null,
                    SeekFlushLeadMs: null, SeekFillLeadMs: null, MemoryLimitMb: null }
                    ? Task.FromResult<Result<AudioSettingsReply, CoreError>>(
                        Result.Fail(CoreError.InvalidArgument, "an audio patch needs at least one setting"))
                    : link.SendAsync(new AudioConfigure(p.MinimumStartBufferMs, p.ClientReadyTimeoutMs,
                        p.StartDelayMs, p.SeekFlushLeadMs, p.SeekFillLeadMs, p.MemoryLimitMb), c.RequestAborted))));
        app.MapPut("/api/v1/core/resampler", (Delegate)((HttpContext c) =>
            WithBody<ResamplerRequest, ResamplerConfigured, CoreError>(c,
                r => link.SendAsync(new ResamplerConfigure(r.Resampler), c.RequestAborted))));
    }

    private async Task<IResult> AttachAsync(string client, HttpContext context)
    {
        var body = await ApiJson.ReadAsync<AttachRequest>(context.Request, optional: true, context.RequestAborted)
            .ConfigureAwait(false);
        if (!body.IsSuccess)
            return Error(body.Failure);
        var server = string.IsNullOrEmpty(body.Value?.Server) ? null : body.Value.Server;
        return Answer(await link.SendAsync(new ClientAttach(client, server), context.RequestAborted)
            .ConfigureAwait(false));
    }

    // Each setting is its own command to the core, sent in the patch's order;
    // a refusal stops there, and the settings before it stay sent. With no
    // core connected each is refused as notConnected, and recorded in the
    // desired half all the same: it is asserted when the core is back.
    private Task<IResult> PatchRoomAsync(string client, HttpContext context) =>
        WithBody<RoomPatch, RoomView, CoreError>(context, async patch =>
        {
            var ct = context.RequestAborted;
            if (patch is { AudioControl: null, OutputLatencyMs: null, Volume: null, Muted: null,
                AudioDeviceId: null, BufferMs: null, VolumeControl: null, Name: null, Equalizer: null,
                MaxVolumeDb: null, DriftCorrection: null })
                return Result.Fail(CoreError.InvalidArgument, "a room patch needs at least one setting");
            if (patch.Volume is < 0.0 or > 1.0)
                return Result.Fail(CoreError.InvalidArgument,
                    string.Create(CultureInfo.InvariantCulture, $"volume {patch.Volume} is outside 0.0..1.0"));

            if (patch.AudioControl is { } audioControl &&
                Refused(await link.SendAsync(new ClientSetAudioControl(client, audioControl), ct).ConfigureAwait(false)) is { } f1)
            {
                return f1;
            }
            if (patch.OutputLatencyMs is { } latency &&
                Refused(await link.SendAsync(new ClientSetOutputLatency(client, latency), ct).ConfigureAwait(false)) is { } f3)
            {
                return f3;
            }
            if (patch.Equalizer is { } equalizer &&
                Refused(await house.SetEqualizerAsync(client, equalizer, ct).ConfigureAwait(false)) is { } f9)
            {
                return f9;
            }
            if (patch.MaxVolumeDb is { } maxVolumeDb &&
                Refused(await house.SetMaxVolumeDbAsync(client, maxVolumeDb, ct).ConfigureAwait(false)) is { } f10)
            {
                return f10;
            }
            if ((patch.Volume is not null || patch.Muted is not null) &&
                Refused(await link.SendAsync(new ClientSetVolume(client, patch.Volume, patch.Muted), ct).ConfigureAwait(false)) is { } f4)
            {
                return f4;
            }
            // Last, these two: they need the room connected, and the client
            // keeps them, so nothing is recorded here when they are refused.
            if (patch.AudioDeviceId is { } device &&
                Refused(await link.SendAsync(new ClientSetAudioDevice(client, device), ct).ConfigureAwait(false)) is { } f5)
            {
                return f5;
            }
            // The client keeps this too, and judges its range itself.
            if (patch.BufferMs is { } buffer &&
                Refused(await link.SendAsync(new ClientSetBuffer(client, buffer), ct).ConfigureAwait(false)) is { } f6)
            {
                return f6;
            }
            // The client keeps this as well. After the Audio Control, so that
            // one patch can leave bitPerfect and ask for software together.
            if (patch.VolumeControl is { } volumeControl &&
                Refused(await link.SendAsync(new ClientSetVolumeControl(client, volumeControl), ct).ConfigureAwait(false)) is { } f7)
            {
                return f7;
            }
            // And this: the client keeps its drift-correction actuator, and
            // changing it reopens the room's stream.
            if (patch.DriftCorrection is { } driftCorrection &&
                Refused(await link.SendAsync(new ClientSetDriftCorrection(client, driftCorrection), ct).ConfigureAwait(false)) is { } f11)
            {
                return f11;
            }
            // The client keeps its name too (MATP section 60).
            if (patch.Name is { } name &&
                Refused(await house.RenameAsync(client, name, ct).ConfigureAwait(false)) is { } f8)
            {
                return f8;
            }
            return house.Room(client);
        });

    private Task<IResult> ApplyEqualizerProfileAsync(string client, HttpContext context) =>
        WithBody<EqualizerProfileApply, RoomView, CoreError>(context,
            r => house.ApplyEqualizerProfileAsync(client, r.Profile, context.RequestAborted));

    private IResult ListEqualizerProfiles() => Json(equalizerProfiles.All);

    private Task<IResult> SaveEqualizerProfileAsync(string name, HttpContext context) =>
        WithBody<EqualizerProfileRequest, EqualizerProfile, CoreError>(context,
            r => Task.FromResult(equalizerProfiles.Save(name, r.PreampDb, r.Bands)));

    private IResult RemoveEqualizerProfile(string name)
    {
        var removed = equalizerProfiles.Remove(name);
        return removed.IsSuccess ? Results.NoContent() : Error(removed.Failure);
    }

    private async Task<IResult> ForgetRoomAsync(string client, HttpContext context)
    {
        var forgot = await zones.ForgetAsync(client, context.RequestAborted).ConfigureAwait(false);
        if (!forgot.IsSuccess)
            return Error(forgot.Failure);
        if (profile.RemoveImage(ServerProfileStore.RoomOwner(client)) is { IsSuccess: false } removed)
            LogImageNotRemoved(_log, client, removed.Failure.Detail);
        events.Publish("room.forgotten", new { client });
        return Results.NoContent();
    }

    private static Failure<CoreError>? Refused<T>(Result<T, CoreError> answer) =>
        answer.IsSuccess ? null : answer.Failure;

    [LoggerMessage(Level = LogLevel.Warning, Message = "room {Client} forgotten, but its image could not be removed: {Detail}")]
    private static partial void LogImageNotRemoved(ILogger log, string client, string detail);
}
