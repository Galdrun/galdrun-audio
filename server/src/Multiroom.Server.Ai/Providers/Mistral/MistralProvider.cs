using Microsoft.Extensions.DependencyInjection;

namespace Multiroom.Server.Ai.Providers.Mistral;

public static class MistralProviderExtensions
{
    public static IServiceCollection AddMistral(this IServiceCollection services) =>
        services.AddAiProvider(MistralProvider.ProviderId, c => new MistralProvider(c));
}

// Mistral's own API, which speaks Chat Completions, with a free tier.
public sealed class MistralProvider(AiProviderContext context) : OpenAiCompatibleProvider(context)
{
    public const string ProviderId = "mistral";

    private static readonly Uri Default = new("https://api.mistral.ai/v1/");

    public override string Id => ProviderId;

    public override string DisplayName => "Mistral AI";

    protected override Uri DefaultBaseUrl => Default;

    // Its Voxtral models on audio/transcriptions.
    public override IReadOnlyList<AiCapability> Capabilities => [AiCapability.Transcription];

    protected override SpeechRoute Speech => SpeechRoute.Transcriptions;
}
