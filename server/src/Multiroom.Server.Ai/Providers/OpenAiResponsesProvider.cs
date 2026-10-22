using System.Net.Http.Headers;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using Multiroom.Server.Abstractions;

namespace Multiroom.Server.Ai.Providers;

// OpenAI's Responses wire format, a format of
// its own and so a base class of its own: the system prompt as
// `instructions`, messages and tool results as `input` items, tools as
// `function` tools, `function_call` items back as tool calls. The answer is
// asked as a server-sent event stream (`store: false, stream: true`, as the
// ChatGPT plan requires) and read to its `response.completed`, so the
// provider still answers one AiResponse: nothing above it streams.
public abstract class OpenAiResponsesProvider(AiProviderContext context) : IAiProvider
{
    // What one answer's stream may hold, and one event of it (rule 6).
    public const int MaxStreamChars = 16 * 1024 * 1024;
    public const int MaxEventChars = 4 * 1024 * 1024;

    public abstract string Id { get; }

    public abstract string DisplayName { get; }

    public virtual IReadOnlyList<AiAuthMethod> AuthMethods => [AiAuthMethod.ApiKey];

    // reasoning: {effort} (phase 29); the web_search tool (phase 27).
    public virtual IReadOnlyList<AiCapability> Capabilities => [AiCapability.Reasoning, AiCapability.WebSearch];

    protected Uri BaseUrl => context.BaseUrl ?? DefaultBaseUrl;

    protected abstract Uri DefaultBaseUrl { get; }

    // Whether max_output_tokens is sent. A route that takes only what it
    // documents (the ChatGPT plan's) leaves it out.
    protected virtual bool SendsMaxTokens => true;

    // What the status says when there is nothing to call with.
    protected virtual string Unconfigured => $"no API key; set one with PUT /api/v1/ai/providers/{Id}/key";

    public Task<AiProviderStatus> GetStatusAsync(CancellationToken cancellationToken) =>
        Task.FromResult(context.HasCredential ? new AiProviderStatus(true) : new AiProviderStatus(false, Unconfigured));

    public async Task<Result<IReadOnlyList<AiModel>, AiError>> ListModelsAsync(CancellationToken cancellationToken)
    {
        var key = await KeyAsync(cancellationToken).ConfigureAwait(false);
        if (!key.IsSuccess)
            return key.Failure;
        using var request = new HttpRequestMessage(HttpMethod.Get, new Uri(BaseUrl, "models"));
        request.Headers.Authorization = new AuthenticationHeaderValue("Bearer", key.Value);
        var answer = await AiHttp.SendAsync(context.Http, request, key.Value, cancellationToken).ConfigureAwait(false);
        if (!answer.IsSuccess)
            return Result.Fail(answer.Failure.Code, $"{Id}: {answer.Failure.Detail}");
        return ModelsOf(answer.Value).ToList();
    }

    public async Task<Result<AiResponse, AiError>> CompleteAsync(AiRequest request,
        CancellationToken cancellationToken)
    {
        var key = await KeyAsync(cancellationToken).ConfigureAwait(false);
        if (!key.IsSuccess)
            return key.Failure;
        using var message = new HttpRequestMessage(HttpMethod.Post, new Uri(BaseUrl, "responses"))
        {
            Content = AiHttp.Body(RequestOf(request)),
        };
        message.Headers.Authorization = new AuthenticationHeaderValue("Bearer", key.Value);
        message.Headers.Accept.Add(new MediaTypeWithQualityHeaderValue("text/event-stream"));
        var answer = await StreamAsync(message, key.Value, cancellationToken).ConfigureAwait(false);
        if (!answer.IsSuccess)
            return Result.Fail(answer.Failure.Code, $"{Id}: {answer.Failure.Detail}");
        var response = ResponseOf(answer.Value);
        return response.IsSuccess ? response : Result.Fail(response.Failure.Code, $"{Id}: {response.Failure.Detail}");
    }

    // GET models: the vendor's `data`, by id.
    protected virtual IEnumerable<AiModel> ModelsOf(JsonElement answer) =>
        answer.Items("data").Where(m => m.Text("id") is { Length: > 0 }).Select(m => AiModel.Named(m.Text("id")!));

    // The AiError a vendor's error code means, where it means more than its
    // HTTP status says; null leaves the status to decide.
    protected virtual Failure<AiError>? FailureOf(string code) => code switch
    {
        "rate_limit_exceeded" => Result.Fail(AiError.RateLimited, code),
        "server_error" => Result.Fail(AiError.Unavailable, code),
        _ => null,
    };

    private async Task<Result<string, AiError>> KeyAsync(CancellationToken cancellationToken)
    {
        var credential = await context.CredentialAsync(cancellationToken).ConfigureAwait(false);
        if (!credential.IsSuccess)
            return Result.Fail(credential.Failure.Code, $"{Id}: {credential.Failure.Detail}");
        return credential.Value is { } key ? key : Result.Fail(AiError.NotConfigured, $"{Id}: {Unconfigured}");
    }

    private JsonObject RequestOf(AiRequest request)
    {
        var input = new JsonArray();
        foreach (var message in request.Messages)
        {
            switch (message.Role)
            {
                case AiRole.User:
                    input.Add(new JsonObject { ["role"] = "user", ["content"] = message.Text ?? "" });
                    break;
                case AiRole.Assistant:
                    if (!string.IsNullOrEmpty(message.Text))
                        input.Add(new JsonObject { ["role"] = "assistant", ["content"] = message.Text });
                    foreach (var call in message.ToolCalls ?? [])
                    {
                        input.Add(new JsonObject
                        {
                            ["type"] = "function_call",
                            ["call_id"] = call.Id,
                            ["name"] = call.Name,
                            ["arguments"] = call.Arguments.GetRawText(),
                        });
                    }
                    break;
                default:
                    input.Add(new JsonObject
                    {
                        ["type"] = "function_call_output",
                        ["call_id"] = message.ToolCallId,
                        ["output"] = message.Text ?? "",
                    });
                    break;
            }
        }

        var body = new JsonObject { ["model"] = request.Model, ["input"] = input };
        if (request.System is not null)
            body["instructions"] = request.System;
        if (request.Tools is { Count: > 0 } tools)
        {
            body["tools"] = new JsonArray([.. tools.Select(t => (JsonNode)new JsonObject
            {
                ["type"] = "function",
                ["name"] = t.Name,
                ["description"] = t.Description,
                ["parameters"] = JsonNode.Parse(t.Parameters.GetRawText()),
            })]);
        }
        // The vendor's own search beside the functions (phase 27).
        if (request.WebSearch)
        {
            var all = body["tools"] as JsonArray ?? [];
            all.Add(new JsonObject { ["type"] = "web_search" });
            body["tools"] = all;
        }
        if (request.RequiredTool is { } required)
            body["tool_choice"] = new JsonObject { ["type"] = "function", ["name"] = required };
        if (request.Reasoning != AiReasoning.Default)
        {
            body["reasoning"] = new JsonObject
            {
                ["effort"] = request.Reasoning switch
                {
                    AiReasoning.Off => "none",
                    AiReasoning.Low => "low",
                    AiReasoning.Medium => "medium",
                    _ => "high",
                },
            };
        }
        if (SendsMaxTokens)
            body["max_output_tokens"] = request.MaxTokens;
        body["store"] = false;
        body["stream"] = true;
        return body;
    }

    // The stream read to its final event: the response object of
    // response.completed (or response.incomplete). A stream that ends
    // before it, or breaks, is InvalidAnswer.
    private async Task<Result<JsonElement, AiError>> StreamAsync(HttpRequestMessage message, string key,
        CancellationToken cancellationToken)
    {
        HttpResponseMessage response;
        try
        {
            response = await context.Http.SendAsync(message, HttpCompletionOption.ResponseHeadersRead, cancellationToken)
                .ConfigureAwait(false);
        }
        catch (HttpRequestException e)
        {
            return Result.Fail(AiError.Unavailable, $"{message.RequestUri?.Host}: {e.Message}");
        }
        using (response)
        {
            if (!response.IsSuccessStatusCode)
            {
                string text;
                try
                {
                    text = await response.Content.ReadAsStringAsync(cancellationToken).ConfigureAwait(false);
                }
                catch (HttpRequestException e)
                {
                    return Result.Fail(AiError.Unavailable, $"{message.RequestUri?.Host}: {e.Message}");
                }
                var json = AiHttp.Parse(text);
                return Known(json, key) ?? AiHttp.StatusFailure(response.StatusCode, json, key);
            }

            try
            {
                var stream = await response.Content.ReadAsStreamAsync(cancellationToken).ConfigureAwait(false);
                using var reader = new StreamReader(stream, Encoding.UTF8);
                var data = new StringBuilder();
                var total = 0;
                // Each finished output item, in order: the ChatGPT plan's
                // route answers response.completed with an empty output and
                // sends the items only as response.output_item.done.
                var items = new JsonArray();
                while (true)
                {
                    var line = await reader.ReadLineAsync(cancellationToken).ConfigureAwait(false);
                    if (line is not null)
                    {
                        total += line.Length + 1;
                        if (total > MaxStreamChars)
                            return Result.Fail(AiError.InvalidAnswer, $"a stream of more than {MaxStreamChars} characters");
                        if (line.StartsWith("data:", StringComparison.Ordinal))
                        {
                            data.Append(data.Length > 0 ? "\n" : "").Append(line.AsSpan(line.StartsWith("data: ", StringComparison.Ordinal) ? 6 : 5));
                            if (data.Length > MaxEventChars)
                                return Result.Fail(AiError.InvalidAnswer, $"an event of more than {MaxEventChars} characters");
                        }
                        if (line.Length > 0)
                            continue;
                    }
                    // A blank line (or the end) dispatches the event.
                    if (data.Length > 0)
                    {
                        var json = AiHttp.Parse(data.ToString());
                        data.Clear();
                        if (json is not { ValueKind: JsonValueKind.Object } evt)
                            return Result.Fail(AiError.InvalidAnswer, "an event that is not a JSON object");
                        switch (evt.Text("type"))
                        {
                            case "response.output_item.done" when evt.Get("item") is { ValueKind: JsonValueKind.Object } item:
                                items.Add(JsonNode.Parse(item.GetRawText()));
                                break;
                            case "response.completed" or "response.incomplete":
                                if (evt.Get("response") is not { ValueKind: JsonValueKind.Object } done)
                                    return Result.Fail(AiError.InvalidAnswer, "a final event without its response");
                                if (done.Items("output").Any() || items.Count == 0)
                                    return done;
                                var whole = JsonNode.Parse(done.GetRawText())!.AsObject();
                                whole["output"] = items;
                                return JsonSerializer.SerializeToElement(whole);
                            case "response.failed":
                                return Known(evt.Get("response"), key) ??
                                    Result.Fail(AiError.Unavailable, "the response failed");
                            case "error":
                                return Known(evt, key) ?? Result.Fail(AiError.Unavailable, "the stream answered an error");
                        }
                    }
                    if (line is null)
                        return Result.Fail(AiError.InvalidAnswer, "the stream ended before the response was complete");
                }
            }
            catch (Exception e) when (e is HttpRequestException or IOException)
            {
                return Result.Fail(AiError.InvalidAnswer, $"the stream broke before the response was complete: {e.Message}");
            }
        }
    }

    // A vendor error the subclass knows by its code, with the vendor's
    // message where it gives one (the key taken out): nested as
    // {error: {code}}, {detail: {code}}, {code}, or OAuth's {error: "<code>"}.
    private Failure<AiError>? Known(JsonElement? json, string key)
    {
        var detail = json;
        for (var depth = 0; depth < 4 && detail is { } d; depth++)
        {
            if (d.Get("error") is { ValueKind: JsonValueKind.Object } error)
                detail = error;
            else if (d.Get("detail") is { ValueKind: JsonValueKind.Object } inner)
                detail = inner;
            else
                break;
        }
        if (detail is not { } found || (found.Text("code") ?? found.Text("error")) is not { Length: > 0 } code ||
            FailureOf(code) is not { } failure)
        {
            return null;
        }
        var said = found.Text("message") is { Length: > 0 } text
            ? text.Replace(key, "***", StringComparison.Ordinal)
            : null;
        return said is null || said.Length > 200 ? failure : Result.Fail(failure.Code, $"{failure.Detail}: {said}");
    }

    private static Result<AiResponse, AiError> ResponseOf(JsonElement response)
    {
        var texts = new StringBuilder();
        var calls = new List<AiToolCall>();
        var sources = new List<AiSource>();
        var searches = 0;
        foreach (var item in response.Items("output"))
        {
            switch (item.Text("type"))
            {
                case "message":
                    foreach (var part in item.Items("content"))
                    {
                        if (part.Text("type") != "output_text")
                            continue;
                        texts.Append(part.Text("text"));
                        // What a web search cited (phase 27).
                        sources.AddRange(part.Items("annotations")
                            .Where(a => a.Text("type") == "url_citation" && a.Text("url") is { Length: > 0 })
                            .Select(a => new AiSource(a.Text("url")!, a.Text("title"))));
                    }
                    break;
                case "web_search_call":
                    searches++;
                    break;
                case "function_call":
                    var name = item.Text("name");
                    if (string.IsNullOrEmpty(name))
                        return Result.Fail(AiError.InvalidAnswer, "a function_call without a name");
                    if (AiHttp.Parse(item.Text("arguments") is { Length: > 0 } text ? text : "{}") is not
                        { ValueKind: JsonValueKind.Object } arguments)
                    {
                        return Result.Fail(AiError.InvalidAnswer, $"the arguments of function_call {name} are not a JSON object");
                    }
                    var id = item.Text("call_id") is { Length: > 0 } given ? given : $"call_{calls.Count}";
                    calls.Add(new AiToolCall(id, name, arguments));
                    break;
            }
        }

        var stop = calls.Count > 0
            ? AiStopReason.ToolUse
            : response.Text("status") switch
            {
                "completed" => AiStopReason.EndTurn,
                "incomplete" when response.Get("incomplete_details")?.Text("reason") == "max_output_tokens" =>
                    AiStopReason.MaxTokens,
                _ => AiStopReason.Other,
            };
        var usage = response.Get("usage") is { } u
            ? new AiUsage(u.Number("input_tokens"), u.Number("output_tokens"),
                u.Get("output_tokens_details")?.Number("reasoning_tokens") ?? 0, searches)
            : new AiUsage(0, 0, 0, searches);
        return new AiResponse(texts.Length == 0 ? null : texts.ToString(), calls, stop, usage,
            sources.Count > 0 ? [.. sources.DistinctBy(s => s.Url)] : null);
    }
}
