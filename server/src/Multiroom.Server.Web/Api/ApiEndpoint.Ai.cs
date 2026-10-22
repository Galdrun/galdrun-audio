using System.Globalization;
using System.Net;
using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Http;
using Multiroom.Server.Abstractions;
using Multiroom.Server.Ai;

namespace Multiroom.Server.Web.Api;

// The key in the body, never in a URL (the core logs none of ours, but a
// proxy may). Prints without it (rule 7).
public sealed record AiKeyRequest(string Key)
{
    public override string ToString() => nameof(AiKeyRequest);
}

// The settings as answered: every field present, the lists never null.
// SongProviders is the order found songs come from, the default's included
// (songProvidersDefault says which it is).
public sealed record AiSettingsView(string? DefaultProvider, string? DefaultModel,
    IReadOnlyList<string> DisabledProviders, IReadOnlyList<string> SongProviders, bool SongProvidersDefault,
    int MaxSuggestionRounds, AgentLimits Agent, AiReasoningSettings Reasoning, AiWebLookup WebLookup, AiSpeechSettings Speech);

public sealed record TranscriptionView(string Text);

// Where the browser returns: the app's own page, which posts the code to
// ai/login/complete; without one, this server's ai/login/callback.
public sealed record AiLoginStartRequest(string? RedirectUri = null);

// The redirect's code and state, and its client_id where the vendor names
// one (ChatGPT, phase 28). Prints without the code (rule 7).
public sealed record AiLoginCompleteRequest(string State, string Code, string? ClientId = null)
{
    public override string ToString() => nameof(AiLoginCompleteRequest);
}

// The owner's OAuth client from their own cloud project. Prints without the
// secret (rule 7).
public sealed record AiClientRequest(string ClientId, string? ClientSecret = null)
{
    public override string ToString() => nameof(AiClientRequest);
}

// The AI providers (phase 21): listed with whether they can be asked, their
// keys set and removed, their models listed, and the user's settings. A key
// from the config file is read-only here (origin: config); no answer carries
// a key, only hasKey. Phase 22 adds the browser logins: started here, opened
// by the app, completed by the app's page or by the callback, which alone
// takes no bearer token.
public sealed partial class ApiEndpoint
{
    private const string LoginCallbackPath = "/api/v1/ai/login/callback";

    private void MapAi(WebApplication app)
    {
        app.MapPost("/api/v1/ai/providers/{id}/login", StartLoginAsync);
        app.MapPost("/api/v1/ai/login/complete", (Delegate)((HttpContext c) =>
            WithBody<AiLoginCompleteRequest, AiProviderView, AiError>(c,
                r => ai.CompleteLoginAsync(r.State, r.Code, null, r.ClientId, c.RequestAborted))));
        app.MapGet(LoginCallbackPath, (Delegate)LoginCallbackAsync);
        app.MapPut("/api/v1/ai/providers/{id}/client", SetClientAsync);
        app.MapDelete("/api/v1/ai/providers/{id}/client", RemoveClientAsync);
        app.MapGet("/api/v1/ai/providers", (Delegate)(async (HttpContext c) =>
            Json(await ai.ViewsAsync(c.RequestAborted).ConfigureAwait(false))));
        app.MapPut("/api/v1/ai/providers/{id}/key", SetKeyAsync);
        app.MapDelete("/api/v1/ai/providers/{id}/key", RemoveKeyAsync);
        app.MapGet("/api/v1/ai/providers/{id}/models", ModelsAsync);
        app.MapGet("/api/v1/ai/usage", (Delegate)Usage);
        app.MapGet("/api/v1/ai/settings", () => Json(SettingsView(ai.Settings)));
        app.MapPost("/api/v1/ai/transcriptions", (Delegate)TranscribeAsync);
        app.MapMethods("/api/v1/ai/settings", [HttpMethods.Patch], (Delegate)((HttpContext c) =>
            WithBody<AiSettingsPatch, AiSettingsView, AiError>(c, p =>
            {
                // Song providers are music providers, which the AI registry does not know.
                if (p.SongProviders?.FirstOrDefault(id => !providers.Ids.Contains(id)) is { } unknown)
                {
                    return Task.FromResult<Result<AiSettingsView, AiError>>(
                        Result.Fail(AiError.NotFound, $"songProviders: no provider {unknown}"));
                }
                var saved = ai.PatchSettings(p);
                return Task.FromResult<Result<AiSettingsView, AiError>>(saved.IsSuccess ? SettingsView(saved.Value) : saved.Failure);
            })));
    }

    // The token usage per server-local day, origin, provider and model,
    // oldest first; ?from= and ?to= (yyyy-MM-dd, both included) bound it.
    private IResult Usage(HttpContext context)
    {
        if (UnknownQuery(context.Request, "from", "to") is { } unknown)
            return Error(unknown);
        var from = Day("from");
        if (!from.IsSuccess)
            return Error(from.Failure);
        var to = Day("to");
        if (!to.IsSuccess)
            return Error(to.Failure);
        var read = aiUsage.Days(from.Value, to.Value);
        return read.IsSuccess ? Json(read.Value) : Error(read.Failure);

        Result<DateOnly?, ApiRequestError> Day(string name)
        {
            var text = context.Request.Query[name].ToString();
            if (text.Length == 0)
                return (DateOnly?)null;
            return DateOnly.TryParseExact(text, "yyyy-MM-dd", CultureInfo.InvariantCulture, DateTimeStyles.None, out var day)
                ? day
                : Result.Fail(ApiRequestError.InvalidRequest, $"{name}: a day as yyyy-MM-dd, was {text}");
        }
    }

    private Task<IResult> SetKeyAsync(string id, HttpContext context) =>
        WithBody<AiKeyRequest, AiProviderView, AiError>(context,
            r => ai.SetKeyAsync(id, r.Key, context.RequestAborted));

    private async Task<IResult> RemoveKeyAsync(string id, HttpContext context) =>
        Answer(await ai.RemoveKeyAsync(id, context.RequestAborted).ConfigureAwait(false));

    // Cached per provider; refresh=true asks the vendor again.
    private async Task<IResult> ModelsAsync(string id, HttpContext context)
    {
        if (UnknownQuery(context.Request, "refresh") is { } unknown)
            return Error(unknown);
        var text = context.Request.Query["refresh"].ToString();
        if (text is not ("" or "true" or "false"))
            return Error(Result.Fail(ApiRequestError.InvalidRequest, $"refresh: true or false, was {text}"));
        return Answer(await ai.ListModelsAsync(id, text == "true", context.RequestAborted).ConfigureAwait(false));
    }

    private async Task<IResult> StartLoginAsync(string id, HttpContext context)
    {
        var body = await ApiJson.ReadAsync<AiLoginStartRequest>(context.Request, optional: true, context.RequestAborted)
            .ConfigureAwait(false);
        if (!body.IsSuccess)
            return Error(body.Failure);
        Uri redirect;
        if (body.Value?.RedirectUri is { } text)
        {
            if (!Uri.TryCreate(text, UriKind.Absolute, out var given))
                return Error(Result.Fail(AiError.InvalidArgument, "redirectUri: an absolute http or https URL"));
            redirect = given;
        }
        else
        {
            redirect = LoginCallbackUri;
        }
        return Answer(ai.StartLogin(id, redirect));
    }

    // The vendor's redirect: ?state&code, or ?state&error; ChatGPT adds the
    // client it issued (client_id, phase 28). Google adds
    // parameters of its own (scope, authuser, prompt), so the query is not
    // checked for unknown keys here.
    private async Task<IResult> LoginCallbackAsync(HttpContext context)
    {
        var query = context.Request.Query;
        return Answer(await ai.CompleteLoginAsync(query["state"].ToString(), query["code"].ToString(),
            query["error"].ToString(), query["client_id"].ToString(), context.RequestAborted).ConfigureAwait(false));
    }

    private Task<IResult> SetClientAsync(string id, HttpContext context) =>
        WithBody<AiClientRequest, AiProviderView, AiError>(context,
            r => ai.SetClientAsync(id, r.ClientId, r.ClientSecret, context.RequestAborted));

    private async Task<IResult> RemoveClientAsync(string id, HttpContext context) =>
        Answer(await ai.RemoveClientAsync(id, context.RequestAborted).ConfigureAwait(false));

    // The callback as a browser on this machine reaches it: a wildcard bind
    // is reached on loopback.
    private Uri LoginCallbackUri
    {
        get
        {
            var builder = new UriBuilder(BaseUri!);
            if (options.Bind.Equals(IPAddress.Any) || options.Bind.Equals(IPAddress.IPv6Any))
                builder.Host = "127.0.0.1";
            return new Uri(builder.Uri, LoginCallbackPath);
        }
    }

    private static bool IsLoginCallback(HttpRequest request) =>
        HttpMethods.IsGet(request.Method) && request.Path.Equals(LoginCallbackPath, StringComparison.Ordinal);

    // The assistant's microphone (speech.recognition server): WAV in, the
    // words spoken out, by the speech settings' provider and model.
    private async Task<IResult> TranscribeAsync(HttpContext context)
    {
        if (context.Request.ContentType?.Split(';')[0].Trim().ToLowerInvariant() is not ("audio/wav" or "audio/wave" or "audio/x-wav"))
            return Error(Result.Fail(AiError.InvalidArgument, "the body is audio/wav"));
        using var buffer = new MemoryStream();
        await context.Request.Body.CopyToAsync(buffer, context.RequestAborted).ConfigureAwait(false);
        if (buffer.Length == 0)
            return Error(Result.Fail(AiError.InvalidArgument, "the audio's bytes are required"));
        var text = await ai.TranscribeAsync(buffer.ToArray(), "audio/wav", context.RequestAborted).ConfigureAwait(false);
        return text.IsSuccess ? Json(new TranscriptionView(text.Value)) : Error(text.Failure);
    }

    private AiSettingsView SettingsView(AiSettings settings) =>
        new(settings.DefaultProvider, settings.DefaultModel, settings.Disabled, resolver.ProviderOrder(),
            settings.SongProviders is null, settings.SuggestionRounds, settings.Limits, settings.ReasoningLevels,
            settings.Lookup, settings.SpeechOptions);
}
