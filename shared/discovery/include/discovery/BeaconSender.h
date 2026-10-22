#pragma once

// Announces this process on the local network: every
// matp::kBeaconIntervalNs, one UDP datagram to the broadcast address of every
// IPv4 interface that is up. What it says is the caller's -- the client sends
// `matp/ClientBeacon.h`, the server core `matp/CoreBeacon.h` -- so both
// compile this one sender and neither has its own copy of the interface
// listing below.
//
// Per interface and not to 255.255.255.255: Windows sends a limited broadcast
// out of one interface of its own choosing, and on a machine with Hyper-V or
// WSL that is routinely a virtual switch rather than the LAN. The interfaces
// are listed again before every send, because at boot the LAN often comes up
// after the process does, and a DHCP lease can change the address.
//
// Not part of MATP: the protocol leaves discovery to a later version (section
// 52), and nothing here changes what happens once a connection exists.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace multiroom {
namespace discovery {

class BeaconSender
{
public:
    // The client logs printf-style and the server core with std::format, so
    // the sender logs through whichever the owner hands it. `warning` is false
    // for the informational lines. Called on the owner's thread, once per
    // change rather than once per beacon.
    using Log = std::function<void(bool warning, const std::string& message)>;

    BeaconSender();
    ~BeaconSender();

    BeaconSender(const BeaconSender&) = delete;
    BeaconSender& operator=(const BeaconSender&) = delete;

    // Empty `destination` broadcasts on every interface; a dotted IPv4 address
    // sends there alone, which is what the tests do with loopback and what a
    // core whose control API is loopback-only does. Opening is the only step
    // that can fail outright; a send that fails is retried at the next
    // interval.
    bool start(uint16_t port, std::string& error, const std::string& destination = std::string(),
               Log log = Log());
    void stop();

    // Owner thread. Sends on the first call and then once per
    // matp::kBeaconIntervalNs. The payload is asked for on every send rather
    // than taken at start, so a changed id is announced from the next one on.
    void poll(uint64_t nowNs, const std::function<std::string()>& payload);

    // Beacons that went out on at least one interface.
    uint32_t sent() const;
    // The last beacon's failure, empty once one goes out everywhere again.
    const std::string& lastError() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace discovery
} // namespace multiroom
