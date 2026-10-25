#pragma once

// What the discovery beacons have told this server (phase 33): which clients
// are on the network, at which address, and where their control API listens.
//
// A client broadcasts `matp/ClientBeacon.h` every two seconds whether or not
// it is connected to anybody, so this is a different list from the
// RoomDirectory's: a room there has said HELLO to this server, a client here
// has only said that it exists. The high-end server reads it through
// `discovery.clients` and decides which ones to attach; `client.attach` then
// points the client's own control API at this server (its `setServer`).
//
// **Bounded** like everything else: `maxClients` entries. At the ceiling the
// one heard from longest ago is forgotten and counted, because a beacon is
// unauthenticated and a stranger could otherwise fill it; a real client that
// was forgotten is back within one interval.
//
// **Latest beacon wins.** A client that moves to another address -- a new
// DHCP lease, the other network card -- is the same client at its new
// address. Two machines cloned with the same id would alternate here, which
// is the clone's problem to fix with `setClientId`.
//
// Thread-safe: the discovery listener writes, the control API reads. Time is
// a parameter, never read here.

#include "core/Ids.h"
#include "matp/ClientBeacon.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace multiroom {

struct DiscoveredClient
{
    bool active = true;
    ClientId client;
    std::string address; // the beacon's source address, dotted IPv4
    uint16_t controlPort = 0;
    uint32_t protocolVersion = 0;
    std::string name;          // the room's name (section 60); empty when the beacon did not say
    std::string clientVersion; // empty when the beacon did not say
    std::string platform;      // likewise
    std::string host;          // the device's own name; likewise
    // The MATP server the client is pointed at, "" for none, nullopt when its
    // beacon is from before the field.
    std::optional<std::string> server;
    uint64_t firstSeenNs = 0;
    uint64_t lastSeenNs = 0;
    uint64_t beacons = 0;
};

// Three beacons missed in a row, which one lost datagram is not.
inline constexpr uint64_t kDiscoveryPresentWithinNs = 3 * matp::kBeaconIntervalNs;

// Whether the client has been heard recently enough to call it on the network.
[[nodiscard]] bool isPresent(const DiscoveredClient& client, uint64_t nowNs) noexcept;

class DiscoveredClients
{
public:
    static constexpr size_t kDefaultMaxClients = 256;

    explicit DiscoveredClients(size_t maxClients = kDefaultMaxClients);

    DiscoveredClients(const DiscoveredClients&) = delete;
    DiscoveredClients& operator=(const DiscoveredClients&) = delete;

    // True for a client this list did not hold.
    bool onBeacon(const matp::ClientBeacon& beacon, const std::string& address, uint64_t nowNs);

    // By client id.
    [[nodiscard]] std::vector<DiscoveredClient> clients() const;
    [[nodiscard]] std::optional<DiscoveredClient> find(const ClientId& client) const;

    [[nodiscard]] size_t size() const;
    [[nodiscard]] size_t maxClients() const noexcept { return maxClients_; }
    // Entries dropped at the ceiling.
    [[nodiscard]] uint64_t forgotten() const;

private:
    mutable std::mutex mutex_;
    std::map<ClientId, DiscoveredClient> clients_;
    const size_t maxClients_;
    uint64_t forgotten_ = 0;
};

} // namespace multiroom
