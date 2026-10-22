using Microsoft.Extensions.DependencyInjection;

namespace Multiroom.Server.Ai.Providers.Groq;

public static class GroqProviderExtensions
{
    public static IServiceCollection AddGroq(this IServiceCollection services) =>
        services.AddAiProvider(GroqProvider.ProviderId, c => new GroqProvider(c));
}

// Groq's OpenAI-compatible endpoint, with a free tier.
public sealed class GroqProvider(AiProviderContext context) : OpenAiCompatibleProvider(context)
{
    public const string ProviderId = "groq";

    private static readonly Uri Default = new("https://api.groq.com/openai/v1/");

    public override string Id => ProviderId;

    public override string DisplayName => "Groq";

    protected override Uri DefaultBaseUrl => Default;

    // reasoning_effort, on the models that take it (phase 29).
    public override IReadOnlyList<AiCapability> Capabilities => [AiCapability.Reasoning, AiCapability.Transcription];

    // Its Whisper models on audio/transcriptions.
    protected override SpeechRoute Speech => SpeechRoute.Transcriptions;
}
