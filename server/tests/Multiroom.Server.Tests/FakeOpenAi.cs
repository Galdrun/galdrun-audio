using System.Collections.Concurrent;
using System.Text.Json.Nodes;
using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Http;

namespace Multiroom.Server.Tests;

// A vendor that speaks Chat Completions, in process: GET models and POST
// chat/completions under /v1/, a bearer key checked as OpenAI checks it (and
// echoed in the refusal, as OpenAI does), and the answers the test scripts.
// Started with ModelPrefix "models/", it lists models as Google's Gemini
// endpoint does: it stands in for FakeGemini.
internal sealed class FakeOpenAi : IAsyncDisposable
{
    public const string Key = "sk-fixture-0123456789";

    private readonly WebApplication _app;

    private FakeOpenAi(WebApplication app, Uri baseUri, string modelPrefix)
    {
        _app = app;
        BaseUri = new Uri(baseUri, "v1/");
        ModelPrefix = modelPrefix;
    }

    public Uri BaseUri { get; }

    public string ModelPrefix { get; }

    // Which bearer it takes instead of Key: a login's live access token (phase 22).
    public Func<string, bool>? Accepts { get; set; }

    // A local server (Ollama) takes no key.
    public bool RequireKey { get; set; } = true;

    public bool RateLimited { get; set; }

    // Never answers: the request is held until the caller gives up.
    public bool Hang { get; set; }

    // Answers chat/completions with this status and body instead, the next
    // RefuseTimes requests (every one by default).
    public (int Status, string Body)? Refusal { get; set; }

    public int RefuseTimes { get; set; } = int.MaxValue;

    // Every chat/completions body, in order.
    public ConcurrentQueue<JsonNode> Requests { get; } = new();

    // Every audio/transcriptions form: its model, language and file's length.
    public ConcurrentQueue<(string? Model, string? Language, long Bytes)> Transcriptions { get; } = new();

    // The next chat/completions answers; with none, a plain "ok".
    public ConcurrentQueue<string> Answers { get; } = new();

    public static async Task<FakeOpenAi> StartAsync(string modelPrefix = "")
    {
        FakeOpenAi? fake = null;
        var (app, uri) = await FixtureHttp.StartAsync(context => fake!.HandleAsync(context));
        fake = new FakeOpenAi(app, uri, modelPrefix);
        return fake;
    }

    public ValueTask DisposeAsync() => _app.DisposeAsync();

    private async Task HandleAsync(HttpContext context)
    {
        var request = context.Request;
        var response = context.Response;
        var authorization = request.Headers.Authorization.ToString();
        if (RequireKey && !(Accepts is { } accepts
                ? authorization.StartsWith("Bearer ", StringComparison.Ordinal) && accepts(authorization[7..])
                : authorization == $"Bearer {Key}"))
        {
            var sent = authorization.StartsWith("Bearer ", StringComparison.Ordinal) ? authorization[7..] : "";
            await Answer(response, 401, $$$"""
                {"error":{"message":"Incorrect API key provided: {{{sent}}}. You can find your API key at https://platform.openai.com/account/api-keys.","type":"invalid_request_error","code":"invalid_api_key"}}
                """);
            return;
        }
        if (RateLimited)
        {
            await Answer(response, 429, """{"error":{"message":"Rate limit reached for requests","type":"requests"}}""");
            return;
        }
        if (Hang)
        {
            try
            {
                await Task.Delay(Timeout.Infinite, context.RequestAborted);
            }
            catch (OperationCanceledException)
            {
            }
            return;
        }

        switch (request.Method, request.Path.Value)
        {
            case ("GET", "/v1/models"):
                await Answer(response, 200, $$"""
                    {"object":"list","data":[
                      {"id":"{{ModelPrefix}}model-a","object":"model","owned_by":"fixture"},
                      {"id":"{{ModelPrefix}}model-b","object":"model","name":"Model B","owned_by":"fixture"}]}
                    """);
                break;
            case ("POST", "/v1/chat/completions") when Refusal is { } refusal && RefuseTimes > 0:
                RefuseTimes--;
                await Answer(response, refusal.Status, refusal.Body);
                break;
            case ("POST", "/v1/chat/completions"):
                Requests.Enqueue((await JsonNode.ParseAsync(request.Body))!);
                await Answer(response, 200, Answers.TryDequeue(out var answer) ? answer : """
                    {"id":"chatcmpl-1","object":"chat.completion","choices":[{"index":0,
                      "message":{"role":"assistant","content":"ok"},"finish_reason":"stop"}],
                     "usage":{"prompt_tokens":1,"completion_tokens":1,"total_tokens":2}}
                    """);
                break;
            case ("POST", "/v1/audio/transcriptions"):
                var form = await request.ReadFormAsync();
                Transcriptions.Enqueue((form["model"], form["language"], form.Files["file"]?.Length ?? 0));
                await Answer(response, 200, """{"text":" Turn the kitchen up. "}""");
                break;
            default:
                await Answer(response, 404, """{"error":{"message":"Unknown request URL","type":"invalid_request_error"}}""");
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
