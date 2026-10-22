using System.Buffers.Text;
using System.Collections.Concurrent;
using System.Security.Cryptography;
using System.Text;
using Microsoft.Extensions.Logging;
using Multiroom.Server.Abstractions;

namespace Multiroom.Server.Ai;

// Browser logins. This server opens no browser:
// an app starts a login, opens the URL it is given, and the redirect brings
// the code back to the app's page (which completes it) or to this server's
// callback. What every login shares is kept here: a state is single-use,
// lives LoginLifetime, and carries the PKCE verifier on this server only.
// At most MaxPendingLogins are in progress (rule 6); an expired one is
// dropped when the next starts.
public sealed partial class AiProviderRegistry
{
    public const int MaxPendingLogins = 16;

    public static readonly TimeSpan LoginLifetime = TimeSpan.FromMinutes(10);

    private const int MaxVendorError = 100;

    private readonly ConcurrentDictionary<string, PendingLogin> _pending = new(StringComparer.Ordinal);
    private readonly Lock _loginLock = new();

    public int PendingLogins => _pending.Count;

    public Result<AiLoginStart, AiError> StartLogin(string id, Uri redirectUri)
    {
        var entry = Find(id);
        if (!entry.IsSuccess)
            return entry.Failure;
        if (entry.Value.Login is not { } login)
            return Result.Fail(AiError.InvalidArgument, $"{id} has no browser login");
        if (credentials.Origin(id) == AiKeyOrigin.Config)
            return Result.Fail(AiError.InvalidArgument, $"the key of {id} is from the config file; change it there");
        if (!redirectUri.IsAbsoluteUri || redirectUri.Scheme is not ("http" or "https") ||
            redirectUri.Fragment.Length > 0)
        {
            return Result.Fail(AiError.InvalidArgument, "redirectUri: an absolute http or https URL without a fragment");
        }
        var client = credentials.Client(id);
        if (login.NeedsClient && client is null)
        {
            return Result.Fail(AiError.NotConfigured,
                $"{id} needs your own OAuth client; set it with PUT /api/v1/ai/providers/{id}/client");
        }

        var now = _time.GetUtcNow();
        var verifier = RandomText();
        var request = new AiLoginRequest(RandomText(), verifier,
            Base64Url.EncodeToString(SHA256.HashData(Encoding.ASCII.GetBytes(verifier))), redirectUri, client,
            RandomText());
        var url = login.Start(request);
        if (!url.IsSuccess)
            return url.Failure;
        var pending = new PendingLogin(id, request, now + LoginLifetime);
        lock (_loginLock)
        {
            foreach (var (state, old) in _pending)
            {
                if (old.ExpiresAt <= now)
                    _pending.TryRemove(state, out _);
            }
            if (_pending.Count >= MaxPendingLogins)
            {
                return Result.Fail(AiError.RateLimited,
                    $"{MaxPendingLogins} logins are in progress; finish one or wait {LoginLifetime.TotalMinutes:0} minutes");
            }
            _pending[request.State] = pending;
        }
        return new AiLoginStart(url.Value, request.State, redirectUri, pending.ExpiresAt);
    }

    // The redirect's answer: a code, or the vendor's error, and the client
    // the vendor issued during the flow where it names one (ChatGPT's
    // client_id, phase 28). The state is taken whatever happens, so it is
    // never used twice.
    public Task<Result<AiProviderView, AiError>> CompleteLoginAsync(string? state, string? code, string? error,
        CancellationToken cancellationToken) =>
        CompleteLoginAsync(state, code, error, null, cancellationToken);

    public async Task<Result<AiProviderView, AiError>> CompleteLoginAsync(string? state, string? code, string? error,
        string? clientId, CancellationToken cancellationToken)
    {
        if (string.IsNullOrEmpty(state) || !_pending.TryRemove(state, out var pending))
            return Result.Fail(AiError.InvalidArgument, "state: unknown or already used; start the login again");
        if (pending.ExpiresAt <= _time.GetUtcNow())
            return Result.Fail(AiError.InvalidArgument, "state: the login expired; start it again");
        if (!string.IsNullOrEmpty(error))
        {
            var said = error.Length <= MaxVendorError ? error : error[..MaxVendorError] + "…";
            return Result.Fail(AiError.AuthFailed, $"the login was refused: {said}");
        }
        if (string.IsNullOrWhiteSpace(code))
            return Result.Fail(AiError.InvalidArgument, "code: the code the redirect brought");

        var entry = _entries[pending.ProviderId];
        var login = entry.Login!;
        var credential = await CallAsync(entry, ct => login.CompleteAsync(pending.Request, code,
            string.IsNullOrEmpty(clientId) ? null : clientId, ct), cancellationToken)
            .ConfigureAwait(false);
        if (!credential.IsSuccess)
            return credential.Failure;
        var stored = credentials.SetCredential(pending.ProviderId, credential.Value);
        if (!stored.IsSuccess)
            return stored.Failure;
        entry.Forget();
        LogLoggedIn(log, pending.ProviderId);
        return await ViewAsync(entry, cancellationToken).ConfigureAwait(false);
    }

    // The owner's OAuth client, for a login that needs one. Logging in again
    // is up to the owner: a token got with the old client stays.
    public async Task<Result<AiProviderView, AiError>> SetClientAsync(string id, string clientId, string? clientSecret,
        CancellationToken cancellationToken)
    {
        var entry = Find(id);
        if (!entry.IsSuccess)
            return entry.Failure;
        if (entry.Value.Login?.NeedsClient != true)
            return Result.Fail(AiError.InvalidArgument, $"{id} takes no OAuth client");
        if (string.IsNullOrWhiteSpace(clientId))
            return Result.Fail(AiError.InvalidArgument, "clientId: the OAuth client's id");
        var secret = string.IsNullOrWhiteSpace(clientSecret) ? null : clientSecret.Trim();
        var set = credentials.SetClient(id, new AiOAuthClient(clientId.Trim(), secret));
        if (!set.IsSuccess)
            return set.Failure;
        return await ViewAsync(entry.Value, cancellationToken).ConfigureAwait(false);
    }

    public async Task<Result<AiProviderView, AiError>> RemoveClientAsync(string id, CancellationToken cancellationToken)
    {
        var entry = Find(id);
        if (!entry.IsSuccess)
            return entry.Failure;
        if (entry.Value.Login?.NeedsClient != true)
            return Result.Fail(AiError.InvalidArgument, $"{id} takes no OAuth client");
        var removed = credentials.SetClient(id, null);
        if (!removed.IsSuccess)
            return removed.Failure;
        return await ViewAsync(entry.Value, cancellationToken).ConfigureAwait(false);
    }

    // 32 random bytes: a state, or a PKCE verifier (43 characters, within RFC 7636's 43 to 128).
    private static string RandomText() => Base64Url.EncodeToString(RandomNumberGenerator.GetBytes(32));

    [LoggerMessage(Level = LogLevel.Information, Message = "AI provider {Id}: logged in")]
    private static partial void LogLoggedIn(ILogger log, string id);

    private sealed record PendingLogin(string ProviderId, AiLoginRequest Request, DateTimeOffset ExpiresAt);
}
