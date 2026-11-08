using System.Net;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json.Nodes;
using Microsoft.Extensions.Logging.Abstractions;
using Multiroom.Server.Playback.Core;
using Multiroom.Server.Web.Updates;

namespace Multiroom.Server.Tests;

public sealed class ClientUpdatesTests : IAsyncDisposable
{
    private readonly FakeCore _core = new();
    private readonly FakeClient _client = new();
    private readonly Synchronised _events = new();
    private readonly CoreLink _link;
    private readonly string _directory = Directory.CreateTempSubdirectory("galdrun-client-updates").FullName;
    private readonly ClientUpdates _updates;

    public ClientUpdatesTests()
    {
        _link = new CoreLink(new CoreLinkOptions("127.0.0.1", _core.Port), [_events], NullLogger<CoreLink>.Instance);
        _core.Answer("discovery.clients", _ => new JsonObject
        {
            ["clients"] = new JsonArray(new JsonObject
            {
                ["client"] = "kitchen",
                ["address"] = "127.0.0.1",
                ["controlPort"] = _client.Port,
                ["protocolVersion"] = 2,
                ["present"] = true,
                ["lastSeenMsAgo"] = 500,
                ["firstSeenMsAgo"] = 9000,
                ["beacons"] = 5,
                ["connected"] = true,
            }),
            ["forgotten"] = 0,
        });
        // No release on "GitHub": only the package dropped into the directory.
        _updates = new ClientUpdates(new ClientUpdatesOptions(_directory,
            new Uri($"http://127.0.0.1:{_client.Port}/releases")), _link, NullLogger<ClientUpdates>.Instance);
    }

    private static CancellationToken Ct => TestContext.Current.CancellationToken;

    public async ValueTask DisposeAsync()
    {
        _updates.Dispose();
        await _link.StopAsync(CancellationToken.None);
        _link.Dispose();
        await _core.DisposeAsync();
        _client.Dispose();
        Directory.Delete(_directory, recursive: true);
    }

    private async Task<byte[]> StartWithPackageAsync(string name)
    {
        await _link.StartAsync(Ct);
        await _events.Done.Task.WaitAsync(TimeSpan.FromSeconds(10), Ct);
        var bytes = Encoding.UTF8.GetBytes("not really a tarball");
        await File.WriteAllBytesAsync(Path.Combine(_directory, name), bytes, Ct);
        return bytes;
    }

    [Fact]
    public async Task A_newer_package_is_offered_and_uploaded_with_its_hash()
    {
        var bytes = await StartWithPackageAsync("galdrun-client-v0.2.0-linux-armv7.tar.gz");

        var view = await _updates.GetAsync("kitchen", Ct);
        Assert.True(view.IsSuccess, view.ToString());
        Assert.Equal(("0.1.0", "linux-armv7", "v0.2.0", true), (view.Value.Version, view.Value.Target,
            view.Value.Latest, view.Value.Available));

        var listed = await _updates.ListAsync(Ct);
        Assert.True(listed.IsSuccess, listed.ToString());
        Assert.Equal(("kitchen", true), (Assert.Single(listed.Value).Client, listed.Value[0].Available));

        var updated = await _updates.UpdateAsync("kitchen", Ct);
        Assert.True(updated.IsSuccess, updated.ToString());
        Assert.True(updated.Value.Updating);
        var upload = Assert.Single(_client.Uploads);
        Assert.Equal(bytes, upload.Body);
        Assert.Equal(Convert.ToHexStringLower(SHA256.HashData(bytes)), upload.Sha256);
    }

    [Fact]
    public async Task Nothing_newer_or_an_unsupported_client_is_refused_without_an_upload()
    {
        await StartWithPackageAsync("galdrun-client-v0.1.0-linux-armv7.tar.gz");
        var same = await _updates.UpdateAsync("kitchen", Ct);
        Assert.Equal(ClientUpdateError.Conflict, same.Failure.Code);

        _client.Supported = false;
        var unsupported = await _updates.UpdateAsync("kitchen", Ct);
        Assert.Equal(ClientUpdateError.Unavailable, unsupported.Failure.Code);
        Assert.Contains("RAM", unsupported.Failure.Detail, StringComparison.Ordinal);

        Assert.Equal(ClientUpdateError.NotFound, (await _updates.GetAsync("attic", Ct)).Failure.Code);
        Assert.Empty(_client.Uploads);
    }

    private sealed class Synchronised : ICoreEventSubscriber
    {
        public TaskCompletionSource Done { get; } = new(TaskCreationOptions.RunContinuationsAsynchronously);

        public ValueTask HandleAsync(CoreEvent coreEvent, CancellationToken cancellationToken)
        {
            if (coreEvent is CoreSynchronised)
                Done.TrySetResult();
            return ValueTask.CompletedTask;
        }
    }

    // The client's control port: POST /api answers getUpdate, POST
    // /api/update records the upload, GET /releases is an empty release list.
    private sealed class FakeClient : IDisposable
    {
        private readonly TcpListener _listener = new(IPAddress.Loopback, 0);
        private readonly CancellationTokenSource _stop = new();

        public sealed record Upload(byte[] Body, string? Sha256);

        public List<Upload> Uploads { get; } = [];

        public bool Supported { get; set; } = true;

        public int Port => ((IPEndPoint)_listener.LocalEndpoint).Port;

        public FakeClient()
        {
            _listener.Start();
            _ = Task.Run(ServeAsync);
        }

        public void Dispose()
        {
            _stop.Cancel();
            _listener.Stop();
            _stop.Dispose();
        }

        private async Task ServeAsync()
        {
            while (!_stop.IsCancellationRequested)
            {
                TcpClient connection;
                try
                {
                    connection = await _listener.AcceptTcpClientAsync(_stop.Token);
                }
                catch (Exception e) when (e is OperationCanceledException or SocketException or ObjectDisposedException)
                {
                    return;
                }
                using (connection)
                    await AnswerAsync(connection.GetStream());
            }
        }

        private async Task AnswerAsync(NetworkStream stream)
        {
            var buffer = new List<byte>();
            var chunk = new byte[8192];
            int headerEnd;
            while ((headerEnd = IndexOf(buffer, "\r\n\r\n"u8.ToArray())) < 0)
            {
                var read = await stream.ReadAsync(chunk);
                if (read == 0)
                    return;
                buffer.AddRange(chunk.AsSpan(0, read));
            }
            var head = Encoding.ASCII.GetString([.. buffer.Take(headerEnd)]).Split("\r\n");
            string? Header(string name) => head.Skip(1)
                .Select(l => l.Split(':', 2))
                .FirstOrDefault(p => p.Length == 2 && p[0].Trim().Equals(name, StringComparison.OrdinalIgnoreCase))?[1].Trim();
            var length = int.Parse(Header("Content-Length") ?? "0", System.Globalization.CultureInfo.InvariantCulture);
            while (buffer.Count < headerEnd + 4 + length)
            {
                var read = await stream.ReadAsync(chunk);
                if (read == 0)
                    return;
                buffer.AddRange(chunk.AsSpan(0, read));
            }
            var body = buffer.Skip(headerEnd + 4).Take(length).ToArray();
            var target = head[0].Split(' ')[1];

            var answer = target switch
            {
                "/releases" => "[]",
                "/api/update" => Record(body, Header("X-Galdrun-Sha256")),
                _ => Supported
                    ? """{"ok":true,"command":"getUpdate","version":"0.1.0","target":"linux-armv7","supported":true,"trial":false,"updating":false}"""
                    : """{"ok":true,"command":"getUpdate","version":"0.1.0","target":"linux-armv7","supported":false,"reason":"the client runs from RAM"}""",
            };
            var bytes = Encoding.UTF8.GetBytes(answer);
            await stream.WriteAsync(Encoding.ASCII.GetBytes(
                $"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: {bytes.Length}\r\nConnection: close\r\n\r\n"));
            await stream.WriteAsync(bytes);
        }

        private string Record(byte[] body, string? sha256)
        {
            Uploads.Add(new Upload(body, sha256));
            return """{"ok":true,"command":"update","updating":true}""";
        }

        private static int IndexOf(List<byte> data, byte[] pattern)
        {
            for (var i = 0; i + pattern.Length <= data.Count; i++)
            {
                var match = true;
                for (var j = 0; j < pattern.Length && match; j++)
                    match = data[i + j] == pattern[j];
                if (match)
                    return i;
            }
            return -1;
        }
    }
}
