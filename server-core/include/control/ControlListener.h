#pragma once

// The control API's listener: lines in, lines out, and nothing else (phase 17).
//
// One thread and one select() loop for every connection, like the client's
// own control listener, which this is the server-side copy of. A thread per
// connection would buy nothing: the caller is one high-end server, and every
// request is answered in order on this thread anyway.
//
// It is not MATP's listener and shares nothing with it but the socket shim:
// another port, another thread, another parser. A request that takes time --
// `zone.play` opens its source synchronously, `zone.stop` joins threads --
// holds up the next request and never a room, because no room is served here.
//
// **Bound to loopback by default** (ControlConfig). There is no
// authentication, so the address is the access control.
//
// Bounded like everything else: a handful of connections, a longest request
// line, and a ceiling on responses a caller has not read. A line past its
// ceiling is answered once and dropped up to its newline; a caller that does
// not read its answers is dropped outright.
//
// **Events (phase 24)** go out on the same connection, to the connections the
// handler marked as subscribed, and never inside another line: a response is
// always written whole, and an event line only once the connection has
// written everything it owed before it. Each subscribed connection has a queue
// of `kMaxQueuedEvents` lines that `publish()` fills from any thread and this
// thread empties; a caller that stops reading loses the oldest of them rather
// than the server's memory, and the next line it does get is
// `{"event":"events.lost","count":N}` -- the signal to read `status`.

#include "core/Error.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace multiroom {

enum class ControlListenerError
{
    SocketLibrary, // Winsock would not start
    BindFailed     // the address does not parse, or the port is taken
};

const char* errorName(ControlListenerError error);

struct ControlListenerSettings
{
    std::string bindAddress = "127.0.0.1";

    // Zero binds whichever port is free, which is what the tests use.
    uint16_t port = 4713;
};

class ControlListener
{
public:
    // Event lines one subscribed connection may have waiting, starting value.
    // A few seconds of a busy house; each line is a few hundred bytes.
    static constexpr size_t kMaxQueuedEvents = 256;

    // One request line in -- without its newline -- and one whole response,
    // newline included, out. `subscribed` is the connection's own flag, false
    // on a new connection: the handler sets it to have this connection sent
    // events from then on, and clears it to stop. Called on this listener's
    // thread only.
    using Handler = std::function<std::string(const std::string& line, bool& subscribed)>;

    // Binds and listens on the calling thread, so a taken port or a bad
    // address is reported to whoever is starting the server.
    [[nodiscard]] static Result<std::unique_ptr<ControlListener>, ControlListenerError>
    start(ControlListenerSettings settings, Handler handler);

    // Stops the thread and closes every connection.
    ~ControlListener();

    ControlListener(const ControlListener&) = delete;
    ControlListener& operator=(const ControlListener&) = delete;

    [[nodiscard]] uint16_t port() const noexcept { return port_; }
    [[nodiscard]] uint64_t acceptedCount() const noexcept;

    // One whole event line, newline included, for every subscribed
    // connection. From any thread; it takes one short lock, never waits on a
    // socket, and drops the line when nobody is subscribed.
    void publish(const std::string& line);

private:
    class Impl;

    ControlListener(std::unique_ptr<Impl> impl, uint16_t port);

    std::unique_ptr<Impl> impl_;
    uint16_t port_ = 0;
};

} // namespace multiroom
