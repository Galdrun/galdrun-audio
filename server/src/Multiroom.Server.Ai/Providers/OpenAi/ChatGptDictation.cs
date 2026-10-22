using System.Buffers.Text;
using System.Net.Http.Headers;
using System.Text.Json;
using Multiroom.Server.Abstractions;

namespace Multiroom.Server.Ai.Providers.OpenAi;

// Speech on the ChatGPT sign-in: ChatGPT's own dictation route, POST
// chatgpt.com/backend-api/transcribe, multipart `file` and `language`,
// answering {text}. It is undocumented -- OpenAI's Sign in with ChatGPT
// takes no audio -- and was read from community clients (oh-my-pi #15391,
// 2026-10): the Codex desktop app's originator and user agent, the account
// id and data residency from the token's claims. It may change or refuse
// this sign-in's token without notice; the GUI then hears in the browser.
// The backend picks the speech model, so none is sent. The bearer goes to
// chatgpt.com only (a test's fake aside), never to a configured base URL.
public sealed class ChatGptDictation(AiProviderContext context, Uri? backend = null)
{
    public const string Originator = "Codex Desktop";
    public const string UserAgent = "Codex Desktop/0.159.0";

    private const string Claims = "https://api.openai.com/auth";
    private const int MaxDetail = 200;

    private static readonly Uri Default = new("https://chatgpt.com/backend-api/");

    private readonly Uri _backend = backend ?? Default;

    public async Task<Result<string, AiError>> TranscribeAsync(AiTranscriptionRequest request,
        CancellationToken cancellationToken)
    {
        var credential = await context.CredentialAsync(cancellationToken).ConfigureAwait(false);
        if (!credential.IsSuccess)
            return Result.Fail(credential.Failure.Code, $"chatgpt dictation: {credential.Failure.Detail}");
        if (credential.Value is not { } token)
            return Result.Fail(AiError.NotConfigured, "chatgpt dictation: not signed in with ChatGPT");

        var audio = new ByteArrayContent(request.Audio);
        audio.Headers.ContentType = new MediaTypeHeaderValue(request.MediaType);
        // It picks its decoder by the file's extension.
        using var form = new MultipartFormDataContent { { audio, "file", "audio.wav" } };
        if (request.Language is { } language)
            form.Add(new StringContent(language.Split('-')[0].ToLowerInvariant()), "language");
        using var message = new HttpRequestMessage(HttpMethod.Post, new Uri(_backend, "transcribe")) { Content = form };
        message.Headers.Authorization = new AuthenticationHeaderValue("Bearer", token);
        message.Headers.Accept.Add(new MediaTypeWithQualityHeaderValue("application/json"));
        message.Headers.TryAddWithoutValidation("User-Agent", UserAgent);
        message.Headers.Add("originator", Originator);
        if (Claim(token, "chatgpt_account_id") is { } account)
            message.Headers.Add("chatgpt-account-id", account);
        // A region-pinned workspace is refused without its residency.
        if ((Claim(token, "chatgpt_data_residency") ?? Claim(token, "chatgpt_compute_residency")) is { } residency)
            message.Headers.Add("x-openai-internal-codex-residency", residency);

        HttpResponseMessage response;
        try
        {
            response = await context.Http.SendAsync(message, cancellationToken).ConfigureAwait(false);
        }
        catch (HttpRequestException e)
        {
            return Result.Fail(AiError.Unavailable, $"chatgpt dictation: {e.Message}");
        }
        using (response)
        {
            var body = await response.Content.ReadAsStringAsync(cancellationToken).ConfigureAwait(false);
            if (!response.IsSuccessStatusCode)
            {
                var code = (int)response.StatusCode switch
                {
                    401 or 403 => AiError.AuthFailed,
                    429 => AiError.RateLimited,
                    >= 500 => AiError.Unavailable,
                    _ => AiError.InvalidArgument,
                };
                // Never the body whole: a Cloudflare page, or an echo of the token.
                var detail = body.Replace(token, "***", StringComparison.Ordinal);
                return Result.Fail(code, $"chatgpt dictation: {(int)response.StatusCode} {(detail.Length > MaxDetail ? detail[..MaxDetail] + "…" : detail)}");
            }
            try
            {
                using var json = JsonDocument.Parse(body);
                return json.RootElement.Text("text") is { } text
                    ? text.Trim()
                    : Result.Fail(AiError.InvalidAnswer, "chatgpt dictation: an answer without text");
            }
            catch (JsonException)
            {
                return Result.Fail(AiError.InvalidAnswer, "chatgpt dictation: an answer that is not JSON");
            }
        }
    }

    // One claim of the access token's OpenAI section; null when the token is
    // no JWT or has none. Read, never verified: it is only sent back.
    private static string? Claim(string token, string name)
    {
        var parts = token.Split('.');
        if (parts.Length != 3)
            return null;
        try
        {
            using var payload = JsonDocument.Parse(Base64Url.DecodeFromChars(parts[1]));
            return payload.RootElement.TryGetProperty(Claims, out var auth) && auth.ValueKind == JsonValueKind.Object &&
                auth.TryGetProperty(name, out var value) && value.ValueKind == JsonValueKind.String &&
                value.GetString() is { Length: > 0 } text
                ? text
                : null;
        }
        catch (Exception e) when (e is FormatException or JsonException)
        {
            return null;
        }
    }
}
