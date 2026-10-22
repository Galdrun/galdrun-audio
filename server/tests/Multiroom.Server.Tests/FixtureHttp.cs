using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Hosting;
using Microsoft.AspNetCore.Http;
using Microsoft.Extensions.Logging;

namespace Multiroom.Server.Tests;

// An HTTP server in process, on a free loopback port, answering every request
// with one handler: the stand-in for a service on the internet.
internal static class FixtureHttp
{
    public static async Task<(WebApplication App, Uri BaseUri)> StartAsync(RequestDelegate handle)
    {
        var builder = WebApplication.CreateEmptyBuilder(new WebApplicationOptions());
        builder.WebHost.UseKestrelCore().ConfigureKestrel(k => k.Listen(System.Net.IPAddress.Loopback, 0));
        builder.Logging.ClearProviders();
        var app = builder.Build();
        app.Run(handle);
        await app.StartAsync();
        return (app, new Uri(app.Urls.First() + "/"));
    }
}
