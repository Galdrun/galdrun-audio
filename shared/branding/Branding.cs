namespace Multiroom;

// The product's brand for .NET code, linked into Multiroom.Server.Abstractions
// and Multiroom.App.Api. Namespaces use the fixed codename "Multiroom"; only
// what a user or another program sees comes from here. Branding.props carries
// the same values for MSBuild.
public static class Branding
{
    // Display: window title, wordmark, mDNS instance text, AI and MCP names.
    public const string Name = "Galdrun";

    // Contracts: exe and service names, paths, the mDNS service type, claims.
    public const string Slug = "galdrun";

    // Environment variables read by the server: GALDRUN_logging__level=debug.
    public const string EnvironmentPrefix = "GALDRUN_";

    public const string ServerName = Slug + "-server";
    public const string ServiceType = "_" + Slug + "._tcp.local";
    public const string InstancePrefix = Name + " on ";
}
