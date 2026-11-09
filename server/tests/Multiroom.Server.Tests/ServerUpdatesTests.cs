using System.IO.Compression;
using System.Text.Json.Nodes;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging.Abstractions;
using Multiroom.Server.Playback.Core;
using Multiroom.Server.Web.Updates;

namespace Multiroom.Server.Tests;

public sealed class ServerUpdatesTests : IAsyncDisposable
{
    private readonly FakeCore _core = new();
    private readonly QueueEngineTests.Synchronised _events = new();
    private readonly CoreLink _link;
    private readonly Lifetime _lifetime = new();
    private readonly string _root = Directory.CreateTempSubdirectory("galdrun-server-updates").FullName;
    private string Packages => Path.Combine(_root, "packages");
    private string ServerDirectory => Path.Combine(_root, "server");
    private string CoreDirectory => Path.Combine(_root, "core");

    public ServerUpdatesTests()
    {
        Directory.CreateDirectory(Packages);
        Directory.CreateDirectory(ServerDirectory);
        Directory.CreateDirectory(CoreDirectory);
        File.WriteAllText(Path.Combine(ServerDirectory, "galdrun-server"), "old server");
        _link = new CoreLink(new CoreLinkOptions("127.0.0.1", _core.Port), [_events], NullLogger<CoreLink>.Instance);
        _core.StatusBody = $$"""
            {"server":{"version":"0.1.0","uptimeMs":1,"zones":0,"clients":0,"connected":0,"members":0,
             "executable":{{JsonValue.Create(Path.Combine(CoreDirectory, "galdrun-core")).ToJsonString()}},
             "restartable":true},"clients":[],"zones":[]}
            """;
    }

    private static CancellationToken Ct => TestContext.Current.CancellationToken;

    public async ValueTask DisposeAsync()
    {
        await _link.StopAsync(CancellationToken.None);
        _link.Dispose();
        await _core.DisposeAsync();
        Directory.Delete(_root, recursive: true);
    }

    // No release on "GitHub" (nothing listens there): only what is in Packages.
    private ServerUpdates Updates(string? target = "windows-x64") =>
        new(new ServerUpdatesOptions(Packages, new Uri("http://127.0.0.1:9/releases"), ServerDirectory, target,
            "0.1.0+abc", WindowsService: false), _link, _lifetime, NullLogger<ServerUpdates>.Instance);

    private async Task StartAsync()
    {
        await _link.StartAsync(Ct);
        await _events.Next.Task.WaitAsync(TimeSpan.FromSeconds(10), Ct);
    }

    // A bundle whose programs are not programs: they cannot answer --version.
    private void DropBundle(string name)
    {
        using var zip = ZipFile.Open(Path.Combine(Packages, name), ZipArchiveMode.Create);
        foreach (var entry in new[] { "server/galdrun-server", "server/galdrun-server.exe", "core/galdrun-core",
                     "core/galdrun-core.exe", "README.md" })
        {
            using var writer = new StreamWriter(zip.CreateEntry($"galdrun-server-bundle-v0.2.0-windows-x64/{entry}").Open());
            writer.Write("new " + entry);
        }
    }

    [Fact]
    public void Release_targets_follow_the_runtime_identifier()
    {
        Assert.Equal("windows-x64", ServerUpdates.TargetOf("win-x64"));
        Assert.Equal("windows-armv8", ServerUpdates.TargetOf("win-arm64"));
        Assert.Equal("linux-armv7", ServerUpdates.TargetOf("linux-arm"));
        Assert.Equal("linux-armv8", ServerUpdates.TargetOf("linux-arm64"));
        Assert.Null(ServerUpdates.TargetOf("osx-arm64"));
    }

    [Fact]
    public async Task A_newer_bundle_is_shown_even_where_this_installation_cannot_take_it()
    {
        await StartAsync();
        DropBundle("galdrun-server-bundle-v0.2.0-windows-x64.zip");
        using var updates = Updates();

        // No build-info.txt beside the server: not a published release.
        var view = await updates.GetAsync(Ct);
        Assert.Equal(("0.1.0+abc", "0.1.0", "v0.2.0", true, false), (view.Version, view.CoreVersion, view.Latest,
            view.Available, view.Supported));
        Assert.Contains("not a published release", view.Reason, StringComparison.Ordinal);
        var refused = await updates.UpdateAsync(Ct);
        Assert.Equal(UpdateError.Unavailable, refused.Failure.Code);

        using var unknown = Updates(target: null);
        Assert.Equal((false, false), ((await unknown.GetAsync(Ct)).Available, (await unknown.GetAsync(Ct)).Supported));
    }

    [Fact]
    public async Task A_bundle_whose_programs_do_not_start_changes_nothing()
    {
        await StartAsync();
        await File.WriteAllTextAsync(Path.Combine(ServerDirectory, "build-info.txt"), "Version: 0.1.0", Ct);
        DropBundle("galdrun-server-bundle-v0.2.0-windows-x64.zip");
        using var updates = Updates();
        Assert.True((await updates.GetAsync(Ct)).Supported);

        var failed = await updates.UpdateAsync(Ct);
        Assert.Equal(UpdateError.Failed, failed.Failure.Code);
        Assert.Contains("the new server does not start here", failed.Failure.Detail, StringComparison.Ordinal);
        Assert.Equal("old server", await File.ReadAllTextAsync(Path.Combine(ServerDirectory, "galdrun-server"), Ct));
        Assert.False(Directory.Exists(Path.Combine(ServerDirectory, ".update", "staged")));
        Assert.DoesNotContain(_core.All, r => r.Command == "core.restart");
        Assert.False(updates.RestartRequested);
        Assert.False(_lifetime.Stopped);
    }

    [Fact]
    public async Task Nothing_newer_is_refused()
    {
        await StartAsync();
        await File.WriteAllTextAsync(Path.Combine(ServerDirectory, "build-info.txt"), "Version: 0.1.0", Ct);
        DropBundle("galdrun-server-bundle-v0.1.0-windows-x64.zip");
        using var updates = Updates();

        Assert.False((await updates.GetAsync(Ct)).Available);
        Assert.Equal(UpdateError.Conflict, (await updates.UpdateAsync(Ct)).Failure.Code);
    }

    private sealed class Lifetime : IHostApplicationLifetime
    {
        public bool Stopped { get; private set; }
        public CancellationToken ApplicationStarted => CancellationToken.None;
        public CancellationToken ApplicationStopping => CancellationToken.None;
        public CancellationToken ApplicationStopped => CancellationToken.None;
        public void StopApplication() => Stopped = true;
    }
}
