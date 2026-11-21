using Multiroom.Server.Web.Api;

namespace Multiroom.Server.Tests;

public sealed class ServiceAnnouncerTests
{
    // Two servers on one machine are told apart by their API port; one on the usual port keeps the name an app
    // remembered.
    [Theory]
    [InlineData(4725, "Galdrun on studio._galdrun._tcp.local")]
    [InlineData(4735, "Galdrun on studio (4735)._galdrun._tcp.local")]
    public void The_instance_name_carries_the_port_unless_it_is_the_usual_one(int port, string expected) =>
        Assert.Equal(expected, ServiceAnnouncer.InstanceName("studio", port));
}
