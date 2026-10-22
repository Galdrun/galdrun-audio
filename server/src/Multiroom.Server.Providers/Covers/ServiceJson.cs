using System.Net;
using System.Text.Json;
using Multiroom.Server.Abstractions;

namespace Multiroom.Server.Providers.Covers;

// The artwork sources' JSON requests: an answer, null for 404, Unavailable
// for too many requests or the service down. A message names the host,
// never the URL, which may carry a key.
internal static class ServiceJson
{
    // A Wikidata item of a famous artist runs to a few MB.
    private const long MaxAnswerBytes = 16 * 1024 * 1024;

    private static readonly JsonElement NoItems = JsonSerializer.SerializeToElement(Array.Empty<int>());

    public static async Task<Result<JsonDocument?, ProviderError>> GetAsync(HttpClient http, Uri uri,
        CancellationToken cancellationToken)
    {
        try
        {
            using var response = await http.GetAsync(uri, HttpCompletionOption.ResponseHeadersRead, cancellationToken)
                .ConfigureAwait(false);
            if (response.StatusCode == HttpStatusCode.NotFound)
                return (JsonDocument?)null;
            if (!response.IsSuccessStatusCode)
            {
                return Result.Fail(
                    response.StatusCode is HttpStatusCode.TooManyRequests || (int)response.StatusCode >= 500
                        ? ProviderError.Unavailable
                        : ProviderError.Failed,
                    $"{uri.Host}: HTTP {(int)response.StatusCode}");
            }
            await response.Content.LoadIntoBufferAsync(MaxAnswerBytes, cancellationToken).ConfigureAwait(false);
            var body = await response.Content.ReadAsStreamAsync(cancellationToken).ConfigureAwait(false);
            await using (body.ConfigureAwait(false))
                return await JsonDocument.ParseAsync(body, cancellationToken: cancellationToken).ConfigureAwait(false);
        }
        catch (HttpRequestException e)
        {
            // Its message names the host; the kind is enough.
            return Result.Fail(ProviderError.Unavailable, $"{uri.Host} cannot be reached ({e.HttpRequestError})");
        }
        catch (JsonException e)
        {
            return Result.Fail(ProviderError.Failed, $"{uri.Host}: not JSON ({e.Message})");
        }
        catch (OperationCanceledException) when (!cancellationToken.IsCancellationRequested)
        {
            return Result.Fail(ProviderError.Unavailable, $"{uri.Host} did not answer in time");
        }
    }

    public static JsonElement.ArrayEnumerator Items(JsonElement element, string name) =>
        element.ValueKind == JsonValueKind.Object && element.TryGetProperty(name, out var items) &&
        items.ValueKind == JsonValueKind.Array
            ? items.EnumerateArray()
            : NoItems.EnumerateArray();

    public static string? String(JsonElement element, string name) =>
        element.ValueKind == JsonValueKind.Object && element.TryGetProperty(name, out var value) &&
        value.ValueKind == JsonValueKind.String
            ? value.GetString()
            : null;
}
