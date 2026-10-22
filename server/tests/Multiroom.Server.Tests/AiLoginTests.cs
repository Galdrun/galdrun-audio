using Microsoft.AspNetCore.WebUtilities;
using Microsoft.Extensions.Logging.Abstractions;
using Multiroom.Server.Ai;
using Multiroom.Server.Ai.Providers;
using Multiroom.Server.Ai.Providers.Gemini;
using Multiroom.Server.Ai.Providers.OpenRouter;

namespace Multiroom.Server.Tests;

// Browser logins (phase 22) against FakeOAuth, with FakeOpenAi as the vendor
// the credential is then used with: OpenRouter's PKCE exchange for a key,
// Google's for a token that is renewed before it expires; states that are
// reused, expired or unknown refused; a wrong verifier refused by the fake;
// logout revoking; no token, verifier or code in any answer.
public sealed class AiLoginTests : IAsyncLifetime
{
    private static readonly HttpClient Http = new(new SocketsHttpHandler { UseProxy = false });
    private static readonly Uri AppPage = new("http://app.local/ai/done");

    private readonly TempDirectory _dir = new();
    private readonly Clock _clock = new();
    private FakeOAuth _oauth = null!;
    private FakeOpenAi _vendor = null!;
    private AiCredentialStore _credentials = null!;
    private AiProviderRegistry _registry = null!;

    private static CancellationToken Ct => TestContext.Current.CancellationToken;

    public async ValueTask InitializeAsync()
    {
        _oauth = await FakeOAuth.StartAsync();
        _vendor = await FakeOpenAi.StartAsync();
        AiProviderConfig[] config =
        [
            new(OpenRouterProvider.ProviderId, null, _vendor.BaseUri),
            new(GeminiProvider.ProviderId, null, _vendor.BaseUri),
        ];
        _credentials = new AiCredentialStore(Path.Combine(_dir.Path, "credentials.json"), config);
        _registry = new AiProviderRegistry(
            [
                new AiProviderRegistration(OpenRouterProvider.ProviderId, c => new OpenRouterProvider(c), TimeSpan.FromSeconds(5)),
                new AiProviderRegistration(GeminiProvider.ProviderId, c => new GeminiProvider(c), TimeSpan.FromSeconds(5)),
            ],
            config, _credentials, new AiSettingsStore(Path.Combine(_dir.Path, "settings.json")),
            NullLogger<AiProviderRegistry>.Instance,
            [
                new AiLoginRegistration(OpenRouterProvider.ProviderId, h => new OpenRouterLogin(h, _oauth.BaseUri)),
                new AiLoginRegistration(GeminiProvider.ProviderId, h => new GoogleLogin(h, _oauth.Google, _clock)),
            ],
            _clock);
        await _registry.StartAsync(Ct);
    }

    public async ValueTask DisposeAsync()
    {
        _registry.Dispose();
        await _vendor.DisposeAsync();
        await _oauth.DisposeAsync();
        _dir.Dispose();
    }

    private static AiRequest Hello => new("model-a", [new AiMessage(AiRole.User, "hi")]);

    [Fact]
    public async Task An_openrouter_login_exchanges_its_code_for_a_key_that_is_then_used()
    {
        var start = _registry.StartLogin(OpenRouterProvider.ProviderId, AppPage).Value;
        var query = QueryHelpers.ParseQuery(start.AuthorizeUrl.Query);

        Assert.Equal(new Uri(_oauth.BaseUri, "auth").AbsoluteUri, start.AuthorizeUrl.GetLeftPart(UriPartial.Path));
        // OpenRouter brings back only the code: the state rides in the callback's query.
        Assert.Equal($"{AppPage.AbsoluteUri}?state={start.State}", query["callback_url"].ToString());
        Assert.Equal(("S256", 43), (query["code_challenge_method"].ToString(), query["code_challenge"].ToString().Length));
        Assert.Equal(1, _registry.PendingLogins);

        var code = _oauth.Consent(start.AuthorizeUrl);
        var view = await _registry.CompleteLoginAsync(start.State, code, null, Ct);

        Assert.True(view.IsSuccess, view.ToString());
        Assert.Equal((true, true, AiKeyOrigin.Api, false, (bool?)null),
            (view.Value.Configured, view.Value.HasKey, view.Value.Origin, view.Value.HasToken, view.Value.HasClient));
        Assert.Equal([AiAuthMethod.ApiKey, AiAuthMethod.OAuth], view.Value.AuthMethods);
        Assert.Equal(0, _registry.PendingLogins);
        Assert.Equal(FakeOpenAi.Key, _credentials.Key(OpenRouterProvider.ProviderId));
        Assert.True((await _registry.CompleteAsync(OpenRouterProvider.ProviderId, Hello, Ct)).IsSuccess);

        // Single-use: the same state again is unknown.
        var again = await _registry.CompleteLoginAsync(start.State, code, null, Ct);
        Assert.Equal((AiError.InvalidArgument, "state: unknown or already used; start the login again"),
            (again.Failure.Code, again.Failure.Detail));
    }

    [Fact]
    public async Task A_reused_expired_unknown_or_refused_state_and_a_wrong_verifier_are_refused()
    {
        var unknown = await _registry.CompleteLoginAsync("made-up", "code", null, Ct);
        Assert.Equal(AiError.InvalidArgument, unknown.Failure.Code);

        var expiring = _registry.StartLogin(OpenRouterProvider.ProviderId, AppPage).Value;
        var code = _oauth.Consent(expiring.AuthorizeUrl);
        _clock.Advance(AiProviderRegistry.LoginLifetime + TimeSpan.FromSeconds(1));
        var expired = await _registry.CompleteLoginAsync(expiring.State, code, null, Ct);
        Assert.Equal((AiError.InvalidArgument, "state: the login expired; start it again"),
            (expired.Failure.Code, expired.Failure.Detail));
        Assert.Equal(AiError.InvalidArgument, (await _registry.CompleteLoginAsync(expiring.State, code, null, Ct)).Failure.Code);

        var denied = _registry.StartLogin(OpenRouterProvider.ProviderId, AppPage).Value;
        var refused = await _registry.CompleteLoginAsync(denied.State, null, "access_denied", Ct);
        Assert.Equal((AiError.AuthFailed, "the login was refused: access_denied"), (refused.Failure.Code, refused.Failure.Detail));
        Assert.Equal(0, _registry.PendingLogins);

        // The verifier stays on this server; the fake refuses a code redeemed
        // with another, as a real server does.
        var start = _registry.StartLogin(OpenRouterProvider.ProviderId, AppPage).Value;
        var stolen = _oauth.Consent(start.AuthorizeUrl);
        var challenge = QueryHelpers.ParseQuery(start.AuthorizeUrl.Query)["code_challenge"].ToString();
        var wrong = await new OpenRouterLogin(Http, _oauth.BaseUri).CompleteAsync(
            new AiLoginRequest(start.State, "not-the-verifier-0123456789-0123456789-0123", challenge, AppPage, null), stolen, null, Ct);
        Assert.Equal(AiError.AuthFailed, wrong.Failure.Code);
        Assert.DoesNotContain(stolen, wrong.Failure.Detail, StringComparison.Ordinal);

        await _registry.SetClientAsync(GeminiProvider.ProviderId, FakeOAuth.ClientId, FakeOAuth.ClientSecret, Ct);
        var google = _registry.StartLogin(GeminiProvider.ProviderId, AppPage).Value;
        var googleCode = _oauth.Consent(google.AuthorizeUrl);
        var wrongGoogle = await new GoogleLogin(Http, _oauth.Google).CompleteAsync(
            new AiLoginRequest(google.State, "not-the-verifier-0123456789-0123456789-0123", "x", AppPage,
                new AiOAuthClient(FakeOAuth.ClientId, FakeOAuth.ClientSecret)), googleCode, null, Ct);
        Assert.Equal((AiError.InvalidArgument, "google login: HTTP 400: invalid_grant"),
            (wrongGoogle.Failure.Code, wrongGoogle.Failure.Detail));

        // In progress at once: bounded.
        for (var i = _registry.PendingLogins; i < AiProviderRegistry.MaxPendingLogins; i++)
            Assert.True(_registry.StartLogin(OpenRouterProvider.ProviderId, AppPage).IsSuccess);
        Assert.Equal(AiError.RateLimited, _registry.StartLogin(OpenRouterProvider.ProviderId, AppPage).Failure.Code);
        _clock.Advance(AiProviderRegistry.LoginLifetime);
        Assert.True(_registry.StartLogin(OpenRouterProvider.ProviderId, AppPage).IsSuccess);
        Assert.Equal(1, _registry.PendingLogins);

        Assert.Equal(AiError.NotFound, _registry.StartLogin("nobody", AppPage).Failure.Code);
        Assert.Equal(AiError.InvalidArgument,
            _registry.StartLogin(OpenRouterProvider.ProviderId, new Uri("ftp://app.local/")).Failure.Code);
    }

    [Fact]
    public async Task A_google_login_needs_the_owners_client_and_its_token_is_renewed_before_it_expires()
    {
        var noClient = _registry.StartLogin(GeminiProvider.ProviderId, AppPage);
        Assert.Equal(AiError.NotConfigured, noClient.Failure.Code);
        var before = Assert.Single(await _registry.ViewsAsync(Ct), v => v.Id == GeminiProvider.ProviderId);
        Assert.Equal((false, (bool?)false), (before.Configured, before.HasClient));
        Assert.Contains("POST /api/v1/ai/providers/gemini/login", before.Detail, StringComparison.Ordinal);

        var withClient = await _registry.SetClientAsync(GeminiProvider.ProviderId, $" {FakeOAuth.ClientId} ",
            FakeOAuth.ClientSecret, Ct);
        Assert.Equal(true, withClient.Value.HasClient);
        var start = _registry.StartLogin(GeminiProvider.ProviderId, AppPage).Value;
        var query = QueryHelpers.ParseQuery(start.AuthorizeUrl.Query);
        Assert.Equal(_oauth.Google.Authorize.AbsoluteUri, start.AuthorizeUrl.GetLeftPart(UriPartial.Path));
        Assert.Equal((FakeOAuth.ClientId, AppPage.AbsoluteUri, "code", GoogleLogin.Scope, start.State, "S256", "offline"),
            (query["client_id"].ToString(), query["redirect_uri"].ToString(), query["response_type"].ToString(),
                query["scope"].ToString(), query["state"].ToString(), query["code_challenge_method"].ToString(),
                query["access_type"].ToString()));

        var view = (await _registry.CompleteLoginAsync(start.State, _oauth.Consent(start.AuthorizeUrl), null, Ct)).Value;
        Assert.Equal((true, false, true, AiKeyOrigin.Api), (view.Configured, view.HasKey, view.HasToken, view.Origin));

        // The vendor takes only the token issued last.
        _vendor.Accepts = token => token == _oauth.CurrentAccess;
        Assert.True((await _registry.CompleteAsync(GeminiProvider.ProviderId, Hello, Ct)).IsSuccess);
        Assert.Equal(0, _oauth.Refreshes);

        // Within a minute of its end: renewed before the call, the refresh token kept.
        var first = _credentials.Token(GeminiProvider.ProviderId)!;
        _clock.Advance(TimeSpan.FromSeconds(3600) - AiTokenSource.Margin + TimeSpan.FromSeconds(1));
        var answers = await Task.WhenAll(Enumerable.Range(0, 3)
            .Select(_ => _registry.CompleteAsync(GeminiProvider.ProviderId, Hello, Ct)));
        Assert.All(answers, a => Assert.True(a.IsSuccess, a.ToString()));
        Assert.Equal(1, _oauth.Refreshes);
        var renewed = _credentials.Token(GeminiProvider.ProviderId)!;
        Assert.NotEqual(first.AccessToken, renewed.AccessToken);
        Assert.Equal(first.RefreshToken, renewed.RefreshToken);
        Assert.Equal(_clock.GetUtcNow().AddSeconds(3600), renewed.ExpiresAt);

        // The owner revoked it at Google: the next renewal is refused, and the
        // provider's status says the vendor no longer takes it.
        _oauth.RevokedTokens[renewed.RefreshToken!] = true;
        _clock.Advance(TimeSpan.FromSeconds(3600));
        var refused = await _registry.CompleteAsync(GeminiProvider.ProviderId, Hello, Ct);
        Assert.Equal(AiError.AuthFailed, refused.Failure.Code);
        var after = Assert.Single(await _registry.ViewsAsync(Ct), v => v.Id == GeminiProvider.ProviderId);
        Assert.Equal(((bool?)false, true), (after.Authorised, after.HasToken));

        Assert.All(_oauth.Issued, secret => Assert.DoesNotContain(secret, refused.Failure.Detail, StringComparison.Ordinal));
    }

    [Fact]
    public async Task Logout_forgets_the_token_revokes_it_at_google_and_keeps_the_client()
    {
        await _registry.SetClientAsync(GeminiProvider.ProviderId, FakeOAuth.ClientId, FakeOAuth.ClientSecret, Ct);
        var start = _registry.StartLogin(GeminiProvider.ProviderId, AppPage).Value;
        await _registry.CompleteLoginAsync(start.State, _oauth.Consent(start.AuthorizeUrl), null, Ct);
        var refresh = _credentials.Token(GeminiProvider.ProviderId)!.RefreshToken!;

        var view = (await _registry.RemoveKeyAsync(GeminiProvider.ProviderId, Ct)).Value;

        Assert.Equal((false, false, AiKeyOrigin.None, (bool?)true, false),
            (view.HasToken, view.HasKey, view.Origin, view.HasClient, view.Configured));
        Assert.True(_oauth.RevokedTokens.ContainsKey(refresh));
        Assert.Null(_credentials.Token(GeminiProvider.ProviderId));
        Assert.Equal(AiError.NotConfigured, (await _registry.CompleteAsync(GeminiProvider.ProviderId, Hello, Ct)).Failure.Code);

        // A key set after a login replaces it; one login at a time.
        var restart = _registry.StartLogin(GeminiProvider.ProviderId, AppPage).Value;
        await _registry.CompleteLoginAsync(restart.State, _oauth.Consent(restart.AuthorizeUrl), null, Ct);
        var keyed = (await _registry.SetKeyAsync(GeminiProvider.ProviderId, "AIza-key", Ct)).Value;
        Assert.Equal((true, false), (keyed.HasKey, keyed.HasToken));

        var file = await File.ReadAllTextAsync(Path.Combine(_dir.Path, "credentials.json"), Ct);
        Assert.Contains(FakeOAuth.ClientId, file, StringComparison.Ordinal);
        Assert.Equal(AiError.InvalidArgument,
            (await _registry.SetClientAsync(OpenRouterProvider.ProviderId, "id", null, Ct)).Failure.Code);
        Assert.False((await _registry.RemoveClientAsync(GeminiProvider.ProviderId, Ct)).Value.HasClient ?? true);
    }

    [Fact]
    public async Task The_registry_refuses_a_login_for_no_provider_twice_or_for_one_that_does_not_list_oauth()
    {
        async Task<string> Refusal(AiLoginRegistration[] logins)
        {
            using var registry = new AiProviderRegistry(
                [new AiProviderRegistration(GeminiProvider.ProviderId, c => new GeminiProvider(c), TimeSpan.FromSeconds(1)),
                 new AiProviderRegistration(OpenRouterProvider.ProviderId, c => new OpenRouterProvider(c), TimeSpan.FromSeconds(1))],
                [], _credentials, new AiSettingsStore(Path.Combine(_dir.Path, "settings.json")),
                NullLogger<AiProviderRegistry>.Instance, logins);
            return (await Assert.ThrowsAsync<InvalidOperationException>(() => registry.StartAsync(Ct))).Message;
        }

        var google = new AiLoginRegistration(GeminiProvider.ProviderId, h => new GoogleLogin(h));
        var openRouter = new AiLoginRegistration(OpenRouterProvider.ProviderId, h => new OpenRouterLogin(h));

        Assert.Equal("AI login nobody is for no AI provider", await Refusal([google, openRouter, google with { Id = "nobody" }]));
        Assert.Equal("AI login gemini is registered twice", await Refusal([google, google, openRouter]));
        Assert.Equal("AI provider openrouter lists oauth but has no login", await Refusal([google]));
    }

    // A UTC clock moved by the test alone.
    private sealed class Clock : TimeProvider
    {
        private DateTimeOffset _now = new(2026, 10, 1, 12, 0, 0, TimeSpan.Zero);

        public override DateTimeOffset GetUtcNow() => _now;

        public void Advance(TimeSpan by) => _now += by;
    }
}
