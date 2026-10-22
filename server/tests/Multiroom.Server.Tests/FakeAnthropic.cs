using System.Collections.Concurrent;
using System.Text.Json.Nodes;
using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Http;

namespace Multiroom.Server.Tests;

// Anthropic's Messages API in process: GET models and POST messages under
// /v1/, the key in x-api-key and the version header checked as Anthropic
// checks them, and the answers the test scripts.
internal sealed class FakeAnthropic : IAsyncDisposable
{
    public const string Key = "sk-ant-fixture-0123456789";

    private readonly WebApplication _app;

    private FakeAnthropic(WebApplication app, Uri baseUri)
    {
        _app = app;
        BaseUri = new Uri(baseUri, "v1/");
    }

    public Uri BaseUri { get; }

    // Every messages body, in order.
    public ConcurrentQueue<JsonNode> Requests { get; } = new();

    public ConcurrentQueue<string> Answers { get; } = new();

    public static async Task<FakeAnthropic> StartAsync()
    {
        FakeAnthropic? fake = null;
        var (app, uri) = await FixtureHttp.StartAsync(context => fake!.HandleAsync(context));
        fake = new FakeAnthropic(app, uri);
        return fake;
    }

    public ValueTask DisposeAsync() => _app.DisposeAsync();

    private async Task HandleAsync(HttpContext context)
    {
        var request = context.Request;
        var response = context.Response;
        if (request.Headers["x-api-key"].ToString() != Key)
        {
            await Answer(response, 401, """{"type":"error","error":{"type":"authentication_error","message":"invalid x-api-key"}}""");
            return;
        }
        if (request.Headers["anthropic-version"].ToString() != "2023-06-01")
        {
            await Answer(response, 400, """{"type":"error","error":{"type":"invalid_request_error","message":"anthropic-version: header is required"}}""");
            return;
        }

        switch (request.Method, request.Path.Value)
        {
            case ("GET", "/v1/models") when request.Query["limit"] == "1000":
                await Answer(response, 200, """
                    {"data":[{"type":"model","id":"claude-a","display_name":"Claude A","created_at":"2026-01-01T00:00:00Z"},
                             {"type":"model","id":"claude-b","display_name":"Claude B","created_at":"2026-01-01T00:00:00Z"}],
                     "has_more":false,"first_id":"claude-a","last_id":"claude-b"}
                    """);
                break;
            case ("POST", "/v1/messages"):
                Requests.Enqueue((await JsonNode.ParseAsync(request.Body))!);
                await Answer(response, 200, Answers.TryDequeue(out var answer) ? answer : """
                    {"id":"msg_1","type":"message","role":"assistant","content":[{"type":"text","text":"ok"}],
                     "stop_reason":"end_turn","usage":{"input_tokens":1,"output_tokens":1}}
                    """);
                break;
            default:
                await Answer(response, 404, """{"type":"error","error":{"type":"not_found_error","message":"Not found"}}""");
                break;
        }
    }

    private static Task Answer(HttpResponse response, int status, string json)
    {
        response.StatusCode = status;
        response.ContentType = "application/json";
        return response.WriteAsync(json);
    }
}
