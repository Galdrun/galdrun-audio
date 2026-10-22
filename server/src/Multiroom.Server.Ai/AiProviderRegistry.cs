using System.Collections.Concurrent;
using System.Diagnostics;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;
using Multiroom.Server.Abstractions;
using Multiroom.Server.Ai.Providers;

namespace Multiroom.Server.Ai;

// One AI provider to be started: its id, how to make it, and how long one
// call of it may take.
public sealed record AiProviderRegistration(string Id, Func<AiProviderContext, IAiProvider> Create, TimeSpan Timeout);

public static class AiServiceCollectionExtensions
{
    public static readonly TimeSpan DefaultTimeout = TimeSpan.FromSeconds(60);

    // A web lookup (phase 27) runs the vendor's searches before it answers,
    // which takes minutes, not seconds: it is bounded by this when longer
    // than the registration's timeout.
    public static readonly TimeSpan WebLookupTimeout = TimeSpan.FromSeconds(180);

    // One line per AI provider, in the host or its AddXxx().
    public static IServiceCollection AddAiProvider(this IServiceCollection services, string id,
        Func<AiProviderContext, IAiProvider> create, TimeSpan? timeout = null) =>
        services.AddSingleton(new AiProviderRegistration(id, create, timeout ?? DefaultTimeout));

    // The stores and the registry. `directory` is <dataDirectory>/ai; `config`
    // is the config's ai.providers.
    public static IServiceCollection AddAi(this IServiceCollection services, IReadOnlyList<AiProviderConfig> config,
        string directory)
    {
        services.AddSingleton(new AiCredentialStore(Path.Combine(directory, "credentials.json"), config));
        services.AddSingleton(new AiSettingsStore(Path.Combine(directory, "settings.json")));
        // Read when first needed, which is when the outward API is made at
        // start: a file that cannot be read stops the server, named.
        services.AddSingleton(_ =>
        {
            var loaded = AiInstructions.Load(Path.Combine(AppContext.BaseDirectory, AiInstructions.FileName),
                Path.Combine(directory, "instructions.json"));
            return loaded.IsSuccess ? loaded.Value : throw new InvalidOperationException(loaded.Failure.Detail);
        });
        services.AddSingleton<Suggestions.SongSuggester>();
        services.AddSingleton<Suggestions.ProposalSuggester>();
        services.AddSingleton<Agent.MultiroomAgent>();
        services.AddSingleton(new Agent.AgentChatStore(Path.Combine(directory, "chats.json")));
        services.AddSingleton(s => new AiProviderRegistry(s.GetServices<AiProviderRegistration>(), config,
            s.GetRequiredService<AiCredentialStore>(), s.GetRequiredService<AiSettingsStore>(),
            s.GetRequiredService<ILogger<AiProviderRegistry>>(), s.GetServices<AiLoginRegistration>()));
        return services.AddHostedService(s => s.GetRequiredService<AiProviderRegistry>());
    }
}

// A provider as the API shows it: what it is, whether it is switched on and
// has what it needs, and what its last call said about the vendor
// (authorised, reachable; null until a call). Never the key: hasKey and
// where it came from; never a login's token: hasToken, and hasClient where
// its login needs the owner's OAuth client (null where it takes none).
public sealed record AiProviderView(
    string Id,
    string DisplayName,
    IReadOnlyList<AiAuthMethod> AuthMethods,
    bool Enabled,
    bool Configured,
    bool? Authorised,
    bool? Reachable,
    string Detail,
    bool HasKey,
    AiKeyOrigin Origin,
    bool HasToken,
    bool? HasClient,
    IReadOnlyList<AiCapability> Capabilities);

// What a provider has cost since the server started, on status.
public sealed record AiProviderUsage(
    string Id,
    long Calls,
    long Failures,
    long Timeouts,
    long Retries,
    long InputTokens,
    long OutputTokens,
    int CachedModels,
    long ReasoningTokens,
    long WebLookups,
    long WebSearches);

// One call sent to a vendor (AiProviderRegistry.Called): the request as sent,
// how long it took with its retry, the answer or the failure, and what asked
// (AiOriginScope).
public sealed record AiCall(string Provider, AiRequest Request, TimeSpan Elapsed, AiResponse? Answer,
    Failure<AiError>? Failure, AiOrigin Origin = AiOrigin.Other);

// Every registered AI provider, made once at start in registration order.
// An id registered twice, a provider that reports another id, or a config
// block for an id nobody registered refuses the server: each is a mistake
// that would otherwise be silently ignored.
//
// Every call goes through here: bounded by the registration's timeout,
// refused when the provider is switched off, counted, and remembered as the
// vendor's last word on the key. Model lists are cached per provider, at most
// MaxModels each, until asked to refresh or the key changes.
public sealed partial class AiProviderRegistry(
    IEnumerable<AiProviderRegistration> registrations,
    IReadOnlyList<AiProviderConfig> config,
    AiCredentialStore credentials,
    AiSettingsStore settings,
    ILogger<AiProviderRegistry> log,
    IEnumerable<AiLoginRegistration>? logins = null,
    TimeProvider? time = null,
    TimeSpan? retryDelay = null) : IHostedService, IDisposable
{
    public const int MaxModels = 1000;

    // How long a call waits before its one retry of a vendor that is
    // overloaded (503) or rate-limited (429).
    public static readonly TimeSpan DefaultRetryDelay = TimeSpan.FromSeconds(2);

    private readonly TimeSpan _retryDelay = retryDelay ?? DefaultRetryDelay;

    private readonly List<AiProviderRegistration> _registrations = [.. registrations];
    private readonly List<AiLoginRegistration> _loginRegistrations = [.. logins ?? []];
    private readonly TimeProvider _time = time ?? TimeProvider.System;
    private readonly HttpClient _http = CreateHttp();
    private readonly ConcurrentDictionary<string, Entry> _entries = new(StringComparer.Ordinal);
    private IReadOnlyList<Entry> _ordered = [];

    public AiSettings Settings => settings.Current;

    public Task StartAsync(CancellationToken cancellationToken)
    {
        var loginsById = new Dictionary<string, IAiLogin>(StringComparer.Ordinal);
        foreach (var login in _loginRegistrations)
        {
            if (!_registrations.Any(r => r.Id == login.Id))
                throw new InvalidOperationException($"AI login {login.Id} is for no AI provider");
            if (!loginsById.TryAdd(login.Id, login.Create(_http)))
                throw new InvalidOperationException($"AI login {login.Id} is registered twice");
        }

        var ordered = new List<Entry>();
        foreach (var registration in _registrations)
        {
            var id = registration.Id;
            var baseUrl = config.FirstOrDefault(c => c.Id == id)?.BaseUrl;
            var login = loginsById.GetValueOrDefault(id);
            var token = login is null ? null : new AiTokenSource(id, credentials, login, _time);
            var provider = registration.Create(new AiProviderContext(_http, () => credentials.Key(id), baseUrl, token));
            if (provider.Id != id)
                throw new InvalidOperationException($"AI provider registered as {id} reports the id {provider.Id}");
            // A provider lists oauth exactly when it has a login.
            if (provider.AuthMethods.Contains(AiAuthMethod.OAuth) != login is not null)
            {
                throw new InvalidOperationException(login is null
                    ? $"AI provider {id} lists oauth but has no login"
                    : $"AI provider {id} has a login but does not list oauth");
            }
            var entry = new Entry(registration, provider, login, token);
            if (!_entries.TryAdd(id, entry))
                throw new InvalidOperationException($"AI provider {id} is registered twice");
            ordered.Add(entry);
        }
        if (config.FirstOrDefault(c => !_entries.ContainsKey(c.Id)) is { } unknown)
            throw new InvalidOperationException($"ai.providers names {unknown.Id}, which is no AI provider");
        _ordered = ordered;

        var loaded = settings.Load();
        if (!loaded.IsSuccess)
            LogSettingsUnreadable(log, loaded.Failure.Detail);
        var ids = string.Join(", ", ordered.Select(e => e.Provider.Id));
        LogStarted(log, ids);
        return Task.CompletedTask;
    }

    public Task StopAsync(CancellationToken cancellationToken) => Task.CompletedTask;

    public void Dispose()
    {
        foreach (var entry in _ordered)
            entry.Token?.Dispose();
        _http.Dispose();
    }

    public async Task<IReadOnlyList<AiProviderView>> ViewsAsync(CancellationToken cancellationToken) =>
        await Task.WhenAll(_ordered.Select(e => ViewAsync(e, cancellationToken))).ConfigureAwait(false);

    public IReadOnlyList<AiProviderUsage> Usage =>
        [.. _ordered.Select(e => new AiProviderUsage(e.Provider.Id, Interlocked.Read(ref e.Calls),
            Interlocked.Read(ref e.Failures), Interlocked.Read(ref e.Timeouts), Interlocked.Read(ref e.Retries), Interlocked.Read(ref e.InputTokens),
            Interlocked.Read(ref e.OutputTokens), e.Models?.Length ?? 0, Interlocked.Read(ref e.ReasoningTokens),
            Interlocked.Read(ref e.WebLookups), Interlocked.Read(ref e.WebSearches)))];

    // The key is used from the next call on; what the vendor said of the old
    // one, and the models it listed, are forgotten.
    public async Task<Result<AiProviderView, AiError>> SetKeyAsync(string id, string key,
        CancellationToken cancellationToken)
    {
        var entry = Find(id);
        if (!entry.IsSuccess)
            return entry.Failure;
        if (!entry.Value.Provider.AuthMethods.Contains(AiAuthMethod.ApiKey))
            return Result.Fail(AiError.InvalidArgument, $"{id} takes no API key");
        var set = credentials.Set(id, key);
        if (!set.IsSuccess)
            return set.Failure;
        entry.Value.Forget();
        return await ViewAsync(entry.Value, cancellationToken).ConfigureAwait(false);
    }

    // Logout, too: a login's token is forgotten and, where the vendor offers
    // it, revoked. A revocation that fails is logged; the token is gone here
    // either way.
    public async Task<Result<AiProviderView, AiError>> RemoveKeyAsync(string id, CancellationToken cancellationToken)
    {
        var entry = Find(id);
        if (!entry.IsSuccess)
            return entry.Failure;
        var token = credentials.Token(id);
        var removed = credentials.Remove(id);
        if (!removed.IsSuccess)
            return removed.Failure;
        entry.Value.Forget();
        if (token is not null && entry.Value.Login is { } login)
        {
            var revoked = await BoundedAsync(entry.Value,
                ct => login.RevokeAsync(token, credentials.Client(id), ct), cancellationToken).ConfigureAwait(false);
            if (!revoked.IsSuccess)
                LogRevokeFailed(log, id, revoked.Failure.ToString());
        }
        return await ViewAsync(entry.Value, cancellationToken).ConfigureAwait(false);
    }

    public async Task<Result<IReadOnlyList<AiModel>, AiError>> ListModelsAsync(string id, bool refresh,
        CancellationToken cancellationToken)
    {
        var entry = Usable(id);
        if (!entry.IsSuccess)
            return entry.Failure;
        if (!refresh && entry.Value.Models is { } cached)
            return cached;
        var listed = await CallAsync(entry.Value, ct => entry.Value.Provider.ListModelsAsync(ct), cancellationToken)
            .ConfigureAwait(false);
        if (!listed.IsSuccess)
            return listed.Failure;
        AiModel[] models = [.. listed.Value.Take(MaxModels)];
        entry.Value.Models = models;
        return models;
    }

    public async Task<Result<AiResponse, AiError>> CompleteAsync(string id, AiRequest request,
        CancellationToken cancellationToken)
    {
        var entry = Usable(id);
        if (!entry.IsSuccess)
            return entry.Failure;
        if (string.IsNullOrWhiteSpace(request.Model))
            return Result.Fail(AiError.InvalidArgument, "model: the model to ask");
        if (request.MaxTokens < 1)
            return Result.Fail(AiError.InvalidArgument, $"maxTokens: at least 1, was {request.MaxTokens}");
        if (request.RequiredTool is { } required && request.Tools?.Any(t => t.Name == required) != true)
            return Result.Fail(AiError.InvalidArgument, $"requiredTool: {required} is none of the request's tools");
        // A reasoning level is refused by name where there is no mechanism
        // for it (rule 5), and gets room for its thinking where there is.
        if (request.Reasoning != AiReasoning.Default)
        {
            if (!entry.Value.Provider.Capabilities.Contains(AiCapability.Reasoning))
                return Result.Fail(AiError.InvalidArgument, $"{id} takes no reasoning setting");
            request = request with { MaxTokens = request.MaxTokens + AiReasoningBudget.Of(request.Reasoning) };
        }
        // A web lookup likewise (phase 27); it is counted when asked, as most
        // vendors bill it so, beside the searches the vendor says it ran.
        if (request.WebSearch)
        {
            if (!entry.Value.Provider.Capabilities.Contains(AiCapability.WebSearch))
                return Result.Fail(AiError.InvalidArgument, $"{id} has no web lookup");
            Interlocked.Increment(ref entry.Value.WebLookups);
        }
        var asked = request;
        var bound = request.WebSearch && AiServiceCollectionExtensions.WebLookupTimeout > entry.Value.Registration.Timeout
            ? AiServiceCollectionExtensions.WebLookupTimeout
            : (TimeSpan?)null;
        var clock = Stopwatch.StartNew();
        var answer = await CallAsync(entry.Value, ct => entry.Value.Provider.CompleteAsync(asked, ct),
            cancellationToken, bound).ConfigureAwait(false);
        Called?.Invoke(this, new AiCall(id, asked, clock.Elapsed, answer.IsSuccess ? answer.Value : null,
            answer.IsSuccess ? null : answer.Failure, AiOriginScope.Current));
        if (answer.IsSuccess)
        {
            Interlocked.Add(ref entry.Value.InputTokens, answer.Value.Usage.InputTokens);
            Interlocked.Add(ref entry.Value.OutputTokens, answer.Value.Usage.OutputTokens);
            Interlocked.Add(ref entry.Value.ReasoningTokens, answer.Value.Usage.ReasoningTokens);
            Interlocked.Add(ref entry.Value.WebSearches, answer.Value.Usage.WebSearches);
        }
        return answer;
    }

    // The words spoken, by the speech settings' provider and model.
    public async Task<Result<string, AiError>> TranscribeAsync(byte[] audio, string mediaType,
        CancellationToken cancellationToken)
    {
        var speech = settings.Current.SpeechOptions;
        // No model is the vendor's choice where it makes one (ChatGPT's dictation); the others refuse it.
        if (speech.Provider is not { } id)
            return Result.Fail(AiError.NotConfigured, "no speech provider in the AI settings (speech.provider)");
        var model = speech.Model ?? "";
        var entry = Usable(id);
        if (!entry.IsSuccess)
            return entry.Failure;
        if (!entry.Value.Provider.Capabilities.Contains(AiCapability.Transcription))
            return Result.Fail(AiError.InvalidArgument, $"{id} cannot transcribe speech");
        var request = new AiTranscriptionRequest(model, audio, mediaType, speech.Language);
        return await CallAsync(entry.Value, ct => entry.Value.Provider.TranscribeAsync(request, ct), cancellationToken)
            .ConfigureAwait(false);
    }

    // Null keeps a setting (AiSettingsPatch). Song providers are music
    // provider ids, which this registry does not know: the caller checks them.
    public Result<AiSettings, AiError> PatchSettings(AiSettingsPatch patch)
    {
        var current = settings.Current;
        if (patch.DefaultProvider is { Length: > 0 } provider && !_entries.ContainsKey(provider))
            return Result.Fail(AiError.NotFound, $"defaultProvider: no AI provider {provider}");
        if (patch.DisabledProviders?.FirstOrDefault(p => !_entries.ContainsKey(p)) is { } unknown)
            return Result.Fail(AiError.NotFound, $"disabledProviders: no AI provider {unknown}");
        if (patch.MaxSuggestionRounds is < 1 or > 5)
            return Result.Fail(AiError.InvalidArgument, $"maxSuggestionRounds: 1 to 5, was {patch.MaxSuggestionRounds}");
        var limits = current.Limits;
        if (patch.Agent is { } agent)
        {
            if (agent.MaxToolCalls is < 1 or > 50)
                return Result.Fail(AiError.InvalidArgument, $"agent.maxToolCalls: 1 to 50, was {agent.MaxToolCalls}");
            if (agent.TimeoutSeconds is < 1 or > 600)
                return Result.Fail(AiError.InvalidArgument, $"agent.timeoutSeconds: 1 to 600, was {agent.TimeoutSeconds}");
            if (agent.TokenBudget is < 1000 or > 10_000_000)
                return Result.Fail(AiError.InvalidArgument, $"agent.tokenBudget: 1000 to 10000000, was {agent.TokenBudget}");
            if (agent.MaxConcurrent is < 1 or > 8)
                return Result.Fail(AiError.InvalidArgument, $"agent.maxConcurrent: 1 to 8, was {agent.MaxConcurrent}");
            limits = new AgentLimits(agent.MaxToolCalls ?? limits.MaxToolCalls, agent.TimeoutSeconds ?? limits.TimeoutSeconds,
                agent.TokenBudget ?? limits.TokenBudget, agent.MaxConcurrent ?? limits.MaxConcurrent);
        }
        var proposals = current.Proposals;
        if (patch.Proposals is { } p)
        {
            static string? Outside(int? value, int low, int high, string name) =>
                value is { } v && (v < low || v > high) ? $"proposals.{name}: {low} to {high}, was {v}" : null;
            if ((Outside(p.IntervalHours, 1, 168, "intervalHours") ?? Outside(p.DebounceMinutes, 0, 120, "debounceMinutes") ??
                Outside(p.MaxRunsPerDay, 1, 24, "maxRunsPerDay") ?? Outside(p.Tracks, 0, 100, "tracks") ??
                Outside(p.Albums, 0, 50, "albums") ?? Outside(p.Artists, 0, 50, "artists") ??
                Outside(p.Audiobooks, 0, 50, "audiobooks") ?? Outside(p.Keep, 1, 1000, "keep")) is { } outside)
            {
                return Result.Fail(AiError.InvalidArgument, outside);
            }
            var was = current.ProposalOptions;
            proposals = new ProposalSettings(p.Enabled ?? was.Enabled, p.IntervalHours ?? was.IntervalHours,
                p.DebounceMinutes ?? was.DebounceMinutes, p.MaxRunsPerDay ?? was.MaxRunsPerDay, p.Tracks ?? was.Tracks,
                p.Albums ?? was.Albums, p.Artists ?? was.Artists, p.Audiobooks ?? was.Audiobooks,
                p.Request is null ? was.Request : NullIfEmpty(p.Request.Trim()), p.Keep ?? was.Keep);
        }
        var speech = current.Speech;
        if (patch.Speech is { } voice)
        {
            if (voice.Provider is { Length: > 0 } speaker && !Supports(speaker, AiCapability.Transcription))
            {
                return _entries.ContainsKey(speaker)
                    ? Result.Fail(AiError.InvalidArgument, $"speech.provider: {speaker} cannot transcribe speech")
                    : Result.Fail(AiError.NotFound, $"speech.provider: no AI provider {speaker}");
            }
            if (voice.Language is { Length: > 0 } language &&
                (language.Length > 35 || !language.All(c => char.IsAsciiLetterOrDigit(c) || c == '-')))
            {
                return Result.Fail(AiError.InvalidArgument, $"speech.language: a language tag such as de-DE, was {language}");
            }
            var was = current.SpeechOptions;
            speech = new AiSpeechSettings(voice.Recognition ?? was.Recognition,
                voice.Provider is null ? was.Provider : NullIfEmpty(voice.Provider),
                voice.Model is null ? was.Model : NullIfEmpty(voice.Model.Trim()),
                voice.Language is null ? was.Language : NullIfEmpty(voice.Language.Trim()),
                voice.ReadReplies ?? was.ReadReplies);
        }
        var saved = settings.Save(new AiSettings(
            patch.DefaultProvider is null ? current.DefaultProvider : NullIfEmpty(patch.DefaultProvider),
            patch.DefaultModel is null ? current.DefaultModel : NullIfEmpty(patch.DefaultModel.Trim()),
            patch.DisabledProviders is null ? current.DisabledProviders : [.. patch.DisabledProviders.Distinct(StringComparer.Ordinal)],
            patch.SongProviders switch
            {
                null => current.SongProviders,
                [] => null,
                var ids => [.. ids.Distinct(StringComparer.Ordinal)],
            },
            patch.MaxSuggestionRounds ?? current.MaxSuggestionRounds,
            patch.Agent is null ? current.Agent : limits,
            patch.Reasoning is { } reasoning
                ? new AiReasoningSettings(reasoning.Agent ?? current.ReasoningLevels.Agent,
                    reasoning.Suggestions ?? current.ReasoningLevels.Suggestions)
                : current.Reasoning,
            patch.WebLookup ?? current.WebLookup,
            proposals,
            speech));
        if (saved.IsSuccess)
            SettingsChanged?.Invoke(this, saved.Value);
        return saved;
    }

    // Every call sent to a vendor, as asked and as answered: the event stream
    // shows it (ai.request), the prompt included. Never the log file.
    public event EventHandler<AiCall>? Called;

    // A settings change, by whoever (phase 38: the proposals' runner reads them).
    public event EventHandler<AiSettings>? SettingsChanged;

    // Whether a provider has a capability; false for an unknown one.
    public bool Supports(string id, AiCapability capability) =>
        _entries.TryGetValue(id, out var entry) && entry.Provider.Capabilities.Contains(capability);

    // The provider and model a call names, else the settings' defaults.
    public Result<(string Provider, string Model), AiError> Choose(string? provider, string? model)
    {
        var id = string.IsNullOrWhiteSpace(provider) ? settings.Current.DefaultProvider : provider;
        if (id is null)
            return Result.Fail(AiError.NotConfigured, "no provider named and no default provider in the AI settings");
        var chosen = string.IsNullOrWhiteSpace(model)
            ? id == settings.Current.DefaultProvider ? settings.Current.DefaultModel : null
            : model.Trim();
        if (chosen is null)
            return Result.Fail(AiError.NotConfigured, $"no model named and no default model for {id} in the AI settings");
        return (id, chosen);
    }

    private static string? NullIfEmpty(string text) => text.Length == 0 ? null : text;

    private Result<Entry, AiError> Find(string id) =>
        _entries.TryGetValue(id, out var entry) ? entry : Result.Fail(AiError.NotFound, $"no AI provider {id}");

    private Result<Entry, AiError> Usable(string id)
    {
        var entry = Find(id);
        if (entry.IsSuccess && settings.Current.Disabled.Contains(id, StringComparer.Ordinal))
            return Result.Fail(AiError.NotConfigured, $"{id} is switched off in the AI settings");
        return entry;
    }

    private async Task<AiProviderView> ViewAsync(Entry entry, CancellationToken cancellationToken)
    {
        var provider = entry.Provider;
        var status = await provider.GetStatusAsync(cancellationToken).ConfigureAwait(false);
        var origin = credentials.Origin(provider.Id);
        var outcome = entry.Outcome;
        var detail = status.Detail.Length > 0 ? status.Detail : outcome.Failure ?? "";
        return new AiProviderView(provider.Id, provider.DisplayName, provider.AuthMethods,
            !settings.Current.Disabled.Contains(provider.Id, StringComparer.Ordinal), status.Configured,
            outcome.Authorised, outcome.Reachable, detail, credentials.Key(provider.Id) is not null, origin,
            credentials.Token(provider.Id) is not null,
            entry.Login?.NeedsClient == true ? credentials.Client(provider.Id) is not null : null,
            provider.Capabilities);
    }

    // The call, bounded by the registration's timeout and counted once. A
    // vendor that answers it is overloaded or rate-limited is asked once more
    // after RetryDelay, within the same timeout: such a spike is usually
    // gone in seconds. What the vendor answered says whether it took the key
    // and could be reached.
    private async Task<Result<T, AiError>> CallAsync<T>(Entry entry,
        Func<CancellationToken, Task<Result<T, AiError>>> call, CancellationToken cancellationToken, TimeSpan? bound = null)
    {
        Interlocked.Increment(ref entry.Calls);
        var clock = Stopwatch.StartNew();
        var result = await BoundedAsync(entry, async ct =>
        {
            var first = await call(ct).ConfigureAwait(false);
            if (first.IsSuccess || first.Failure.Code is not (AiError.Unavailable or AiError.RateLimited))
                return first;
            LogRetrying(log, entry.Provider.Id, first.Failure, (long)_retryDelay.TotalMilliseconds);
            Interlocked.Increment(ref entry.Retries);
            await Task.Delay(_retryDelay, _time, ct).ConfigureAwait(false);
            return await call(ct).ConfigureAwait(false);
        }, cancellationToken, bound).ConfigureAwait(false);
        if (result.IsSuccess)
        {
            entry.Outcome = new Outcome(true, true, null);
            return result;
        }
        Interlocked.Increment(ref entry.Failures);
        var failure = result.Failure;
        if (failure.Code == AiError.Timeout)
            Interlocked.Increment(ref entry.Timeouts);
        var last = entry.Outcome;
        entry.Outcome = failure.Code switch
        {
            AiError.AuthFailed => new Outcome(false, true, failure.Detail),
            AiError.Unavailable or AiError.Timeout => new Outcome(last.Authorised, false, failure.Detail),
            AiError.NotConfigured => last,
            _ => new Outcome(true, true, failure.Detail),
        };
        LogCallFailed(log, entry.Provider.Id, failure.ToString(), clock.ElapsedMilliseconds);
        return result;
    }

    // The call bounded by the registration's timeout, or `bound` in its
    // place: a provider or a login that throws fails its call, not the caller.
    private static async Task<Result<T, AiError>> BoundedAsync<T>(Entry entry,
        Func<CancellationToken, Task<Result<T, AiError>>> call, CancellationToken cancellationToken, TimeSpan? bound = null)
    {
        var limit = bound ?? entry.Registration.Timeout;
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        timeout.CancelAfter(limit);
        try
        {
            return await call(timeout.Token).ConfigureAwait(false);
        }
        catch (OperationCanceledException) when (!cancellationToken.IsCancellationRequested)
        {
            return Result.Fail(AiError.Timeout,
                $"{entry.Provider.Id} did not answer within {limit.TotalSeconds:0.#} s");
        }
#pragma warning disable CA1031 // The provider boundary: a provider that throws fails its call, not the caller.
        catch (Exception e) when (e is not OperationCanceledException)
#pragma warning restore CA1031
        {
            return Result.Fail(AiError.Unavailable, $"{entry.Provider.Id} failed: {e.GetType().Name}");
        }
    }

    private static HttpClient CreateHttp()
    {
        // No timeout of its own: each call is bounded by its registration's.
        var http = new HttpClient(new SocketsHttpHandler { ConnectTimeout = TimeSpan.FromSeconds(10) })
        {
            Timeout = Timeout.InfiniteTimeSpan,
        };
        http.DefaultRequestHeaders.UserAgent.ParseAdd(Branding.ServerName);
        return http;
    }

    [LoggerMessage(Level = LogLevel.Information, Message = "AI providers: {Providers}")]
    private static partial void LogStarted(ILogger log, string providers);

    [LoggerMessage(Level = LogLevel.Warning, Message = "AI settings unreadable, the defaults are used: {Detail}")]
    private static partial void LogSettingsUnreadable(ILogger log, string detail);

    [LoggerMessage(Level = LogLevel.Information, Message = "AI provider {Id}: {Failure}; asking once more in {Ms} ms")]
    private static partial void LogRetrying(ILogger log, string id, Failure<AiError> failure, long ms);

    [LoggerMessage(Level = LogLevel.Warning, Message = "AI provider {Id}: {Failure} after {Ms} ms")]
    private static partial void LogCallFailed(ILogger log, string id, string failure, long ms);

    [LoggerMessage(Level = LogLevel.Warning, Message = "AI provider {Id}: the login is forgotten here but was not revoked: {Failure}")]
    private static partial void LogRevokeFailed(ILogger log, string id, string failure);

    private sealed record Outcome(bool? Authorised, bool? Reachable, string? Failure);

    private sealed class Entry(AiProviderRegistration registration, IAiProvider provider, IAiLogin? login,
        AiTokenSource? token)
    {
        public IAiLogin? Login { get; } = login;

        public AiTokenSource? Token { get; } = token;

        public long Calls;
        public long Failures;
        public long Timeouts;
        public long Retries;
        public long InputTokens;
        public long OutputTokens;
        public long ReasoningTokens;
        public long WebLookups;
        public long WebSearches;

        public AiProviderRegistration Registration { get; } = registration;

        public IAiProvider Provider { get; } = provider;

        public volatile AiModel[]? Models;

        public volatile Outcome Outcome = new(null, null, null);

        public void Forget()
        {
            Models = null;
            Outcome = new Outcome(null, null, null);
        }
    }
}
