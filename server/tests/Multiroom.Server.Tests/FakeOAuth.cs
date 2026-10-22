using System.Buffers.Text;
using System.Collections.Concurrent;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Http;
using Microsoft.AspNetCore.WebUtilities;
using Multiroom.Server.Ai.Providers.OpenAi;
using Multiroom.Server.Ai.Providers.Gemini;

namespace Multiroom.Server.Tests;

// An authorization server in process, shaped as OpenRouter's key exchange
// (POST api/v1/auth/keys), Google's token and revoke endpoints, and Sign in
// with ChatGPT's (api/accounts/oauth/token and revoke, phase 28). The
// browser is the test: Consent(authorizeUrl) reads the URL an app would open
// and answers the code the redirect would carry. A code is single-use and
// tied to its PKCE challenge, its redirect URI and its client, as a real
// server ties it; refresh tokens can be revoked.
internal sealed class FakeOAuth : IAsyncDisposable
{
    public const string ClientId = "client-fixture.apps.googleusercontent.com";
    public const string ClientSecret = "client-secret-fixture";

    private readonly WebApplication _app;
    private readonly ConcurrentDictionary<string, Grant> _codes = new(StringComparer.Ordinal);
    private int _next;

    private FakeOAuth(WebApplication app, Uri baseUri)
    {
        _app = app;
        BaseUri = baseUri;
    }

    public Uri BaseUri { get; }

    public GoogleOAuthEndpoints Google => new(new Uri(BaseUri, "o/oauth2/v2/auth"), new Uri(BaseUri, "token"),
        new Uri(BaseUri, "revoke"));

    // Sign in with ChatGPT (phase 28): the issuer is this server's origin.
    public ChatGptEndpoints ChatGpt => new(Issuer, new Uri(BaseUri, "api/accounts/authorize"),
        new Uri(BaseUri, "api/accounts/oauth/token"), new Uri(BaseUri, "api/accounts/oauth/revoke"));

    public string Issuer => BaseUri.GetLeftPart(UriPartial.Authority);

    // The clock the ChatGPT tokens' times are read from: the test's.
    public Func<DateTimeOffset> Now { get; set; } = () => DateTimeOffset.UtcNow;

    // earliest_refresh_at, this long after a token is issued.
    public int EarliestRefreshIn { get; set; } = 3000;

    // Whether the owner allowed the plan's scope on the consent page.
    public bool GrantPlan { get; set; } = true;

    // An ID token with another nonce than the one asked for.
    public bool WrongNonce { get; set; }

    // The clients registered for dynamic_agent_client, in order.
    public ConcurrentQueue<string> RegisteredClients { get; } = new();

    // Each live ChatGPT refresh token and its client; a used one is gone.
    private readonly ConcurrentDictionary<string, string> _chatRefresh = new(StringComparer.Ordinal);

    // The key OpenRouter's exchange answers.
    public string OpenRouterKey { get; set; } = FakeOpenAi.Key;

    // The lifetime Google's tokens are given.
    public int ExpiresIn { get; set; } = 3600;

    // Every token and code this server has issued: none may be in a log or an answer.
    public ConcurrentBag<string> Issued { get; } = [];

    public ConcurrentDictionary<string, bool> RevokedTokens { get; } = new(StringComparer.Ordinal);

    public int Refreshes;

    // The access token issued last: the only one a vendor should take once it is renewed.
    public string? CurrentAccess { get; private set; }

    public static async Task<FakeOAuth> StartAsync()
    {
        FakeOAuth? fake = null;
        var (app, uri) = await FixtureHttp.StartAsync(context => fake!.HandleAsync(context));
        fake = new FakeOAuth(app, uri);
        return fake;
    }

    public ValueTask DisposeAsync() => _app.DisposeAsync();

    // The owner consents in the browser: the code for this URL's challenge.
    public string Consent(Uri authorizeUrl)
    {
        var query = QueryHelpers.ParseQuery(authorizeUrl.Query);
        if (query["code_challenge_method"] != "S256")
            throw new InvalidOperationException("no S256 challenge");
        var code = $"code-{Interlocked.Increment(ref _next)}-{Guid.NewGuid():N}";
        _codes[code] = new Grant(query["code_challenge"]!, query.GetValueOrDefault("redirect_uri").ToString(),
            query.GetValueOrDefault("client_id").ToString(), null);
        Issued.Add(code);
        return code;
    }

    // The owner signs in with ChatGPT: the code, and the client_id the
    // redirect names (a new one registered for dynamic_agent_client).
    public (string Code, string ClientId) ConsentChatGpt(Uri authorizeUrl)
    {
        var query = QueryHelpers.ParseQuery(authorizeUrl.Query);
        if (query["code_challenge_method"] != "S256" || query["resource"] != ChatGptLogin.Resource)
            throw new InvalidOperationException("no S256 challenge or no resource");
        var client = query["client_id"].ToString();
        if (client == ChatGptLogin.DynamicClient)
        {
            client = $"app_{Interlocked.Increment(ref _next)}";
            RegisteredClients.Enqueue(client);
        }
        var code = $"ac_{Interlocked.Increment(ref _next)}-{Guid.NewGuid():N}";
        _codes[code] = new Grant(query["code_challenge"]!, query["redirect_uri"].ToString(), client,
            query["nonce"].ToString());
        Issued.Add(code);
        return (code, client);
    }

    private async Task HandleAsync(HttpContext context)
    {
        var request = context.Request;
        switch (request.Method, request.Path.Value)
        {
            case ("POST", "/api/accounts/oauth/token"):
            {
                var form = await request.ReadFormAsync();
                if (form["resource"] != ChatGptLogin.Resource)
                {
                    await Answer(context.Response, 400, """{"error":"invalid_request","error_description":"resource"}""");
                    return;
                }
                var client = form["client_id"].ToString();
                if (form["grant_type"] == "authorization_code")
                {
                    var code = form["code"].ToString();
                    var nonce = _codes.TryGetValue(code, out var grant) ? grant.Nonce : null;
                    if (!Redeem(code, form["code_verifier"]!, form["redirect_uri"]!, client))
                    {
                        await Answer(context.Response, 400, """{"error":"invalid_grant","error_description":"Invalid authorization code"}""");
                        return;
                    }
                    await Answer(context.Response, 200, ChatTokens(client, WrongNonce ? "another-nonce" : nonce));
                    return;
                }
                if (form["grant_type"] == "refresh_token")
                {
                    Interlocked.Increment(ref Refreshes);
                    var refresh = form["refresh_token"].ToString();
                    if (RevokedTokens.ContainsKey(refresh) || !_chatRefresh.TryRemove(refresh, out var owner) || owner != client)
                    {
                        await Answer(context.Response, 400, """{"error":"invalid_grant","error_description":"refresh_token_reused"}""");
                        return;
                    }
                    await Answer(context.Response, 200, ChatTokens(client, null));
                    return;
                }
                await Answer(context.Response, 400, """{"error":"unsupported_grant_type"}""");
                return;
            }
            case ("POST", "/api/accounts/oauth/revoke"):
            {
                var form = await request.ReadFormAsync();
                var token = form["token"].ToString();
                RevokedTokens[token] = true;
                _chatRefresh.TryRemove(token, out _);
                context.Response.StatusCode = 200;
                return;
            }
            case ("POST", "/api/v1/auth/keys"):
            {
                using var body = await JsonDocument.ParseAsync(request.Body);
                var root = body.RootElement;
                if (!Redeem(root.GetProperty("code").GetString()!, root.GetProperty("code_verifier").GetString()!, null, null))
                {
                    await Answer(context.Response, 403, """{"error":{"message":"Invalid code or code_verifier","code":403}}""");
                    return;
                }
                await Answer(context.Response, 200, JsonSerializer.Serialize(new { key = OpenRouterKey, user_id = "user-1" }));
                return;
            }
            case ("POST", "/token"):
            {
                var form = await request.ReadFormAsync();
                if (form["client_id"] != ClientId || form["client_secret"] != ClientSecret)
                {
                    await Answer(context.Response, 401, """{"error":"invalid_client","error_description":"Unauthorized"}""");
                    return;
                }
                if (form["grant_type"] == "authorization_code")
                {
                    if (!Redeem(form["code"]!, form["code_verifier"]!, form["redirect_uri"]!, form["client_id"]!))
                    {
                        await Answer(context.Response, 400, """{"error":"invalid_grant","error_description":"Bad Request"}""");
                        return;
                    }
                    await Answer(context.Response, 200, Tokens(withRefresh: true));
                    return;
                }
                if (form["grant_type"] == "refresh_token")
                {
                    Interlocked.Increment(ref Refreshes);
                    var refresh = form["refresh_token"].ToString();
                    if (RevokedTokens.ContainsKey(refresh) || !Issued.Contains(refresh))
                    {
                        await Answer(context.Response, 400, """{"error":"invalid_grant","error_description":"Token has been expired or revoked."}""");
                        return;
                    }
                    await Answer(context.Response, 200, Tokens(withRefresh: false));
                    return;
                }
                await Answer(context.Response, 400, """{"error":"unsupported_grant_type"}""");
                return;
            }
            case ("POST", "/revoke"):
            {
                var form = await request.ReadFormAsync();
                RevokedTokens[form["token"].ToString()] = true;
                context.Response.StatusCode = 200;
                return;
            }
            default:
                await Answer(context.Response, 404, """{"error":"not_found"}""");
                return;
        }
    }

    // A code once, with the verifier of its challenge, from the redirect and
    // the client it was given for.
    private bool Redeem(string code, string verifier, string? redirectUri, string? clientId)
    {
        if (!_codes.TryRemove(code, out var grant))
            return false;
        var challenge = Base64Url.EncodeToString(SHA256.HashData(Encoding.ASCII.GetBytes(verifier)));
        return challenge == grant.Challenge && (redirectUri is null || redirectUri == grant.RedirectUri) &&
            (clientId is null || clientId == grant.ClientId);
    }

    private string Tokens(bool withRefresh)
    {
        var n = Interlocked.Increment(ref _next);
        var access = $"ya29.access-{n}";
        Issued.Add(access);
        CurrentAccess = access;
        string? refresh = null;
        if (withRefresh)
        {
            refresh = $"1//refresh-{n}";
            Issued.Add(refresh);
        }
        return JsonSerializer.Serialize(new Dictionary<string, object?>
        {
            ["access_token"] = access,
            ["expires_in"] = ExpiresIn,
            ["refresh_token"] = refresh,
            ["scope"] = GoogleLogin.Scope,
            ["token_type"] = "Bearer",
        });
    }

    // ChatGPT's token answer: a bearer token, a rotating refresh token, the
    // scopes granted, earliest_refresh_at, and (with a nonce, on a sign-in)
    // an ID token from this issuer for the client. Unsigned: the login takes
    // it straight from the token endpoint and does not check a signature.
    private string ChatTokens(string client, string? nonce)
    {
        var n = Interlocked.Increment(ref _next);
        var access = $"chatgpt-access-{n}";
        var refresh = $"chatgpt-refresh-{n}";
        Issued.Add(access);
        Issued.Add(refresh);
        CurrentAccess = access;
        _chatRefresh[refresh] = client;
        var now = Now();
        var answer = new Dictionary<string, object?>
        {
            ["access_token"] = access,
            ["refresh_token"] = refresh,
            ["token_type"] = "Bearer",
            ["expires_in"] = ExpiresIn,
            ["earliest_refresh_at"] = now.AddSeconds(EarliestRefreshIn).ToUnixTimeSeconds(),
            ["scope"] = GrantPlan ? ChatGptLogin.Scopes : "openid profile email offline_access",
        };
        if (nonce is not null)
        {
            static string Part(object value) => Base64Url.EncodeToString(JsonSerializer.SerializeToUtf8Bytes(value));
            var idToken = $"{Part(new { alg = "RS256", typ = "JWT" })}.{Part(new Dictionary<string, object>
            {
                ["iss"] = Issuer,
                ["aud"] = client,
                ["sub"] = "user-fixture",
                ["nonce"] = nonce,
                ["iat"] = now.ToUnixTimeSeconds(),
                ["exp"] = now.AddHours(1).ToUnixTimeSeconds(),
                ["email"] = "owner@example.com",
            })}.signature";
            Issued.Add(idToken);
            answer["id_token"] = idToken;
        }
        return JsonSerializer.Serialize(answer);
    }

    private static Task Answer(HttpResponse response, int status, string json)
    {
        response.StatusCode = status;
        response.ContentType = "application/json";
        return response.WriteAsync(json);
    }

    private sealed record Grant(string Challenge, string RedirectUri, string ClientId, string? Nonce);
}
