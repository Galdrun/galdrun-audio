using System.Text.Json;
using Multiroom.Server.Abstractions;

namespace Multiroom.Server.Ai.Providers.OpenAi;

// OpenAI's signed-in path: the Responses API
// on the owner's ChatGPT plan, with no API key and no per-token bill.
// OpenAiProvider asks it when the credential is a ChatGPT sign-in; it is
// public so a test can drive it with a token of its own.
public sealed class ChatGptPlan(AiProviderContext context) : OpenAiResponsesProvider(context)
{
    private static readonly Uri Default = new("https://api.openai.com/v1/");

    public override string Id => OpenAiProvider.ProviderId;

    public override string DisplayName => "ChatGPT plan";

    public override IReadOnlyList<AiAuthMethod> AuthMethods => [AiAuthMethod.OAuth];

    protected override Uri DefaultBaseUrl => Default;

    // The plan's route documents model, input, instructions, store and
    // stream; what this server needs beyond them (tools, tool_choice,
    // reasoning) is sent, a limit on the answer is not.
    protected override bool SendsMaxTokens => false;

    protected override string Unconfigured =>
        $"not signed in; sign in with POST /api/v1/ai/providers/{Id}/login";

    // The models this account may use, as the plan lists them.
    protected override IEnumerable<AiModel> ModelsOf(JsonElement answer) =>
        answer.Items("models")
            .Where(m => m.Text("visibility") == "list" && m.Text("slug") is { Length: > 0 })
            .Select(m => new AiModel(m.Text("slug")!, m.Text("display_name")));

    // The plan's own refusals, said so an app can tell the owner (phase 28).
    protected override Failure<AiError>? FailureOf(string code) => code switch
    {
        "subscription_sharing_usage_limit_exceeded" =>
            Result.Fail(AiError.RateLimited, "the ChatGPT plan's usage limit is reached"),
        "subscription_sharing_user_not_eligible" =>
            Result.Fail(AiError.AuthFailed, "this ChatGPT account has no plan that may be used here (Plus or Pro)"),
        "subscription_sharing_usage_unavailable" or "subscription_sharing_user_unavailable" =>
            Result.Fail(AiError.Unavailable, $"ChatGPT cannot check the plan now ({code})"),
        "subscription_sharing_unsupported_capability" or "subscription_sharing_route_not_supported" =>
            Result.Fail(AiError.InvalidArgument, $"the ChatGPT plan does not take this request ({code})"),
        "invalid_token" or "token_expired" =>
            Result.Fail(AiError.AuthFailed, "ChatGPT refused the sign-in; sign in again"),
        "model_not_found" => Result.Fail(AiError.InvalidArgument, "this model is not available to the ChatGPT plan"),
        _ => base.FailureOf(code),
    };
}
