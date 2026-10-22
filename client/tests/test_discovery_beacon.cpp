#include "TestSupport.h"

#include "matp/ClientBeacon.h"
#include "matp/Protocol.h"
#include "discovery/BeaconSender.h"
#include "net/Socket.h" // not a public header; the test plays the listening server with it

#include <optional>
#include <string>

using namespace multiroom;

namespace {

// A UDP socket on an ephemeral loopback port, standing in for a server
// listening on matp::kDiscoveryPort.
class BeaconListener
{
public:
    BeaconListener()
    {
        socket_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket_ == net::kInvalidSocket)
            return;

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        socklen_t length = sizeof(address);
        if (::bind(socket_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
            ::getsockname(socket_, reinterpret_cast<sockaddr*>(&address), &length) != 0)
        {
            net::closeSocket(socket_);
            socket_ = net::kInvalidSocket;
            return;
        }
        port_ = ntohs(address.sin_port);
    }

    ~BeaconListener() { net::closeSocket(socket_); }

    BeaconListener(const BeaconListener&) = delete;
    BeaconListener& operator=(const BeaconListener&) = delete;

    uint16_t port() const { return port_; }

    // One datagram, or an empty string once the timeout passes without one.
    std::string receive(int timeoutMs)
    {
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(socket_, &readable);
        timeval timeout{};
        timeout.tv_sec = timeoutMs / 1000;
        timeout.tv_usec = (timeoutMs % 1000) * 1000;
        if (::select(static_cast<int>(socket_ + 1), &readable, nullptr, nullptr, &timeout) <= 0)
            return std::string();

        char buffer[2048];
        const int received = net::receive(socket_, buffer, sizeof(buffer));
        return received > 0 ? std::string(buffer, static_cast<size_t>(received)) : std::string();
    }

private:
    net::Library library_; // before the socket: Winsock must be up first
    net::Socket socket_ = net::kInvalidSocket;
    uint16_t port_ = 0;
};

matp::ClientBeacon testBeacon()
{
    matp::ClientBeacon beacon;
    beacon.clientId = "livingroom";
    beacon.name = "Living room";
    beacon.clientVersion = "0.1.0";
    beacon.platform = "linux-armv6";
    beacon.host = "livingroom-pi";
    beacon.controlPort = 4720;
    beacon.server = "10.0.0.7:4711";
    return beacon;
}

} // namespace

MULTIROOM_TEST(beacon_round_trips_through_the_shared_parser)
{
    // The server reads it with this same parser, so this is the contract.
    matp::ClientBeacon parsed;
    std::string error;
    CHECK(matp::parseClientBeacon(matp::makeClientBeacon(testBeacon()), parsed, error));
    CHECK_EQ(parsed.protocolVersion, matp::kProtocolVersion);
    CHECK_EQ(parsed.clientId, std::string("livingroom"));
    CHECK_EQ(parsed.name, std::string("Living room"));
    CHECK_EQ(parsed.clientVersion, std::string("0.1.0"));
    CHECK_EQ(parsed.platform, std::string("linux-armv6"));
    CHECK_EQ(parsed.host, std::string("livingroom-pi"));
    CHECK_EQ(parsed.controlPort, static_cast<uint16_t>(4720));
    CHECK(parsed.server == std::optional<std::string>("10.0.0.7:4711"));
}

MULTIROOM_TEST(beacon_tells_no_server_from_an_older_client)
{
    // "" is a client nobody attached; a missing key is a client from before
    // the field, which says nothing either way.
    matp::ClientBeacon beacon = testBeacon();
    beacon.server.reset();
    matp::ClientBeacon parsed;
    std::string error;
    CHECK(matp::parseClientBeacon(matp::makeClientBeacon(beacon), parsed, error));
    CHECK(parsed.server == std::optional<std::string>(""));

    CHECK(matp::parseClientBeacon("{\"type\":\"clientBeacon\",\"protocolVersion\":2,"
                                  "\"clientId\":\"a\",\"controlPort\":4720}",
                                  parsed, error));
    CHECK(!parsed.server.has_value());
    CHECK(!matp::parseClientBeacon("{\"type\":\"clientBeacon\",\"protocolVersion\":2,"
                                   "\"clientId\":\"a\",\"controlPort\":4720,\"server\":1}",
                                   parsed, error));
}

MULTIROOM_TEST(beacon_parser_refuses_what_names_no_client)
{
    matp::ClientBeacon parsed;
    std::string error;
    CHECK(!matp::parseClientBeacon("not json", parsed, error));
    CHECK(!matp::parseClientBeacon("{\"type\":\"hello\",\"protocolVersion\":2,\"clientId\":\"a\","
                                   "\"controlPort\":1}",
                                   parsed, error));
    CHECK(!matp::parseClientBeacon("{\"type\":\"clientBeacon\",\"protocolVersion\":2,"
                                   "\"clientId\":\"\",\"controlPort\":4720}",
                                   parsed, error));
    CHECK(!matp::parseClientBeacon("{\"type\":\"clientBeacon\",\"protocolVersion\":2,"
                                   "\"clientId\":\"a\",\"controlPort\":0}",
                                   parsed, error));
    CHECK(!matp::parseClientBeacon("{\"type\":\"clientBeacon\",\"clientId\":\"a\","
                                   "\"controlPort\":4720}",
                                   parsed, error));

    // An unknown key is a newer client, not a broken one.
    CHECK(matp::parseClientBeacon("{\"type\":\"clientBeacon\",\"protocolVersion\":3,"
                                  "\"clientId\":\"a\",\"controlPort\":4720,\"extra\":true}",
                                  parsed, error));
}

MULTIROOM_TEST(beacon_is_sent_once_per_interval)
{
    BeaconListener listener;
    CHECK(listener.port() != 0);

    discovery::BeaconSender beacon;
    std::string error;
    CHECK(beacon.start(listener.port(), error, "127.0.0.1"));

    // The clock is passed in, so the interval is walked without sleeping.
    const uint64_t t0 = 1000000000ULL;
    matp::ClientBeacon info = testBeacon();
    beacon.poll(t0, [&]() { return matp::makeClientBeacon(info); });
    CHECK_EQ(beacon.sent(), 1u);
    CHECK_EQ(listener.receive(2000), matp::makeClientBeacon(info));

    beacon.poll(t0 + matp::kBeaconIntervalNs - 1, [&]() { return matp::makeClientBeacon(info); });
    CHECK_EQ(beacon.sent(), 1u);

    // A changed id is announced by the next beacon, not by a restart.
    info.clientId = "kitchen";
    beacon.poll(t0 + matp::kBeaconIntervalNs, [&]() { return matp::makeClientBeacon(info); });
    CHECK_EQ(beacon.sent(), 2u);
    CHECK_EQ(listener.receive(2000), matp::makeClientBeacon(info));
    CHECK(beacon.lastError().empty());
}

MULTIROOM_TEST(beacon_refuses_a_destination_that_is_not_an_ipv4_address)
{
    discovery::BeaconSender beacon;
    std::string error;
    CHECK(!beacon.start(matp::kDiscoveryPort, error, "server.local"));
    CHECK(!error.empty());

    // Not started, so polling is a no-op rather than a send to nowhere.
    beacon.poll(0, []() { return matp::makeClientBeacon(testBeacon()); });
    CHECK_EQ(beacon.sent(), 0u);
}
