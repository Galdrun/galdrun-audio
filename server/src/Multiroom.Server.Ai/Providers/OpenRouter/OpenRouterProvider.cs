using System.Text.Json;
using System.Text.Json.Nodes;
using Microsoft.Extensions.DependencyInjection;

namespace Multiroom.Server.Ai.Providers.OpenRouter;

public static class OpenRouterProviderExtensions
{
    public static IServiceCollection AddOpenRouter(this IServiceCollection services) =>
        services.AddAiProvider(OpenRouterProvider.ProviderId, c => new OpenRouterProvider(c))
            .AddAiLogin(OpenRouterProvider.ProviderId, http => new OpenRouterLogin(http));
}

// OpenRouter: many vendors' models behind one key; the `:free` ones cost
// nothing. Its model listing names each model, which the base class reads,
// and the parameters it takes: tools, reasoning.
// Its key is set, or brought by a login (OpenRouterLogin).
public sealed class OpenRouterProvider(AiProviderContext context) : OpenAiCompatibleProvider(context)
{
    public const string ProviderId = "openrouter";

    private static readonly Uri Default = new("https://openrouter.ai/api/v1/");

    public override string Id => ProviderId;

    public override string DisplayName => "OpenRouter";

    protected override Uri DefaultBaseUrl => Default;

    protected override AiModel? ModelOf(JsonElement item)
    {
        if (base.ModelOf(item) is not { } model)
            return null;
        if (!item.TryGetProperty("supported_parameters", out var taken) || taken.ValueKind != JsonValueKind.Array)
            return model;
        var names = taken.EnumerateArray().Select(p => p.GetString()).ToHashSet();
        return model with { Tools = names.Contains("tools"), Reasoning = names.Contains("reasoning") };
    }

    public override IReadOnlyList<AiAuthMethod> AuthMethods => [AiAuthMethod.ApiKey, AiAuthMethod.OAuth];

    public override IReadOnlyList<AiCapability> Capabilities =>
        [AiCapability.Reasoning, AiCapability.WebSearch, AiCapability.Transcription];

    // Speech as input_audio on a chat turn, to a model that takes audio.
    protected override SpeechRoute Speech => SpeechRoute.ChatAudio;

    // Its unified reasoning, which it translates per model (phase 29); the
    // thinking text is excluded from the answer, since nothing here shows it.
    protected override void AddReasoning(JsonObject body, AiReasoning level) =>
        body["reasoning"] = new JsonObject { ["effort"] = Effort(level), ["exclude"] = true };

    // Its web plugin (phase 27): OpenRouter searches and hands the model the
    // results, on any model; they come back as url_citation annotations.
    protected override void AddWebSearch(JsonObject body) =>
        body["plugins"] = new JsonArray(new JsonObject { ["id"] = "web" });
}
