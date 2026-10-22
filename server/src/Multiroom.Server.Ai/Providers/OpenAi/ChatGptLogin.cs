using System.Buffers.Text;
using System.Globalization;
using System.Text.Json;
using System.Text.RegularExpressions;
using Multiroom.Server.Abstractions;

namespace Multiroom.Server.Ai.Providers.OpenAi;

// Where Sign in with ChatGPT lives (auth.openai.com's OpenID discovery,
// read 2026-10-01); a test points them at a fake.
public sealed record ChatGptEndpoints(string Issuer, Uri Authorize, Uri Token, Uri Revoke)
{
    public static ChatGptEndpoints Default { get; } = new("https://auth.openai.com",
        new Uri("https://auth.openai.com/api/accounts/authorize"),
        new Uri("https://auth.openai.com/api/accounts/oauth/token"),
        new Uri("https://auth.openai.com/api/accounts/oauth/revoke"));
}

// Sign in with ChatGPT with plan usage, on
// phase 22's seam: OpenID Connect with PKCE and a loopback redirect. The first
// sign-in sends `dynamic_agent_client`; OpenAI registers a client for the
// user during the flow and names it on the redirect (`client_id`), and it is
// kept beside the tokens and sent from then on. The access token lives an
// hour and is renewed from `earliest_refresh_at` on; each renewal brings a
// new refresh token. No OpenAI code is copied: OpenAI's devkit (noncommercial
// licence, Node.js) was read as the reference for the requests.
public sealed partial class ChatGptLogin(HttpClient http, ChatGptHost host, ChatGptEndpoints? endpoints = null,
    TimeProvider? time = null) : IAiLogin
{
    public const string DynamicClient = "dynamic_agent_client";
    public const string Resource = "https://api.openai.com/v1";
    public const string PlanScope = "chatgpt.tokens.use.direct";
    public const string Scopes = "openid profile email offline_access resource.invoke " + PlanScope;
    public const string AppName = Branding.Name;

    // An ID token is taken this far past its exp, for a clock a little behind.
    private static readonly TimeSpan Skew = TimeSpan.FromSeconds(5);

    private readonly ChatGptEndpoints _endpoints = endpoints ?? ChatGptEndpoints.Default;
    private readonly TimeProvider _time = time ?? TimeProvider.System;

    public bool NeedsClient => false;

    // OpenAI takes a loopback redirect: this server's callback reached on
    // 127.0.0.1, or an app's own page on its machine's loopback.
    public Result<Uri, AiError> Start(AiLoginRequest request)
    {
        if (!request.RedirectUri.IsLoopback || request.RedirectUri.Scheme != "http")
        {
            return Result.Fail(AiError.InvalidArgument,
                "redirectUri: ChatGPT takes only a loopback http redirect (127.0.0.1 or localhost)");
        }
        var hostId = host.Id();
        if (!hostId.IsSuccess)
            return hostId.Failure;
        List<(string, string)> query =
        [
            ("client_id", request.Client?.ClientId ?? DynamicClient),
            ("response_type", "code"),
            ("redirect_uri", request.RedirectUri.AbsoluteUri),
            ("scope", Scopes),
            ("resource", Resource),
            ("state", request.State),
            ("nonce", request.Nonce),
            ("code_challenge_method", "S256"),
            ("code_challenge", request.Challenge),
            ("ext_agent_host_id", hostId.Value),
        ];
        if (request.Client is null)
            query.Add(("agent_name_hint", AppName));
        return AiHttp.WithQuery(_endpoints.Authorize, [.. query]);
    }

    public async Task<Result<AiCredential, AiError>> CompleteAsync(AiLoginRequest request, string code,
        string? issuedClientId, CancellationToken cancellationToken)
    {
        var stored = request.Client?.ClientId;
        if (stored is not null && issuedClientId is not null && issuedClientId != stored)
            return Result.Fail(AiError.AuthFailed, "chatgpt sign-in: the redirect named another client; sign in again");
        var clientId = issuedClientId ?? stored;
        if (clientId is null || clientId == DynamicClient || !ClientIdPattern().IsMatch(clientId))
            return Result.Fail(AiError.AuthFailed, "chatgpt sign-in: ChatGPT did not complete the app's registration; sign in again");

        var answer = await TokenAsync(code, cancellationToken,
            ("grant_type", "authorization_code"),
            ("client_id", clientId),
            ("code", code),
            ("code_verifier", request.Verifier),
            ("redirect_uri", request.RedirectUri.AbsoluteUri),
            ("resource", Resource)).ConfigureAwait(false);
        if (!answer.IsSuccess)
            return Result.Fail(answer.Failure.Code, $"chatgpt sign-in: {answer.Failure.Detail}");
        var credential = CredentialOf(answer.Value, clientId, request.Nonce, null);
        return credential.IsSuccess ? credential : Result.Fail(credential.Failure.Code, $"chatgpt sign-in: {credential.Failure.Detail}");
    }

    // A refused refresh token is the owner's sign-in gone: AuthFailed, so the
    // status says to sign in again.
    public async Task<Result<AiCredential, AiError>> RefreshAsync(AiCredential credential, AiOAuthClient? client,
        CancellationToken cancellationToken)
    {
        if (client is null || credential.RefreshToken is not { } refresh)
            return Result.Fail(AiError.AuthFailed, "chatgpt: nothing to renew the sign-in with; sign in again");
        var answer = await TokenAsync(refresh, cancellationToken,
            ("grant_type", "refresh_token"),
            ("client_id", client.ClientId),
            ("refresh_token", refresh),
            ("resource", Resource)).ConfigureAwait(false);
        if (!answer.IsSuccess)
        {
            return Result.Fail(answer.Failure.Code is AiError.InvalidArgument ? AiError.AuthFailed : answer.Failure.Code,
                $"chatgpt refused to renew the sign-in; sign in again ({answer.Failure.Detail})");
        }
        var renewed = CredentialOf(answer.Value, client.ClientId, null, refresh);
        return renewed.IsSuccess ? renewed : Result.Fail(renewed.Failure.Code, $"chatgpt renewal: {renewed.Failure.Detail}");
    }

    // The refresh token, which ends the renewable session.
    public async Task<Result<bool, AiError>> RevokeAsync(AiCredential credential, AiOAuthClient? client,
        CancellationToken cancellationToken)
    {
        if (client is null || credential.RefreshToken is not { } token)
            return false;
        using var message = new HttpRequestMessage(HttpMethod.Post, _endpoints.Revoke)
        {
            Content = AiHttp.Form(("token", token), ("token_type_hint", "refresh_token"), ("client_id", client.ClientId)),
        };
        try
        {
            using var response = await http.SendAsync(message, cancellationToken).ConfigureAwait(false);
            return response.IsSuccessStatusCode
                ? true
                : Result.Fail(AiError.Unavailable, $"chatgpt revoke: HTTP {(int)response.StatusCode}");
        }
        catch (HttpRequestException e)
        {
            return Result.Fail(AiError.Unavailable, $"chatgpt revoke: {e.Message}");
        }
    }

    private async Task<Result<JsonElement, AiError>> TokenAsync(string secret, CancellationToken cancellationToken,
        params (string Name, string? Value)[] fields)
    {
        using var message = new HttpRequestMessage(HttpMethod.Post, _endpoints.Token) { Content = AiHttp.Form(fields) };
        return await AiHttp.SendAsync(http, message, secret, cancellationToken).ConfigureAwait(false);
    }

    // The token answer as a credential: a bearer token with its lifetime and
    // a refresh token, the plan's scope granted, and (on a sign-in, or where a
    // renewal brings one) an ID token from the issuer for this client, with
    // the nonce sent. The ID token came straight from the token endpoint over
    // TLS, so its signature is not checked (OpenID Connect Core 3.1.3.7, 6).
    private Result<AiCredential, AiError> CredentialOf(JsonElement json, string clientId, string? nonce,
        string? previousRefresh)
    {
        if (json.Text("access_token") is not { Length: > 0 } access ||
            !string.Equals(json.Text("token_type"), "bearer", StringComparison.OrdinalIgnoreCase) ||
            json.Number("expires_in") <= 0)
        {
            return Result.Fail(AiError.InvalidAnswer, "an answer without a bearer token and its lifetime");
        }
        var refresh = json.Text("refresh_token") is { Length: > 0 } given ? given : previousRefresh;
        if (refresh is null)
            return Result.Fail(AiError.InvalidAnswer, "an answer without a refresh token");
        // A renewal may leave the scope out when it is unchanged.
        if (json.Text("scope") is { } scope && !scope.Split(' ').Contains(PlanScope, StringComparer.Ordinal))
            return Result.Fail(AiError.AuthFailed, "the sign-in did not grant use of the ChatGPT plan; sign in again and allow it");
        if (json.Text("scope") is null && nonce is not null)
            return Result.Fail(AiError.InvalidAnswer, "an answer that does not say what was granted");

        if (json.Text("id_token") is { } idToken)
        {
            var checkedToken = CheckIdToken(idToken, clientId, nonce);
            if (!checkedToken.IsSuccess)
                return checkedToken.Failure;
        }
        else if (nonce is not null)
        {
            return Result.Fail(AiError.InvalidAnswer, "an answer without an ID token");
        }

        var now = _time.GetUtcNow();
        return new AiCredential
        {
            AccessToken = access,
            RefreshToken = refresh,
            ExpiresAt = now.AddSeconds(json.Number("expires_in")),
            RefreshAfter = EarliestRefresh(json.Get("earliest_refresh_at")),
            Client = new AiOAuthClient(clientId, null),
        };
    }

    // iss, aud (or azp), nonce and exp of the ID token's payload.
    private Result<bool, AiError> CheckIdToken(string idToken, string clientId, string? nonce)
    {
        const string Refused = "the ChatGPT identity could not be verified; sign in again";
        var parts = idToken.Split('.');
        if (parts.Length != 3)
            return Result.Fail(AiError.AuthFailed, Refused);
        JsonElement payload;
        try
        {
            using var document = JsonDocument.Parse(Base64Url.DecodeFromChars(parts[1]));
            payload = document.RootElement.Clone();
        }
        catch (Exception e) when (e is FormatException or JsonException)
        {
            return Result.Fail(AiError.AuthFailed, Refused);
        }
        var audience = payload.Get("aud") switch
        {
            { ValueKind: JsonValueKind.String } one => [one.GetString()],
            { ValueKind: JsonValueKind.Array } many => many.EnumerateArray().Select(a => a.GetString()).ToArray(),
            _ => Array.Empty<string?>(),
        };
        var expires = DateTimeOffset.FromUnixTimeSeconds(payload.Number("exp"));
        var ok = payload.Text("iss") == _endpoints.Issuer && audience.Contains(clientId) &&
            (payload.Text("azp") is not { } azp || azp == clientId) &&
            (nonce is null || payload.Text("nonce") == nonce) &&
            payload.Text("sub") is { Length: > 0 } &&
            expires + Skew > _time.GetUtcNow();
        return ok ? true : Result.Fail(AiError.AuthFailed, Refused);
    }

    // earliest_refresh_at as Unix seconds or an ISO 8601 time.
    private static DateTimeOffset? EarliestRefresh(JsonElement? value) => value switch
    {
        { ValueKind: JsonValueKind.Number } n when n.TryGetInt64(out var seconds) => DateTimeOffset.FromUnixTimeSeconds(seconds),
        { ValueKind: JsonValueKind.String } s when DateTimeOffset.TryParse(s.GetString(), CultureInfo.InvariantCulture,
            DateTimeStyles.AssumeUniversal, out var at) => at,
        _ => null,
    };

    [GeneratedRegex("^[A-Za-z0-9_-]{1,200}$")]
    private static partial Regex ClientIdPattern();
}
