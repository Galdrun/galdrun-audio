using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization.Metadata;
using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Http;
using Microsoft.Extensions.DependencyInjection;
using ModelContextProtocol.Client;
using ModelContextProtocol.Protocol;
using Multiroom.Server.Abstractions;
using Multiroom.Server.Mcp;

namespace Multiroom.Server.Web.Api;

// The tiers external agents get without a named client, and the internal
// agent's (phase 25).
public sealed record McpSettingsView(IReadOnlyList<McpTier> ExternalTiers, IReadOnlyList<McpTier> AgentTiers);

// Null keeps a list.
public sealed record McpSettingsPatch(IReadOnlyList<McpTier>? ExternalTiers = null, IReadOnlyList<McpTier>? AgentTiers = null);

public sealed record McpClientCreate(string Name, IReadOnlyList<McpTier> Tiers);

// The Galdrun MCP server (phase 23): Streamable HTTP at /api/v1/mcp on this
// listener, stateless (no session to keep or bound), with the tool catalog
// (Multiroom.Server.Mcp) and each caller's tiers. Who calls is decided here, from
// the bearer token: api.token (or none, on an API without one) is an external
// agent with the external tiers; a named client's token is that client with
// its own. A token that is neither is 401. A named client's token opens only
// /api/v1/mcp.
public sealed partial class ApiEndpoint
{
    private const string McpPath = "/api/v1/mcp";
    private const string ExternalCaller = "external";
    private const string AgentCaller = "agent";

    private MultiroomTools? _tools;

    private void AddMcp(IServiceCollection services)
    {
        // The API's own JSON, so a tool answers what the matching route does;
        // the SDK builds the tools' schemas from it.
        var json = new JsonSerializerOptions(ApiJson.Options) { TypeInfoResolver = ApiJson.Resolver() };
        _tools = new MultiroomTools(search, providers, house, queues, player, playlists, likes, addIns, resolver, history, bookPositions, zones, users, audiobookLikes, proposals,
            equalizerProfiles, stations, json);
        services.AddMcpServer(o => o.ServerInfo = McpInfo)
            .WithHttpTransport(o => o.Stateless = true)
            .WithMultiroomTools(_tools, c => events.Publish("ai.toolCalled", c));
    }

    private static Implementation McpInfo => new() { Name = Branding.Name, Version = Version };

    private McpSettingsView McpSettings() => new(mcpClients.ExternalTiers, mcpClients.AgentTiers);

    // The internal agent's connection (phase 25): the same catalog in memory,
    // as the caller "agent" with the agent's tiers.
    private Task<McpClient> ConnectAgentAsync(CancellationToken cancellationToken) =>
        MultiroomMcpServer.ConnectAsync(_tools!, McpCaller.Principal(AgentCaller, mcpClients.AgentTiers), McpInfo,
            c => events.Publish("ai.toolCalled", c), loggerFactory, cancellationToken);

    private void MapMcp(WebApplication app)
    {
        app.MapMcp(McpPath);
        app.MapGet("/api/v1/ai/mcp", () => Json(McpSettings()));
        app.MapMethods("/api/v1/ai/mcp", [HttpMethods.Patch], (Delegate)((HttpContext c) =>
            WithBody<McpSettingsPatch, McpSettingsView, McpError>(c, p =>
            {
                var set = mcpClients.SetTiers(p.ExternalTiers, p.AgentTiers);
                return Task.FromResult<Result<McpSettingsView, McpError>>(set.IsSuccess ? McpSettings() : set.Failure);
            })));
        app.MapGet("/api/v1/ai/mcp/clients", () => Json(mcpClients.Clients));
        app.MapPost("/api/v1/ai/mcp/clients", (Delegate)((HttpContext c) =>
            WithBody<McpClientCreate, McpClientCreated, McpError>(c,
                r => Task.FromResult(mcpClients.Create(r.Name, r.Tiers)), StatusCodes.Status201Created)));
        app.MapDelete("/api/v1/ai/mcp/clients/{name}", (string name) =>
        {
            var removed = mcpClients.Remove(name);
            return removed.IsSuccess ? Results.NoContent() : Error(removed.Failure);
        });
    }

    private static bool IsMcp(HttpRequest request) =>
        request.Path.StartsWithSegments(McpPath, StringComparison.Ordinal);

    // The caller as the tools see it, or false for a token that is no one's.
    private bool AuthorizeMcp(HttpContext context)
    {
        var header = context.Request.Headers.Authorization.ToString();
        string? token = header.StartsWith("Bearer ", StringComparison.Ordinal) ? header[7..] : null;
        if (token is not null && mcpClients.Find(token) is { } client)
        {
            context.User = McpCaller.Principal(client.Name, client.Tiers);
            return true;
        }
        var external = options.Token is null
            ? token is null
            : token is not null && CryptographicOperations.FixedTimeEquals(Encoding.UTF8.GetBytes(token),
                Encoding.UTF8.GetBytes(options.Token));
        if (!external)
            return false;
        context.User = McpCaller.Principal(ExternalCaller, mcpClients.ExternalTiers);
        return true;
    }
}
