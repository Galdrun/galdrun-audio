// Phase 33: the discovery beacons' listener and what it remembers.
//
// The beacons come from the client's discovery::BeaconSender, whose wire format is
// `shared/matp/ClientBeacon.h`; here a test socket plays the client, sending
// that same encoding to the listener on loopback.

#include "ManualClock.h"
#include "TestSupport.h"
#include "discovery/DiscoveredClients.h"
#include "discovery/DiscoveryListener.h"
#include "matp/CoreBeacon.h"
#include "matp/Protocol.h"
#include "util/Json.h"
#include "net/Socket.h"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

using namespace multiroom;

namespace {

matp::ClientBeacon beaconFor(const std::string& id, uint16_t controlPort = 4720)
{
    matp::ClientBeacon beacon;
    beacon.protocolVersion = matp::kProtocolVersion;
    beacon.clientId = id;
    beacon.clientVersion = "0.1.0";
    beacon.platform = "linux-armv6";
    beacon.host = "kitchen-pi";
    beacon.controlPort = controlPort;
    return beacon;
}

template <typename Predicate>
bool waitFor(Predicate predicate)
{
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < until)
    {
        if (predicate())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return predicate();
}

// A client's socket, reduced to sending one datagram to loopback.
class BeaconSender
{
public:
    BeaconSender() { socket_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP); }
    ~BeaconSender() { net::closeSocket(socket_); }

    BeaconSender(const BeaconSender&) = delete;
    BeaconSender& operator=(const BeaconSender&) = delete;

    bool send(uint16_t port, const std::string& payload)
    {
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(port);
        return ::sendto(socket_, payload.data(), static_cast<int>(payload.size()), 0,
                        reinterpret_cast<const sockaddr*>(&address),
                        static_cast<int>(sizeof(address))) >= 0;
    }

private:
    net::Library library_; // before the socket: Winsock must be up first
    net::Socket socket_ = net::kInvalidSocket;
};

} // namespace

MULTIROOM_TEST(discovery_remembers_a_client_and_follows_it_to_a_new_address)
{
    DiscoveredClients clients;
    CHECK(clients.onBeacon(beaconFor("kitchen"), "10.0.0.7", 1000));
    CHECK(!clients.onBeacon(beaconFor("kitchen", 4799), "10.0.0.8", 3000));

    const auto kitchen = clients.find(ClientId("kitchen"));
    CHECK(kitchen.has_value());
    CHECK_EQ(kitchen->address, std::string("10.0.0.8"));
    CHECK_EQ(kitchen->controlPort, uint16_t(4799));
    CHECK_EQ(kitchen->firstSeenNs, uint64_t(1000));
    CHECK_EQ(kitchen->lastSeenNs, uint64_t(3000));
    CHECK_EQ(kitchen->beacons, uint64_t(2));
    CHECK_EQ(clients.size(), size_t(1));
}

MULTIROOM_TEST(discovery_calls_a_client_present_until_three_beacons_are_missed)
{
    DiscoveredClient client;
    client.lastSeenNs = 1000;
    CHECK(isPresent(client, 1000));
    CHECK(isPresent(client, 1000 + kDiscoveryPresentWithinNs));
    CHECK(!isPresent(client, 1000 + kDiscoveryPresentWithinNs + 1));
}

MULTIROOM_TEST(discovery_forgets_the_longest_silent_client_at_its_ceiling)
{
    DiscoveredClients clients(2);
    clients.onBeacon(beaconFor("a"), "10.0.0.1", 100);
    clients.onBeacon(beaconFor("b"), "10.0.0.2", 200);
    clients.onBeacon(beaconFor("a"), "10.0.0.1", 300); // a is the fresher one now
    clients.onBeacon(beaconFor("c"), "10.0.0.3", 400);

    CHECK_EQ(clients.size(), size_t(2));
    CHECK(clients.find(ClientId("a")).has_value());
    CHECK(!clients.find(ClientId("b")).has_value());
    CHECK(clients.find(ClientId("c")).has_value());
    CHECK_EQ(clients.forgotten(), uint64_t(1));
}

MULTIROOM_TEST(discovery_listener_hears_a_beacon_and_drops_noise)
{
    multiroomtest::ManualClock clock{5000};
    DiscoveredClients clients;
    DiscoveryListenerSettings settings;
    settings.bindAddress = "127.0.0.1";
    settings.port = 0;
    auto listener = DiscoveryListener::start(settings, clients, clock);
    CHECK_OK(listener);
    CHECK((*listener)->port() != 0);

    BeaconSender sender;
    CHECK(sender.send((*listener)->port(), "not a beacon"));
    CHECK(sender.send((*listener)->port(), matp::makeClientBeacon(beaconFor("kitchen"))));

    CHECK(waitFor([&] { return (*listener)->received() == 1 && (*listener)->rejected() == 1; }));
    const auto kitchen = clients.find(ClientId("kitchen"));
    CHECK(kitchen.has_value());
    CHECK_EQ(kitchen->address, std::string("127.0.0.1"));
    CHECK_EQ(kitchen->controlPort, uint16_t(4720));
    CHECK_EQ(kitchen->platform, std::string("linux-armv6"));
    CHECK_EQ(kitchen->host, std::string("kitchen-pi"));
    CHECK_EQ(kitchen->lastSeenNs, uint64_t(5000));
}

MULTIROOM_TEST(discovery_listener_announces_disabled_and_reactivated_clients_once_per_change)
{
    multiroomtest::ManualClock clock{0};
    DiscoveredClients clients;
    std::atomic<int> changes{0};
    DiscoveryListenerSettings settings;
    settings.bindAddress = "127.0.0.1";
    settings.port = 0;
    settings.onActiveChanged = [&](const std::string&, bool active) {
        changes.fetch_add(active ? 10 : 1);
    };
    auto listener = DiscoveryListener::start(settings, clients, clock);
    CHECK_OK(listener);
    BeaconSender sender;
    auto beacon = beaconFor("kitchen");
    beacon.active = false; // A core starting while this client is disabled.
    CHECK(sender.send((*listener)->port(), matp::makeClientBeacon(beacon)));
    CHECK(waitFor([&] { return changes.load() == 1; }));
    CHECK(sender.send((*listener)->port(), matp::makeClientBeacon(beacon)));
    CHECK(waitFor([&] { return (*listener)->received() == 2; }));
    CHECK_EQ(changes.load(), 1);
    beacon.active = true;
    CHECK(sender.send((*listener)->port(), matp::makeClientBeacon(beacon)));
    CHECK(waitFor([&] { return changes.load() == 11; }));
}

MULTIROOM_TEST(discovery_listener_says_so_when_select_fails_and_stops_ticking)
{
    // As for the TCP listeners: stopped for good and said so, so that main()
    // can have the server restarted -- and not spinning on a select() that
    // fails at once, which would also call the tick as fast as it could.
    multiroomtest::ManualClock clock{0};
    DiscoveredClients clients;
    std::atomic<uint64_t> ticks{0};
    DiscoveryListenerSettings settings;
    settings.bindAddress = "127.0.0.1";
    settings.port = 0;
    settings.onTick = [&ticks] { ticks.fetch_add(1); };
    auto listener = DiscoveryListener::start(settings, clients, clock);
    CHECK_OK(listener);
    CHECK((*listener)->listening());

    CHECK(multiroomtest::closeListenerOn((*listener)->port(), true));
    CHECK(waitFor([&] { return !(*listener)->listening(); }));

    const uint64_t stoppedAt = ticks.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK_EQ(ticks.load(), stoppedAt);

    const auto before = std::chrono::steady_clock::now();
    listener->reset();
    CHECK(std::chrono::steady_clock::now() - before < std::chrono::seconds(1));
}

MULTIROOM_TEST(discovery_listener_refuses_an_address_it_cannot_bind)
{
    multiroomtest::ManualClock clock{0};
    DiscoveredClients clients;
    DiscoveryListenerSettings settings;
    settings.bindAddress = "not-an-address";
    settings.port = 0;
    CHECK_ERR(DiscoveryListener::start(settings, clients, clock),
              DiscoveryListenerError::BindFailed);
}

MULTIROOM_TEST(core_beacon_carries_what_the_high_end_server_needs)
{
    // The reader is C# (the high-end server's CoreDiscovery), so this pins the
    // field names it looks for.
    matp::CoreBeacon beacon;
    beacon.coreId = "galdrun-core-nas-1a2b3c4d";
    beacon.coreVersion = "0.1.0";
    beacon.controlPort = 4713;

    json::Value root;
    std::string error;
    CHECK(json::parse(matp::makeCoreBeacon(beacon), root, error));
    std::string text;
    CHECK(json::getString(root, "type", text));
    CHECK_EQ(text, std::string("coreBeacon"));
    CHECK(json::getString(root, "coreId", text));
    CHECK_EQ(text, beacon.coreId);
    CHECK(json::getString(root, "coreVersion", text));
    CHECK_EQ(text, std::string("0.1.0"));
    CHECK_EQ(root.find("controlPort")->asInt(), 4713);
    CHECK_EQ(root.find("protocolVersion")->asInt(), static_cast<int64_t>(matp::kProtocolVersion));
}
