using System.Security.Claims;

namespace Multiroom.Server.Mcp;

// What a tool may do. Every tool is in exactly
// one tier; a caller is allowed a set of them.
public enum McpTier
{
    // Reads the house: search, browse, zones, queues, rooms, collections.
    Read,

    // Plays and steers: transport, queue additions, volume, membership.
    Control,

    // Adds to what the household keeps: playlists, favourites, likes.
    Modify,

    // Takes away: queue items, playlists, favourites, zones.
    Destructive,
}

// Who is calling and what it may do, carried as claims on the request's user,
// so the SDK hands them to every filter. Set by whoever authenticates the
// caller: the outward API for HTTP, the agent for the in-memory transport.
public static class McpCaller
{
    public const string AuthenticationType = Branding.Slug;
    public const string NameClaim = Branding.Slug + ":caller";
    public const string TierClaim = Branding.Slug + ":tier";

    public static ClaimsPrincipal Principal(string name, IEnumerable<McpTier> tiers) =>
        new(new ClaimsIdentity(
            [
                new Claim(ClaimTypes.NameIdentifier, name),
                new Claim(NameClaim, name),
                .. tiers.Distinct().Select(t => new Claim(TierClaim, t.ToString())),
            ],
            AuthenticationType));

    // A request without the claims is allowed nothing.
    public static (string Name, IReadOnlySet<McpTier> Tiers) Of(ClaimsPrincipal? user)
    {
        var name = user?.FindFirst(NameClaim)?.Value ?? "unknown";
        var tiers = user?.FindAll(TierClaim).Select(c => Enum.TryParse<McpTier>(c.Value, out var t) ? t : (McpTier?)null)
            .OfType<McpTier>().ToHashSet() ?? [];
        return (name, tiers);
    }
}
