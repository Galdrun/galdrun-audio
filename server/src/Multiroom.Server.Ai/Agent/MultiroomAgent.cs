using System.Text;
using System.Text.Json;
using Microsoft.Extensions.Logging;
using ModelContextProtocol;
using ModelContextProtocol.Client;
using TextContentBlock = ModelContextProtocol.Protocol.TextContentBlock;
using Multiroom.Server.Abstractions;

namespace Multiroom.Server.Ai.Agent;

// A command in words, for a zone where it names one. The provider and model
// default to the settings'. A conversation id keeps the last turns, so
// "another one like that" works. Reasoning wins over the settings' agent
// level (phase 29). History is the conversation's earlier turns as stored
// elsewhere (AgentChatStore), used when the agent no longer remembers them.
// User is who speaks, a name UserStore knows; empty or null is the household.
public sealed record AgentCommand(string Text, string? Zone = null, string? Provider = null, string? Model = null,
    string? ConversationId = null, AiReasoning? Reasoning = null, IReadOnlyList<AiMessage>? History = null,
    string? User = null);

// One tool call: which tool, whether it answered, and in one line what was
// asked (or, when it failed, why).
public sealed record AgentAction(string Tool, bool Ok, string Summary);

// What a command did: the model's reply, every tool call in order, the
// tokens it cost, and the limit or failure that ended it early, if one did,
// with the failure's message (Detail) when it was a failure, and the
// provider and model it ran on.
public sealed record AgentReply(string Reply, IReadOnlyList<AgentAction> Actions, AiUsage Usage, string? StoppedBy = null,
    string? ConversationId = null, string? Detail = null, string? Provider = null, string? Model = null);

// The Galdrun AI agent: a bounded tool-calling
// loop. It asks the chosen provider with the MCP server's tools/list as tool
// definitions, runs every tool call through its MCP client, feeds the results
// back, and stops at a final answer or a limit. It knows no service and no
// music: what it can do is exactly what its tools allow, and the tools are
// whatever the in-memory MCP connection it is handed lists for its caller.
//
// Limits are the user's (AiSettings.Limits): tool calls and tokens per
// command, wall time per command, commands at once. Hitting one ends the
// command with what was done so far, named. A provider that fails ends it
// too; tools already called stay done (no rollback) and are listed.
public sealed partial class MultiroomAgent(AiProviderRegistry ai, AiInstructions instructions, ILogger<MultiroomAgent> log,
    TimeProvider? time = null)
{
    public const int MaxTextLength = 4000;
    public const int MaxConversations = 64;
    public const int MaxConversationIdLength = 64;
    public const string CannotPrefix = "CANNOT:";

    // User and assistant messages kept per conversation (tool traffic is not).
    public const int MaxTurns = 20;

    public static readonly TimeSpan ConversationIdle = TimeSpan.FromMinutes(15);

    // A tool's answer as the model sees it, at most this long.
    private const int MaxToolResult = 32_000;
    private const int MaxSummary = 200;
    // Room for an answer to a question looked up on the web; a reasoning
    // budget is added on top (AiReasoningBudget).
    public const int MaxReplyTokens = 4096;

    private readonly TimeProvider _time = time ?? TimeProvider.System;
    private readonly Lock _lock = new();
    private readonly Dictionary<string, Conversation> _conversations = new(StringComparer.Ordinal);
    private int _running;

    public int Running => Volatile.Read(ref _running);

    public int Conversations
    {
        get
        {
            lock (_lock)
                return _conversations.Count;
        }
    }

    // What the agent is told (AiInstructions.Agent), with the zone filled in,
    // and the parts that apply: the web search, who speaks, and that it may
    // ask back in a conversation.
    public string SystemPrompt(string? zone, bool webSearch = false, string? user = null, bool conversation = false)
    {
        var text = instructions.Agent;
        var system = new StringBuilder(AiInstructions.Fill(text.System, "zone",
            zone is null ? text.NoZone : AiInstructions.Fill(text.Zone, "zone", zone)));
        if (webSearch && text.WebSearch is { Length: > 0 } search)
            system.Append('\n').Append(search);
        if (!string.IsNullOrWhiteSpace(user) && text.User is { Length: > 0 } speaker)
            system.Append('\n').Append(AiInstructions.Fill(speaker, "user", user.Trim()));
        if (conversation && text.AskBack is { Length: > 0 } ask)
            system.Append('\n').Append(ask);
        return system.ToString();
    }
    // `connect` opens the MCP connection for the agent's caller; it is closed
    // when the command ends.
    public async Task<Result<AgentReply, AiError>> RunAsync(AgentCommand command,
        Func<CancellationToken, Task<McpClient>> connect, CancellationToken cancellationToken)
    {
        using var origin = AiOriginScope.Begin(AiOrigin.Agent);
        if (string.IsNullOrWhiteSpace(command.Text) || command.Text.Length > MaxTextLength)
            return Result.Fail(AiError.InvalidArgument, $"text: 1 to {MaxTextLength} characters");
        if (command.ConversationId is { } id && (id.Length is 0 or > MaxConversationIdLength || string.IsNullOrWhiteSpace(id)))
            return Result.Fail(AiError.InvalidArgument, $"conversationId: 1 to {MaxConversationIdLength} characters");
        var chosen = ai.Choose(command.Provider, command.Model);
        if (!chosen.IsSuccess)
            return chosen.Failure;
        var limits = ai.Settings.Limits;
        if (Interlocked.Increment(ref _running) > limits.MaxConcurrent)
        {
            Interlocked.Decrement(ref _running);
            return Result.Fail(AiError.Busy, $"the agent already runs {limits.MaxConcurrent} command(s); try again when one has ended");
        }
        try
        {
            return await RunBoundedAsync(command, chosen.Value.Provider, chosen.Value.Model, limits, connect,
                cancellationToken).ConfigureAwait(false);
        }
        finally
        {
            Interlocked.Decrement(ref _running);
        }
    }

    private async Task<Result<AgentReply, AiError>> RunBoundedAsync(AgentCommand command, string provider, string model,
        AgentLimits limits, Func<CancellationToken, Task<McpClient>> connect, CancellationToken cancellationToken)
    {
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        timeout.CancelAfter(TimeSpan.FromSeconds(limits.TimeoutSeconds));
        var token = timeout.Token;
        var actions = new List<AgentAction>();
        long input = 0, output = 0, reasoning = 0;
        string? reply = null;
        string? stoppedBy = null;
        string? detail = null;
        var history = Turns(command.ConversationId);
        if (history.Count == 0 && command.History is { Count: > 0 } stored)
            history = [.. stored.TakeLast(MaxTurns)];
        var messages = new List<AiMessage>(history) { new(AiRole.User, command.Text.Trim()) };

        try
        {
            await using var client = await connect(token).ConfigureAwait(false);
            var listed = await client.ListToolsAsync(cancellationToken: token).ConfigureAwait(false);
            List<AiTool> tools = [.. listed.Select(t => new AiTool(t.Name, t.Description ?? "", t.JsonSchema))];
            // Phase 27: the vendor's own search, where the owner allows it and it has one.
            var webSearch = ai.Settings.Lookup == AiWebLookup.WhenAvailable && ai.Supports(provider, AiCapability.WebSearch);
            var system = SystemPrompt(command.Zone, webSearch, command.User, command.ConversationId is not null);

            while (stoppedBy is null)
            {
                var answer = await ai.CompleteAsync(provider, new AiRequest(model, messages, system, tools, MaxReplyTokens,
                    Reasoning: command.Reasoning ?? ai.Settings.ReasoningLevels.Agent, WebSearch: webSearch),
                    token).ConfigureAwait(false);
                if (!answer.IsSuccess)
                {
                    if (actions.Count == 0)
                        return answer.Failure;
                    stoppedBy = ApiError.From(answer.Failure).Error;
                    detail = answer.Failure.Detail;
                    break;
                }
                input += answer.Value.Usage.InputTokens;
                output += answer.Value.Usage.OutputTokens;
                reasoning += answer.Value.Usage.ReasoningTokens;
                reply = answer.Value.Text ?? reply;
                if (answer.Value.ToolCalls.Count == 0)
                    break;

                messages.Add(new AiMessage(AiRole.Assistant, answer.Value.Text, answer.Value.ToolCalls));
                foreach (var call in answer.Value.ToolCalls)
                {
                    if (actions.Count >= limits.MaxToolCalls)
                    {
                        stoppedBy = "maxToolCalls";
                        break;
                    }
                    var (ok, text) = await CallAsync(client, call, token).ConfigureAwait(false);
                    actions.Add(new AgentAction(call.Name, ok, Cut(ok ? Compact(call.Arguments) : text, MaxSummary)));
                    messages.Add(new AiMessage(AiRole.Tool, Cut(text, MaxToolResult), ToolCallId: call.Id, IsError: !ok));
                }
                if (stoppedBy is null && input + output > limits.TokenBudget)
                    stoppedBy = "tokenBudget";
            }
        }
        catch (OperationCanceledException) when (timeout.IsCancellationRequested && !cancellationToken.IsCancellationRequested)
        {
            if (actions.Count == 0)
                return Result.Fail(AiError.Timeout, $"the command did not end within {limits.TimeoutSeconds} s");
            stoppedBy = "timeout";
        }

        reply ??= "";
        // A model that answered nothing at all is named, never a silent success.
        if (stoppedBy is null && actions.Count == 0 && string.IsNullOrWhiteSpace(reply))
        {
            stoppedBy = "emptyAnswer";
            detail = $"{provider} answered neither text nor a tool call";
        }
        // The instructions have the model refuse with "CANNOT: <reason>";
        // the reason becomes the reply and stoppedBy names it for clients.
        if (stoppedBy is null && reply.TrimStart().StartsWith(CannotPrefix, StringComparison.OrdinalIgnoreCase))
        {
            reply = reply.TrimStart()[CannotPrefix.Length..].Trim();
            stoppedBy = "cannot";
        }
        Remember(command.ConversationId, command.Text.Trim(), reply);
        LogEnded(log, provider, actions.Count, stoppedBy ?? "answer", input + output);
        return new AgentReply(reply, actions, new AiUsage(input, output, reasoning), stoppedBy, command.ConversationId,
            detail, provider, model);
    }

    // A tool's answer as text, and whether it answered. A refusal of the
    // protocol (an unknown tool) is the tool's error, as a refusal by tier is.
    private static async Task<(bool Ok, string Text)> CallAsync(McpClient client, AiToolCall call,
        CancellationToken cancellationToken)
    {
        var arguments = call.Arguments.ValueKind == JsonValueKind.Object
            ? call.Arguments.EnumerateObject().ToDictionary(p => p.Name, p => (object?)p.Value.Clone())
            : [];
        try
        {
            var result = await client.CallToolAsync(call.Name, arguments, cancellationToken: cancellationToken)
                .ConfigureAwait(false);
            var text = string.Concat(result.Content.OfType<TextContentBlock>().Select(b => b.Text));
            return (result.IsError != true, text);
        }
        catch (McpException e)
        {
            return (false, e.Message);
        }
    }

    private static string Compact(JsonElement arguments) => arguments.ValueKind == JsonValueKind.Undefined
        ? "{}"
        : arguments.GetRawText().ReplaceLineEndings(" ");

    private static string Cut(string text, int max) => text.Length <= max ? text : text[..max] + "…";

    // A conversation's last turns, after forgetting those idle too long.
    private List<AiMessage> Turns(string? id)
    {
        lock (_lock)
        {
            Forget();
            return id is not null && _conversations.TryGetValue(id, out var conversation) ? [.. conversation.Turns] : [];
        }
    }

    private void Remember(string? id, string text, string reply)
    {
        if (id is null)
            return;
        lock (_lock)
        {
            Forget();
            if (!_conversations.TryGetValue(id, out var conversation))
            {
                if (_conversations.Count >= MaxConversations)
                    _conversations.Remove(_conversations.MinBy(c => c.Value.Used).Key);
                conversation = new Conversation();
                _conversations[id] = conversation;
            }
            conversation.Turns.Add(new AiMessage(AiRole.User, text));
            conversation.Turns.Add(new AiMessage(AiRole.Assistant, reply.Length == 0 ? "(no reply)" : reply));
            if (conversation.Turns.Count > MaxTurns)
                conversation.Turns.RemoveRange(0, conversation.Turns.Count - MaxTurns);
            conversation.Used = _time.GetUtcNow();
        }
    }

    private void Forget()
    {
        var now = _time.GetUtcNow();
        foreach (var idle in _conversations.Where(c => now - c.Value.Used > ConversationIdle).Select(c => c.Key).ToList())
            _conversations.Remove(idle);
    }

    // Counts only: the command and the reply are user content.
    [LoggerMessage(Level = LogLevel.Information, Message = "agent command on {Provider}: {Calls} tool call(s), ended by {Ended}, {Tokens} tokens")]
    private static partial void LogEnded(ILogger log, string provider, int calls, string ended, long tokens);

    private sealed class Conversation
    {
        public List<AiMessage> Turns { get; } = [];

        public DateTimeOffset Used { get; set; }
    }
}
