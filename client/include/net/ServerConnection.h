#pragma once

// The client's TCP connection to the audio server.
//
// This is the transport only: it connects, reads, writes and reconnects with a
// backoff, on one thread of its own. It does not know what MATP is -- framing
// and the handshake sit on top of it in matp::Session -- so it hands received
// bytes to its owner exactly as they arrive.
//
// Nothing here is ever touched from the audio thread. Audio reaches the render
// callback through AudioRingBuffer, not through this class.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace multiroom {

enum class ServerConnectionState
{
    NotConfigured, // client.json has no server address
    Disconnected,  // configured, and waiting out the backoff before trying again
    Connecting,
    Connected
};

const char* serverConnectionStateName(ServerConnectionState state);

class ServerConnection
{
public:
    // Called on the connection's own thread as bytes arrive. TCP is a byte
    // stream, so one call is not one message: the handler has to buffer
    // (MATP section 16).
    using DataHandler = std::function<void(const void* data, size_t size)>;

    ServerConnection();
    ~ServerConnection();

    ServerConnection(const ServerConnection&) = delete;
    ServerConnection& operator=(const ServerConnection&) = delete;

    // Starts the connection thread. An empty handler is allowed: incoming
    // bytes are then discarded, which is what a client without a protocol
    // layer on top of this one wants.
    bool start(DataHandler onData, std::string& error);

    // Idempotent; also called by the destructor.
    void stop();

    // "host:port", or an empty string to disconnect and stay idle. An address
    // that does not parse is refused without disturbing the current
    // connection. A new address takes effect immediately: the open connection
    // is dropped and the backoff starts over.
    bool setServer(const std::string& address, std::string& error);
    std::string server() const;

    ServerConnectionState state() const;

    // Queued and, whenever the socket takes it, written from the calling
    // thread rather than handed to the connection thread. Clock sync measures
    // round trips, so a message must not sit in a queue until the next poll.
    // Returns false when there is no connection or the peer has stopped
    // reading.
    bool send(const void* data, size_t size);

    // Drops the current connection, if there is one, and lets the reconnect
    // path start over. The reason is what the disconnect is logged and
    // reported as. Meant for the layer above: a protocol violation leaves the
    // byte stream out of step, and only a new connection resynchronizes it.
    void dropConnection(const std::string& reason);

    // Attempts made and connections established, for diagnostics and tests.
    uint64_t connectAttempts() const;
    uint64_t connectionCount() const;

    // Reads that returned data and the bytes they returned, since start: the
    // average read size is what the receive path costs per byte hinges on.
    uint64_t receiveCalls() const;
    uint64_t receivedBytes() const;
    // Wall time inside those reads and inside the data handler after them.
    uint64_t receiveNs() const;
    uint64_t handlerNs() const;

    // Why the last attempt failed; empty after a successful connect.
    std::string lastError() const;

    // The kind of interface the open connection leaves by: "ethernet" or
    // "wifi", empty when unknown or not connected.
    std::string networkLink() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace multiroom
