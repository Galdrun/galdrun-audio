using System.Text.Json;
using System.Text.Json.Serialization;
using Multiroom.Server.Abstractions;

namespace Multiroom.Server.Ai;

// The AI provider SPI, shaped like the music
// provider SPI: a provider talks to one vendor's model API and knows no tool,
// no agent and no service. It answers data and fails with a Result, never by
// throwing; a cancelled call throws OperationCanceledException, which is not
// a failure. The registry (AiProviderRegistry) bounds every call by its
// timeout and counts what it cost.
public interface IAiProvider
{
    // "openai", "anthropic", "ollama": the id it is registered under.
    string Id { get; }

    string DisplayName { get; }

    // How it is authorised: an API key, a browser login (phase 22), or not at all.
    IReadOnlyList<AiAuthMethod> AuthMethods { get; }

    // What it can do beyond one turn with tools (phase 29). A request that
    // asks for a capability the provider lacks is refused by the registry,
    // by name, never sent with the ask silently dropped.
    IReadOnlyList<AiCapability> Capabilities => [];

    // Whether it has what it needs to be asked (a key, where it takes one).
    // Whether the vendor took the key and answered is the registry's to say,
    // from the last call: asking the vendor here would cost a call per status.
    Task<AiProviderStatus> GetStatusAsync(CancellationToken cancellationToken);

    // The models as the vendor lists them, free ones included; nothing here
    // knows a price.
    Task<Result<IReadOnlyList<AiModel>, AiError>> ListModelsAsync(CancellationToken cancellationToken);

    // One turn: the answer's text and/or tool calls, why it stopped, and the
    // tokens it cost. Whole, not streamed (-> Open decisions).
    Task<Result<AiResponse, AiError>> CompleteAsync(AiRequest request, CancellationToken cancellationToken);

    // The words spoken in the audio; only a provider listing
    // AiCapability.Transcription is asked.
    Task<Result<string, AiError>> TranscribeAsync(AiTranscriptionRequest request, CancellationToken cancellationToken) =>
        Task.FromResult<Result<string, AiError>>(Result.Fail(AiError.InvalidArgument, $"{Id} cannot transcribe speech"));
}

public enum AiAuthMethod
{
    ApiKey,

    // "oauth" on the wire, as the roadmap names it; camel case would say "oAuth".
    [JsonStringEnumMemberName("oauth")]
    OAuth,
    None,
}

public enum AiCapability
{
    // Takes AiRequest.Reasoning other than Default (phase 29).
    Reasoning,

    // Takes AiRequest.WebSearch: a search the vendor runs itself (phase 27).
    WebSearch,

    // Turns speech into text (TranscribeAsync), for the assistant's microphone.
    Transcription,
}

// Speech to transcribe: the audio's bytes and media type (audio/wav is what
// every route takes), and the language spoken (BCP 47, "de-DE"), where known.
public sealed record AiTranscriptionRequest(string Model, byte[] Audio, string MediaType, string? Language = null);

// How hard a model thinks before it answers (phase 29): the same words on
// every provider, each mapped by the provider to its vendor's field. Default
// sends nothing and the vendor decides.
public enum AiReasoning
{
    Default,
    Off,
    Low,
    Medium,
    High,
}

// The tokens a reasoning level adds to a request's MaxTokens: reasoning
// counts against the answer's limit on most vendors, so the answer is not
// spent before it is written. One table, not a setting (rule 5).
public static class AiReasoningBudget
{
    public static int Of(AiReasoning level) => level switch
    {
        AiReasoning.Low => 2048,
        AiReasoning.Medium => 8192,
        AiReasoning.High => 24576,
        _ => 0,
    };
}

public sealed record AiProviderStatus(bool Configured, string Detail = "");

// Tools and Reasoning: whether the model takes tool calls and a reasoning
// level, where the vendor's listing or the model's name tells; null where
// nothing does.
public sealed record AiModel(string Id, string? Name = null, bool? Tools = null, bool? Reasoning = null)
{
    // Models that speak, draw, film, hear, embed or run live sessions: none
    // of them answers a tool call. By name, as the vendors' listings say no more.
    private static readonly string[] NotChat =
    [
        "tts", "image", "imagen", "nano-banana", "dall-e", "sora", "veo", "lyria", "audio", "whisper", "transcribe",
        "translate", "realtime", "live", "embedding", "moderation", "robotics", "computer-use", "deep-research", "aqa",
        "search", "babbage", "davinci",
    ];

    public static AiModel Named(string id, string? name = null) =>
        new(id, name, NotChat.Any(n => id.Contains(n, StringComparison.OrdinalIgnoreCase)) ? false : null);
}

public enum AiRole
{
    User,
    Assistant,

    // A tool's result, answering the assistant's call ToolCallId.
    Tool,
}

// One message of the conversation. An assistant's may carry tool calls
// beside its text; a tool's carries the result as Text, and IsError when the
// tool failed.
public sealed record AiMessage(
    AiRole Role,
    string? Text = null,
    IReadOnlyList<AiToolCall>? ToolCalls = null,
    string? ToolCallId = null,
    bool IsError = false);

// Arguments are a JSON object, as the tool's schema describes. Extra is what
// a vendor attaches to a call and wants back with it, opaque here: Gemini 3's
// thought signature (extra_content on a Chat Completions tool call).
public sealed record AiToolCall(string Id, string Name, JsonElement Arguments, JsonElement? Extra = null);

// Parameters are the tool's JSON Schema, an object.
public sealed record AiTool(string Name, string Description, JsonElement Parameters);

// RequiredTool forces the answer to be a call of that tool, one of Tools
// (each wire format's forced tool choice): a structured answer on every
// vendor (phase 24). Reasoning is how hard the model thinks (phase 29); the
// registry raises MaxTokens by its budget before the provider sees it.
// WebSearch lets the model look things up on the web through the vendor's
// own search (phase 27); a provider without AiCapability.WebSearch is
// refused by the registry.
public sealed record AiRequest(
    string Model,
    IReadOnlyList<AiMessage> Messages,
    string? System = null,
    IReadOnlyList<AiTool>? Tools = null,
    int MaxTokens = AiRequest.DefaultMaxTokens,
    string? RequiredTool = null,
    AiReasoning Reasoning = AiReasoning.Default,
    bool WebSearch = false)
{
    public const int DefaultMaxTokens = 1024;
}

public enum AiStopReason
{
    // The model finished its answer.
    EndTurn,

    // It asks for its tool calls to be run.
    ToolUse,

    // It was cut at MaxTokens.
    MaxTokens,

    // Anything else the vendor names: a content filter, a stop sequence.
    Other,
}

// ReasoningTokens where the vendor reports them (phase 29); on most vendors
// they are already inside OutputTokens. WebSearches: the searches the vendor
// says it ran (phase 27), where it says so.
public sealed record AiUsage(long InputTokens, long OutputTokens, long ReasoningTokens = 0, long WebSearches = 0);

// A page the vendor cited for its answer (phase 27): plain text to show,
// never fetched by this server.
public sealed record AiSource(string Url, string? Title = null);

// Sources: what a web lookup cited, in order, each once (phase 27).
public sealed record AiResponse(string? Text, IReadOnlyList<AiToolCall> ToolCalls, AiStopReason StopReason,
    AiUsage Usage, IReadOnlyList<AiSource>? Sources = null);

public enum AiError
{
    // No key where one is needed, or the provider is disabled in the settings.
    NotConfigured,

    // The vendor refused the key. Not our 401: the caller of this server is
    // authorised, the vendor refused us.
    AuthFailed,

    RateLimited,

    // The vendor could not be reached, or answered 5xx.
    Unavailable,

    // The vendor answered something that is not a model's answer.
    InvalidAnswer,

    // The call outlasted its registration's timeout.
    Timeout,

    // No such provider.
    NotFound,

    // A request the provider or the vendor refused as wrong: an unknown
    // model, a key that is blank, a key that is the config's.
    InvalidArgument,

    // The agent already runs as many commands as it may (phase 25).
    Busy,
}
