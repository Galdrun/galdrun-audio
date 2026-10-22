using System.Buffers.Text;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;
using Multiroom.Server.Abstractions;

namespace Multiroom.Server.Mcp;

public enum McpError
{
    // No such named client.
    NotFound,

    // A name, a tier list or a client the file cannot take.
    InvalidArgument,

    // The file cannot be written.
    Unavailable,
}

// A named MCP client as the API shows it: never its token.
public sealed record McpClientView(string Name, IReadOnlyList<McpTier> Tiers, DateTimeOffset Created);

// A client just created: its token, answered this once.
public sealed record McpClientCreated(McpClientView Client, string Token)
{
    public override string ToString() => $"{nameof(McpClientCreated)} {{ Client = {Client} }}";
}

// The external MCP callers: the tiers allowed
// to whoever holds api.token (or anyone, on a loopback API without one), and
// named clients, each with its own token and tiers. A token is answered once,
// when the client is made, and kept only as its SHA-256; a lost token is a
// new client. <dataDirectory>/ai/mcp.json, owner-only, written whole. The
// tiers live here, not in ai/settings.json: the AI providers know no tool.
public sealed class McpClientStore
{
    public const int MaxClients = 32;
    public const int MaxNameLength = 64;

    public static readonly IReadOnlyList<McpTier> DefaultExternalTiers = [McpTier.Read, McpTier.Control];

    // The internal agent's (phase 25).
    public static readonly IReadOnlyList<McpTier> DefaultAgentTiers = [McpTier.Read, McpTier.Control, McpTier.Modify];

    private static readonly JsonSerializerOptions Json = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        Converters = { new JsonStringEnumConverter(JsonNamingPolicy.CamelCase) },
    };

    private readonly string _path;
    private readonly Lock _lock = new();
    private StoredMcp _stored;

    public McpClientStore(string path)
    {
        _path = path;
        _stored = Read(path);
    }

    public IReadOnlyList<McpTier> ExternalTiers
    {
        get
        {
            lock (_lock)
                return _stored.ExternalTiers ?? DefaultExternalTiers;
        }
    }

    public IReadOnlyList<McpTier> AgentTiers
    {
        get
        {
            lock (_lock)
                return _stored.AgentTiers ?? DefaultAgentTiers;
        }
    }

    public IReadOnlyList<McpClientView> Clients
    {
        get
        {
            lock (_lock)
                return [.. _stored.Clients.Select(View)];
        }
    }

    // Null keeps a list. None is allowed: those agents then see no tool.
    public Result<bool, McpError> SetTiers(IReadOnlyList<McpTier>? external, IReadOnlyList<McpTier>? agent)
    {
        lock (_lock)
        {
            return Write(_stored with
            {
                ExternalTiers = external is null ? _stored.ExternalTiers : [.. external.Distinct().Order()],
                AgentTiers = agent is null ? _stored.AgentTiers : [.. agent.Distinct().Order()],
            });
        }
    }

    public Result<McpClientCreated, McpError> Create(string name, IReadOnlyList<McpTier> tiers)
    {
        name = name.Trim();
        if (name.Length is 0 or > MaxNameLength || !name.All(c => char.IsAsciiLetterOrDigit(c) || c is '-' or '_' or '.'))
            return Result.Fail(McpError.InvalidArgument, $"name: 1 to {MaxNameLength} letters, digits, '-', '_' or '.'");
        if (tiers.Count == 0)
            return Result.Fail(McpError.InvalidArgument, "tiers: at least one of read, control, modify, destructive");
        var token = "vxm_" + Base64Url.EncodeToString(RandomNumberGenerator.GetBytes(32));
        lock (_lock)
        {
            if (_stored.Clients.Any(c => c.Name == name))
                return Result.Fail(McpError.InvalidArgument, $"name: {name} exists; delete it to make it again");
            if (_stored.Clients.Count >= MaxClients)
                return Result.Fail(McpError.InvalidArgument, $"at most {MaxClients} named clients");
            var client = new StoredClient(name, [.. tiers.Distinct().Order()], Hash(token), DateTimeOffset.UtcNow);
            var written = Write(_stored with { Clients = [.. _stored.Clients, client] });
            return written.IsSuccess ? new McpClientCreated(View(client), token) : written.Failure;
        }
    }

    public Result<bool, McpError> Remove(string name)
    {
        lock (_lock)
        {
            if (!_stored.Clients.Any(c => c.Name == name))
                return Result.Fail(McpError.NotFound, $"no MCP client {name}");
            var written = Write(_stored with { Clients = [.. _stored.Clients.Where(c => c.Name != name)] });
            return written.IsSuccess ? true : written.Failure;
        }
    }

    // The named client this token is, compared in fixed time.
    public McpClientView? Find(string token)
    {
        var hash = Convert.FromHexString(Hash(token));
        lock (_lock)
        {
            var found = _stored.Clients.FirstOrDefault(c =>
                CryptographicOperations.FixedTimeEquals(Convert.FromHexString(c.TokenHash), hash));
            return found is null ? null : View(found);
        }
    }

    private static McpClientView View(StoredClient client) => new(client.Name, client.Tiers, client.Created);

    private static string Hash(string token) => Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(token)));

    private Result<bool, McpError> Write(StoredMcp next)
    {
        try
        {
            OwnerOnlyFile.Write(_path, JsonSerializer.SerializeToUtf8Bytes(next, Json));
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            return Result.Fail(McpError.Unavailable, $"the MCP clients cannot be written: {e.Message}");
        }
        _stored = next;
        return true;
    }

    // A file that cannot be read is no clients and the default tiers.
    private static StoredMcp Read(string path)
    {
        try
        {
            return JsonSerializer.Deserialize<StoredMcp>(File.ReadAllBytes(path), Json) ?? new StoredMcp();
        }
        catch (Exception e) when (e is FileNotFoundException or DirectoryNotFoundException or JsonException
            or IOException or UnauthorizedAccessException)
        {
            return new StoredMcp();
        }
    }

    private sealed record StoredMcp(IReadOnlyList<McpTier>? ExternalTiers = null, IReadOnlyList<StoredClient>? Clients = null,
        IReadOnlyList<McpTier>? AgentTiers = null)
    {
        public IReadOnlyList<StoredClient> Clients { get; init; } = Clients ?? [];
    }

    private sealed record StoredClient(string Name, IReadOnlyList<McpTier> Tiers, string TokenHash, DateTimeOffset Created);
}
