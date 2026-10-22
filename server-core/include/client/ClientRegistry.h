#pragma once

// Which rooms this server currently has a control connection to.
//
// One entry per `ClientId`, because that is the identity everything about a
// room is keyed on: its output stream, its drift model, its resampler state
// and -- from phase 12 -- its clock model. Section 11 requires the id to be
// stable across restarts and reconnects for exactly this reason, and a client
// that reports a new one on every start is a new room here, as the section
// says it should be.
//
// **A session does not enter the registry until HELLO has named it.** A socket
// that has connected and said nothing has no id to be keyed on, so the
// listener holds it and registers it once the session identifies. That is also
// what keeps a peer that never speaks from occupying a room's name.
//
// Reconnection (section 43) is the case this shape exists for: a second
// connection announcing an id that is already here **replaces** the first. The
// old socket is a room that is gone whether or not its TCP connection has
// noticed -- a cut cable leaves a half-open connection that never ends -- and
// the new one is the same room saying so. What survives the replacement is
// nothing yet; phase 16 is where the drift model and the output plan start
// outliving their connection, and this is where they will be kept.

#include "client/ClientSession.h"
#include "core/Error.h"
#include "core/Ids.h"

#include <cstddef>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace multiroom {

enum class RegistryError
{
    Full,       // the client ceiling was reached
    Unnamed,    // the session has not been identified by a HELLO yet
    NotPresent  // no session under that id
};

const char* errorName(RegistryError error);

class ClientRegistry
{
public:
    // Memory is bounded, and a listener is the one place a stranger decides
    // how many rooms this process holds. Sixty-four is far past the ten the
    // threading section sizes for and small enough that the map, the sessions
    // and their queues stay a fraction of a megabyte.
    static constexpr size_t kDefaultMaxClients = 64;

    explicit ClientRegistry(size_t maxClients = kDefaultMaxClients);

    // Takes ownership of an identified session. Replaces any session already
    // under that id and returns the displaced one, so the caller can close the
    // socket that belonged to it -- the registry owns no sockets and must not
    // decide when one is shut.
    [[nodiscard]] Result<std::shared_ptr<ClientSession>, RegistryError>
    add(std::shared_ptr<ClientSession> session);

    // Removes the session under `id` only when it is the one given. A stale
    // connection tidying up after it has already been replaced must not take
    // the live one with it.
    bool remove(const ClientId& id, const ClientSession* session);

    [[nodiscard]] std::shared_ptr<ClientSession> find(const ClientId& id) const;

    [[nodiscard]] size_t size() const noexcept { return sessions_.size(); }
    [[nodiscard]] size_t maxClients() const noexcept { return maxClients_; }
    [[nodiscard]] bool empty() const noexcept { return sessions_.empty(); }

    // Every id currently connected, for the status API and the tests. Sorted,
    // so that a caller reading it gets a stable order out of a hash map.
    [[nodiscard]] std::vector<ClientId> ids() const;

    // Connections refused because the ceiling was reached, which is the number
    // that turns "memory is bounded" into something status can report rather
    // than something a log line mentions once.
    [[nodiscard]] uint64_t refusedCount() const noexcept { return refused_; }

private:
    size_t maxClients_;
    std::unordered_map<ClientId, std::shared_ptr<ClientSession>> sessions_;
    uint64_t refused_ = 0;
};

} // namespace multiroom
