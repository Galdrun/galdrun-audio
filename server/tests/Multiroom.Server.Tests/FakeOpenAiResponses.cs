using System.Collections.Concurrent;
using System.Text.Json;
using System.Text.Json.Nodes;
using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Http;

namespace Multiroom.Server.Tests;

// OpenAI's Responses API as the ChatGPT plan answers it, in process (phase
// 28): GET models in the plan's shape (slug, display_name, visibility) and
// POST responses as a server-sent event stream, a bearer checked by Accepts,
// and the streams the test scripts. With Cut, a stream stops after its first
// event without response.completed.
internal sealed class FakeOpenAiResponses : IAsyncDisposable
{
    private readonly WebApplication _app;

    private FakeOpenAiResponses(WebApplication app, Uri baseUri)
    {
        _app = app;
        BaseUri = new Uri(baseUri, "v1/");
    }

    public Uri BaseUri { get; }

    // Which bearer it takes.
    public Func<string, bool> Accepts { get; set; } = _ => true;

    // Answers responses with this status and body instead, once.
    public (int Status, string Body)? Refusal { get; set; }

    public bool Cut { get; set; }

    // Every responses body, in order.
    public ConcurrentQueue<JsonNode> Requests { get; } = new();

    // The next responses streams, as the events' data; with none, a plain "ok".
    public ConcurrentQueue<IReadOnlyList<string>> Streams { get; } = new();

    public static async Task<FakeOpenAiResponses> StartAsync()
    {
        FakeOpenAiResponses? fake = null;
        var (app, uri) = await FixtureHttp.StartAsync(context => fake!.HandleAsync(context));
        fake = new FakeOpenAiResponses(app, uri);
        return fake;
    }

    public ValueTask DisposeAsync() => _app.DisposeAsync();

    // A stream that completes with this response object: the deltas first,
    // as the API sends them, then response.completed.
    public static IReadOnlyList<string> Completed(string response) =>
    [
        """{"type":"response.created","response":{"id":"resp_1","status":"in_progress","output":[]}}""",
        """{"type":"response.output_text.delta","delta":"partial"}""",
        $$"""{"type":"response.completed","response":{{response}}}""",
    ];

    private async Task HandleAsync(HttpContext context)
    {
        var request = context.Request;
        var response = context.Response;
        var authorization = request.Headers.Authorization.ToString();
        if (!authorization.StartsWith("Bearer ", StringComparison.Ordinal) || !Accepts(authorization[7..]))
        {
            await Answer(response, 401, """{"error":{"message":"Invalid token","type":"invalid_request_error","code":"invalid_token"}}""");
            return;
        }
        switch (request.Method, request.Path.Value)
        {
            case ("GET", "/v1/models"):
                await Answer(response, 200, """
                    {"models":[{"slug":"gpt-plan","display_name":"GPT Plan","visibility":"list"},
                               {"slug":"gpt-hidden","display_name":"Hidden","visibility":"hide"}]}
                    """);
                return;
            case ("POST", "/v1/responses"):
                Requests.Enqueue((await JsonNode.ParseAsync(request.Body))!);
                if (Refusal is { } refusal)
                {
                    Refusal = null;
                    await Answer(response, refusal.Status, refusal.Body);
                    return;
                }
                var events = Streams.TryDequeue(out var scripted) ? scripted : Completed("""
                    {"id":"resp_1","status":"completed","output":[{"type":"message","role":"assistant",
                      "content":[{"type":"output_text","text":"ok"}]}],"usage":{"input_tokens":1,"output_tokens":1}}
                    """);
                response.ContentType = "text/event-stream";
                foreach (var data in Cut ? events.Take(1) : events)
                {
                    var type = JsonElement.Parse(data).GetProperty("type").GetString();
                    // CRLF and one data line per line of the event, as a server may send them.
                    var lines = string.Join("\r\n", data.Split('\n').Select(l => $"data: {l.TrimEnd('\r')}"));
                    await response.WriteAsync($"event: {type}\r\n{lines}\r\n\r\n");
                    await response.Body.FlushAsync();
                }
                return;
            default:
                await Answer(response, 404, """{"error":{"message":"Unknown request URL","type":"invalid_request_error"}}""");
                return;
        }
    }

    private static Task Answer(HttpResponse response, int status, string json)
    {
        response.StatusCode = status;
        response.ContentType = "application/json";
        return response.WriteAsync(json);
    }
}
