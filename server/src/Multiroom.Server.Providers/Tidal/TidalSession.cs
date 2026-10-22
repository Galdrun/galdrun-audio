using System.Net;
using System.Text.Json;
using Microsoft.Extensions.Logging;
using Multiroom.Server.Abstractions;

namespace Multiroom.Server.Providers.Tidal;

// What Tidal granted: the tokens, when the access token expires, and the
// account's user id and country, which every catalog call names.
internal sealed record TidalToken(string AccessToken, string RefreshToken, DateTimeOffset ExpiresAt, long UserId,
    string CountryCode);

// The account is linked with a device code approved in any browser.
//
// The tokens are kept in `tokenPath`, readable by this account only, and
// refreshed silently: a minute before they expire, and once more when the
// API answers 401 anyway. A refresh token Tidal refuses is forgotten, and
// the owner starts a new browser login.
//
// Refreshing is the renewal client's: the playback client when one is set,
// else the client that signed in. A device login is renewed by it at once,
// and a kept token last renewed by another client is renewed at its first use.
//
// No token, no client secret and no signed URL goes in a message or a log
// line (rule 7).
internal sealed partial class TidalSession : IDisposable
{
    private const string Scope = "r_usr w_usr w_sub";
    private static readonly TimeSpan RefreshMargin = TimeSpan.FromMinutes(1);

    private readonly HttpClient _http;
    private readonly TidalSettings _settings;
    private readonly TidalEndpoints _endpoints;
    private readonly string _tokenPath;
    private readonly ILogger _log;
    private readonly SemaphoreSlim _refresh = new(1, 1);
    private readonly CancellationTokenSource _stop = new();
    private volatile TidalToken? _token;
    private volatile string _waiting;

    public TidalSession(HttpClient http, TidalSettings settings, TidalEndpoints endpoints, string tokenPath,
        ILogger log)
    {
        _http = http;
        _settings = settings;
        _endpoints = endpoints;
        _tokenPath = tokenPath;
        _log = log;
        _waiting = settings.ClientId is null
            ? "no Tidal client id; set providers.tidal.clientId in the config file"
            : "sign in with POST /api/v1/providers/tidal/login";
    }

    public TidalSettings Settings => _settings;

    public TidalToken? Token => _token;

    // Why there is no token, and what to open: the AuthRequired detail.
    public string Waiting => _waiting;

    // Reads the kept tokens. A browser login starts only when requested.
    // Without a client id, neither: every call is Unavailable.
    public void Start()
    {
        if (_settings.ClientId is null)
            return;
        _token = Load();
    }

    // GET <api>/<path>?<query>&countryCode=<the account's>, authorized. `what`
    // names the item in a failure.
    public async Task<Result<JsonElement, ProviderError>> GetAsync(string path,
        IReadOnlyList<(string Key, string Value)> query, string what, CancellationToken cancellationToken)
    {
        var token = _token;
        if (token is null)
        {
            return Result.Fail(ProviderError.Unavailable, $"{what}: Tidal is not authorized; {_waiting}");
        }
        if (token.ExpiresAt - RefreshMargin <= DateTimeOffset.UtcNow)
        {
            var refreshed = await RefreshAsync(token, cancellationToken).ConfigureAwait(false);
            if (!refreshed.IsSuccess)
                return Result.Fail(refreshed.Failure.Code, $"{what}: {refreshed.Failure.Detail}");
            token = refreshed.Value;
        }

        var answer = await SendAsync(path, query, token, cancellationToken).ConfigureAwait(false);
        if (answer.Status == HttpStatusCode.Unauthorized)
        {
            // Expired early, or revoked: one refresh, one retry.
            var refreshed = await RefreshAsync(token, cancellationToken).ConfigureAwait(false);
            if (!refreshed.IsSuccess)
                return Result.Fail(refreshed.Failure.Code, $"{what}: {refreshed.Failure.Detail}");
            answer = await SendAsync(path, query, refreshed.Value, cancellationToken).ConfigureAwait(false);
        }

        if (answer.Status == HttpStatusCode.OK && answer.Json is { } json)
            return json;
        var detail = answer.Error ?? $"Tidal answered HTTP {(int)answer.Status}";
        return Result.Fail(answer.Status switch
        {
            HttpStatusCode.NotFound => ProviderError.NotFound,
            HttpStatusCode.OK => ProviderError.Failed,
            0 or HttpStatusCode.Unauthorized or HttpStatusCode.Forbidden or HttpStatusCode.TooManyRequests => ProviderError.Unavailable,
            >= HttpStatusCode.InternalServerError => ProviderError.Unavailable,
            _ => ProviderError.Failed,
        }, $"{what}: {detail}");
    }

    public void Dispose()
    {
        lock (_loginLock)
        {
            _login = null;
            _stop.Cancel();
            _stop.Dispose();
        }
        // _refresh is not disposed: a refresh still waiting on it at shutdown
        // would throw, and its wait handle is never read, so it holds nothing.
    }

    private async Task<Result<TidalToken, ProviderError>> RefreshAsync(TidalToken stale,
        CancellationToken cancellationToken)
    {
        await _refresh.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            var current = _token;
            if (current is null)
                return Result.Fail(ProviderError.Unavailable, $"Tidal is not authorized; {_waiting}");
            if (!ReferenceEquals(current, stale))
                return current;

            var (token, refused) = await RenewAsync(current, cancellationToken).ConfigureAwait(false);
            if (token.IsSuccess)
            {
                Save(token.Value);
                _token = token.Value;
                LogRefreshed(_log, token.Value.ExpiresAt);
                return token.Value;
            }
            if (!refused)
                return token.Failure;
            Forget();
            return Result.Fail(ProviderError.Unavailable, $"Tidal refused the refresh token; {_waiting}");
        }
        finally
        {
            _refresh.Release();
        }
    }

    // One refresh grant by the renewal client, under _refresh. Refused: Tidal
    // will not renew this refresh token at all.
    private async Task<(Result<TidalToken, ProviderError> Token, bool Refused)> RenewAsync(TidalToken current,
        CancellationToken cancellationToken)
    {
        var answer = await PostFormAsync("oauth2/token",
            Form(Renewal, ("grant_type", "refresh_token"), ("refresh_token", current.RefreshToken), ("scope", Scope)),
            cancellationToken).ConfigureAwait(false);
        if (answer.Status == HttpStatusCode.OK && answer.Json is { } json)
            return (await ToTokenAsync(json, current, cancellationToken).ConfigureAwait(false), false);
        if (answer.Status is HttpStatusCode.BadRequest or HttpStatusCode.Unauthorized)
            return (Result.Fail(ProviderError.Unavailable, "Tidal refused the refresh token"), true);
        return (Result.Fail(ProviderError.Unavailable,
            $"cannot refresh the Tidal token: {answer.Error ?? $"HTTP {(int)answer.Status}"}"), false);
    }

    private void Forget()
    {
        _token = null;
        _waiting = "the account link expired; sign in with POST /api/v1/providers/tidal/login";
        LogForgotten(_log);
        try
        {
            File.Delete(_tokenPath);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            LogTokenFileFailed(_log, _tokenPath, e.Message);
        }
    }

    // Tidal's token answer. A refresh may leave out the refresh token and the
    // user, which are then the previous token's. Without a user, the session
    // endpoint names it.
    private async Task<Result<TidalToken, ProviderError>> ToTokenAsync(JsonElement json, TidalToken? previous,
        CancellationToken cancellationToken)
    {
        var access = json.Text("access_token");
        var refresh = json.Text("refresh_token") ?? previous?.RefreshToken;
        if (access is null || refresh is null)
            return Result.Fail(ProviderError.Failed, "Tidal's token answer has no tokens");
        var expires = DateTimeOffset.UtcNow + TimeSpan.FromSeconds(json.Number("expires_in") ?? 3600);

        var user = json.Get("user");
        var userId = user?.Number("userId") ?? json.Number("user_id") ?? previous?.UserId;
        var country = user?.Text("countryCode") ?? previous?.CountryCode;
        if (userId is null || country is null)
        {
            var partial = new TidalToken(access, refresh, expires, 0, "");
            var session = await SendAsync("sessions", [], partial, cancellationToken, withCountry: false)
                .ConfigureAwait(false);
            if (session.Status != HttpStatusCode.OK || session.Json is not { } found)
                return Result.Fail(ProviderError.Unavailable, $"cannot read the Tidal session: {session.Error ?? $"HTTP {(int)session.Status}"}");
            userId ??= found.Number("userId");
            country ??= found.Text("countryCode");
            if (userId is null || country is null)
                return Result.Fail(ProviderError.Failed, "the Tidal session names no user or country");
        }
        return new TidalToken(access, refresh, expires, userId.Value, country);
    }

    private async Task<Answer> SendAsync(string path, IReadOnlyList<(string Key, string Value)> query, TidalToken token,
        CancellationToken cancellationToken, bool withCountry = true)
    {
        IEnumerable<(string Key, string Value)> all = withCountry ? [.. query, ("countryCode", token.CountryCode)] : query;
        var text = string.Join('&', all.Select(q => $"{Uri.EscapeDataString(q.Key)}={Uri.EscapeDataString(q.Value)}"));
        using var request = new HttpRequestMessage(HttpMethod.Get, new Uri(_endpoints.Api, text.Length == 0 ? path : $"{path}?{text}"));
        request.Headers.Authorization = new System.Net.Http.Headers.AuthenticationHeaderValue("Bearer", token.AccessToken);
        return await ExchangeAsync(request, cancellationToken).ConfigureAwait(false);
    }

    private async Task<Answer> PostFormAsync(string path, List<KeyValuePair<string, string>> form,
        CancellationToken cancellationToken)
    {
        using var request = new HttpRequestMessage(HttpMethod.Post, new Uri(_endpoints.Auth, path));
        using var content = new FormUrlEncodedContent(form);
        request.Content = content;
        return await ExchangeAsync(request, cancellationToken).ConfigureAwait(false);
    }

    // The client that signs in, and the one that renews the tokens.
    private (string Id, string? Secret) SignIn => (_settings.ClientId!, _settings.ClientSecret);

    private (string Id, string? Secret) Renewal =>
        _settings.PlaybackClientId is { } id ? (id, _settings.PlaybackClientSecret) : SignIn;

    // The client's id and secret go with every form.
    private static List<KeyValuePair<string, string>> Form((string Id, string? Secret) client,
        params (string Key, string Value)[] fields)
    {
        var form = new List<KeyValuePair<string, string>> { new("client_id", client.Id) };
        if (client.Secret is { Length: > 0 } secret)
            form.Add(new("client_secret", secret));
        form.AddRange(fields.Select(f => new KeyValuePair<string, string>(f.Key, f.Value)));
        return form;
    }

    // Status 0: no answer at all, with Error saying why. Error is otherwise
    // Tidal's own message, when its answer has one.
    private async Task<Answer> ExchangeAsync(HttpRequestMessage request, CancellationToken cancellationToken)
    {
        try
        {
            using var response = await _http.SendAsync(request, cancellationToken).ConfigureAwait(false);
            var body = await response.Content.ReadAsByteArrayAsync(cancellationToken).ConfigureAwait(false);
            JsonElement? json = null;
            if (body.Length > 0)
            {
                try
                {
                    using var document = JsonDocument.Parse(body);
                    json = document.RootElement.Clone();
                }
                catch (JsonException)
                {
                    if (response.IsSuccessStatusCode)
                        return new Answer(HttpStatusCode.OK, null, "Tidal's answer is not JSON");
                }
            }
            var message = response.IsSuccessStatusCode
                ? null
                : json?.Text("userMessage") ?? json?.Text("error_description") ?? json?.Text("error");
            return new Answer(response.StatusCode, json,
                message is null ? null : $"Tidal answered HTTP {(int)response.StatusCode}: {message}");
        }
        catch (OperationCanceledException) when (!cancellationToken.IsCancellationRequested)
        {
            return new Answer(0, null, "Tidal did not answer in time");
        }
        catch (HttpRequestException e)
        {
            return new Answer(0, null, $"Tidal cannot be reached ({e.HttpRequestError})");
        }
    }

    private TidalToken? Load()
    {
        if (!File.Exists(_tokenPath))
            return null;
        try
        {
            using var document = JsonDocument.Parse(File.ReadAllBytes(_tokenPath));
            var root = document.RootElement;
            if (root.Text("clientId") != _settings.ClientId)
                return null;
            if (root.Text("accessToken") is { } access && root.Text("refreshToken") is { } refresh &&
                root.Number("expiresAt") is { } expires && root.Number("userId") is { } user &&
                root.Text("countryCode") is { } country)
            {
                // Last renewed by another client (none named: the one that
                // signed in): expired, so its first use renews it by this one.
                var renewedBy = root.Text("renewedBy") ?? _settings.ClientId;
                return new TidalToken(access, refresh,
                    renewedBy == Renewal.Id ? DateTimeOffset.FromUnixTimeSeconds(expires) : DateTimeOffset.UnixEpoch,
                    user, country);
            }
            LogTokenFileFailed(_log, _tokenPath, "it is incomplete");
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or JsonException)
        {
            LogTokenFileFailed(_log, _tokenPath, e.Message);
        }
        return null;
    }

    // Written whole under a temporary name, readable by this account only,
    // then moved over the old one. A failure is logged: the tokens still work
    // until this process ends.
    private void Save(TidalToken token, string? renewedBy = null)
    {
        renewedBy ??= Renewal.Id;
        var bytes = JsonSerializer.SerializeToUtf8Bytes(new Dictionary<string, object>
        {
            ["clientId"] = _settings.ClientId!,
            ["renewedBy"] = renewedBy,
            ["accessToken"] = token.AccessToken,
            ["refreshToken"] = token.RefreshToken,
            ["expiresAt"] = token.ExpiresAt.ToUnixTimeSeconds(),
            ["userId"] = token.UserId,
            ["countryCode"] = token.CountryCode,
        });
        try
        {
            OwnerOnlyFile.Write(_tokenPath, bytes);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            LogTokenFileFailed(_log, _tokenPath, e.Message);
        }
    }

    [LoggerMessage(Level = LogLevel.Information, Message = "tidal: authorized as user {UserId} ({CountryCode})")]
    private static partial void LogAuthorized(ILogger log, long userId, string countryCode);

    [LoggerMessage(Level = LogLevel.Debug, Message = "tidal: token refreshed, valid until {ExpiresAt:u}")]
    private static partial void LogRefreshed(ILogger log, DateTimeOffset expiresAt);

    [LoggerMessage(Level = LogLevel.Warning, Message = "tidal: the refresh token was refused; the account link is forgotten")]
    private static partial void LogForgotten(ILogger log);

    [LoggerMessage(Level = LogLevel.Warning, Message = "tidal: the playback client did not renew the new link ({Reason}); the signed-in client's token is used until the next refresh")]
    private static partial void LogPlaybackRenewalFailed(ILogger log, string reason);

    [LoggerMessage(Level = LogLevel.Warning, Message = "tidal: token file {Path}: {Reason}")]
    private static partial void LogTokenFileFailed(ILogger log, string path, string reason);

    private readonly record struct Answer(HttpStatusCode Status, JsonElement? Json, string? Error);
}
