#include "matp/SocketPacketSink.h"

#include <algorithm>
#include <limits>

namespace multiroom {

SocketPacketSink::SocketPacketSink(net::Socket socket) noexcept : socket_(socket) {}

SocketPacketSink::~SocketPacketSink()
{
    close();
}

Result<size_t, PacketSinkError> SocketPacketSink::write(std::span<const std::byte> bytes)
{
    if (socket_ == net::kInvalidSocket)
        return fail(PacketSinkError::Closed, "the audio connection is closed");
    if (bytes.empty())
        return size_t{0};

    const int length = static_cast<int>(
        std::min<size_t>(bytes.size(), static_cast<size_t>(std::numeric_limits<int>::max())));
    const int sent =
        net::transmit(socket_, reinterpret_cast<const char*>(bytes.data()), length);

    if (sent > 0)
        return static_cast<size_t>(sent);

    if (sent < 0)
    {
        const int code = net::lastError();
        if (net::wouldBlock(code))
        {
            // The peer's receive window is closed, which is what a room
            // holding a full prebuffer looks like from this end. Not a
            // failure, and nothing waits on it.
            return size_t{0};
        }
        const std::string message = net::errorMessage(code);
        close();
        return fail(PacketSinkError::Closed, message);
    }

    return size_t{0};
}

void SocketPacketSink::close()
{
    if (socket_ == net::kInvalidSocket)
        return;
    net::closeSocket(socket_);
    socket_ = net::kInvalidSocket;
}

} // namespace multiroom
