using Microsoft.Extensions.DependencyInjection;

namespace Multiroom.Server.Ai.Providers.Ollama;

public static class OllamaProviderExtensions
{
    public static IServiceCollection AddOllama(this IServiceCollection services) =>
        services.AddAiProvider(OllamaProvider.ProviderId, c => new OllamaProvider(c));
}

// A local Ollama server: free, no key. LM Studio is this class with its own
// base URL (ai.providers' baseUrl).
public sealed class OllamaProvider(AiProviderContext context) : OpenAiCompatibleProvider(context)
{
    public const string ProviderId = "ollama";

    private static readonly Uri Default = new("http://127.0.0.1:11434/v1/");

    public override string Id => ProviderId;

    public override string DisplayName => "Ollama";

    protected override Uri DefaultBaseUrl => Default;

    public override IReadOnlyList<AiAuthMethod> AuthMethods => [AiAuthMethod.None];

    // reasoning_effort, which Ollama maps to a thinking model's think (phase 29).
    public override IReadOnlyList<AiCapability> Capabilities => [AiCapability.Reasoning];
}
