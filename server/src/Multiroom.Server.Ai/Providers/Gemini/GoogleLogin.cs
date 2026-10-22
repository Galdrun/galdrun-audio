using Multiroom.Server.Abstractions;

namespace Multiroom.Server.Ai.Providers.Gemini;

// Where Google's OAuth lives; a test points them at a fake.
public sealed record GoogleOAuthEndpoints(Uri Authorize, Uri Token, Uri Revoke)
{
    public static GoogleOAuthEndpoints Default { get; } = new(
        new Uri("https://accounts.google.com/o/oauth2/v2/auth"),
        new Uri("https://oauth2.googleapis.com/token"),
        new Uri("https://oauth2.googleapis.com/revoke"));
}

// Google's OAuth 2.0 with PKCE (phase 22), for Gemini: an access token for
// the generative-language scope and a refresh token that renews it. The
// OAuth client is the owner's, from their own Google Cloud project (a
// desktop client, or a web client with its secret); there is no built-in one.
public sealed class GoogleLogin(HttpClient http, GoogleOAuthEndpoints? endpoints = null, TimeProvider? time = null)
    : IAiLogin
{
    public const string Scope = "https://www.googleapis.com/auth/generative-language";

    private readonly GoogleOAuthEndpoints _endpoints = endpoints ?? GoogleOAuthEndpoints.Default;
    private readonly TimeProvider _time = time ?? TimeProvider.System;

    public bool NeedsClient => true;

    // offline and consent: Google gives a refresh token only so, and only on
    // a consent it has just shown.
    public Result<Uri, AiError> Start(AiLoginRequest request) =>
        AiHttp.WithQuery(_endpoints.Authorize,
            ("client_id", request.Client?.ClientId ?? ""),
            ("redirect_uri", request.RedirectUri.AbsoluteUri),
            ("response_type", "code"),
            ("scope", Scope),
            ("state", request.State),
            ("code_challenge", request.Challenge),
            ("code_challenge_method", "S256"),
            ("access_type", "offline"),
            ("prompt", "consent"));

    public async Task<Result<AiCredential, AiError>> CompleteAsync(AiLoginRequest request, string code,
        string? issuedClientId, CancellationToken cancellationToken)
    {
        if (request.Client is not { } client)
            return Result.Fail(AiError.NotConfigured, "google login: no OAuth client");
        var answer = await TokenAsync(code, cancellationToken,
            ("grant_type", "authorization_code"),
            ("code", code),
            ("code_verifier", request.Verifier),
            ("redirect_uri", request.RedirectUri.AbsoluteUri),
            ("client_id", client.ClientId),
            ("client_secret", client.ClientSecret)).ConfigureAwait(false);
        return answer.IsSuccess ? answer.Value : Result.Fail(answer.Failure.Code, $"google login: {answer.Failure.Detail}");
    }

    // Google refuses a revoked or expired refresh token with 400
    // invalid_grant: that is the owner's login gone, not a wrong request.
    public async Task<Result<AiCredential, AiError>> RefreshAsync(AiCredential credential, AiOAuthClient? client,
        CancellationToken cancellationToken)
    {
        if (client is null || credential.RefreshToken is not { } refresh)
            return Result.Fail(AiError.AuthFailed, "google login: nothing to renew it with; log in again");
        var answer = await TokenAsync(refresh, cancellationToken,
            ("grant_type", "refresh_token"),
            ("refresh_token", refresh),
            ("client_id", client.ClientId),
            ("client_secret", client.ClientSecret)).ConfigureAwait(false);
        if (answer.IsSuccess)
        {
            // A renewal carries no new refresh token: the old one stays.
            var renewed = answer.Value;
            return new AiCredential
            {
                AccessToken = renewed.AccessToken,
                RefreshToken = renewed.RefreshToken ?? refresh,
                ExpiresAt = renewed.ExpiresAt,
            };
        }
        return Result.Fail(answer.Failure.Code is AiError.InvalidArgument ? AiError.AuthFailed : answer.Failure.Code,
            $"google login refused the refresh token; log in again ({answer.Failure.Detail})");
    }

    // The refresh token, which revokes its access tokens with it.
    public async Task<Result<bool, AiError>> RevokeAsync(AiCredential credential, AiOAuthClient? client,
        CancellationToken cancellationToken)
    {
        if ((credential.RefreshToken ?? credential.AccessToken) is not { } token)
            return false;
        using var message = new HttpRequestMessage(HttpMethod.Post, _endpoints.Revoke)
        {
            Content = AiHttp.Form(("token", token)),
        };
        try
        {
            using var response = await http.SendAsync(message, cancellationToken).ConfigureAwait(false);
            return response.IsSuccessStatusCode
                ? true
                : Result.Fail(AiError.Unavailable, $"google revoke: HTTP {(int)response.StatusCode}");
        }
        catch (HttpRequestException e)
        {
            return Result.Fail(AiError.Unavailable, $"google revoke: {e.Message}");
        }
    }

    private async Task<Result<AiCredential, AiError>> TokenAsync(string secret, CancellationToken cancellationToken,
        params (string Name, string? Value)[] fields)
    {
        using var message = new HttpRequestMessage(HttpMethod.Post, _endpoints.Token) { Content = AiHttp.Form(fields) };
        var answer = await AiHttp.SendAsync(http, message, secret, cancellationToken).ConfigureAwait(false);
        if (!answer.IsSuccess)
            return answer.Failure;
        var json = answer.Value;
        if (json.Text("access_token") is not { Length: > 0 } access)
            return Result.Fail(AiError.InvalidAnswer, "an answer without an access token");
        var expiresIn = json.Number("expires_in");
        return new AiCredential
        {
            AccessToken = access,
            RefreshToken = json.Text("refresh_token"),
            ExpiresAt = expiresIn > 0 ? _time.GetUtcNow().AddSeconds(expiresIn) : null,
        };
    }
}
