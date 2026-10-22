using System.Net;
using Multiroom.Server.Abstractions;

namespace Multiroom.Server.Providers.Tidal;

public sealed record TidalLoginStart(Uri AuthorizeUrl, string UserCode, DateTimeOffset ExpiresAt);

internal sealed partial class TidalSession
{
    private readonly Lock _loginLock = new();
    private readonly SemaphoreSlim _loginStart = new(1, 1);
    private TidalLoginStart? _login;

    public TidalLoginStart? Login { get { lock (_loginLock) return _login; } }

    public async Task<Result<TidalLoginStart, ProviderError>> StartLoginAsync(CancellationToken cancellationToken)
    {
        await _loginStart.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            CancellationToken stop;
            lock (_loginLock)
            {
                if (_stop.IsCancellationRequested || string.IsNullOrEmpty(_settings.ClientId))
                    return Result.Fail(ProviderError.Unavailable, "set providers.tidal.clientId in the config file before signing in");
                if (_login is { } active && active.ExpiresAt > DateTimeOffset.UtcNow)
                    return active;
                stop = _stop.Token;
            }
            using var linked = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken, stop);
            var answer = await PostFormAsync("oauth2/device_authorization", Form(SignIn, ("scope", Scope)), linked.Token)
                .ConfigureAwait(false);
            if (answer.Status != HttpStatusCode.OK || answer.Json is not { } json)
                return Result.Fail(ProviderError.Unavailable, $"Tidal device sign-in could not start (HTTP {(int)answer.Status}); check the application ID and secret");
            var device = json.Text("deviceCode") ?? json.Text("device_code");
            var code = json.Text("userCode") ?? json.Text("user_code");
            var address = json.Text("verificationUriComplete") ?? json.Text("verification_uri_complete") ??
                json.Text("verificationUri") ?? json.Text("verification_uri");
            var seconds = json.Number("expiresIn") ?? json.Number("expires_in") ?? 300;
            if (address is not null && !address.Contains("://", StringComparison.Ordinal))
                address = "https://" + address;
            if (device is null || code is null || seconds is <= 0 or > 86400 ||
                !Uri.TryCreate(address, UriKind.Absolute, out var url) || url.Scheme is not ("https" or "http"))
                return Result.Fail(ProviderError.Failed, "Tidal's device sign-in answer is incomplete");
            var login = new TidalLoginStart(url, code, DateTimeOffset.UtcNow.AddSeconds(seconds));
            var interval = TimeSpan.FromSeconds(Math.Clamp(json.Number("interval") ?? 5, 1, 86400));
            lock (_loginLock)
            {
                linked.Token.ThrowIfCancellationRequested();
                _login = login;
                _waiting = "approve the device code in Tidal to finish signing in";
                _ = PollLoginAsync(device, login, interval, stop);
            }
            return login;
        }
        catch (OperationCanceledException) when (!cancellationToken.IsCancellationRequested)
        {
            return Result.Fail(ProviderError.Unavailable, "the Tidal application changed; start the login again");
        }
        finally { _loginStart.Release(); }
    }

    // Only this server sees deviceCode. The browser approves userCode; no redirect is needed.
    private async Task PollLoginAsync(string device, TidalLoginStart login, TimeSpan interval, CancellationToken stop)
    {
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(stop);
        timeout.CancelAfter(login.ExpiresAt - DateTimeOffset.UtcNow);
        string? failure = null;
        try
        {
            while (true)
            {
                await Task.Delay(interval, timeout.Token).ConfigureAwait(false);
                await _refresh.WaitAsync(timeout.Token).ConfigureAwait(false);
                try
                {
                    var answer = await PostFormAsync("oauth2/token",
                        Form(SignIn, ("grant_type", "urn:ietf:params:oauth:grant-type:device_code"),
                            ("device_code", device), ("scope", Scope)), timeout.Token).ConfigureAwait(false);
                    if (answer.Status == HttpStatusCode.OK && answer.Json is { } json)
                    {
                        var token = await ToTokenAsync(json, null, timeout.Token).ConfigureAwait(false);
                        // Renewed at once by the playback client; if that
                        // fails, the device token is used, and the next
                        // refresh tries again.
                        var renewedBy = SignIn.Id;
                        if (token.IsSuccess && Renewal.Id != SignIn.Id)
                        {
                            var renewed = (await RenewAsync(token.Value, timeout.Token).ConfigureAwait(false)).Token;
                            if (renewed.IsSuccess)
                                (token, renewedBy) = (renewed, Renewal.Id);
                            else
                                LogPlaybackRenewalFailed(_log, renewed.Failure.Detail);
                        }
                        lock (_loginLock)
                        {
                            timeout.Token.ThrowIfCancellationRequested();
                            if (!ReferenceEquals(_login, login)) return;
                            if (!token.IsSuccess) { failure = token.Failure.Detail; return; }
                            Save(token.Value, renewedBy);
                            _token = token.Value;
                            _waiting = "";
                        }
                        LogAuthorized(_log, token.Value.UserId, token.Value.CountryCode);
                        return;
                    }
                    var error = answer.Json?.Text("error");
                    if (error == "slow_down") interval += TimeSpan.FromSeconds(5);
                    else if (error != "authorization_pending" && answer.Status is not (0 or >= HttpStatusCode.InternalServerError or HttpStatusCode.TooManyRequests))
                    {
                        failure = "Tidal refused or expired the device login; sign in again";
                        return;
                    }
                }
                finally { _refresh.Release(); }
            }
        }
        catch (OperationCanceledException) when (timeout.IsCancellationRequested)
        {
            if (!stop.IsCancellationRequested) failure = "the Tidal device login expired; sign in again";
        }
        finally
        {
            lock (_loginLock)
            {
                if (ReferenceEquals(_login, login))
                {
                    _login = null;
                    if (failure is not null) _waiting = failure;
                }
            }
        }
    }
}
