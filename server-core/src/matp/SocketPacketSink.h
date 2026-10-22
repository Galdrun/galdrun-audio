#pragma once

// An audio connection, as the thing a ClientOutput writes into.
//
// It lives beside AudioServer.cpp rather than under include/ for the reason
// src/net/Socket.h does: nothing above the listeners may pull <winsock2.h>
// into its translation unit. This is the only implementation of IPacketSink
// that is a socket, and the audio listener is the only thing that builds one.
//
// It owns the descriptor from the moment the listener hands it over. That is
// what makes the handover a single move: the listener stops watching the
// socket, the room's thread starts writing to it, and there is never a moment
// where both would have to agree about who closes it.

#include "client/IPacketSink.h"
#include "net/Socket.h"

#include <cstddef>
#include <span>

namespace multiroom {

class SocketPacketSink final : public IPacketSink
{
public:
    explicit SocketPacketSink(net::Socket socket) noexcept;
    ~SocketPacketSink() override;

    [[nodiscard]] Result<size_t, PacketSinkError> write(std::span<const std::byte> bytes) override;
    void close() override;

private:
    net::Socket socket_ = net::kInvalidSocket;
};

} // namespace multiroom
