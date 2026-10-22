using System.Text.Json;
using System.Text.Json.Nodes;
using Microsoft.Extensions.DependencyInjection;
using Multiroom.Server.Abstractions;

namespace Multiroom.Server.Ai.Providers.Anthropic;

public static class AnthropicProviderExtensions
{
    public static IServiceCollection AddAnthropic(this IServiceCollection services) =>
        services.AddAiProvider(AnthropicProvider.ProviderId, c => new AnthropicProvider(c));
}

// Anthropic's Messages API, a wire format of its own: the system prompt
// beside the messages, content as blocks, tool results as blocks of a user
// message, and a key in x-api-key. An API-key provider only (-> Open
// decisions: no subscription login).
public sealed class AnthropicProvider(AiProviderContext context) : IAiProvider
{
    public const string ProviderId = "anthropic";
    public const string ApiVersion = "2023-06-01";

    // The most models one listing asks for; the API's own maximum.
    private const int ModelPage = 1000;

    private static readonly Uri DefaultBaseUrl = new("https://api.anthropic.com/v1/");

    public string Id => ProviderId;

    public string DisplayName => "Anthropic";

    public IReadOnlyList<AiAuthMethod> AuthMethods => [AiAuthMethod.ApiKey];

    public IReadOnlyList<AiCapability> Capabilities => [AiCapability.Reasoning, AiCapability.WebSearch];

    // The searches one web lookup may run (phase 27): each is billed.
    public const int MaxSearches = 5;

    private Uri BaseUrl => context.BaseUrl ?? DefaultBaseUrl;

    public Task<AiProviderStatus> GetStatusAsync(CancellationToken cancellationToken) =>
        Task.FromResult(context.Key() is not null
            ? new AiProviderStatus(true)
            : new AiProviderStatus(false, $"no API key; set one with PUT /api/v1/ai/providers/{Id}/key"));

    public async Task<Result<IReadOnlyList<AiModel>, AiError>> ListModelsAsync(CancellationToken cancellationToken)
    {
        var answer = await SendAsync(HttpMethod.Get, $"models?limit={ModelPage}", null, cancellationToken)
            .ConfigureAwait(false);
        if (!answer.IsSuccess)
            return answer.Failure;
        return answer.Value.Items("data")
            .Where(m => m.Text("id") is { Length: > 0 })
            .Select(m => new AiModel(m.Text("id")!, m.Text("display_name")))
            .ToList();
    }

    public async Task<Result<AiResponse, AiError>> CompleteAsync(AiRequest request,
        CancellationToken cancellationToken)
    {
        // The API refuses a forced tool while the model thinks.
        if (request.RequiredTool is not null && request.Reasoning is not (AiReasoning.Default or AiReasoning.Off))
        {
            return Result.Fail(AiError.InvalidArgument,
                $"{Id} cannot force a tool while the model thinks; set this use's reasoning to default or off");
        }
        var answer = await SendAsync(HttpMethod.Post, "messages", RequestOf(request), cancellationToken)
            .ConfigureAwait(false);
        if (!answer.IsSuccess)
            return answer.Failure;
        var response = ResponseOf(answer.Value);
        return response.IsSuccess ? response : Result.Fail(response.Failure.Code, $"{Id}: {response.Failure.Detail}");
    }

    private async Task<Result<JsonElement, AiError>> SendAsync(HttpMethod method, string path, JsonObject? body,
        CancellationToken cancellationToken)
    {
        var key = context.Key();
        if (key is null)
            return Result.Fail(AiError.NotConfigured, $"{Id}: no API key");
        using var request = new HttpRequestMessage(method, new Uri(BaseUrl, path));
        if (body is not null)
            request.Content = AiHttp.Body(body);
        request.Headers.Add("x-api-key", key);
        request.Headers.Add("anthropic-version", ApiVersion);
        var answer = await AiHttp.SendAsync(context.Http, request, key, cancellationToken).ConfigureAwait(false);
        return answer.IsSuccess ? answer : Result.Fail(answer.Failure.Code, $"{Id}: {answer.Failure.Detail}");
    }

    // Tool results that follow each other go as the blocks of one user
    // message: the API wants user and assistant turns to alternate.
    private static JsonObject RequestOf(AiRequest request)
    {
        var messages = new JsonArray();
        JsonArray? results = null;
        foreach (var message in request.Messages)
        {
            if (message.Role == AiRole.Tool)
            {
                if (results is null)
                {
                    results = [];
                    messages.Add(new JsonObject { ["role"] = "user", ["content"] = results });
                }
                results.Add(new JsonObject
                {
                    ["type"] = "tool_result",
                    ["tool_use_id"] = message.ToolCallId,
                    ["content"] = message.Text ?? "",
                    ["is_error"] = message.IsError,
                });
                continue;
            }
            results = null;
            var content = new JsonArray();
            // The turn's thinking blocks go back first, unchanged, with the
            // tool calls they led to (they ride on the first call's Extra).
            if (message.ToolCalls is [{ Extra: { ValueKind: JsonValueKind.Array } thinking }, ..])
            {
                foreach (var block in thinking.EnumerateArray())
                    content.Add(JsonNode.Parse(block.GetRawText()));
            }
            if (!string.IsNullOrEmpty(message.Text))
                content.Add(new JsonObject { ["type"] = "text", ["text"] = message.Text });
            foreach (var call in message.ToolCalls ?? [])
            {
                content.Add(new JsonObject
                {
                    ["type"] = "tool_use",
                    ["id"] = call.Id,
                    ["name"] = call.Name,
                    ["input"] = JsonNode.Parse(call.Arguments.GetRawText()),
                });
            }
            messages.Add(new JsonObject
            {
                ["role"] = message.Role == AiRole.User ? "user" : "assistant",
                ["content"] = content,
            });
        }

        var body = new JsonObject
        {
            ["model"] = request.Model,
            ["max_tokens"] = request.MaxTokens,
            ["messages"] = messages,
        };
        if (request.System is not null)
            body["system"] = request.System;
        if (request.Tools is { Count: > 0 } tools)
        {
            body["tools"] = new JsonArray([.. tools.Select(t => (JsonNode)new JsonObject
            {
                ["name"] = t.Name,
                ["description"] = t.Description,
                ["input_schema"] = JsonNode.Parse(t.Parameters.GetRawText()),
            })]);
        }
        // Its web_search server tool (phase 27), run by Anthropic within the
        // turn; the basic version, which every model with the tool takes.
        if (request.WebSearch)
        {
            var all = body["tools"] as JsonArray ?? [];
            all.Add(new JsonObject { ["type"] = "web_search_20250305", ["name"] = "web_search", ["max_uses"] = MaxSearches });
            body["tools"] = all;
        }
        if (request.RequiredTool is { } required)
            body["tool_choice"] = new JsonObject { ["type"] = "tool", ["name"] = required };
        // Adaptive thinking with an effort, the current models' mechanism
        // (phase 29); a model that takes only budget_tokens, or that cannot
        // switch thinking off, answers 400, which is invalidArgument.
        switch (request.Reasoning)
        {
            case AiReasoning.Off:
                body["thinking"] = new JsonObject { ["type"] = "disabled" };
                break;
            case AiReasoning.Low or AiReasoning.Medium or AiReasoning.High:
                body["thinking"] = new JsonObject { ["type"] = "adaptive" };
                body["output_config"] = new JsonObject
                {
                    ["effort"] = request.Reasoning switch
                    {
                        AiReasoning.Low => "low",
                        AiReasoning.Medium => "medium",
                        _ => "high",
                    },
                };
                break;
        }
        return body;
    }

    private static Result<AiResponse, AiError> ResponseOf(JsonElement answer)
    {
        if (answer.Get("content") is not { ValueKind: JsonValueKind.Array } content)
            return Result.Fail(AiError.InvalidAnswer, "an answer without content");

        var texts = new List<string>();
        var calls = new List<AiToolCall>();
        var sources = new List<AiSource>();
        var thinking = new JsonArray();
        foreach (var block in content.EnumerateArray())
        {
            switch (block.Text("type"))
            {
                // Kept only to be sent back with this turn's tool results: the
                // thinking, and a web search's call and results (phase 27).
                case "thinking" or "redacted_thinking" or "server_tool_use" or "web_search_tool_result":
                    thinking.Add(JsonNode.Parse(block.GetRawText()));
                    break;
                case "text" when block.Text("text") is { } text:
                    texts.Add(text);
                    sources.AddRange(block.Items("citations")
                        .Where(c => c.Text("type") == "web_search_result_location" && c.Text("url") is { Length: > 0 })
                        .Select(c => new AiSource(c.Text("url")!, c.Text("title"))));
                    break;
                case "tool_use":
                    if (block.Text("id") is not { Length: > 0 } id || block.Text("name") is not { Length: > 0 } name)
                        return Result.Fail(AiError.InvalidAnswer, "a tool_use block without an id or a name");
                    var input = block.Get("input") is { ValueKind: JsonValueKind.Object } given
                        ? given.Clone()
                        : JsonSerializer.SerializeToElement(new { });
                    calls.Add(new AiToolCall(id, name, input));
                    break;
            }
        }

        if (calls.Count > 0 && thinking.Count > 0)
            calls[0] = calls[0] with { Extra = JsonSerializer.SerializeToElement(thinking) };

        var stop = answer.Text("stop_reason") switch
        {
            "end_turn" => AiStopReason.EndTurn,
            "tool_use" => AiStopReason.ToolUse,
            "max_tokens" => AiStopReason.MaxTokens,
            _ => AiStopReason.Other,
        };
        var usage = answer.Get("usage") is { } u
            ? new AiUsage(u.Number("input_tokens"), u.Number("output_tokens"), 0,
                u.Get("server_tool_use")?.Number("web_search_requests") ?? 0)
            : new AiUsage(0, 0);
        return new AiResponse(texts.Count == 0 ? null : string.Concat(texts), calls, stop, usage,
            sources.Count > 0 ? [.. sources.DistinctBy(s => s.Url)] : null);
    }
}
