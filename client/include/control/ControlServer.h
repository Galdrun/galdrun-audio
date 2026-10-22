#pragma once

// Local TCP control interface.
//
// One thread for the whole server: it accepts, reads, dispatches and writes
// for every connection through a single select() loop. A thread per connection
// would buy nothing here -- responses are serialized through CommandQueue onto
// the owner thread anyway -- and would cost a stack per curious netcat.
//
// Bound to every interface by default (controlBindAddress), because a server
// that found this client by its beacon attaches it through this API. There is
// no authentication, so anything on the network can drive it.
//
// The audio thread is never involved: the handler runs on the thread that
// drains the command queue.

#include "control/ControlProtocol.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace multiroom {

class ControlServer
{
public:
    ControlServer();
    ~ControlServer();

    ControlServer(const ControlServer&) = delete;
    ControlServer& operator=(const ControlServer&) = delete;

    // Binds and listens on the calling thread, so a taken port or a bad
    // address is reported here rather than disappearing into a worker. The
    // handler must outlive the server.
    bool start(const std::string& bindAddress, uint16_t port, IControlHandler& handler,
               std::string& error);

    // Idempotent; also called by the destructor.
    void stop();

    bool isRunning() const;

    // The port actually bound. Differs from the requested one when port 0 was
    // asked for, which is how the tests avoid a fixed port.
    uint16_t boundPort() const;

    // Connections accepted since start(), for diagnostics and tests.
    uint64_t acceptedConnections() const;

    // Owner thread publishes physical button levels; the existing socket loop
    // streams them to browsers. No audio thread or additional worker involved.
    void setInputButtons(const std::vector<std::string>& ids);
    void publishInputState(const std::string& id, bool pressed);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace multiroom
