using System.Diagnostics;
using System.IO.Pipelines;
using System.Security.Claims;
using System.Text.Json;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Options;
using ModelContextProtocol.Client;
using ModelContextProtocol.Protocol;
using ModelContextProtocol.Server;

namespace Multiroom.Server.Mcp;

// One tool call, as the event stream tells it: who, which tool, whether it
// answered or failed, and how long it took. No arguments: a search text is
// user content.
public sealed record ToolCalled(string Caller, string Tool, bool Ok, long Ms);

public static class MultiroomMcpServer
{
    // The catalog on an MCP server, with the caller's tiers enforced
    // A tool outside them is not listed, and is
    // refused by name if called anyway. The caller is the request's user
    // (McpCaller); one with no claims is allowed nothing.
    public static IMcpServerBuilder WithMultiroomTools(this IMcpServerBuilder builder, MultiroomTools tools,
        Action<ToolCalled>? called = null) =>
        builder.WithTools(tools.Tools.Select(t => t.Tool)).WithRequestFilters(filters =>
        {
            filters.AddListToolsFilter(next => async (context, cancellationToken) =>
            {
                var result = await next(context, cancellationToken).ConfigureAwait(false);
                var (_, tiers) = McpCaller.Of(context.User);
                result.Tools = [.. result.Tools.Where(t => tools.TierOf(t.Name) is { } tier && tiers.Contains(tier))];
                return result;
            });
            filters.AddCallToolFilter(next => async (context, cancellationToken) =>
            {
                var (caller, tiers) = McpCaller.Of(context.User);
                var name = context.Params?.Name ?? "";
                if (tools.TierOf(name) is { } tier && !tiers.Contains(tier))
                {
                    called?.Invoke(new ToolCalled(caller, name, false, 0));
                    return new CallToolResult
                    {
                        IsError = true,
                        Content = [new TextContentBlock { Text = $"forbidden: {name} is a {tier.ToString().ToLowerInvariant()} tool, which {caller} is not allowed" }],
                    };
                }
                // Every tool's zone may be named by its id, its name or one of its rooms.
                if (context.Params?.Arguments is { } arguments && arguments.TryGetValue("zone", out var zone) &&
                    zone.ValueKind == JsonValueKind.String)
                {
                    arguments["zone"] = JsonSerializer.SerializeToElement(tools.ResolveZone(zone.GetString()!));
                }
                var clock = Stopwatch.StartNew();
                var ok = false;
                try
                {
                    var result = await next(context, cancellationToken).ConfigureAwait(false);
                    ok = result.IsError != true;
                    return result;
                }
                finally
                {
                    called?.Invoke(new ToolCalled(caller, name, ok, clock.ElapsedMilliseconds));
                }
            });
        });

    // An SDK client of the same catalog over an in-memory pipe (phase 25):
    // the internal agent's connection. The same tools/list, schemas, handlers
    // and tier filters as /api/v1/mcp; only the transport differs, and the
    // caller is set here instead of from a bearer token. Disposing the client
    // ends the server.
    public static async Task<McpClient> ConnectAsync(MultiroomTools tools, ClaimsPrincipal caller, Implementation info,
        Action<ToolCalled>? called, ILoggerFactory loggerFactory, CancellationToken cancellationToken)
    {
        var services = new ServiceCollection();
        services.AddSingleton(loggerFactory);
        services.AddMcpServer(o => o.ServerInfo = info)
            .WithMultiroomTools(tools, called)
            .WithMessageFilters(filters => filters.AddIncomingFilter(next => (context, ct) =>
            {
                context.User = caller;
                return next(context, ct);
            }));
        var provider = services.BuildServiceProvider();

        var toServer = new Pipe();
        var toClient = new Pipe();
        var server = McpServer.Create(new StreamServerTransport(toServer.Reader.AsStream(), toClient.Writer.AsStream(),
            info.Name, loggerFactory), provider.GetRequiredService<IOptions<McpServerOptions>>().Value, loggerFactory, provider);
        var running = RunAsync(server, provider);
        try
        {
            return await McpClient.CreateAsync(new StreamClientTransport(toServer.Writer.AsStream(),
                toClient.Reader.AsStream(), loggerFactory), loggerFactory: loggerFactory,
                cancellationToken: cancellationToken).ConfigureAwait(false);
        }
        catch
        {
            await toServer.Writer.CompleteAsync().ConfigureAwait(false);
            await running.ConfigureAwait(false);
            throw;
        }
    }

    // Until the client's end of the pipe closes. Nothing waits on it, so it
    // ends quietly whatever ended it.
    private static async Task RunAsync(McpServer server, ServiceProvider provider)
    {
        try
        {
            await server.RunAsync().ConfigureAwait(false);
        }
#pragma warning disable CA1031 // Nothing observes this task: a server ended by a broken pipe is just ended.
        catch (Exception)
#pragma warning restore CA1031
        {
        }
        finally
        {
            await server.DisposeAsync().ConfigureAwait(false);
            await provider.DisposeAsync().ConfigureAwait(false);
        }
    }
}
