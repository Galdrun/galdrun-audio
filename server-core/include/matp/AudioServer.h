#pragma once

// The MATP audio listener: the second TCP connection of section 3, and the one
// place a bare socket becomes a room's audio stream.
//
// It does exactly one thing with a connection and then gives it away. A client
// opens the audio port and sends `STREAM_ATTACH` -- the one message the
// protocol does not name, and the one the shared library documents as the
// binding section 13 assumes and never provides. This listener reads it, and
// hands the socket to whoever owns that session as an `IPacketSink`. From that
// moment the connection belongs to that room's own thread and this listener
// has forgotten it: there is no moment at which two threads would have to
// agree about who closes it, and a room that stops draining costs this thread
// nothing at all.
//
// **What it never does is write audio.** The packets are the room's, produced
// on the room's thread, and a listener that carried them would be the single
// loop the specification warns against -- one slow client serializing with
// every other one.
//
// An attach that comes before its room is a member of a zone waits here,
// open, until it is (phase 35): the probe in the settings says which.
//
// A connection that never attaches is closed on a timeout. A fresh TCP
// connection to this port carries no identity, so until `STREAM_ATTACH`
// arrives there is nothing to attribute it to and nothing to tell it: it is
// the one thing on this port that a stranger can cost this process, and the
// timeout plus `maxConnections` is what bounds it.

#include "client/IPacketSink.h"
#include "core/Error.h"
#include "core/Ids.h"
#include "util/Clock.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace multiroom {

enum class AudioServerError
{
    SocketLibrary, // Winsock would not start
    BindFailed,    // the port is taken, or not ours to take
    Unsupported,   // more connections than select() can watch on this platform
    NoHandler      // started without somewhere for an attached connection to go
};

const char* errorName(AudioServerError error);

// Which room a connection said it belongs to. Both halves are required by the
// parser: the session id is what the connection is bound to, and the client id
// is what lets a server that has already torn that session down still say
// which room came back.
struct StreamAttachment
{
    SessionId session;
    ClientId client;
};

// Where an attached connection goes. Called on the listener's thread, once per
// connection; returning false closes it, which is the answer for a session
// this server does not have.
//
// The sink is given up in the call, because that *is* the handover: after it
// the descriptor belongs to whatever took it.
using StreamAttachHandler =
    std::function<bool(const StreamAttachment&, std::unique_ptr<IPacketSink>)>;

// Whether an attachment can be handed over (phase 35). A client attaches as
// soon as it holds a prepared stream, which may be before a zone has made its
// room a member -- or while the room is in no zone at all. That is "not yet",
// not "never": the connection is kept and asked about again.
enum class AttachReadiness
{
    Now,   // hand it to onAttach
    Later, // the session is live, its room not a member yet: keep it open
    Never  // no such session: close it
};

using StreamAttachProbe = std::function<AttachReadiness(const StreamAttachment&)>;

struct AudioServerSettings
{
    // Section 3's example pair is 4711 control / 4712 audio. The client's
    // fallback for an unannounced audio port is "control plus one", so the
    // default pair makes that fallback right -- and PREPARE_STREAM announces
    // the port anyway, because relying on a fallback to be right is how a
    // non-default port becomes a silent failure.
    //
    // Zero binds whichever port the system has free, which is what the tests
    // use; port() reads back what was bound.
    uint16_t port = 4712;

    int backlog = 16;

    // Sockets this listener watches at once, all of them unattached by
    // definition: an attached one has left. Kept under the 64 that select()
    // watches on Windows by default.
    size_t maxConnections = 32;

    // How long a connection may say nothing. A client opens this socket and
    // sends STREAM_ATTACH immediately, so this is generous rather than tight;
    // what it bounds is a peer that connects and never speaks.
    uint64_t attachTimeoutNs = 5ULL * 1000000000ULL;

    StreamAttachHandler onAttach;

    // Asked before every handover, on the listener's thread. A connection
    // answered Later has no attach deadline: it waits, still read (a close or
    // a second STREAM_ATTACH is seen), and is asked again on every pass of
    // the loop. Unset: every attachment is handed over at once.
    StreamAttachProbe readiness;
};

class AudioServer
{
public:
    [[nodiscard]] static Result<std::unique_ptr<AudioServer>, AudioServerError>
    start(const IClock& clock, AudioServerSettings settings);

    // Stops the thread and closes every connection that has not been handed
    // over. The ones that have belong to their rooms and are not touched.
    ~AudioServer();

    AudioServer(const AudioServer&) = delete;
    AudioServer& operator=(const AudioServer&) = delete;

    [[nodiscard]] uint16_t port() const noexcept { return port_; }

    // Read from other threads -- the tests', and from phase 17 the control
    // API's -- so each is an atomic the listener thread writes.
    [[nodiscard]] uint64_t acceptedCount() const noexcept;
    [[nodiscard]] uint64_t refusedCount() const noexcept;  // the ceiling, at the door
    [[nodiscard]] uint64_t attachedCount() const noexcept; // handed to a room
    [[nodiscard]] uint64_t rejectedCount() const noexcept; // no such room
    [[nodiscard]] uint64_t failedCount() const noexcept;   // malformed, or silent too long
    [[nodiscard]] size_t connectionCount() const noexcept; // accepted, not yet attached
    [[nodiscard]] size_t waitingCount() const noexcept;    // of those, attached and waiting

private:
    class Impl;

    AudioServer(std::unique_ptr<Impl> impl, uint16_t port);

    std::unique_ptr<Impl> impl_;
    uint16_t port_ = 0;
};

} // namespace multiroom
