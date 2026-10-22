#pragma once

#include <cstdint>

// The captive portal's DNS half: while the setup access point is up, every
// name resolves to the board itself, so a phone that joins it runs its
// connectivity check against the web UI and opens the setup page on its own.
namespace captive_dns {

// `address` in network byte order. Runs its own task until stop().
bool start(uint32_t address);
void stop();

} // namespace captive_dns
