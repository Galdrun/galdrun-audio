using System.Text.Json;
using System.Text.Json.Serialization;
using Multiroom.Server.Abstractions;

namespace Multiroom.Server.Ai.Agent;

// One command of a chat as it ran: what was asked, with which provider,
// model and reasoning, and what came back. A command that failed has Error
// (the code) and Detail instead of a reply.
public sealed record AgentChatTurn(
    string Text,
    DateTimeOffset At,
    string? Provider,
    string? Model,
    AiReasoning? Reasoning,
    string? Reply,
    IReadOnlyList<AgentAction> Actions,
    AiUsage? Usage,
    string? StoppedBy = null,
    string? Detail = null,
    string? Error = null);

public sealed record AgentChatSummary(string Id, string Title, DateTimeOffset Created, DateTimeOffset Updated, int Turns);

public sealed record AgentChat(string Id, string Title, DateTimeOffset Created, DateTimeOffset Updated,
    IReadOnlyList<AgentChatTurn> Turns);

// The agent's chats, by conversation id: every command sent with one, kept
// so a chat can be read again and continued after the agent's own memory
// of it (MultiroomAgent.ConversationIdle) is gone. <dataDirectory>/ai/chats.json,
// owner-only, written whole. The least recently used chat goes past MaxChats,
// a chat's oldest turns past MaxTurns.
public sealed class AgentChatStore
{
    public const int MaxChats = 100;
    public const int MaxTurns = 200;
    public const int MaxTitleLength = 80;

    private static readonly JsonSerializerOptions Json = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        Converters = { new JsonStringEnumConverter(JsonNamingPolicy.CamelCase) },
    };

    private readonly string _path;
    private readonly Lock _lock = new();
    private List<AgentChat> _chats;

    public AgentChatStore(string path)
    {
        _path = path;
        _chats = Read(path);
    }

    // Most recently used first.
    public IReadOnlyList<AgentChatSummary> Chats
    {
        get
        {
            lock (_lock)
                return [.. _chats.OrderByDescending(c => c.Updated).Select(c => new AgentChatSummary(c.Id, c.Title, c.Created, c.Updated, c.Turns.Count))];
        }
    }

    public Result<AgentChat, AiError> Get(string id)
    {
        lock (_lock)
            return _chats.Find(c => c.Id == id) is { } chat ? chat : Result.Fail(AiError.NotFound, $"no chat {id}");
    }

    // A new id is a new chat, titled by its first command.
    public Result<bool, AiError> Add(string id, AgentChatTurn turn)
    {
        lock (_lock)
        {
            var chat = _chats.Find(c => c.Id == id);
            List<AgentChat> next = [.. _chats.Where(c => c.Id != id)];
            if (chat is null)
            {
                chat = new AgentChat(id, Title(turn.Text), turn.At, turn.At, []);
                if (next.Count >= MaxChats)
                    next.Remove(next.MinBy(c => c.Updated)!);
            }
            next.Add(chat with { Updated = turn.At, Turns = [.. chat.Turns.Append(turn).TakeLast(MaxTurns)] });
            return Write(next);
        }
    }

    public Result<AgentChatSummary, AiError> Rename(string id, string title)
    {
        title = title.Trim();
        if (title.Length is 0 or > MaxTitleLength)
            return Result.Fail(AiError.InvalidArgument, $"title: 1 to {MaxTitleLength} characters");
        lock (_lock)
        {
            if (_chats.Find(c => c.Id == id) is not { } chat)
                return Result.Fail(AiError.NotFound, $"no chat {id}");
            var renamed = chat with { Title = title };
            var written = Write([.. _chats.Select(c => c.Id == id ? renamed : c)]);
            return written.IsSuccess
                ? new AgentChatSummary(id, title, renamed.Created, renamed.Updated, renamed.Turns.Count)
                : written.Failure;
        }
    }

    public Result<bool, AiError> Remove(string id)
    {
        lock (_lock)
        {
            if (!_chats.Any(c => c.Id == id))
                return Result.Fail(AiError.NotFound, $"no chat {id}");
            return Write([.. _chats.Where(c => c.Id != id)]);
        }
    }

    // What the agent is told of a chat it no longer remembers: its answered
    // turns, as MultiroomAgent keeps them.
    public static IReadOnlyList<AiMessage> History(AgentChat chat) =>
    [
        .. chat.Turns.Where(t => t.Error is null).SelectMany(t => new[]
        {
            new AiMessage(AiRole.User, t.Text),
            new AiMessage(AiRole.Assistant, string.IsNullOrEmpty(t.Reply) ? "(no reply)" : t.Reply),
        }),
    ];

    private static string Title(string text)
    {
        var line = text.Trim().ReplaceLineEndings(" ");
        return line.Length <= MaxTitleLength ? line : line[..(MaxTitleLength - 1)] + "…";
    }

    private Result<bool, AiError> Write(List<AgentChat> next)
    {
        try
        {
            OwnerOnlyFile.Write(_path, JsonSerializer.SerializeToUtf8Bytes(next, Json));
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            return Result.Fail(AiError.Unavailable, $"the chats cannot be written: {e.Message}");
        }
        _chats = next;
        return true;
    }

    // A file that cannot be read is no chats.
    private static List<AgentChat> Read(string path)
    {
        try
        {
            return JsonSerializer.Deserialize<List<AgentChat>>(File.ReadAllBytes(path), Json) ?? [];
        }
        catch (Exception e) when (e is FileNotFoundException or DirectoryNotFoundException or JsonException
            or IOException or UnauthorizedAccessException)
        {
            return [];
        }
    }
}
