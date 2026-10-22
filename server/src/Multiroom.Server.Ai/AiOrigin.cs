namespace Multiroom.Server.Ai;

// What asked the AI, for the token usage per day: the background proposals,
// a queue's auto-fill, the agent (which drives the house through its MCP
// tools), or a person asking for suggestions. Other is anything untagged.
public enum AiOrigin
{
    Other,
    Proposals,
    AutoFill,
    Agent,
    Manual,
}

// The origin of the calls made within a feature's work: set with Begin around
// it, read by the registry when it sends a call. Async-local, so a suggester
// that several features share takes no parameter for it.
public static class AiOriginScope
{
    private static readonly AsyncLocal<AiOrigin> Value = new();

    public static AiOrigin Current => Value.Value;

    public static IDisposable Begin(AiOrigin origin)
    {
        var was = Value.Value;
        Value.Value = origin;
        return new Restore(was);
    }

    private sealed class Restore(AiOrigin was) : IDisposable
    {
        public void Dispose() => Value.Value = was;
    }
}
