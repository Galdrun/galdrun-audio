using System.Text.Json;
using Microsoft.Extensions.DependencyInjection;

namespace Multiroom.Server.Ai.Providers.Gemini;

public static class GeminiProviderExtensions
{
    public static IServiceCollection AddGemini(this IServiceCollection services) =>
        services.AddAiProvider(GeminiProvider.ProviderId, c => new GeminiProvider(c))
            .AddAiLogin(GeminiProvider.ProviderId, http => new GoogleLogin(http));
}

// Google's OpenAI-compatible endpoint for Gemini, with a free tier: an API
// key, or a Google login (GoogleLogin), whose access token it is sent as the
// key would be.
public sealed class GeminiProvider(AiProviderContext context) : OpenAiCompatibleProvider(context)
{
    public const string ProviderId = "gemini";

    private static readonly Uri Default = new("https://generativelanguage.googleapis.com/v1beta/openai/");

    public override string Id => ProviderId;

    public override string DisplayName => "Google Gemini";

    protected override Uri DefaultBaseUrl => Default;

    public override IReadOnlyList<AiAuthMethod> AuthMethods => [AiAuthMethod.ApiKey, AiAuthMethod.OAuth];

    // reasoning_effort on the OpenAI-compatible endpoint; Google maps it to
    // the model's thinking (phase 29).
    public override IReadOnlyList<AiCapability> Capabilities => [AiCapability.Reasoning, AiCapability.Transcription];

    // Speech as input_audio on a chat turn: the endpoint has no audio/transcriptions.
    protected override SpeechRoute Speech => SpeechRoute.ChatAudio;

    // It lists "models/gemini-2.5-flash" and takes "gemini-2.5-flash"; the
    // listing says nothing of tools, the name does.
    protected override AiModel? ModelOf(JsonElement item) => base.ModelOf(item) is { } model
        ? AiModel.Named(model.Id.StartsWith("models/", StringComparison.Ordinal) ? model.Id["models/".Length..] : model.Id, model.Name)
        : null;
}
