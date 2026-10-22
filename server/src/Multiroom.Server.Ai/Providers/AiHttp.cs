using System.Net;
using System.Text.Json;
using Multiroom.Server.Abstractions;

namespace Multiroom.Server.Ai.Providers;

// What a provider is given by the registry: the HTTP client every provider
// shares (no timeout of its own: the registry's bounds each call), its key
// as it is now (the store's, read on each call, so a key set through the API
// is used at once), the config's base URL, when one is set, and its login's
// token, when it has a login (phase 22).
public sealed class AiProviderContext(HttpClient http, Func<string?> key, Uri? baseUrl = null,
    AiTokenSource? token = null)
{
    public HttpClient Http { get; } = http;

    public Func<string?> Key { get; } = key;

    public Uri? BaseUrl { get; } = baseUrl;

    public AiTokenSource? Token { get; } = token;

    public bool HasCredential => Key() is not null || Token?.HasToken == true;

    // What a call is authorised with: the key, else the login's access token
    // (renewed when near its end), else null.
    public async Task<Result<string?, AiError>> CredentialAsync(CancellationToken cancellationToken)
    {
        if (Key() is { } key)
            return key;
        if (Token is null || !Token.HasToken)
            return (string?)null;
        var access = await Token.AccessTokenAsync(cancellationToken).ConfigureAwait(false);
        return access.IsSuccess ? access.Value : access.Failure;
    }
}

// The HTTP half every wire format shares: one JSON exchange, its status
// mapped to an AiError, and a vendor's error body summarised, never relayed
// whole -- it can echo the key.
internal static class AiHttp
{
    private const int MaxDetail = 200;

    public static async Task<Result<JsonElement, AiError>> SendAsync(HttpClient http, HttpRequestMessage request,
        string? key, CancellationToken cancellationToken)
    {
        HttpResponseMessage response;
        try
        {
            response = await http.SendAsync(request, cancellationToken).ConfigureAwait(false);
        }
        catch (HttpRequestException e)
        {
            return Result.Fail(AiError.Unavailable, $"{request.RequestUri?.Host}: {e.Message}");
        }
        using (response)
        {
            string text;
            try
            {
                text = await response.Content.ReadAsStringAsync(cancellationToken).ConfigureAwait(false);
            }
            catch (HttpRequestException e)
            {
                return Result.Fail(AiError.Unavailable, $"{request.RequestUri?.Host}: {e.Message}");
            }

            var json = Parse(text);
            if (response.IsSuccessStatusCode)
            {
                return json is { ValueKind: JsonValueKind.Object } answer
                    ? answer
                    : Result.Fail(AiError.InvalidAnswer, $"HTTP {(int)response.StatusCode} without a JSON object");
            }

            return StatusFailure(response.StatusCode, json, key);
        }
    }

    // A refusal by its status, with the vendor's own message summarised.
    public static Failure<AiError> StatusFailure(HttpStatusCode status, JsonElement? json, string? key) =>
        Result.Fail(status switch
        {
            HttpStatusCode.Unauthorized or HttpStatusCode.Forbidden => AiError.AuthFailed,
            HttpStatusCode.TooManyRequests => AiError.RateLimited,
            >= HttpStatusCode.InternalServerError => AiError.Unavailable,
            _ => AiError.InvalidArgument,
        }, $"HTTP {(int)status}" + (Summary(json, key) is { } summary ? $": {summary}" : ""));

    // A JSON text parsed, or null when it is not JSON.
    public static JsonElement? Parse(string text)
    {
        try
        {
            using var document = JsonDocument.Parse(text);
            return document.RootElement.Clone();
        }
        catch (JsonException)
        {
            return null;
        }
    }

    // The URI with the parameters added to its query, each escaped.
    public static Uri WithQuery(Uri uri, params (string Name, string Value)[] parameters)
    {
        var query = string.Join('&', parameters.Select(p =>
            $"{Uri.EscapeDataString(p.Name)}={Uri.EscapeDataString(p.Value)}"));
        var builder = new UriBuilder(uri);
        builder.Query = builder.Query.Length > 1 ? $"{builder.Query[1..]}&{query}" : query;
        return builder.Uri;
    }

    public static FormUrlEncodedContent Form(params (string Name, string? Value)[] fields) =>
        new(fields.Where(f => f.Value is not null).Select(f => new KeyValuePair<string, string>(f.Name, f.Value!)));

    public static StringContent Body(object body) =>
        new(JsonSerializer.Serialize(body), System.Text.Encoding.UTF8, "application/json");

    public static JsonElement? Get(this JsonElement element, string name) =>
        element.ValueKind == JsonValueKind.Object && element.TryGetProperty(name, out var value) &&
        value.ValueKind is not (JsonValueKind.Null or JsonValueKind.Undefined)
            ? value
            : null;

    public static string? Text(this JsonElement element, string name) =>
        element.Get(name) is { ValueKind: JsonValueKind.String } value ? value.GetString() : null;

    public static long Number(this JsonElement element, string name) =>
        element.Get(name) is { ValueKind: JsonValueKind.Number } value && value.TryGetInt64(out var number) ? number : 0;

    public static IEnumerable<JsonElement> Items(this JsonElement element, string name) =>
        element.Get(name) is { ValueKind: JsonValueKind.Array } array ? array.EnumerateArray() : [];

    // The vendor's own message ({"error":{"message"}} or {"error":"..."} or
    // {"message"}), cut short, with the key taken out. Google's
    // OpenAI-compatible endpoint answers its errors as a one-item array.
    private static string? Summary(JsonElement? json, string? key)
    {
        if (json is { ValueKind: JsonValueKind.Array } list && list.GetArrayLength() > 0)
            json = list[0];
        if (json is not { ValueKind: JsonValueKind.Object } body)
            return null;
        var message = body.Get("error") switch
        {
            { ValueKind: JsonValueKind.Object } error => error.Text("message") ?? error.Text("type"),
            { ValueKind: JsonValueKind.String } error => error.GetString(),
            _ => body.Text("message"),
        };
        if (string.IsNullOrWhiteSpace(message))
            return null;
        if (!string.IsNullOrEmpty(key))
            message = message.Replace(key, "***", StringComparison.Ordinal);
        return message.Length <= MaxDetail ? message : message[..MaxDetail] + "…";
    }
}
