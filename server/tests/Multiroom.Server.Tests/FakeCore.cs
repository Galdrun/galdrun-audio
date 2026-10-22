using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Text.Encodings.Web;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace Multiroom.Server.Tests;

// The core's control API on 4713, in process on a free loopback port: one JSON
// object per line each way, the envelope and the per-command params checked
// as ControlApi.cpp checks them -- an unknown or missing key is refused by
// name, so a misspelled field in a serializer fails the test that sends it.
//
// Answers are canned in the core's shapes and derived from the params; a test
// replaces one with Answer or Refuse, and sets what `status` says with
// StatusBody. Events are written with EmitAsync; DropAsync closes the
// connection, the fault a restarting core is.
internal sealed class FakeCore : IAsyncDisposable
{
    public const string EmptyStatus =
        """{"server":{"version":"fake","uptimeMs":1,"zones":0,"clients":0,"connected":0,"members":0},"clients":[],"zones":[]}""";

    // The params each command takes, from ControlApi::handleLine and the
    // command's own reads: a trailing `?` is optional.
    private static readonly Dictionary<string, string[]> Params = new(StringComparer.Ordinal)
    {
        ["status"] = [],
        ["clients"] = [],
        ["zones"] = [],
        ["zone.create"] = ["zone"],
        ["zone.delete"] = ["zone"],
        ["zone.setClients"] = ["zone", "clients"],
        ["source.stage"] = ["zone", "sourceId", "mediaUri", "trackId?", "title?", "startOffsetNs?"],
        ["source.clearStaged"] = ["zone"],
        ["zone.play"] = ["zone"],
        ["zone.pause"] = ["zone"],
        ["zone.stop"] = ["zone"],
        ["zone.seek"] = ["zone", "positionNs"],
        ["client.setGrouped"] = ["client", "grouped"],
        ["client.setAudioControl"] = ["client", "audioControl"],
        ["client.setVolume"] = ["client", "value?", "muted?", "applyAtTimelineNs?"],
        ["client.rejoin"] = ["client"],
        ["client.wake"] = ["client"],
        ["client.setOutputLatency"] = ["client", "latencyMs"],
        ["client.setEqualizer"] = ["client", "enabled", "preampDb?", "bands?"],
        ["client.setAudioDevice"] = ["client", "deviceId"],
        ["client.rescanAudio"] = ["client"],
        ["client.setBuffer"] = ["client", "bufferMs"],
        ["client.setName"] = ["client", "name"],
        ["client.forget"] = ["client"],
        ["client.setVolumeControl"] = ["client", "volumeControl"],
        ["client.setDriftCorrection"] = ["client", "driftCorrection"],
        ["audio.settings"] = [],
        ["audio.configure"] = ["minimumStartBufferMs?", "clientReadyTimeoutMs?", "startDelayMs?",
            "seekFlushLeadMs?", "seekFillLeadMs?", "memoryLimitMb?"],
        ["client.setInputBindings"] = ["client", "inputId", "bindings"],
        ["discovery.clients"] = [],
        ["client.attach"] = ["client", "server?"],
        ["client.detach"] = ["client"],
        ["client.getNetwork"] = ["client"],
        ["client.setEthernetSpeed"] = ["client", "speedMbps"],
        ["client.getLeds"] = ["client"],
        ["client.setLeds"] = ["client", "off"],
        ["client.getPower"] = ["client"],
        ["client.setPower"] = ["client", "standbyAfterSeconds?", "deepSleepAfterSeconds?"],
        ["client.power"] = ["client", "action"],
        ["resampler.configure"] = ["resampler"],
        ["events.subscribe"] = [],
        ["events.unsubscribe"] = [],
    };

    private static readonly JsonSerializerOptions Compact = new()
    {
        Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping,
    };

    private readonly TcpListener _listener = new(IPAddress.Loopback, 0);
    private readonly CancellationTokenSource _stop = new();
    private readonly Lock _lock = new();
    private readonly List<Received> _received = [];
    private readonly Dictionary<string, Func<JsonObject, JsonObject>> _answers = new(StringComparer.Ordinal);
    private readonly Dictionary<string, (string Code, string Message)> _refusals = new(StringComparer.Ordinal);
    private readonly Task _accepting;
    private Session? _session;
    private int _connections;

    public FakeCore()
    {
        _listener.Start();
        _accepting = AcceptAsync();
    }

    public sealed record Received(int Connection, string Command, string Params);

    public int Port => ((IPEndPoint)_listener.LocalEndpoint).Port;

    public string StatusBody { get; set; } = EmptyStatus;

    public IReadOnlyList<Received> All
    {
        get
        {
            lock (_lock)
                return [.. _received];
        }
    }

    public IReadOnlyList<Received> On(int connection) => [.. All.Where(r => r.Connection == connection)];

    public void Answer(string command, Func<JsonObject, JsonObject> body)
    {
        lock (_lock)
            _answers[command] = body;
    }

    public void Refuse(string command, string code, string message)
    {
        lock (_lock)
            _refusals[command] = (code, message);
    }

    public Task EmitAsync(string eventLine) => EmitAsync([eventLine]);

    public async Task EmitAsync(IEnumerable<string> eventLines)
    {
        var session = _session ?? throw new InvalidOperationException("no connection to emit on");
        foreach (var line in eventLines)
            await session.WriteAsync(line).ConfigureAwait(false);
    }

    // The connection closes under the client, as when the core exits.
    public void Drop() => _session?.Client.Dispose();

    public async ValueTask DisposeAsync()
    {
        await _stop.CancelAsync().ConfigureAwait(false);
        _listener.Stop();
        _session?.Client.Dispose();
        try
        {
            await _accepting.ConfigureAwait(false);
        }
        catch (Exception e) when (e is OperationCanceledException or SocketException or ObjectDisposedException)
        {
        }
        _stop.Dispose();
    }

    private async Task AcceptAsync()
    {
        while (!_stop.IsCancellationRequested)
        {
            var client = await _listener.AcceptTcpClientAsync(_stop.Token).ConfigureAwait(false);
            var session = new Session(client, Interlocked.Increment(ref _connections));
            _session = session;
            _ = ServeAsync(session);
        }
    }

    private async Task ServeAsync(Session session)
    {
        try
        {
            using var reader = new StreamReader(session.Client.GetStream(), Encoding.UTF8);
            while (await reader.ReadLineAsync(_stop.Token).ConfigureAwait(false) is { } line)
                await session.WriteAsync(Handle(line, session.Number)).ConfigureAwait(false);
        }
        catch (Exception e) when (e is IOException or ObjectDisposedException or OperationCanceledException)
        {
        }
        finally
        {
            session.Client.Dispose();
        }
    }

    private string Handle(string line, int connection)
    {
        var root = JsonNode.Parse(line)!.AsObject();
        var command = root["command"]!.GetValue<string>();
        var id = root["id"]?.DeepClone();
        var parameters = root["params"]?.AsObject() ?? [];
        lock (_lock)
            _received.Add(new Received(connection, command, parameters.ToJsonString(Compact)));

        if (root.Select(p => p.Key).FirstOrDefault(k => k is not ("command" or "params" or "id")) is { } extra)
            return Error(command, id, "invalidRequest", $"unknown key \"{extra}\" in the request");
        if (!Params.TryGetValue(command, out var spec))
            return Error(command, id, "unknownCommand", $"unknown command \"{command}\"");
        var known = spec.Select(k => k.TrimEnd('?')).ToHashSet(StringComparer.Ordinal);
        if (parameters.Select(p => p.Key).FirstOrDefault(k => !known.Contains(k)) is { } unknown)
            return Error(command, id, "invalidArgument", $"unknown key \"{unknown}\" for {command}");
        if (spec.FirstOrDefault(k => !k.EndsWith('?') && !parameters.ContainsKey(k)) is { } missing)
            return Error(command, id, "invalidArgument", $"\"{missing}\" is required");

        Func<JsonObject, JsonObject>? answer;
        lock (_lock)
        {
            if (_refusals.TryGetValue(command, out var refusal))
                return Error(command, id, refusal.Code, refusal.Message);
            _answers.TryGetValue(command, out answer);
        }

        var response = new JsonObject { ["ok"] = true, ["command"] = command, ["id"] = id };
        foreach (var (key, value) in (answer ?? DefaultAnswer(command))(parameters))
            response[key] = value?.DeepClone();
        return response.ToJsonString(Compact);
    }

    private Func<JsonObject, JsonObject> DefaultAnswer(string command) => command switch
    {
        "status" => _ => JsonNode.Parse(StatusBody)!.AsObject(),
        "clients" => _ => new JsonObject { ["clients"] = JsonNode.Parse(StatusBody)!["clients"]!.DeepClone() },
        "zones" => _ => new JsonObject { ["zones"] = JsonNode.Parse(StatusBody)!["zones"]!.DeepClone() },
        "zone.create" => p => new JsonObject { ["zone"] = Copy(p, "zone"), ["created"] = true },
        "zone.delete" => p => new JsonObject { ["zone"] = Copy(p, "zone"), ["deleted"] = true },
        "zone.setClients" => p => new JsonObject
        {
            ["zone"] = Copy(p, "zone"),
            ["added"] = Copy(p, "clients"),
            ["removed"] = new JsonArray(),
        },
        "source.stage" => p => new JsonObject
        {
            ["zone"] = Copy(p, "zone"),
            ["sourceId"] = Copy(p, "sourceId"),
            ["staged"] = true,
        },
        "source.clearStaged" => p => new JsonObject { ["zone"] = Copy(p, "zone"), ["cleared"] = 0 },
        "zone.play" => p => new JsonObject
        {
            ["zone"] = Copy(p, "zone"),
            ["state"] = "playing",
            ["transport"] = "starting",
        },
        "zone.pause" => p => new JsonObject { ["zone"] = Copy(p, "zone"), ["paused"] = true, ["timelineNs"] = 7 },
        "zone.stop" => p => new JsonObject { ["zone"] = Copy(p, "zone"), ["stopped"] = true, ["timelineNs"] = 9 },
        "zone.seek" => p => new JsonObject
        {
            ["zone"] = Copy(p, "zone"),
            ["sourceId"] = "1",
            ["positionNs"] = Copy(p, "positionNs"),
            ["cutTimelineNs"] = 100,
            ["timelineNs"] = 150,
        },
        "client.setGrouped" => p => new JsonObject
        {
            ["client"] = Copy(p, "client"),
            ["grouped"] = Copy(p, "grouped"),
            ["changed"] = true,
        },
        "client.setAudioControl" => p => new JsonObject
        {
            ["client"] = Copy(p, "client"),
            ["audioControl"] = Copy(p, "audioControl"),
            ["changed"] = true,
        },
        "client.setVolume" => p =>
        {
            var body = new JsonObject { ["client"] = Copy(p, "client"), ["changed"] = true };
            if (p.ContainsKey("value"))
                body["value"] = Copy(p, "value");
            if (p.ContainsKey("muted"))
                body["muted"] = Copy(p, "muted");
            return body;
        },
        "client.rejoin" => p => new JsonObject { ["client"] = Copy(p, "client"), ["zone"] = "living", ["timelineNs"] = 3 },
        "client.wake" => p => new JsonObject { ["client"] = Copy(p, "client"), ["wakeOnLan"] = "02:00:00:00:00:01" },
        "client.setPower" or "client.power" or "client.getPower" => p => new JsonObject { ["client"] = Copy(p, "client"),
            ["supported"] = true, ["state"] = "on", ["standbyAfterSeconds"] = 900, ["deepSleepAfterSeconds"] = 3600, ["deepSleep"] = true },
        "client.setOutputLatency" => p => new JsonObject
        {
            ["client"] = Copy(p, "client"),
            ["latencyMs"] = Copy(p, "latencyMs"),
            ["changed"] = true,
            ["rejoined"] = false,
        },
        "client.setEqualizer" => p => new JsonObject
        {
            ["client"] = Copy(p, "client"),
            ["equalizer"] = new JsonObject
            {
                ["enabled"] = Copy(p, "enabled"),
                ["preampDb"] = p.ContainsKey("preampDb") ? Copy(p, "preampDb") : 0,
                ["bands"] = p.ContainsKey("bands") ? Copy(p, "bands") : new JsonArray(),
            },
            ["changed"] = true,
        },
        "client.rescanAudio" => p => new JsonObject { ["client"] = Copy(p, "client") },
        "audio.settings" => _ => AudioSettings(1000, 64, null),
        "audio.configure" => p => AudioSettings(p["startDelayMs"]?.GetValue<int>() ?? 1000,
            p["memoryLimitMb"]?.GetValue<int>() ?? 64, true),
        "client.setName" => p => new JsonObject
        {
            ["client"] = Copy(p, "client"),
            ["name"] = Copy(p, "name"),
            ["changed"] = true,
        },
        "client.forget" => p => new JsonObject { ["client"] = Copy(p, "client") },
        "client.setBuffer" => p => new JsonObject
        {
            ["client"] = Copy(p, "client"),
            ["bufferMs"] = Copy(p, "bufferMs"),
            ["changed"] = true,
        },
        "client.setVolumeControl" => p => new JsonObject
        {
            ["client"] = Copy(p, "client"),
            ["volumeControl"] = Copy(p, "volumeControl"),
            ["changed"] = true,
        },
        "client.setDriftCorrection" => p => new JsonObject
        {
            ["client"] = Copy(p, "client"),
            ["driftCorrection"] = Copy(p, "driftCorrection"),
            ["changed"] = true,
        },
        "client.setInputBindings" => p => new JsonObject
        {
            ["client"] = Copy(p, "client"),
            ["inputId"] = Copy(p, "inputId"),
            ["bindings"] = Copy(p, "bindings"),
            ["changed"] = true,
        },
        "client.setAudioDevice" => p => new JsonObject
        {
            ["client"] = Copy(p, "client"),
            ["deviceId"] = Copy(p, "deviceId"),
            ["changed"] = true,
        },
        "discovery.clients" => _ => new JsonObject
        {
            ["clients"] = new JsonArray(new JsonObject
            {
                ["client"] = "kitchen",
                ["address"] = "10.0.0.7",
                ["controlPort"] = 4720,
                ["protocolVersion"] = 2,
                ["platform"] = "linux-armv6",
                ["server"] = "",
                ["present"] = true,
                ["lastSeenMsAgo"] = 500,
                ["firstSeenMsAgo"] = 9000,
                ["beacons"] = 5,
                ["connected"] = false,
            }),
            ["forgotten"] = 0,
        },
        "client.attach" => p => new JsonObject
        {
            ["client"] = Copy(p, "client"),
            ["server"] = p["server"]?.DeepClone() ?? "10.0.0.2:4711",
            ["changed"] = true,
        },
        "client.detach" => p => new JsonObject { ["client"] = Copy(p, "client"), ["server"] = "", ["changed"] = true },
        "resampler.configure" => p => new JsonObject
        {
            ["resampler"] = Copy(p, "resampler"),
            ["variableRatio"] = true,
            ["changed"] = true,
        },
        "events.subscribe" => _ => new JsonObject { ["subscribed"] = true, ["changed"] = true },
        "events.unsubscribe" => _ => new JsonObject { ["subscribed"] = false, ["changed"] = true },
        _ => throw new InvalidOperationException(command),
    };

    private static JsonNode? Copy(JsonObject parameters, string key) => parameters[key]?.DeepClone();

    private static JsonObject AudioSettings(int startDelayMs, int memoryLimitMb, bool? changed)
    {
        var answer = new JsonObject
        {
            ["audio"] = new JsonObject
            {
                ["maxSampleRate"] = 192000, ["maxChannels"] = 2, ["packetDurationMs"] = 20,
                ["memoryLimitMb"] = memoryLimitMb,
                ["targetBufferMs"] = 1500, ["minimumStartBufferMs"] = 1000, ["clientReadyTimeoutMs"] = 5000,
                ["startDelayMs"] = startDelayMs, ["seekFlushLeadMs"] = 100, ["seekFillLeadMs"] = 150,
                ["driftWindowMs"] = 20000, ["correctionLimitPpm"] = 100, ["convergenceLimitPpm"] = 500,
                ["resampler"] = "soxr", ["audioControl"] = "auto", ["dither"] = "off",
            },
            ["live"] = new JsonArray("minimumStartBufferMs", "clientReadyTimeoutMs", "startDelayMs",
                "seekFlushLeadMs", "seekFillLeadMs", "memoryLimitMb"),
            ["memoryLimit"] = new JsonObject
            {
                ["mb"] = memoryLimitMb, ["minMb"] = 8, ["maxMb"] = 512, ["usedMb"] = 0,
            },
        };
        if (changed is { } c)
            answer["changed"] = c;
        return answer;
    }

    private static string Error(string command, JsonNode? id, string code, string message) =>
        new JsonObject
        {
            ["ok"] = false,
            ["command"] = command,
            ["id"] = id,
            ["error"] = code,
            ["message"] = message,
        }.ToJsonString(Compact);

    private sealed class Session(TcpClient client, int number)
    {
        private readonly Lock _write = new();

        public TcpClient Client { get; } = client;

        public int Number { get; } = number;

        public Task WriteAsync(string line)
        {
            var bytes = Encoding.UTF8.GetBytes(line + "\n");
            // Whole lines only: an answer and an event never interleave.
            lock (_write)
                Client.GetStream().Write(bytes);
            return Task.CompletedTask;
        }
    }
}
