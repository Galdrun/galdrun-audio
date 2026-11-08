#pragma once

// The discovery beacons' listener (phase 33): one UDP socket, one thread, and
// every beacon that parses written into a DiscoveredClients.
//
// Not MATP and not the control API -- a third port, and UDP. It answers
// nothing: a beacon is an announcement, and what to do about a client is the
// high-end server's decision, taken through `discovery.clients`. A datagram
// that is not a beacon is counted and dropped; the port is open to the whole
// network, so a stranger's noise is expected and never logged above debug.
//
// Bound to every interface by default, because the beacons are broadcasts
// that arrive on whichever card faces the client. IPv4 only, like the beacon.
//
// The one thing it answers: an SNTP request (RFC 4330, mode 3) gets this
// host's wall clock, unless that is still before 2026 (not set yet). The Galdrun appliances fall back to it when their router
// serves no time (hardware/buildroot galdrun-time-sources); this port is used
// because it is already open to the network.

#include "core/Error.h"
#include "discovery/DiscoveredClients.h"
#include "util/Clock.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace multiroom {

enum class DiscoveryListenerError
{
    SocketLibrary, // Winsock would not start
    BindFailed     // the address does not parse, or the port is taken
};

const char* errorName(DiscoveryListenerError error);

struct DiscoveryListenerSettings
{
    std::string bindAddress = "0.0.0.0";

    // Zero binds whichever port is free, which is what the tests use.
    uint16_t port = matp::kDiscoveryPort;

    // Called on the listener's thread at least every 200 ms. The core samples
    // its own system figures here (phase 34): this thread already wakes that
    // often and holds no zone lock, and a sample reads files on Linux.
    std::function<void()> onTick;
    std::function<void(const std::string&, bool)> onActiveChanged;
};

// The SNTP answer to a 48-byte client request, stamped with the times it was
// received and is sent.
std::array<uint8_t, 48> makeTimeReply(const uint8_t* request,
                                      std::chrono::system_clock::time_point received,
                                      std::chrono::system_clock::time_point transmitted);

class DiscoveryListener
{
public:
    // Binds on the calling thread, so a taken port is reported to whoever is
    // starting the server. `clients` and `clock` outlive the listener.
    [[nodiscard]] static Result<std::unique_ptr<DiscoveryListener>, DiscoveryListenerError>
    start(DiscoveryListenerSettings settings, DiscoveredClients& clients, const IClock& clock);

    // Stops the thread and closes the socket.
    ~DiscoveryListener();

    DiscoveryListener(const DiscoveryListener&) = delete;
    DiscoveryListener& operator=(const DiscoveryListener&) = delete;

    [[nodiscard]] uint16_t port() const noexcept { return port_; }
    [[nodiscard]] uint64_t received() const noexcept; // beacons that parsed
    [[nodiscard]] uint64_t rejected() const noexcept; // datagrams that did not
    [[nodiscard]] uint64_t timeRequests() const noexcept; // SNTP requests answered

    // False once the thread has given up on a failed select(), which nothing
    // here retries: the process is expected to stop and be restarted.
    [[nodiscard]] bool listening() const noexcept;

private:
    class Impl;

    DiscoveryListener(std::unique_ptr<Impl> impl, uint16_t port);

    std::unique_ptr<Impl> impl_;
    uint16_t port_ = 0;
};

} // namespace multiroom
