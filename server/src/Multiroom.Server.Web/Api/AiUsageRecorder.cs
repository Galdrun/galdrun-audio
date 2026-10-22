using System.Text.Json;
using Microsoft.Extensions.Hosting;
using Microsoft.Extensions.Logging;
using Multiroom.Server.Ai;
using Multiroom.Server.Library;

namespace Multiroom.Server.Web.Api;

// The AI's token usage per day: every call the registry sends to a vendor,
// added to its server-local day's row under what asked (AiOrigin), failures
// included. A row that cannot be written is logged; the call is not touched.
public sealed partial class AiUsageRecorder(AiProviderRegistry ai, AiUsageStore store, ILogger<AiUsageRecorder> log,
    TimeProvider? time = null) : IHostedService
{
    private readonly TimeProvider _time = time ?? TimeProvider.System;

    public Task StartAsync(CancellationToken cancellationToken)
    {
        ai.Called += OnCalled;
        return Task.CompletedTask;
    }

    public Task StopAsync(CancellationToken cancellationToken)
    {
        ai.Called -= OnCalled;
        return Task.CompletedTask;
    }

    private void OnCalled(object? sender, AiCall call)
    {
        var usage = call.Answer?.Usage ?? new AiUsage(0, 0);
        var recorded = store.Record(DateOnly.FromDateTime(_time.GetLocalNow().DateTime),
            JsonNamingPolicy.CamelCase.ConvertName(call.Origin.ToString()), call.Provider, call.Request.Model,
            call.Failure is not null, usage.InputTokens, usage.OutputTokens, usage.ReasoningTokens, usage.WebSearches);
        if (!recorded.IsSuccess)
            LogNotRecorded(log, call.Provider, recorded.Failure.Detail);
    }

    [LoggerMessage(Level = LogLevel.Warning, Message = "AI usage of a {Provider} call not recorded: {Detail}")]
    private static partial void LogNotRecorded(ILogger log, string provider, string detail);
}
