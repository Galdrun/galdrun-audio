using Microsoft.Extensions.DependencyInjection;
using Multiroom.Server.Abstractions;

namespace Multiroom.Server.Ai;

// The one seam every browser login implements.
// The registry keeps what every login shares -- the single-use state, its
// 10 minutes, the PKCE verifier, the store -- so a login only builds the
// vendor's authorization URL and speaks to its token endpoint. It answers
// data and fails with a Result, never by throwing.
public interface IAiLogin
{
    // Whether it needs an OAuth client of the owner's (Google): set with
    // PUT ai/providers/{id}/client before a login can start.
    bool NeedsClient { get; }

    // The URL the app opens in its browser, or why this login cannot start
    // (ChatGPT takes only a loopback redirect, phase 28).
    Result<Uri, AiError> Start(AiLoginRequest request);

    // The code the redirect brought, exchanged for a credential for the
    // store. `issuedClientId` is the client the vendor registered during the
    // flow and named on the redirect (ChatGPT's client_id, phase 28); null
    // where it names none.
    Task<Result<AiCredential, AiError>> CompleteAsync(AiLoginRequest request, string code, string? issuedClientId,
        CancellationToken cancellationToken);

    // A credential with ExpiresAt, renewed. AuthFailed when the vendor
    // refuses the refresh token: the owner must log in again.
    Task<Result<AiCredential, AiError>> RefreshAsync(AiCredential credential, AiOAuthClient? client,
        CancellationToken cancellationToken);

    // Where the vendor offers it; a vendor that does not succeeds at once.
    Task<Result<bool, AiError>> RevokeAsync(AiCredential credential, AiOAuthClient? client,
        CancellationToken cancellationToken);
}

// One login to be registered: the provider it logs into, and how to make it
// from the HTTP client every AI provider shares.
public sealed record AiLoginRegistration(string Id, Func<HttpClient, IAiLogin> Create);

public static class AiLoginServiceCollectionExtensions
{
    // One line per login, in the provider's AddXxx().
    public static IServiceCollection AddAiLogin(this IServiceCollection services, string id,
        Func<HttpClient, IAiLogin> create) =>
        services.AddSingleton(new AiLoginRegistration(id, create));
}

// What a login is given to start or complete: the state, the PKCE pair, where
// the browser returns, the OAuth client (the owner's, or one the vendor
// issued earlier), and an OpenID nonce. A class, not a record: the verifier
// is the login's secret (rule 7).
public sealed class AiLoginRequest(string state, string verifier, string challenge, Uri redirectUri,
    AiOAuthClient? client, string nonce = "")
{
    public string State { get; } = state;

    // What an ID token must carry back (OpenID Connect, phase 28).
    public string Nonce { get; } = nonce;

    public string Verifier { get; } = verifier;

    // base64url(SHA-256(verifier)), sent as S256.
    public string Challenge { get; } = challenge;

    public Uri RedirectUri { get; } = redirectUri;

    public AiOAuthClient? Client { get; } = client;
}

// What a login answers for the store: an API key (OpenRouter), or an access
// token with its expiry and a refresh token (Google). Classes, not records,
// for the same reason as AiProviderConfig.
public sealed class AiCredential
{
    public string? ApiKey { get; init; }

    public string? AccessToken { get; init; }

    public string? RefreshToken { get; init; }

    public DateTimeOffset? ExpiresAt { get; init; }

    // When the vendor wants it renewed, before ExpiresAt (ChatGPT's
    // earliest_refresh_at, phase 28).
    public DateTimeOffset? RefreshAfter { get; init; }

    // A client the vendor registered for this installation during the login
    // (ChatGPT's dynamic client, phase 28): kept beside the token, and
    // through a logout, as the owner's client is.
    public AiOAuthClient? Client { get; init; }
}

// The owner's OAuth client, from their own cloud project: there is no
// built-in one. A desktop client's secret is not secret, a web client's is.
public sealed class AiOAuthClient(string clientId, string? clientSecret)
{
    public string ClientId { get; } = clientId;

    public string? ClientSecret { get; } = clientSecret;
}

// A login started: the URL to open, the state the app hands back, and when
// that state stops being taken.
public sealed record AiLoginStart(Uri AuthorizeUrl, string State, Uri RedirectUri, DateTimeOffset ExpiresAt);
