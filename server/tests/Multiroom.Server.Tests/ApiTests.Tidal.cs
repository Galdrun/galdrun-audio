using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Logging.Abstractions;
using Multiroom.Server.Providers;
using Multiroom.Server.Providers.Tidal;

namespace Multiroom.Server.Tests;

public sealed partial class ApiTests
{
    [Fact]
    public async Task Tidal_device_login_finishes_without_a_browser_callback_and_can_be_resumed()
    {
        await using var tidal = await FakeTidal.StartAsync();
        await StartAsync(services: services =>
        {
            var registration = services.Single(d => d.ImplementationInstance is ProviderRegistration { Id: "tidal" });
            services.Remove(registration);
            services.AddProvider("tidal", _ =>
            {
                var provider = new TidalProvider(new TidalSettings(FakeTidal.ClientId, FakeTidal.ClientSecret, TidalQuality.Lossless,
                    64 * 1024 * 1024, 1024 * 1024), Path.Combine(_dir.Path, "tidal"), NullLogger.Instance, tidal.Endpoints);
                provider.Start();
                return provider;
            });
        });
        var login = await Ok(HttpMethod.Post, "providers/tidal/login");
        Assert.Equal("ABCDE", login["userCode"]!.GetValue<string>());
        Assert.Equal("https://link.tidal.test/ABCDE", login["authorizeUrl"]!.GetValue<string>());
        Assert.Null(login["deviceCode"]);
        Assert.Null(login["redirectUri"]);
        var waiting = await Ok(HttpMethod.Get, "providers/tidal/login");
        Assert.Equal("ABCDE", waiting["login"]!["userCode"]!.GetValue<string>());
        Assert.Equal("authRequired", waiting["status"]!["state"]!.GetValue<string>());
        await Ok(HttpMethod.Post, "providers/tidal/login");
        Assert.Equal(1, tidal.DeviceStarts);
        tidal.Approve();
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(10));
        while (true)
        {
            var status = await Ok(HttpMethod.Get, "providers/tidal/login");
            if (status["status"]!["state"]!.GetValue<string>() == "ready" && status["login"] is null) break;
            await Task.Delay(20, timeout.Token);
        }
        Assert.All(tidal.Forms, form => Assert.Equal(FakeTidal.ClientSecret, form["client_secret"]));
        Assert.DoesNotContain(FakeTidal.ClientSecret, (await Ok(HttpMethod.Get, "providers/tidal/login")).ToJsonString(), StringComparison.Ordinal);
    }
}
