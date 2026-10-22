using System.Runtime.CompilerServices;
using System.Text;

namespace Multiroom.App.Api;

public sealed record ServerEvent(string? Id, string Type, string Data);

public sealed class EventStream(MultiroomClient client)
{
    public string? LastEventId { get; private set; }

    public async Task RunAsync(Func<ServerEvent, Task> received, Func<bool, Task> connection,
        CancellationToken ct, TimeSpan? retryDelay = null)
    {
        var attempt = 0;
        while (!ct.IsCancellationRequested)
        {
            try
            {
                using var request = client.Request(HttpMethod.Get, "events");
                request.Headers.Accept.ParseAdd("text/event-stream");
                if (!string.IsNullOrEmpty(LastEventId)) request.Headers.TryAddWithoutValidation("Last-Event-ID", LastEventId);
                using var headerTimeout = CancellationTokenSource.CreateLinkedTokenSource(ct);
                headerTimeout.CancelAfter(TimeSpan.FromSeconds(30));
                using var response = await client.OpenAsync(request, headerTimeout.Token).ConfigureAwait(false);
                if (response.Content.Headers.ContentType?.MediaType != "text/event-stream")
                    throw new IOException("Unexpected event stream content type.");
                await connection(true).ConfigureAwait(false); // Every reconnect re-reads REST, even with successful replay.
                using var stream = await response.Content.ReadAsStreamAsync(ct).ConfigureAwait(false);
                await foreach (var entry in ParseAsync(stream, ct).ConfigureAwait(false))
                {
                    await received(entry).ConfigureAwait(false);
                    if (entry.Id is not null) LastEventId = entry.Id;
                    attempt = 0;
                }
            }
            catch (OperationCanceledException) when (ct.IsCancellationRequested) { break; }
            catch (Exception e) when (e is HttpRequestException or IOException or ApiException or OperationCanceledException) { }
            await connection(false).ConfigureAwait(false);
            await Task.Delay(retryDelay ?? TimeSpan.FromSeconds(Math.Min(30, Math.Pow(2, Math.Min(attempt++, 5)))), ct).ConfigureAwait(false);
        }
    }

    public static async IAsyncEnumerable<ServerEvent> ParseAsync(Stream stream,
        [EnumeratorCancellation] CancellationToken ct = default)
    {
        using var reader = new StreamReader(stream, Encoding.UTF8, true, 4096, leaveOpen: true);
        string? id = null;
        var type = "message";
        var data = new StringBuilder();
        while (await reader.ReadLineAsync(ct).ConfigureAwait(false) is { } line)
        {
            if (line.Length > 1024 * 1024 || data.Length + line.Length > 4 * 1024 * 1024)
                throw new IOException("Event exceeds the controller's bounded event size.");
            if (line.Length == 0)
            {
                if (data.Length > 0) yield return new ServerEvent(id, type, data.ToString(0, data.Length - 1));
                id = null;
                type = "message";
                data.Clear();
                continue;
            }
            if (line[0] == ':') continue;
            var colon = line.IndexOf(':');
            var field = colon < 0 ? line : line[..colon];
            var value = colon < 0 ? "" : line[(colon + 1)..];
            if (value.StartsWith(' ')) value = value[1..];
            switch (field)
            {
                case "id" when !value.Contains('\0'): id = value; break;
                case "event": type = value; break;
                case "data": data.Append(value).Append('\n'); break;
            }
        }
        // An incomplete frame at EOF is discarded; it will be replayed on reconnect.
    }
}
