using System.Collections.Concurrent;
using System.Net;
using System.Net.Sockets;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Multiroom.Server.Abstractions;
using Multiroom.Server.Playback;
using Multiroom.Server.Providers;
using Multiroom.Server.Web.Media;

namespace Multiroom.Server.Tests;

public sealed class ServerHostTests : IDisposable
{
    private readonly TempDirectory _dir = new();

    public void Dispose() => _dir.Dispose();

    [Fact]
    public async Task The_stop_signal_stops_every_hosted_service_in_reverse_order_and_exits_zero()
    {
        var config = _dir.Write("server.json", $$"""{ "dataDirectory": "data", "artwork": { "musicBrainz": false }, "core": { "port": {{FreePort()}}, "discoveryPort": 0 }, "media": { "port": 0 }, "api": { "port": 0 }, "gui": { "port": 0 } }""");
        var builder = ServerHost.CreateBuilder(["--config", config], TextWriter.Null).Value;
        var journal = new ConcurrentQueue<string>();
        var started = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        // Not AddHostedService: it de-duplicates by type and would drop the second.
        builder.Services.AddSingleton<IHostedService>(new Recording("first", journal, null));
        builder.Services.AddSingleton<IHostedService>(new Recording("second", journal, started));
        using var stop = new CancellationTokenSource();

        var run = ServerHost.RunAsync(builder, stop.Token);
        await started.Task.WaitAsync(TimeSpan.FromSeconds(10), TestContext.Current.CancellationToken);
        await stop.CancelAsync();

        Assert.Equal(0, await run);
        Assert.Equal(["start first", "start second", "stop second", "stop first"], journal);

        var log = await File.ReadAllTextAsync(Path.Combine(_dir.Path, "data", "server.log"),
            TestContext.Current.CancellationToken);
        Assert.Contains($"{Branding.ServerName} {ServerHost.Version} starting", log, StringComparison.Ordinal);
        Assert.Contains($"config {config}", log, StringComparison.Ordinal);
        Assert.Contains($"data directory {Path.Combine(_dir.Path, "data")}", log, StringComparison.Ordinal);
        Assert.Contains("media endpoint listening on http://127.0.0.1:", log, StringComparison.Ordinal);
        Assert.Contains("outward API listening on http://127.0.0.1:", log, StringComparison.Ordinal);
        Assert.Contains("provider local (Local files) started: Search, Browse, Stream", log, StringComparison.Ordinal);
        Assert.Contains("stopped after", log, StringComparison.Ordinal);
        // The startup self-check, and the queues written when it stopped.
        Assert.Contains("self-check: media endpoint reachable at http://127.0.0.1:", log, StringComparison.Ordinal);
        Assert.Contains("self-check: library database", log, StringComparison.Ordinal);
        Assert.True(File.Exists(Path.Combine(_dir.Path, "data", "playback.json")));
    }

    [Fact]
    public async Task A_playback_file_that_cannot_be_read_is_logged_and_the_server_starts_without_queues()
    {
        var config = _dir.Write("server.json", $$"""{ "dataDirectory": "data", "artwork": { "musicBrainz": false }, "core": { "port": {{FreePort()}}, "discoveryPort": 0 }, "media": { "port": 0 }, "api": { "port": 0 }, "gui": { "port": 0 } }""");
        Directory.CreateDirectory(Path.Combine(_dir.Path, "data"));
        var saved = _dir.Write(Path.Combine("data", "playback.json"), """{ "version": 1, "queues": [ """);
        var builder = ServerHost.CreateBuilder(["--config", config], TextWriter.Null).Value;
        var started = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        builder.Services.AddSingleton<IHostedService>(new Recording("last", new ConcurrentQueue<string>(), started));
        using var stop = new CancellationTokenSource();

        var run = ServerHost.RunAsync(builder, stop.Token);
        await started.Task.WaitAsync(TimeSpan.FromSeconds(10), TestContext.Current.CancellationToken);
        await stop.CancelAsync();

        Assert.Equal(0, await run);
        var log = await File.ReadAllTextAsync(Path.Combine(_dir.Path, "data", "server.log"),
            TestContext.Current.CancellationToken);
        Assert.Contains($"could not read {saved}", log, StringComparison.Ordinal);
        // Replaced by a readable one when it stopped.
        Assert.Contains("\"version\":11", await File.ReadAllTextAsync(saved, TestContext.Current.CancellationToken),
            StringComparison.Ordinal);
    }

    [Fact]
    public async Task A_hosted_service_that_fails_to_start_exits_one()
    {
        var config = _dir.Write("server.json",
            $$"""{ "dataDirectory": "data", "artwork": { "musicBrainz": false }, "logging": { "file": "" }, "core": { "port": {{FreePort()}}, "discoveryPort": 0 }, "media": { "port": 0 }, "api": { "port": 0 }, "gui": { "port": 0 } }""");
        var builder = ServerHost.CreateBuilder(["--config", config], TextWriter.Null).Value;
        builder.Services.AddHostedService(_ => new Failing());

        Assert.Equal(1, await ServerHost.RunAsync(builder, TestContext.Current.CancellationToken));
    }

    [Fact]
    public void An_unknown_key_refuses_to_build_the_host()
    {
        var config = _dir.Write("server.json", """{ "dataDirectory": "data", "colour": "red" }""");

        var builder = ServerHost.CreateBuilder(["--config", config], TextWriter.Null);

        Assert.False(builder.IsSuccess);
        Assert.Equal("UnknownKey: colour", builder.Failure.ToString());
    }


    [Fact]
    public async Task A_library_folder_is_indexed_into_the_data_directory_at_startup()
    {
        var music = Path.Combine(_dir.Path, "music");
        Directory.CreateDirectory(music);
        await File.WriteAllBytesAsync(Path.Combine(music, "broken.flac"), [1, 2, 3], Ct);
        var config = _dir.Write("server.json", $$"""
            { "dataDirectory": "data", "artwork": { "musicBrainz": false }, "core": { "port": {{FreePort()}}, "discoveryPort": 0 }, "media": { "port": 0 }, "api": { "port": 0 }, "gui": { "port": 0 },
              "library": { "folders": [ { "path": "music" } ] } }
            """);
        var builder = ServerHost.CreateBuilder(["--config", config], TextWriter.Null).Value;

        var exit = await RunUntilStarted(builder, async () =>
        {
            var deadline = DateTime.UtcNow + TimeSpan.FromSeconds(10);
            while (!(await ReadLog()).Contains($"indexed {music}: 1 file(s), 1 probed, 0 moved, 1 failed",
                       StringComparison.Ordinal))
            {
                Assert.True(DateTime.UtcNow < deadline, "the library folder was not indexed");
                await Task.Delay(20, Ct);
            }
        });

        Assert.Equal(0, exit);
        var log = await ReadLog();
        Assert.Contains($"library database {Path.Combine(_dir.Path, "data", "library.db")} at schema 29", log,
            StringComparison.Ordinal);
        Assert.Contains($"not indexed {Path.Combine(music, "broken.flac")}", log, StringComparison.Ordinal);
    }

    [Fact]
    public async Task A_second_registration_of_a_provider_id_refuses_to_start_by_name()
    {
        var builder = Builder();
        builder.Services.AddProvider(FakeProvider.ProviderId, _ => new FakeProvider());
        builder.Services.AddProvider(FakeProvider.ProviderId, _ => new FakeProvider());

        Assert.Equal(1, await ServerHost.RunAsync(builder, Ct));
        Assert.Contains("provider id fake is registered twice", await ReadLog(), StringComparison.Ordinal);
    }

    [Fact]
    public async Task A_provider_whose_constructor_throws_is_logged_and_skipped_while_the_server_starts()
    {
        var builder = Builder();
        builder.Services.AddProvider("broken", _ => throw new InvalidOperationException("no such account"));
        builder.Services.AddProvider(FakeProvider.ProviderId, _ => new FakeProvider());

        Assert.Equal(0, await RunUntilStarted(builder, () => Task.CompletedTask));
        var log = await ReadLog();
        Assert.Contains("provider broken failed to start and is disabled: no such account", log,
            StringComparison.Ordinal);
        Assert.Contains("provider fake (Fake) started", log, StringComparison.Ordinal);
    }

    [Fact]
    public async Task The_queue_engine_is_composed_over_the_media_endpoint()
    {
        var host = Builder().Build();
        await using var _ = (IAsyncDisposable)host;

        var engine = host.Services.GetRequiredService<IQueueEngine>();

        Assert.Same(host.Services.GetRequiredService<QueueEngine>(), engine);
        Assert.Same(host.Services.GetRequiredService<MediaEndpoint>(), host.Services.GetRequiredService<IMediaPublisher>());
        Assert.Empty(engine.Queues);
    }

    private static CancellationToken Ct => TestContext.Current.CancellationToken;

    private HostApplicationBuilder Builder()
    {
        var config = _dir.Write("server.json",
            $$"""{ "dataDirectory": "data", "core": { "port": {{FreePort()}}, "discoveryPort": 0 }, "media": { "port": 0 }, "api": { "port": 0 }, "gui": { "port": 0 } }""");
        return ServerHost.CreateBuilder(["--config", config], TextWriter.Null).Value;
    }

    // Shared, because the running host holds the file open for writing.
    private async Task<string> ReadLog()
    {
        await using var file = new FileStream(Path.Combine(_dir.Path, "data", "server.log"), FileMode.Open,
            FileAccess.Read, FileShare.ReadWrite);
        using var reader = new StreamReader(file);
        return await reader.ReadToEndAsync(Ct);
    }

    // Starts the host, runs `whileRunning` once every hosted service has
    // started, then stops it and answers the exit code.
    private static async Task<int> RunUntilStarted(HostApplicationBuilder builder, Func<Task> whileRunning)
    {
        var started = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        builder.Services.AddSingleton<IHostedService>(new Recording("last", new ConcurrentQueue<string>(), started));
        using var stop = new CancellationTokenSource();

        var run = ServerHost.RunAsync(builder, stop.Token);
        await started.Task.WaitAsync(TimeSpan.FromSeconds(10), Ct);
        try
        {
            await whileRunning();
        }
        finally
        {
            await stop.CancelAsync();
        }
        return await run;
    }

    // CoreLink starts with the host; it is pointed at a port nothing listens on,
    // never at a core that may be running on this machine.
    private static int FreePort()
    {
        var listener = new TcpListener(IPAddress.Loopback, 0);
        listener.Start();
        var port = ((IPEndPoint)listener.LocalEndpoint).Port;
        listener.Stop();
        return port;
    }

    private sealed class Recording(string name, ConcurrentQueue<string> journal, TaskCompletionSource? started)
        : IHostedService
    {
        public Task StartAsync(CancellationToken cancellationToken)
        {
            journal.Enqueue($"start {name}");
            started?.SetResult();
            return Task.CompletedTask;
        }

        public Task StopAsync(CancellationToken cancellationToken)
        {
            journal.Enqueue($"stop {name}");
            return Task.CompletedTask;
        }
    }

    private sealed class Failing : IHostedService
    {
        public Task StartAsync(CancellationToken cancellationToken) =>
            throw new InvalidOperationException("refused to start");

        public Task StopAsync(CancellationToken cancellationToken) => Task.CompletedTask;
    }
}
