#pragma once

// The MATP control listener: the one place in this server where a socket
// becomes a room.
//
// It accepts connections, gives each one a `ClientSession`, feeds that session
// the bytes that arrive and writes back what it produced. It decides nothing
// about the protocol -- every rule of sections 10 to 12 lives in the session,
// which is why the session can be tested without a port and this class has
// only the socket handling left to get wrong.
//
// **One thread, non-blocking, select().** Ten rooms at one control message
// every five seconds is not a load; what the thread is for is that `accept()`
// and `recv()` block and the process has other work. The per-client *output*
// threads of the roadmap's threading section are phase 9's and are a different
// argument entirely -- there the isolation matters, because an audio stream
// can be back-pressured by a room that stopped reading.
//
// Nothing here blocks on a client:
//
//   * every socket is non-blocking, so a room that stopped reading fills its
//     own bounded queue in its own session and is failed when that queue is
//     full -- the session's `OutboundFull`, which is one room ending and no
//     other room noticing;
//   * a partial write is kept and resumed rather than waited on;
//   * the accept path is bounded by `maxConnections`, so a peer that opens
//     sockets without speaking cannot cost this process more than that.
//
// A connection that has not sent HELLO yet is held here and is **not** in the
// registry: it has no id to be keyed on. The silence timeout is what ends one
// that never speaks, and it is the session's, not this class's.

#include "client/ClientRegistry.h"
#include "client/ClientSession.h"
#include "core/Error.h"
#include "matp/RoomCommand.h"
#include "util/Clock.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace multiroom {

enum class ControlServerError
{
    SocketLibrary, // Winsock would not start
    BindFailed,    // the port is taken, or not ours to take
    Unsupported    // more connections than select() can watch on this platform
};

const char* errorName(ControlServerError error);

struct ControlServerSettings
{
    // MATP section 3's example pair is 4711 for control and 4712 for audio,
    // and phase 9 keeps it: the client's fallback for an unannounced audio
    // port is "control plus one", so the default pair happens to make that
    // fallback right.
    //
    // Zero means "whichever port the system has free", which is what the tests
    // use -- a test on a fixed port fails whenever something else on the
    // machine holds it, and the roadmap forbids one. `port()` reads back what
    // was actually bound.
    uint16_t port = 4711;

    int backlog = 16;

    // Sockets this listener watches at once, including those that have not
    // identified themselves yet. Kept well under the 64 that select() watches
    // on Windows by default (FD_SETSIZE), which is the real ceiling here and
    // the reason this is not simply the registry's client limit.
    size_t maxConnections = 32;

    ClientSessionSettings session;
};

class ControlServer
{
public:
    // Binds, listens, and starts the thread. A failure to bind is returned
    // rather than retried: a port that is taken at startup is a configuration
    // problem or a second copy of this server, and both want an operator.
    [[nodiscard]] static Result<std::unique_ptr<ControlServer>, ControlServerError>
    start(const IClock& clock, ControlServerSettings settings = {});

    // Stops the thread and closes every connection.
    ~ControlServer();

    ControlServer(const ControlServer&) = delete;
    ControlServer& operator=(const ControlServer&) = delete;

    // The port actually bound, which is the one that matters when `port` was
    // zero.
    [[nodiscard]] uint16_t port() const noexcept { return port_; }

    // One message for one room, from a thread that is not this one: section
    // 20's start, sections 37 and 38's pause and stop, section 39's volume and
    // the two metadata messages of sections 23 and 44.
    //
    // A zone decides what to say to which room, on its own pipeline thread,
    // and the sessions belong to *this* thread -- so the message is queued
    // here and written by the listener on its next pass, which is the one
    // place the two threads meet. False is a room this listener does not hold,
    // or a mailbox that has reached its limit: for a start, both leave the
    // room unstarted and the scheduler offers it another when the first goes
    // unanswered.
    //
    // Bounded, like every other queue: a zone that produced messages faster
    // than this thread drained them would otherwise cost the process memory.
    [[nodiscard]] bool send(const RoomCommand& command);

    // Every counter below is read from other threads -- the tests', and from
    // phase 17 the control API's -- so each is an atomic the listener thread
    // writes and nobody else does. The sessions themselves belong to that
    // thread and stay there: handing one out would be handing out a state
    // machine another thread is inside, and no amount of locking here would
    // make reading it mean anything.
    [[nodiscard]] uint64_t acceptedCount() const noexcept;
    [[nodiscard]] uint64_t refusedCount() const noexcept;
    [[nodiscard]] uint64_t closedCount() const noexcept;
    [[nodiscard]] uint64_t identifiedCount() const noexcept;
    [[nodiscard]] size_t connectionCount() const noexcept;

    // Rooms with a name: connections that have completed HELLO and are in the
    // registry. A socket that has connected and said nothing is counted by
    // connectionCount() and not by this.
    [[nodiscard]] size_t clientCount() const noexcept;

    // Messages written to a session by this thread, and the START_ATs among
    // them. What send() queued and this thread could still deliver -- a room
    // that disconnected between the two is counted by neither. The starts are
    // counted separately because "did this room ever get told to play?" is the
    // question phase 11's milestone asks and a volume change should not move
    // the answer.
    [[nodiscard]] uint64_t commandsSentCount() const noexcept;
    [[nodiscard]] uint64_t startsSentCount() const noexcept;

private:
    class Impl;

    ControlServer(std::unique_ptr<Impl> impl, uint16_t port);

    std::unique_ptr<Impl> impl_;
    uint16_t port_ = 0;
};

} // namespace multiroom
