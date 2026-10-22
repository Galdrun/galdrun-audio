using System.Net;
using System.Text;
using Avalonia.Controls;
using Avalonia.Headless.XUnit;
using Avalonia.Threading;
using Avalonia.VisualTree;
using Multiroom.App.Api;

namespace Multiroom.App.Tests;

public sealed class ServersTests
{
    internal static readonly IPAddress Address = IPAddress.Parse("192.0.2.50");

    internal static DiscoveredServer Server(string host = "studio.local", int port = 4725,
        string? version = "1.2.3", bool token = false) =>
        new($"Galdrun on {host.Split('.')[0]}._galdrun._tcp.local", host, Address, port, version, token);

    // --- the mDNS wire, as the server's ServiceAnnouncer writes it ------------

    private static void Name(List<byte> packet, string name)
    {
        foreach (var label in name.Split('.'))
        {
            packet.Add((byte)label.Length);
            packet.AddRange(Encoding.ASCII.GetBytes(label));
        }
        packet.Add(0);
    }

    private static byte[] NameBytes(string name)
    {
        var bytes = new List<byte>();
        Name(bytes, name);
        return [.. bytes];
    }

    private static byte[] Response(ushort id, string instance, string host, int port, IPAddress address,
        string txt = "txtvers=1|version=0.1.0|path=/api/v1/|auth=none", uint ttl = 120)
    {
        var srv = new List<byte> { 0, 0, 0, 0, (byte)(port >> 8), (byte)port };
        Name(srv, host);
        var text = new List<byte>();
        foreach (var entry in txt.Split('|'))
        {
            text.Add((byte)entry.Length);
            text.AddRange(Encoding.ASCII.GetBytes(entry));
        }
        var packet = new List<byte>();
        void Write(int value) { packet.Add((byte)(value >> 8)); packet.Add((byte)value); }
        void Record(string name, int type, byte[] data)
        {
            Name(packet, name);
            Write(type); Write(0x0001);
            packet.AddRange([(byte)(ttl >> 24), (byte)(ttl >> 16), (byte)(ttl >> 8), (byte)ttl]);
            Write(data.Length);
            packet.AddRange(data);
        }
        Write(id); Write(0x8400); Write(0); Write(1); Write(0); Write(3); // a response, one answer, three additionals
        Record(ServerDiscovery.ServiceType, 12, NameBytes(instance)); // PTR
        Record(instance, 33, [.. srv]); // SRV
        Record(instance, 16, [.. text]); // TXT
        Record(host, 1, address.GetAddressBytes()); // A
        return [.. packet];
    }

    [Fact]
    public void The_announcer_packet_becomes_one_server()
    {
        var id = (ushort)0x4725;
        var packet = Response(id, "Galdrun on studio._galdrun._tcp.local", "studio.local", 4725, IPAddress.Parse("192.168.4.9"));
        var server = Assert.Single(ServerDiscovery.ParseResponse(packet, id));
        Assert.Equal("Galdrun on studio._galdrun._tcp.local", server.Instance);
        Assert.Equal("studio", server.Name);
        Assert.Equal("studio.local", server.Host);
        Assert.Equal(IPAddress.Parse("192.168.4.9"), server.Address);
        Assert.Equal(4725, server.Port);
        Assert.Equal("0.1.0", server.Version);
        Assert.False(server.TokenRequired);
        Assert.Equal(new Uri("http://192.168.4.9:4725/"), server.BaseUri);
        Assert.Equal("192.168.4.9:4725", server.Endpoint);
        Assert.Equal("studio.local · version 0.1.0", server.Detail);
    }

    [Fact]
    public void A_token_announcement_is_carried()
    {
        var packet = Response(7, "Galdrun on nas._galdrun._tcp.local", "nas.local", 4726, Address, "txtvers=1|auth=token");
        var server = Assert.Single(ServerDiscovery.ParseResponse(packet, 7));
        Assert.True(server.TokenRequired);
        Assert.Null(server.Version);
    }

    [Fact]
    public void Foreign_ids_and_truncated_packets_are_dropped()
    {
        var packet = Response(1, "Galdrun on a._galdrun._tcp.local", "a.local", 4725, Address);
        Assert.Empty(ServerDiscovery.ParseResponse(packet, 2));
        Assert.Empty(ServerDiscovery.ParseResponse(packet[..^5], 1));
        Assert.Empty(ServerDiscovery.ParseResponse(new byte[20], 1));
    }

    [Fact]
    public void Goodbyes_are_dropped()
    {
        var packet = Response(1, "Galdrun on a._galdrun._tcp.local", "a.local", 4725, Address, ttl: 0);
        Assert.Empty(ServerDiscovery.ParseResponse(packet, 1));
    }

    [Fact]
    public void The_query_asks_once_for_the_service_by_unicast()
    {
        var query = ServerDiscovery.Query(0x1234);
        Assert.Equal(0x1234, (query[0] << 8) | query[1]);
        Assert.Equal(0, query[2]); Assert.Equal(0, query[3]); // a query, no flags
        Assert.Equal(1, (query[4] << 8) | query[5]); // one question
        Assert.Equal(12 + Branding.ServiceType.Length + 2 + 4, query.Length); // header, name, type and class
        var name = query[12..^4];
        var labels = new List<string>();
        for (var i = 0; name[i] != 0; i += 1 + name[i]) labels.Add(Encoding.ASCII.GetString(name, i + 1, name[i]));
        Assert.Equal(Branding.ServiceType, string.Join('.', labels));
        Assert.Equal(0x000C, (query[^4] << 8) | query[^3]); // PTR
        Assert.Equal(0x8001, (query[^2] << 8) | query[^1]); // IN with the unicast-response bit
    }

    // --- the picker view model -------------------------------------------------

    private static async IAsyncEnumerable<DiscoveredServer> Feed(params DiscoveredServer[] servers)
    {
        foreach (var server in servers) yield return server;
    }

    private static async IAsyncEnumerable<DiscoveredServer> Hang(Task wait)
    {
        await wait;
        yield break;
    }

    private static async IAsyncEnumerable<DiscoveredServer> Spy(IAsyncEnumerable<DiscoveredServer> feed, Action each)
    {
        await foreach (var server in feed) { each(); yield return server; }
    }

    [Fact]
    public async Task A_browse_fills_the_list()
    {
        var first = Server(); var second = Server("attic.local", 4726);
        var vm = new ServersViewModel(_ => Task.CompletedTask, (_, _) => Feed(first, second));
        await vm.ScanCommand.ExecuteAsync(null);
        Assert.Equal([first, second], vm.Servers);
        Assert.True(vm.HasServers);
        Assert.False(vm.IsScanning);
        Assert.Contains("2 servers", vm.ScanState);
    }

    [Fact]
    public async Task An_answered_server_is_replaced_not_duplicated()
    {
        var rounds = new Queue<IAsyncEnumerable<DiscoveredServer>>(
            [Feed(Server()), Feed(Server() with { Address = IPAddress.Parse("192.0.2.99"), Port = 4800 })]);
        var vm = new ServersViewModel(_ => Task.CompletedTask, (_, _) => rounds.Count > 0 ? rounds.Dequeue() : Feed());
        await vm.ScanCommand.ExecuteAsync(null);
        await vm.ScanCommand.ExecuteAsync(null);
        var only = Assert.Single(vm.Servers);
        Assert.Equal(IPAddress.Parse("192.0.2.99"), only.Address);
        Assert.Equal(4800, only.Port);
    }

    [Fact]
    public async Task Scanning_again_flushes_the_list_and_the_warning()
    {
        var connected = Server();
        var gate = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var rounds = new Queue<IAsyncEnumerable<DiscoveredServer>>([Feed(connected), Hang(gate.Task)]);
        var vm = new ServersViewModel(_ => Task.CompletedTask, (_, _) => rounds.Count > 0 ? rounds.Dequeue() : Feed());
        await vm.ScanCommand.ExecuteAsync(null);
        Assert.Equal(connected, Assert.Single(vm.Servers));
        vm.Warning = "Server studio was not found.";
        var rescan = vm.ScanAsync();
        Assert.Empty(vm.Servers); // flushed the moment the scan starts
        Assert.Equal("", vm.Warning);
        gate.SetResult();
        await rescan;
        Assert.Empty(vm.Servers); // nothing answered the second time
    }

    [Fact]
    public async Task Find_stops_at_the_stored_id()
    {
        var wanted = Server("attic.local", 4726);
        var seen = 0;
        var vm = new ServersViewModel(_ => Task.CompletedTask,
            (_, _) => Spy(Feed(Server(), wanted, Server("basement.local")), () => seen++));
        Assert.Equal(wanted, await vm.FindAsync(wanted.Instance, TimeSpan.FromSeconds(1)));
        Assert.Equal(2, seen); // stopped right at the match; a third server was never pulled
        Assert.Equal(2, vm.Servers.Count);
    }

    [Fact]
    public async Task Find_without_a_match_returns_null()
    {
        var vm = new ServersViewModel(_ => Task.CompletedTask, (_, _) => Feed(Server()));
        Assert.Null(await vm.FindAsync("Galdrun on elsewhere._galdrun._tcp.local", TimeSpan.FromSeconds(1)));
    }

    [Fact]
    public async Task Connecting_uses_the_picked_server_only()
    {
        var picked = new List<DiscoveredServer>();
        var first = Server(); var second = Server("attic.local", 4726);
        var vm = new ServersViewModel(s => { picked.Add(s); return Task.CompletedTask; }, (_, _) => Feed(first, second));
        await vm.ScanCommand.ExecuteAsync(null);
        await vm.ConnectServerCommand.ExecuteAsync(second);
        await vm.ConnectServerCommand.ExecuteAsync(null); // a cleared parameter connects nothing
        Assert.Equal([second], picked);
    }

    // --- startup ----------------------------------------------------------------

    private const string Status =
        """{"version":"0.1","core":{"connected":true,"connections":1,"zones":1,"rooms":1,"queuedEvents":0,"droppedEvents":0},"media":{},"library":{},"providers":[],"addIns":{},"events":{"subscribers":0,"held":0,"dropped":0}}""";

    private static string TempPath() => Path.Combine(Path.GetTempPath(), "galdrun-servers-" + Guid.NewGuid().ToString("N"));

    private static string SettingsPath()
    {
        var path = Path.Combine(TempPath(), "settings.json");
        System.IO.Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        return path;
    }

    // The shell saves preferences again on its first zone snapshot, slightly
    // after InitializeAsync returns; the folder goes away once that settles.
    private static async Task CleanupAsync(string path)
    {
        var directory = Path.GetDirectoryName(path)!;
        for (var attempt = 0; ; attempt++)
            try
            {
                if (System.IO.Directory.Exists(directory)) System.IO.Directory.Delete(directory, recursive: true);
                return;
            }
            catch (IOException) when (attempt < 500)
            { await Task.Delay(10, TestContext.Current.CancellationToken); }
    }

    private static ShellViewModel Shell(string path,
        Func<TimeSpan, CancellationToken, IAsyncEnumerable<DiscoveredServer>> discovery,
        Func<string, string?, MultiroomClient>? client = null) => new(discovery, path, client);

    private static MultiroomClient ConnectedClient(string address, FixtureEventStream events, List<Uri> bases) =>
        new(address, handler: new FakeHttp((request, _) =>
        {
            var uri = request.RequestUri!;
            bases.Add(uri);
            return Task.FromResult(uri.AbsolutePath switch
            {
                "/api/v1/events" => Events(events),
                "/api/v1/status" => FakeHttp.Json(Status),
                "/api/v1/providers" => FakeHttp.Json("[]"),
                "/api/v1/favorites" => FakeHttp.Json("[]"),
                "/api/v1/zones" => BrowserTests.Reply(PlaybackTests.Snapshot().Zones),
                "/api/v1/rooms" => FakeHttp.Json("[]"),
                _ => throw new InvalidOperationException("Unexpected request " + uri)
            });
        }));

    private static HttpResponseMessage Events(FixtureEventStream stream)
    {
        var content = new StreamContent(stream);
        content.Headers.ContentType = new("text/event-stream");
        return new HttpResponseMessage(HttpStatusCode.OK) { Content = content };
    }

    [Fact]
    public async Task Older_settings_without_a_server_id_still_load()
    {
        var path = SettingsPath();
        try
        {
            await File.WriteAllTextAsync(path, """{"Address":"http://music:4725/api/v1/","ActiveZone":"living"}""",
                TestContext.Current.CancellationToken);
            var preferences = await Preferences.LoadAsync(path, TestContext.Current.CancellationToken);
            Assert.Equal(("http://music:4725/api/v1/", "living", null),
                (preferences!.Address, preferences.ActiveZone, preferences.ServerId));
        }
        finally { await CleanupAsync(path); }
    }

    [Fact]
    public async Task Without_a_stored_server_startup_opens_the_picker()
    {
        var path = SettingsPath();
        try
        {
            var vm = Shell(path, (_, _) => Feed());
            await vm.InitializeAsync();
            Assert.Equal("Servers", vm.SelectedPage);
            Assert.False(vm.Servers.IsScanning);
            Assert.Contains("No servers", vm.Servers.ScanState);
        }
        finally { await CleanupAsync(path); }
    }

    [AvaloniaFact]
    public async Task A_stored_server_id_is_resolved_and_connected_by_id()
    {
        var path = SettingsPath();
        try
        {
            var server = Server();
            // The stored address is stale on purpose: the announcement wins.
            await new Preferences("http://192.0.2.4:4725/api/v1/", null, server.Instance)
                .SaveAsync(path, TestContext.Current.CancellationToken);
            var bases = new List<Uri>();
            var vm = Shell(path, (_, _) => Feed(server), (address, _) => ConnectedClient(address, new(), bases));
            await vm.InitializeAsync();
            Assert.True(vm.Connected);
            Assert.Equal("Home", vm.SelectedPage);
            Assert.True(vm.Home.NoFavourites);
            Assert.All(bases, uri => Assert.StartsWith("http://192.0.2.50:4725/api/v1/", uri.AbsoluteUri));
            var saved = await Preferences.LoadAsync(path, TestContext.Current.CancellationToken);
            Assert.Equal((server.Instance, "http://192.0.2.50:4725/api/v1/"), (saved!.ServerId, saved.Address));
            await vm.DisconnectAsync();
        }
        finally { await CleanupAsync(path); }
    }

    [AvaloniaFact]
    public async Task A_missing_announcement_falls_back_to_the_stored_address_and_keeps_the_id()
    {
        var path = SettingsPath();
        try
        {
            var server = Server();
            await new Preferences("http://192.0.2.4:4725/api/v1/", null, server.Instance)
                .SaveAsync(path, TestContext.Current.CancellationToken);
            var bases = new List<Uri>();
            var vm = Shell(path, (_, _) => Feed(), (address, _) => ConnectedClient(address, new(), bases));
            await vm.InitializeAsync();
            Assert.True(vm.Connected);
            Assert.All(bases, uri => Assert.StartsWith("http://192.0.2.4:4725/api/v1/", uri.AbsoluteUri));
            Assert.Equal(server.Instance, (await Preferences.LoadAsync(path, TestContext.Current.CancellationToken))!.ServerId);
            await vm.DisconnectAsync();
        }
        finally { await CleanupAsync(path); }
    }

    [AvaloniaFact]
    public async Task An_unreachable_stored_server_leaves_the_picker_with_an_error()
    {
        var path = SettingsPath();
        try
        {
            await new Preferences("http://192.0.2.4:4725/api/v1/", null, Server().Instance)
                .SaveAsync(path, TestContext.Current.CancellationToken);
            var vm = Shell(path, (_, _) => Feed(), (_, _) => new MultiroomClient("http://unreachable", handler:
                new FakeHttp((_, _) => Task.FromException<HttpResponseMessage>(new HttpRequestException("gone")))));
            await vm.InitializeAsync();
            Assert.False(vm.Connected);
            Assert.Equal("Servers", vm.SelectedPage);
            Assert.Equal("Server studio was not found.", vm.Servers.Warning);
            Assert.NotEqual("", vm.Error);
            await vm.DisconnectAsync();
        }
        finally { await CleanupAsync(path); }
    }

    [AvaloniaTheory]
    [InlineData(390, 844)] [InlineData(900, 850)] [InlineData(1440, 950)]
    public void The_picker_is_a_plain_page_with_per_server_connect(int width, int height)
    {
        var model = new ShellViewModel((_, _) => Feed(), Path.Combine(TempPath(), "settings.json"));
        var view = new MainView(model, loadPreferences: false);
        var window = new Window { Width = width, Height = height, Content = view };
        try
        {
            window.Show(); Dispatcher.UIThread.RunJobs();
            model.Servers.Servers.Add(Server()); model.Servers.Servers.Add(Server("attic.local", 4726));
            model.Servers.Warning = "Server studio was not found.";
            model.SelectedPage = "Server"; window.UpdateLayout(); Dispatcher.UIThread.RunJobs();
            // The manual page shares the plain fullscreen chrome.
            var serverVisible = view.GetVisualDescendants().OfType<Control>().Where(c => c.IsEffectivelyVisible).ToArray();
            Assert.DoesNotContain(serverVisible, c => c == view.FindControl<Button>("HamburgerButton"));
            Assert.Contains(serverVisible.OfType<TextBlock>(), t => t.Text == "GALDRUN");
            Assert.Contains(serverVisible.OfType<TextBox>(), t => t.Name == "ServerAddress" || (t.Text ?? "") == model.Address);
            var choose = view.GetVisualDescendants().OfType<Button>().Single(b => Equals(b.Content, "Choose server"));
            Assert.Equal(model.NavigateCommand, choose.Command);
            Assert.Equal("Servers", choose.CommandParameter);
            model.SelectedPage = "Servers"; window.UpdateLayout(); Dispatcher.UIThread.RunJobs();
            var visible = view.GetVisualDescendants().OfType<Control>().Where(c => c.IsEffectivelyVisible).ToArray();
            // The top holds only the centered brand, no hamburger or icons, no Refresh.
            Assert.DoesNotContain(visible, c => c == view.FindControl<Button>("HamburgerButton"));
            Assert.DoesNotContain(visible, b => b is Button && (Equals(((Button)b).Content, "Refresh") ||
                Equals(((Button)b).Content, "Search") || Equals(((Button)b).Content, "Queue") ||
                Equals(((Button)b).Content, "Now Playing")));
            Assert.Contains(visible.OfType<TextBlock>(), t => t.Text == "GALDRUN");
            Assert.DoesNotContain(visible.OfType<TextBlock>(), t => t.Text == "Servers");
            // The not-found warning sits above the list, centered.
            var warning = Assert.Single(visible.OfType<TextBlock>(), t => t.Text == "Server studio was not found.");
            var list = Assert.Single(visible.OfType<ListBox>());
            Assert.True(warning.Bounds.Center.Y < list.Bounds.Top);
            // Each found server has its own Connect button carrying the server itself.
            var studio = model.Servers.Servers[0]; var attic = model.Servers.Servers[1];
            var connect = visible.OfType<Button>().Where(b => Equals(b.Content, "Connect")).ToArray();
            Assert.Equal(2, connect.Length);
            Assert.Contains(connect, b => b.Command == model.Servers.ConnectServerCommand && Equals(b.CommandParameter, studio));
            Assert.Contains(connect, b => b.Command == model.Servers.ConnectServerCommand && Equals(b.CommandParameter, attic));
            Assert.Contains(visible.OfType<TextBlock>(), t => t.Text == "studio");
            Assert.Contains(visible.OfType<TextBlock>(), t => t.Text == "192.0.2.50:4725");
            Assert.Contains(visible.OfType<ListBox>(), l => l.Bounds.Height > 100);
            // The way out without a server is still there.
            Assert.Contains(visible.OfType<Button>(), b => Equals(b.Content, "Manual") &&
                b.Command == model.NavigateCommand && Equals(b.CommandParameter, "Server"));
        }
        finally { window.Close(); }
    }
}
