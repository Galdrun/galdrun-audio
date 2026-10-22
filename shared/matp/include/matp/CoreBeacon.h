#pragma once

// The server core's discovery beacon: how the high-end server finds a core it
// was not told about, and follows one it chose to a new address. The same
// arrangement as `ClientBeacon.h` one hop up -- the core sends it, every
// matp::kBeaconIntervalNs, to UDP kCoreBeaconPort on every interface:
//
//   {"type":"coreBeacon","protocolVersion":2,"coreId":"galdrun-core-nas-1a2b3c4d",
//    "coreVersion":"0.1.0","controlPort":4713}
//
// Its own port rather than kDiscoveryPort, because the core listens there for
// the clients' beacons and a high-end server on the same machine could not
// bind it too. The reader is the high-end server (C#,
// `Multiroom.Server.Playback/Core/CoreDiscovery.cs`), which checks the same four
// mandatory fields ClientBeacon.h names: `type`, `protocolVersion`, `coreId`
// and `controlPort`.
//
// The core's address is the datagram's source address, as with the client.

#include <cstdint>
#include <string>

namespace multiroom {
namespace matp {

const uint16_t kCoreBeaconPort = 4722;

struct CoreBeacon
{
    std::string coreId;
    std::string coreVersion;
    // Where the core's control API (the high-end server's line-based JSON
    // API) listens, on the datagram's address.
    uint16_t controlPort = 0;
};

// Always stamps this build's kProtocolVersion.
std::string makeCoreBeacon(const CoreBeacon& beacon);

} // namespace matp
} // namespace multiroom
