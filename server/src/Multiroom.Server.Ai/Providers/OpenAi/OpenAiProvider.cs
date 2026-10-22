using System.Text.Json.Nodes;
using Microsoft.Extensions.DependencyInjection;
using Multiroom.Server.Abstractions;

namespace Multiroom.Server.Ai.Providers.OpenAi;

public static class OpenAiProviderExtensions
{
    // `directory` is <dataDirectory>/ai, where the sign-in's host id is kept.
    public static IServiceCollection AddOpenAi(this IServiceCollection services, string directory) =>
        services.AddAiProvider(OpenAiProvider.ProviderId, c => new OpenAiProvider(c))
            .AddAiLogin(OpenAiProvider.ProviderId,
                http => new ChatGptLogin(http, new ChatGptHost(Path.Combine(directory, ChatGptHost.FileName))));
}

// OpenAI, two ways in, as Gemini has: an API key, billed per token, on Chat
// Completions; or Sign in with ChatGPT (phase 28), on the owner's plan, on
// the Responses API (ChatGptPlan). The credential decides each call: the key
// first, else the sign-in. The store holds one of them at a time.
public sealed class OpenAiProvider : IAiProvider
{
    public const string ProviderId = "openai";

    private readonly AiProviderContext _context;
    private readonly KeyApi _key;
    private readonly ChatGptPlan _plan;
    private readonly ChatGptDictation _dictation;

    // `dictation` is ChatGPT's backend; a test points it at a fake.
    public OpenAiProvider(AiProviderContext context, Uri? dictation = null)
    {
        _context = context;
        _key = new KeyApi(context);
        // The plan path never sends the key, only the sign-in's token.
        _plan = new ChatGptPlan(new AiProviderContext(context.Http, () => null, context.BaseUrl, context.Token));
        _dictation = new ChatGptDictation(new AiProviderContext(context.Http, () => null, null, context.Token), dictation);
    }

    public string Id => ProviderId;

    public string DisplayName => "OpenAI";

    public IReadOnlyList<AiAuthMethod> AuthMethods => [AiAuthMethod.ApiKey, AiAuthMethod.OAuth];

    // reasoning_effort with a key, reasoning: {effort} on the plan (phase 29);
    // web_search_options with a key, the web_search tool on the plan (phase 27).
    // Speech with a key on audio/transcriptions; on the sign-in, whose
    // Responses route takes no audio, on ChatGPT's own dictation (ChatGptDictation).
    public IReadOnlyList<AiCapability> Capabilities => [AiCapability.Reasoning, AiCapability.WebSearch, AiCapability.Transcription];

    public Task<AiProviderStatus> GetStatusAsync(CancellationToken cancellationToken) =>
        Task.FromResult(_context.HasCredential
            ? new AiProviderStatus(true)
            : new AiProviderStatus(false,
                $"no API key; set one with PUT /api/v1/ai/providers/{Id}/key or sign in with ChatGPT with POST /api/v1/ai/providers/{Id}/login"));

    public Task<Result<IReadOnlyList<AiModel>, AiError>> ListModelsAsync(CancellationToken cancellationToken) =>
        Path.ListModelsAsync(cancellationToken);

    public Task<Result<AiResponse, AiError>> CompleteAsync(AiRequest request, CancellationToken cancellationToken) =>
        Path.CompleteAsync(request, cancellationToken);

    public Task<Result<string, AiError>> TranscribeAsync(AiTranscriptionRequest request, CancellationToken cancellationToken) =>
        Path == _key ? _key.TranscribeAsync(request, cancellationToken) : _dictation.TranscribeAsync(request, cancellationToken);

    private IAiProvider Path => _context.Key() is null && _context.Token?.HasToken == true ? _plan : _key;

    // The API key's path: OpenAI's own Chat Completions.
    private sealed class KeyApi(AiProviderContext context) : OpenAiCompatibleProvider(context)
    {
        private static readonly Uri Default = new("https://api.openai.com/v1/");

        public override string Id => ProviderId;

        public override string DisplayName => "OpenAI";

        protected override Uri DefaultBaseUrl => Default;

        // Its newer models refuse max_tokens.
        protected override string MaxTokensField => "max_completion_tokens";

        // reasoning_effort; "none" is off on the models that have it (phase 29).
        public override IReadOnlyList<AiCapability> Capabilities => [AiCapability.Reasoning, AiCapability.WebSearch];

        // Chat Completions searches only on OpenAI's search models
        // (gpt-5-search-api); another model refuses the field with a 400,
        // which is invalidArgument with OpenAI's message (phase 27).
        protected override void AddWebSearch(JsonObject body) => body["web_search_options"] = new JsonObject();

        // gpt-4o-transcribe, whisper-1 and the like.
        protected override SpeechRoute Speech => SpeechRoute.Transcriptions;
    }
}
