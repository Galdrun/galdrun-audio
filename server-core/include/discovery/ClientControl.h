#pragma once

// Calls to a client's control API for attachment and firmware settings.
//
// The high-end server never addresses a client; it asks this core, which
// knows the client's address and control port from its beacon
// (`DiscoveredClients`) and is on the client's network.
//
// Blocking, with a deadline: one TCP connection, one request line, one
// response line, closed. The caller is the control API's thread, which waits
// at most `timeoutMs` twice (connect, then the answer).

#include <cstdint>
#include <string>
#include "util/Json.h"

namespace multiroom {

// Sends `{"command":<command>,"params":<params>}` to `address:controlPort`
// and returns the client's response line. False with `error` set when there
// is no answer, not when the client says no: a refusal is a response like any
// other.
bool sendClientControl(const std::string& address, uint16_t controlPort,
                       const std::string& command, const json::Value& params,
                       uint32_t timeoutMs, std::string& response, std::string& error);

// This machine's IPv4 address on the route to `address` -- the one a client
// there can reach this server on. Empty when there is no route.
std::string localAddressToward(const std::string& address);

} // namespace multiroom
