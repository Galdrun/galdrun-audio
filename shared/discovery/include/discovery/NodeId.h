#pragma once

// The persistent id a client or a server core announces itself with. Both
// generate one on their first start and write it to their config, so what a
// high-end server stored for a node still names it after a restart or a new
// DHCP lease.

#include <string>

namespace multiroom {
namespace discovery {

// 1 to 64 bytes, no control characters, no blank at either end. UTF-8 is
// allowed: the id is also the name a server shows.
bool isValidNodeId(const std::string& id);

// "<prefix>-<host>-<8 hex>", or "<prefix>-<8 hex>" for an empty host name. The
// host name is lowercased, anything but letters, digits and '-' becomes '-',
// and it is cut so the id stays valid. A placeholder meant to be replaced, but
// random so that two boards set up from the same image do not introduce
// themselves as the same node. `prefix` is short and plain ("galdrun",
// "galdrun-core").
std::string generateNodeId(const std::string& prefix, const std::string& hostName);

// 16 random lowercase hex digits: a client's id (client phase 26), which is
// an identity and nothing else -- the room's name is kept apart from it.
std::string generateRandomId();

// The host name, which a generated id is built from. Empty when the system
// will not tell us.
std::string hostName();

} // namespace discovery
} // namespace multiroom
