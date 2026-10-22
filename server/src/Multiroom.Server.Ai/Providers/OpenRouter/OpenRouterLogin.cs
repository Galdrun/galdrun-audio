using Multiroom.Server.Abstractions;

namespace Multiroom.Server.Ai.Providers.OpenRouter;

// OpenRouter's official OAuth PKCE flow (phase 22): the owner logs in on
// openrouter.ai and the code is exchanged for an API key, stored like one.
// The key does not expire, and OpenRouter offers no way for an app to revoke
// it: it is deleted on openrouter.ai/keys.
public sealed class OpenRouterLogin(HttpClient http, Uri? baseUrl = null) : IAiLogin
{
    public static readonly Uri DefaultBaseUrl = new("https://openrouter.ai/");

    private readonly Uri _base = baseUrl ?? DefaultBaseUrl;

    public bool NeedsClient => false;

    // OpenRouter brings back only ?code=, so the state rides in the callback
    // URL's own query.
    public Result<Uri, AiError> Start(AiLoginRequest request) =>
        AiHttp.WithQuery(new Uri(_base, "auth"),
            ("callback_url", AiHttp.WithQuery(request.RedirectUri, ("state", request.State)).AbsoluteUri),
            ("code_challenge", request.Challenge),
            ("code_challenge_method", "S256"));

    public async Task<Result<AiCredential, AiError>> CompleteAsync(AiLoginRequest request, string code,
        string? issuedClientId, CancellationToken cancellationToken)
    {
        using var message = new HttpRequestMessage(HttpMethod.Post, new Uri(_base, "api/v1/auth/keys"))
        {
            Content = AiHttp.Body(new Dictionary<string, string>
            {
                ["code"] = code,
                ["code_verifier"] = request.Verifier,
                ["code_challenge_method"] = "S256",
            }),
        };
        var answer = await AiHttp.SendAsync(http, message, code, cancellationToken).ConfigureAwait(false);
        if (!answer.IsSuccess)
            return Result.Fail(answer.Failure.Code, $"openrouter login: {answer.Failure.Detail}");
        return answer.Value.Text("key") is { Length: > 0 } key
            ? new AiCredential { ApiKey = key }
            : Result.Fail(AiError.InvalidAnswer, "openrouter login: an answer without a key");
    }

    public Task<Result<AiCredential, AiError>> RefreshAsync(AiCredential credential, AiOAuthClient? client,
        CancellationToken cancellationToken) =>
        Task.FromResult<Result<AiCredential, AiError>>(
            Result.Fail(AiError.InvalidArgument, "an OpenRouter key does not expire"));

    public Task<Result<bool, AiError>> RevokeAsync(AiCredential credential, AiOAuthClient? client,
        CancellationToken cancellationToken) =>
        Task.FromResult<Result<bool, AiError>>(false);
}
