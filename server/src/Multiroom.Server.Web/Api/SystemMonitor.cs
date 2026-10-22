using System.Diagnostics;
using Microsoft.Extensions.Hosting;
using Multiroom.Server.Playback.Core;

namespace Multiroom.Server.Web.Api;

public sealed record SystemMonitorOptions(string DataDirectory);

// This server's own figures, and the machine's (MachineStats). Absent is
// unknown, never zero. Handles are open file descriptors outside Windows.
public sealed record ServerSystemMetrics(
    ulong UptimeMs,
    int Threads,
    ulong WorkingSetBytes,
    ulong ManagedHeapBytes,
    double? CpuPercent = null,
    ulong? SystemMemoryTotalBytes = null,
    ulong? DataDriveFreeBytes = null,
    ulong? PrivateMemoryBytes = null,
    int? Handles = null,
    double? SystemCpuPercent = null,
    ulong? SystemMemoryAvailableBytes = null,
    ulong? SystemUptimeMs = null,
    int? Gen0Collections = null,
    int? Gen1Collections = null,
    int? Gen2Collections = null,
    // What the machine is, named as the core and a room name it (MATP section 55).
    string? CpuArchitecture = null,
    string? CpuModel = null,
    string? MachineModel = null,
    ulong? CpuCores = null,
    ulong? CpuMhz = null);

// One room's figures as its client reported them over MATP, and how long ago.
// A room without them says why: `notConnected`, `noReports` (its client sends
// none -- it predates the client's phase 20 -- or has not sent one yet), or
// `coreUnavailable` when the core itself did not answer.
public sealed record RoomSystemMetrics(
    string Client,
    CoreSystemMetrics? Metrics = null,
    ulong? AgeMs = null,
    string? Reason = null);

// GET system and the `system.metrics` event. Core is absent when the core did
// not answer or predates its phase 34.
// CoreMemory (the core's phase 42): every zone's audio memory, from the same
// status; absent when the core did not answer or predates it.
public sealed record SystemView(ServerSystemMetrics Server, CoreSystemMetrics? Core,
    IReadOnlyList<RoomSystemMetrics> Rooms, CoreAudioMemory? CoreMemory = null);

// The core's audio memory: used and the ceiling, and each zone's share, by id.
public sealed record CoreAudioMemory(ulong UsedBytes, ulong LimitBytes, IReadOnlyList<ZoneAudioMemory> Zones);

public sealed record ZoneAudioMemory(string Zone, ulong Bytes, ulong? Blocks = null, ulong? BlocksRefused = null);

// One connected room's sync and buffer, named as in GET rooms' actual half
// (BufferedMs is its output's); what the core has not measured is absent.
// Health is the model's after this status (phase 34: degraded is worked out
// from these reports), so a GUI sees it change without asking.
public sealed record RoomSyncMetrics(
    string Client,
    CoreSync? Sync = null,
    ulong? BufferedMs = null,
    CorePlayback? Playback = null,
    ulong? Rejoins = null,
    CoreRoomHealth? Health = null);

// The `rooms.sync` event: every connected room, from the same core status.
public sealed record RoomsSyncView(IReadOnlyList<RoomSyncMetrics> Rooms);

// Phase 18: every second, this server's own figures and one `status` of the
// core (the only poll this phase adds -- the core has no event for them), kept
// as the latest view and published as `system.metrics`. The same status gives
// every connected room's sync and buffer, published as `rooms.sync`, so a GUI
// needs no poll for them either. Only the latest is kept; history is a GUI's
// business.
public sealed class SystemMonitor(SystemMonitorOptions options, CoreLink link, ApiEvents events) : BackgroundService
{
    public static readonly TimeSpan Interval = TimeSpan.FromSeconds(1);

    private readonly Stopwatch _wall = Stopwatch.StartNew();
    private readonly TaskCompletionSource<SystemView> _first = new(TaskCreationOptions.RunContinuationsAsynchronously);
    private TimeSpan? _lastCpu;
    private TimeSpan _lastWall;
    private (ulong Busy, ulong Total)? _lastMachineCpu;
    private volatile SystemView? _current;

    // The latest view; the first one is waited for.
    public async Task<SystemView> LatestAsync(CancellationToken cancellationToken) =>
        _current ?? await _first.Task.WaitAsync(cancellationToken).ConfigureAwait(false);

    protected override async Task ExecuteAsync(CancellationToken stoppingToken)
    {
        using var timer = new PeriodicTimer(Interval);
        try
        {
            do
            {
                await SampleAsync(stoppingToken).ConfigureAwait(false);
            }
            while (await timer.WaitForNextTickAsync(stoppingToken).ConfigureAwait(false));
        }
        catch (OperationCanceledException) when (stoppingToken.IsCancellationRequested)
        {
        }
    }

    private async Task SampleAsync(CancellationToken cancellationToken)
    {
        var server = SampleServer();

        CoreSystemMetrics? core = null;
        List<RoomSystemMetrics> rooms;
        RoomsSyncView? sync = null;
        CoreAudioMemory? memory = null;
        var status = await link.SendAsync(new GetStatus(), cancellationToken).ConfigureAwait(false);
        if (status.IsSuccess)
        {
            core = status.Value.Server.System;
            if (status.Value.Server is { MemoryUsedBytes: { } used, MemoryLimitBytes: { } limit })
            {
                memory = new CoreAudioMemory(used, limit, [.. status.Value.Zones.Where(z => z.MemoryBytes is not null)
                    .Select(z => new ZoneAudioMemory(z.Zone, z.MemoryBytes!.Value, z.Blocks, z.BlocksRefused))]);
            }
            rooms = [.. status.Value.Clients.Select(RoomOf)];
            sync = new RoomsSyncView([.. status.Value.Clients.Where(c => c.Connected)
                .Select(c => new RoomSyncMetrics(c.ClientId, c.Sync, c.Output?.BufferedMs, c.Playback, c.Rejoins,
                    link.Model.Current.Rooms.GetValueOrDefault(c.ClientId)?.Health))]);
        }
        else
        {
            rooms = [.. link.Model.Current.Rooms.Keys.Order(StringComparer.Ordinal)
                .Select(client => new RoomSystemMetrics(client, Reason: "coreUnavailable"))];
        }

        var view = new SystemView(server, core, rooms, memory);
        _current = view;
        _first.TrySetResult(view);
        events.Publish("system.metrics", view);
        // A core that did not answer said nothing about its rooms.
        if (sync is not null)
            events.Publish("rooms.sync", sync);
    }

    private static RoomSystemMetrics RoomOf(CoreClientInfo client) => client.System is { } metrics
        ? new RoomSystemMetrics(client.ClientId, metrics, client.SystemAgeMs)
        : new RoomSystemMetrics(client.ClientId, Reason: client.Connected ? "noReports" : "notConnected");

    private ServerSystemMetrics SampleServer()
    {
        using var process = Process.GetCurrentProcess();
        var cpu = process.TotalProcessorTime;
        var wall = _wall.Elapsed;

        // A rate, so absent on the first sample.
        double? cpuPercent = null;
        if (_lastCpu is { } lastCpu && wall > _lastWall)
        {
            cpuPercent = Math.Clamp((cpu - lastCpu).TotalMilliseconds * 100.0 /
                ((wall - _lastWall).TotalMilliseconds * Environment.ProcessorCount), 0.0, 100.0);
        }
        _lastCpu = cpu;
        _lastWall = wall;

        double? systemCpuPercent = null;
        if (MachineStats.TryCpuTicks(out var busy, out var total))
        {
            if (_lastMachineCpu is { } last && total > last.Total && busy >= last.Busy)
                systemCpuPercent = Math.Clamp((busy - last.Busy) * 100.0 / (total - last.Total), 0.0, 100.0);
            _lastMachineCpu = (busy, total);
        }
        else
        {
            _lastMachineCpu = null;
        }

        // The OS's total, so used = total - available holds; the GC's (which
        // honours a container's limit) when the OS cannot say.
        var machineMemory = MachineStats.Memory();
        var machine = MachineStats.What();
        var gcMemory = GC.GetGCMemoryInfo();
        ulong? memoryTotal = machineMemory?.Total ??
            (gcMemory.TotalAvailableMemoryBytes > 0 ? (ulong)gcMemory.TotalAvailableMemoryBytes : null);

        return new ServerSystemMetrics(
            (ulong)Math.Max(0, (DateTime.Now - process.StartTime).TotalMilliseconds),
            process.Threads.Count,
            (ulong)process.WorkingSet64,
            // Not GCMemoryInfo.HeapSizeBytes: that is as of the last GC, and 0
            // before the first one.
            (ulong)GC.GetTotalMemory(forceFullCollection: false),
            cpuPercent,
            memoryTotal,
            DataDriveFreeBytes(),
            (ulong)process.PrivateMemorySize64,
            process.HandleCount,
            systemCpuPercent,
            machineMemory?.Available,
            (ulong)Environment.TickCount64,
            GC.CollectionCount(0),
            GC.CollectionCount(1),
            GC.CollectionCount(2),
            machine.Architecture,
            machine.CpuModel,
            machine.MachineModel,
            machine.Cores,
            MachineStats.CpuMhz());
    }

    private ulong? DataDriveFreeBytes()
    {
        try
        {
            return (ulong)new DriveInfo(Path.GetFullPath(options.DataDirectory)).AvailableFreeSpace;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or ArgumentException)
        {
            return null;
        }
    }
}
