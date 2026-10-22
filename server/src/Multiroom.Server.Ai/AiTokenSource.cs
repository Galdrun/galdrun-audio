using Multiroom.Server.Abstractions;

namespace Multiroom.Server.Ai;

// A provider's login token as its calls need it (phase 22): the store's, and
// renewed through the login before it expires, on the provider's own call
// path and under a lock, so two calls renew it once. Nothing that waits on
// the core asks for it.
public sealed class AiTokenSource : IDisposable
{
    // Renewed this long before it expires, so a call never carries a token
    // that dies on its way.
    public static readonly TimeSpan Margin = TimeSpan.FromSeconds(60);

    private readonly string _id;
    private readonly AiCredentialStore _store;
    private readonly IAiLogin _login;
    private readonly TimeProvider _time;
    private readonly SemaphoreSlim _lock = new(1, 1);

    public AiTokenSource(string id, AiCredentialStore store, IAiLogin login, TimeProvider time)
    {
        _id = id;
        _store = store;
        _login = login;
        _time = time;
    }

    public bool HasToken => _store.Token(_id) is not null;

    public void Dispose() => _lock.Dispose();

    // The access token, renewed when it is near its end. NotConfigured
    // without one; AuthFailed when the vendor refused the refresh token.
    public async Task<Result<string, AiError>> AccessTokenAsync(CancellationToken cancellationToken)
    {
        if (Fresh(_store.Token(_id)) is { } fresh)
            return fresh;
        await _lock.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            var token = _store.Token(_id);
            if (token is null)
                return Result.Fail(AiError.NotConfigured, "not logged in");
            if (Fresh(token) is { } renewed)
                return renewed;
            if (token.RefreshToken is null)
                return Result.Fail(AiError.AuthFailed, "the login expired; log in again");
            var refreshed = await _login.RefreshAsync(token, _store.Client(_id), cancellationToken).ConfigureAwait(false);
            if (!refreshed.IsSuccess)
                return refreshed.Failure;
            var stored = _store.SetCredential(_id, refreshed.Value);
            if (!stored.IsSuccess)
                return stored.Failure;
            return refreshed.Value.AccessToken!;
        }
        finally
        {
            _lock.Release();
        }
    }

    // Fresh until Margin before its end, or until the vendor's RefreshAfter
    // (ChatGPT's earliest_refresh_at, phase 28), whichever comes first.
    private string? Fresh(AiCredential? token)
    {
        var now = _time.GetUtcNow();
        return token?.AccessToken is { } access && (token.ExpiresAt is not { } expires || expires - now > Margin) &&
            (token.RefreshAfter is not { } after || now < after)
            ? access
            : null;
    }
}
