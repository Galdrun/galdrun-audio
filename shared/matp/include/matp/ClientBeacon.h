#pragma once

// The discovery beacon: how a server finds a client it was not told about.
//
// Not a MATP message -- the protocol leaves discovery to a later version
// (section 52) -- but the same arrangement as the wire format beside it: the
// client sends it and the server reads it, so both compile this one file and
// cannot disagree about a field name. One UDP datagram, a compact JSON object,
// sent every `kBeaconIntervalNs` to the broadcast address of every interface
// the client has up:
//
//   {"type":"clientBeacon","protocolVersion":2,"clientId":"3f9c2a71e04b8d15",
//    "name":"Kitchen","clientVersion":"0.1.0","platform":"windows-x86_64",
//    "host":"KITCHEN-PC","controlPort":4720,"server":"10.0.0.7:4711"}
//
// `server` is the MATP address the client is pointed at, empty when it has
// none, so a server can tell a client nobody has attached from one attached
// to another core. A beacon from before that field leaves it unknown.
//
// The client's address is not in the body: it is the datagram's source
// address, and the client cannot know which of its addresses a server sees.

#include <cstdint>
#include <optional>
#include <string>

namespace multiroom {
namespace matp {

const uint16_t kDiscoveryPort = 4721;
const uint64_t kBeaconIntervalNs = 2000000000ULL;

struct ClientBeacon
{
    uint32_t protocolVersion = 0;
    std::string clientId;
    // The room's name (MATP section 60), empty when the beacon did not say
    // (an older client).
    std::string name;
    std::string clientVersion;
    std::string platform;
    // The device's own name: its host name (an ESP32's set device name),
    // empty when the beacon did not say (an older client).
    std::string host;
    // Where the client's control API listens, on the datagram's address.
    uint16_t controlPort = 0;
    // The client's configured server, "" for none; nullopt when the beacon
    // did not say (an older client).
    std::optional<std::string> server;
};

// Always stamps this build's kProtocolVersion, whatever the struct holds.
std::string makeClientBeacon(const ClientBeacon& beacon);

// `type`, `protocolVersion`, `clientId` and `controlPort` are mandatory -- a
// beacon without them names nothing a server could attach -- and the other
// others are informational, as in HELLO. Unknown keys are ignored: a newer
// client's beacon is still a client.
bool parseClientBeacon(const std::string& text, ClientBeacon& out, std::string& error);

} // namespace matp
} // namespace multiroom
