#pragma once

// The value this client builds matp::Identity's platform from. The host name
// the generated clientId is built from is discovery::hostName().
//
// It stayed behind when the wire code moved into shared/matp: the server
// builds no HELLO of its own, and platformName() reads the device cache's
// notion of which platform this is, which is a client concept. What a server
// needs from it -- the string itself -- arrives over the socket.

#include <string>

namespace multiroom {
namespace matp {

// "windows-x86_64", "linux-armv6": the operating system the device cache
// already names, plus the architecture this binary was built for.
std::string platformName();

// The device's own name for the discovery beacon: its host name, or on an
// ESP32 the name its station interface was given.
std::string deviceName();

} // namespace matp
} // namespace multiroom
