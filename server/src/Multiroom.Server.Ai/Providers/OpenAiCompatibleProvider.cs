using System.Net.Http.Headers;
using System.Text.Json;
using System.Text.Json.Nodes;
using Multiroom.Server.Abstractions;

namespace Multiroom.Server.Ai.Providers;

// The Chat Completions wire format with tools, which most vendors speak. A
// vendor that speaks it is a small subclass: its id, name and base URL, and
// its quirks as overrides. A vendor with a format of its own gets its own
// base class (AnthropicProvider).
public abstract class OpenAiCompatibleProvider(AiProviderContext context) : IAiProvider
{
    public abstract string Id { get; }

    public abstract string DisplayName { get; }

    public virtual IReadOnlyList<AiAuthMethod> AuthMethods => [AiAuthMethod.ApiKey];

    // A vendor that takes a reasoning level lists Reasoning (phase 29).
    public virtual IReadOnlyList<AiCapability> Capabilities => [];

    // Where the API is, ending in "/": the config's, else the vendor's own.
    protected Uri BaseUrl => context.BaseUrl ?? DefaultBaseUrl;

    protected abstract Uri DefaultBaseUrl { get; }

    // A vendor that takes no key (a local server) is always configured. A key
    // or a login's token is what it is sent (phase 22).
    protected bool NeedsKey => AuthMethods.Contains(AiAuthMethod.ApiKey);

    // The name of the limit on the answer's length: OpenAI's newer models
    // refuse max_tokens.
    protected virtual string MaxTokensField => "max_tokens";

    public Task<AiProviderStatus> GetStatusAsync(CancellationToken cancellationToken) =>
        Task.FromResult(!NeedsKey || context.HasCredential
            ? new AiProviderStatus(true)
            : new AiProviderStatus(false, AuthMethods.Contains(AiAuthMethod.OAuth)
                ? $"no API key; set one with PUT /api/v1/ai/providers/{Id}/key or log in with POST /api/v1/ai/providers/{Id}/login"
                : $"no API key; set one with PUT /api/v1/ai/providers/{Id}/key"));

    public async Task<Result<IReadOnlyList<AiModel>, AiError>> ListModelsAsync(CancellationToken cancellationToken)
    {
        var credential = await context.CredentialAsync(cancellationToken).ConfigureAwait(false);
        if (!credential.IsSuccess)
            return Result.Fail(credential.Failure.Code, $"{Id}: {credential.Failure.Detail}");
        var key = credential.Value;
        if (NeedsKey && key is null)
            return Result.Fail(AiError.NotConfigured, $"{Id}: no API key");
        using var request = new HttpRequestMessage(HttpMethod.Get, new Uri(BaseUrl, "models"));
        Authorize(request, key);
        var answer = await AiHttp.SendAsync(context.Http, request, key, cancellationToken).ConfigureAwait(false);
        if (!answer.IsSuccess)
            return Result.Fail(answer.Failure.Code, $"{Id}: {answer.Failure.Detail}");
        return answer.Value.Items("data").Select(ModelOf).OfType<AiModel>().ToList();
    }

    public async Task<Result<AiResponse, AiError>> CompleteAsync(AiRequest request,
        CancellationToken cancellationToken)
    {
        var credential = await context.CredentialAsync(cancellationToken).ConfigureAwait(false);
        if (!credential.IsSuccess)
            return Result.Fail(credential.Failure.Code, $"{Id}: {credential.Failure.Detail}");
        var key = credential.Value;
        if (NeedsKey && key is null)
            return Result.Fail(AiError.NotConfigured, $"{Id}: no API key");
        using var message = new HttpRequestMessage(HttpMethod.Post, new Uri(BaseUrl, "chat/completions"))
        {
            Content = AiHttp.Body(RequestOf(request)),
        };
        Authorize(message, key);
        var answer = await AiHttp.SendAsync(context.Http, message, key, cancellationToken).ConfigureAwait(false);
        if (!answer.IsSuccess)
            return Result.Fail(answer.Failure.Code, $"{Id}: {answer.Failure.Detail}");
        var response = ResponseOf(answer.Value);
        return response.IsSuccess ? response : Result.Fail(response.Failure.Code, $"{Id}: {response.Failure.Detail}");
    }

    // How the vendor hears speech, where it lists AiCapability.Transcription:
    // its audio/transcriptions route (OpenAI's, Groq's, Mistral's), or a chat
    // turn with the audio as input_audio (Gemini's, OpenRouter's).
    protected enum SpeechRoute
    {
        None,
        Transcriptions,
        ChatAudio,
    }

    protected virtual SpeechRoute Speech => SpeechRoute.None;

    public async Task<Result<string, AiError>> TranscribeAsync(AiTranscriptionRequest request,
        CancellationToken cancellationToken)
    {
        if (Speech == SpeechRoute.None)
            return Result.Fail(AiError.InvalidArgument, $"{Id} cannot transcribe speech");
        if (string.IsNullOrWhiteSpace(request.Model))
            return Result.Fail(AiError.NotConfigured, $"{Id} needs a speech model (speech.model in the AI settings)");
        var credential = await context.CredentialAsync(cancellationToken).ConfigureAwait(false);
        if (!credential.IsSuccess)
            return Result.Fail(credential.Failure.Code, $"{Id}: {credential.Failure.Detail}");
        var key = credential.Value;
        if (NeedsKey && key is null)
            return Result.Fail(AiError.NotConfigured, $"{Id}: no API key");
        using var message = Speech == SpeechRoute.Transcriptions ? TranscriptionOf(request) : ChatAudioOf(request);
        Authorize(message, key);
        var answer = await AiHttp.SendAsync(context.Http, message, key, cancellationToken).ConfigureAwait(false);
        if (!answer.IsSuccess)
            return Result.Fail(answer.Failure.Code, $"{Id}: {answer.Failure.Detail}");
        var text = Speech == SpeechRoute.Transcriptions
            ? answer.Value.Text("text")
            : answer.Value.Items("choices").FirstOrDefault().Get("message")?.Text("content");
        return text is null
            ? Result.Fail(AiError.InvalidAnswer, $"{Id}: an answer without the transcript")
            : text.Trim();
    }

    private HttpRequestMessage TranscriptionOf(AiTranscriptionRequest request)
    {
        var audio = new ByteArrayContent(request.Audio);
        audio.Headers.ContentType = new MediaTypeHeaderValue(request.MediaType);
        var form = new MultipartFormDataContent { { new StringContent(request.Model), "model" }, { audio, "file", "speech.wav" } };
        // ISO 639-1, as the route takes it: "de" of "de-DE".
        if (request.Language is { } language)
            form.Add(new StringContent(language.Split('-')[0].ToLowerInvariant()), "language");
        return new HttpRequestMessage(HttpMethod.Post, new Uri(BaseUrl, "audio/transcriptions")) { Content = form };
    }

    private HttpRequestMessage ChatAudioOf(AiTranscriptionRequest request)
    {
        var ask = "Transcribe this recording exactly as spoken" +
            (request.Language is { } language ? $" (language {language})" : "") +
            ". Answer with the transcript only, nothing else; an empty answer if nothing is said.";
        var body = new JsonObject
        {
            ["model"] = request.Model,
            ["messages"] = new JsonArray(new JsonObject
            {
                ["role"] = "user",
                ["content"] = new JsonArray(
                    new JsonObject { ["type"] = "text", ["text"] = ask },
                    new JsonObject
                    {
                        ["type"] = "input_audio",
                        ["input_audio"] = new JsonObject
                        {
                            ["data"] = Convert.ToBase64String(request.Audio),
                            ["format"] = request.MediaType.Split('/')[^1].Replace("x-", "", StringComparison.Ordinal),
                        },
                    }),
            }),
        };
        return new HttpRequestMessage(HttpMethod.Post, new Uri(BaseUrl, "chat/completions")) { Content = AiHttp.Body(body) };
    }

    protected virtual void Authorize(HttpRequestMessage request, string? key)
    {
        if (key is not null)
            request.Headers.Authorization = new AuthenticationHeaderValue("Bearer", key);
    }

    // A reasoning level other than Default, as the vendor takes it (phase
    // 29): Chat Completions' reasoning_effort, where "none" is off.
    protected virtual void AddReasoning(JsonObject body, AiReasoning level) =>
        body["reasoning_effort"] = Effort(level);

    // A web lookup as the vendor takes it (phase 27); only a vendor that
    // lists AiCapability.WebSearch is ever asked for one.
    protected virtual void AddWebSearch(JsonObject body)
    {
    }

    // The portable level as the effort word most vendors share.
    protected static string Effort(AiReasoning level) => level switch
    {
        AiReasoning.Off => "none",
        AiReasoning.Low => "low",
        AiReasoning.Medium => "medium",
        _ => "high",
    };

    // One entry of GET models' data: its id, and its name where the vendor gives one.
    protected virtual AiModel? ModelOf(JsonElement item) =>
        item.Text("id") is { Length: > 0 } id ? new AiModel(id, item.Text("name")) : null;

    private JsonObject RequestOf(AiRequest request)
    {
        var messages = new JsonArray();
        if (request.System is not null)
            messages.Add(new JsonObject { ["role"] = "system", ["content"] = request.System });
        foreach (var message in request.Messages)
        {
            messages.Add(message.Role switch
            {
                AiRole.User => new JsonObject { ["role"] = "user", ["content"] = message.Text ?? "" },
                AiRole.Assistant => Assistant(message),
                _ => new JsonObject
                {
                    ["role"] = "tool",
                    ["tool_call_id"] = message.ToolCallId,
                    ["content"] = message.Text ?? "",
                },
            });
        }

        var body = new JsonObject
        {
            ["model"] = request.Model,
            ["messages"] = messages,
            [MaxTokensField] = request.MaxTokens,
        };
        if (request.Tools is { Count: > 0 } tools)
        {
            body["tools"] = new JsonArray([.. tools.Select(t => (JsonNode)new JsonObject
            {
                ["type"] = "function",
                ["function"] = new JsonObject
                {
                    ["name"] = t.Name,
                    ["description"] = t.Description,
                    ["parameters"] = JsonNode.Parse(t.Parameters.GetRawText()),
                },
            })]);
        }
        if (request.RequiredTool is { } required)
        {
            body["tool_choice"] = new JsonObject
            {
                ["type"] = "function",
                ["function"] = new JsonObject { ["name"] = required },
            };
        }
        if (request.Reasoning != AiReasoning.Default)
            AddReasoning(body, request.Reasoning);
        if (request.WebSearch)
            AddWebSearch(body);
        return body;
    }

    // Arguments go as a string of JSON, as the format has it; a call's
    // extra_content goes back as it came (Gemini 3 refuses the turn without it).
    private static JsonObject Assistant(AiMessage message)
    {
        var assistant = new JsonObject { ["role"] = "assistant", ["content"] = message.Text };
        if (message.ToolCalls is { Count: > 0 } calls)
        {
            assistant["tool_calls"] = new JsonArray([.. calls.Select(c =>
            {
                var call = new JsonObject
                {
                    ["id"] = c.Id,
                    ["type"] = "function",
                    ["function"] = new JsonObject { ["name"] = c.Name, ["arguments"] = c.Arguments.GetRawText() },
                };
                if (c.Extra is { } extra)
                    call["extra_content"] = JsonNode.Parse(extra.GetRawText());
                return (JsonNode)call;
            })]);
        }
        return assistant;
    }

    private static Result<AiResponse, AiError> ResponseOf(JsonElement answer)
    {
        if (answer.Items("choices").FirstOrDefault() is not { ValueKind: JsonValueKind.Object } choice ||
            choice.Get("message") is not { ValueKind: JsonValueKind.Object } message)
        {
            return Result.Fail(AiError.InvalidAnswer, "an answer without choices[0].message");
        }

        var calls = new List<AiToolCall>();
        foreach (var call in message.Items("tool_calls"))
        {
            var function = call.Get("function");
            var name = function?.Text("name");
            if (string.IsNullOrEmpty(name))
                return Result.Fail(AiError.InvalidAnswer, "a tool call without a function name");
            JsonElement arguments;
            try
            {
                var text = function!.Value.Text("arguments");
                using var parsed = JsonDocument.Parse(string.IsNullOrWhiteSpace(text) ? "{}" : text);
                arguments = parsed.RootElement.Clone();
            }
            catch (JsonException)
            {
                return Result.Fail(AiError.InvalidAnswer, $"the arguments of tool call {name} are not JSON");
            }
            // Some vendors send no id; the result must still name the call.
            var id = call.Text("id") is { Length: > 0 } given ? given : $"call_{calls.Count}";
            calls.Add(new AiToolCall(id, name, arguments, call.Get("extra_content")?.Clone()));
        }

        var stop = choice.Text("finish_reason") switch
        {
            "stop" => AiStopReason.EndTurn,
            "tool_calls" or "function_call" => AiStopReason.ToolUse,
            "length" => AiStopReason.MaxTokens,
            _ => calls.Count > 0 ? AiStopReason.ToolUse : AiStopReason.Other,
        };
        var usage = answer.Get("usage") is { } u
            ? new AiUsage(u.Number("prompt_tokens"), u.Number("completion_tokens"),
                u.Get("completion_tokens_details")?.Number("reasoning_tokens") ?? 0)
            : new AiUsage(0, 0);
        return new AiResponse(message.Text("content"), calls, stop, usage, SourcesOf(message));
    }

    // What a web lookup cited: Chat Completions' url_citation annotations
    // (OpenAI's search models, OpenRouter's web plugin), each page once.
    private static List<AiSource>? SourcesOf(JsonElement message)
    {
        List<AiSource> sources = [.. message.Items("annotations")
            .Where(a => a.Text("type") == "url_citation")
            .Select(a => a.Get("url_citation") is { } c && c.Text("url") is { Length: > 0 } url ? new AiSource(url, c.Text("title")) : null)
            .OfType<AiSource>()
            .DistinctBy(s => s.Url)];
        return sources.Count > 0 ? sources : null;
    }
}
