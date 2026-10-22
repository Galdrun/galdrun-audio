using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;
using Multiroom.Server.Abstractions;

namespace Multiroom.Server.Providers;

// One provider to be started: its id, and how to make it. Each provider ships
// an AddXxx() extension that calls AddProvider; the host calls that and
// nothing else changes.
public sealed record ProviderRegistration(string Id, Func<IServiceProvider, IProvider> Create);

public static class ProviderServiceCollectionExtensions
{
    public static IServiceCollection AddProvider(this IServiceCollection services, string id,
        Func<IServiceProvider, IProvider> create) => services.AddSingleton(new ProviderRegistration(id, create));
}

// Every registered provider, started once when the host starts, in
// registration order.
//
// A provider that fails to start disables itself, not the server: its
// constructor threw, it reports another id than it was registered under, or
// its capabilities and interfaces disagree. It is logged, reported Disabled
// with the reason, and asked for nothing. An id registered twice is a
// composition bug, so the server refuses to start and names it.
//
// The list is fixed once started; readers never lock.
public sealed partial class ProviderRegistry(
    IEnumerable<ProviderRegistration> registrations,
    IServiceProvider services,
    ILogger<ProviderRegistry> log) : IHostedService, IAsyncDisposable
{
    private IReadOnlyList<Entry> _entries = [];

    public async Task StartAsync(CancellationToken cancellationToken)
    {
        var all = registrations.ToList();
        var twice = all.GroupBy(r => r.Id, StringComparer.Ordinal).FirstOrDefault(g => g.Count() > 1);
        if (twice is not null)
            throw new InvalidOperationException($"provider id {twice.Key} is registered twice");

        var entries = new List<Entry>(all.Count);
        foreach (var registration in all)
            entries.Add(await Start(registration).ConfigureAwait(false));
        _entries = entries;
    }

    public Task StopAsync(CancellationToken cancellationToken) => Task.CompletedTask;

    // Every registered id, disabled ones included, in registration order.
    public IReadOnlyList<string> Ids => [.. _entries.Select(e => e.Id)];

    // The provider as the capability asked for: IProvider itself, or one of the
    // capability interfaces.
    public Result<T, ProviderError> Get<T>(string id)
        where T : class
    {
        var entry = _entries.FirstOrDefault(e => e.Id == id);
        if (entry is null)
            return Result.Fail(ProviderError.NotFound, $"no provider {id}");
        if (entry.Provider is null)
            return Result.Fail(ProviderError.Unavailable, $"provider {id} is disabled: {entry.Reason}");
        if (entry.Provider is not T capability)
            return Result.Fail(ProviderError.Unavailable, $"provider {id} is no {typeof(T).Name}");
        return capability;
    }

    // Every registered provider's status, in registration order, disabled ones
    // included. A provider whose status call throws is reported Unavailable.
    public async Task<IReadOnlyList<(string Id, ProviderStatus Status)>> GetStatusesAsync(
        CancellationToken cancellationToken) =>
        await Task.WhenAll(_entries.Select(async e =>
            (e.Id, e.Provider is null
                ? new ProviderStatus(ProviderState.Disabled, e.Reason)
                : await StatusOf(e.Provider, cancellationToken).ConfigureAwait(false)))).ConfigureAwait(false);

    public async ValueTask DisposeAsync()
    {
        foreach (var entry in _entries)
            await DisposeProvider(entry.Provider).ConfigureAwait(false);
        _entries = [];
    }

    private async Task<Entry> Start(ProviderRegistration registration)
    {
        IProvider provider;
        try
        {
            provider = registration.Create(services);
        }
#pragma warning disable CA1031 // A provider's fault is that provider's outage, never the server's.
        catch (Exception e)
#pragma warning restore CA1031
        {
            return Disable(registration.Id, e.Message);
        }

        var refusal = provider.Id != registration.Id
            ? $"registered as {registration.Id} but reports {provider.Id}"
            : Disagreement(provider);
        if (refusal is not null)
        {
            await DisposeProvider(provider).ConfigureAwait(false);
            return Disable(registration.Id, refusal);
        }

        LogStarted(log, provider.Id, provider.DisplayName, provider.Capabilities);
        return new Entry(provider.Id, provider, "");
    }

    private Entry Disable(string id, string reason)
    {
        LogDisabled(log, id, reason);
        return new Entry(id, null, reason);
    }

    private static string? Disagreement(IProvider provider)
    {
        (ProviderCapabilities Flag, bool Implemented)[] checks =
        [
            (ProviderCapabilities.Search, provider is ISearchProvider),
            (ProviderCapabilities.Browse, provider is IBrowseProvider),
            (ProviderCapabilities.Stream, provider is IStreamResolver),
            (ProviderCapabilities.LiveStream, provider is ILiveStreamResolver),
            (ProviderCapabilities.Artwork, provider is IArtworkProvider),
            (ProviderCapabilities.Expand, provider is IContainerExpander),
        ];
        var wrong = checks.Where(c => provider.Capabilities.HasFlag(c.Flag) != c.Implemented).Select(c => c.Flag)
            .ToList();
        return wrong.Count == 0 ? null : $"capabilities and interfaces disagree on {string.Join(", ", wrong)}";
    }

    private static async Task<ProviderStatus> StatusOf(IProvider provider, CancellationToken cancellationToken)
    {
        try
        {
            return await provider.GetStatusAsync(cancellationToken).ConfigureAwait(false);
        }
#pragma warning disable CA1031 // A provider's fault is that provider's outage, never the server's.
        catch (Exception e) when (!cancellationToken.IsCancellationRequested)
#pragma warning restore CA1031
        {
            return new ProviderStatus(ProviderState.Unavailable, e.Message);
        }
    }

    private static async ValueTask DisposeProvider(IProvider? provider)
    {
        if (provider is IAsyncDisposable asyncDisposable)
            await asyncDisposable.DisposeAsync().ConfigureAwait(false);
        else if (provider is IDisposable disposable)
            disposable.Dispose();
    }

    [LoggerMessage(Level = LogLevel.Information, Message = "provider {Id} ({DisplayName}) started: {Capabilities}")]
    private static partial void LogStarted(ILogger log, string id, string displayName,
        ProviderCapabilities capabilities);

    [LoggerMessage(Level = LogLevel.Warning, Message = "provider {Id} failed to start and is disabled: {Reason}")]
    private static partial void LogDisabled(ILogger log, string id, string reason);

    // Provider is null when disabled; Reason says why.
    private sealed record Entry(string Id, IProvider? Provider, string Reason);
}
