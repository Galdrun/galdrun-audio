#pragma once

// Where one room's audio packets go once they are bytes.
//
// It exists for two reasons, and both of them are structural rather than
// speculative:
//
//   * **The socket stays in `matp/`.** `src/net/Socket.h` pulls <winsock2.h>
//     in and nothing above the listener may see it, exactly as nothing above
//     the decoder may see an FFmpeg header. `ClientOutput` is a `client/`
//     component with a thread and a queue, not a socket wrapper, and this is
//     the seam.
//   * **The isolation rule is testable without a port.** "One client can never
//     block another" is proven by a room that stops reading, and a sink that
//     takes nothing is that room exactly -- with no socket, no port and no
//     race against the operating system's own buffer sizes.
//
// **Bytes, not packets, and never blocking.** A non-blocking socket takes what
// its send buffer has room for and no more, so a partial write is the normal
// case and the caller resumes it. Handing whole packets over instead would put
// the leftover in the sink, which is a buffer this server would then have to
// bound in every implementation of this interface -- the same argument that
// keeps unconsumed input on the caller's side of IAudioResampler.

#include "core/Error.h"

#include <cstddef>
#include <span>

namespace multiroom {

enum class PacketSinkError
{
    // The peer is gone: it closed, reset, or the write failed in a way that
    // does not come back. The room is over and rejoins on a new connection.
    Closed
};

const char* errorName(PacketSinkError error);

class IPacketSink
{
public:
    virtual ~IPacketSink() = default;

    // Takes what it can and says how much. **Zero is not a failure**: it is a
    // peer whose receive window is closed, which is what a client holding a
    // full prebuffer looks like from this end, and the caller re-presents the
    // rest rather than waiting on it.
    [[nodiscard]] virtual Result<size_t, PacketSinkError> write(std::span<const std::byte> bytes) = 0;

    // Ends the connection. Idempotent, because both the output that failed and
    // the owner that stopped it will call it.
    virtual void close() = 0;

protected:
    IPacketSink() = default;
    IPacketSink(const IPacketSink&) = delete;
    IPacketSink& operator=(const IPacketSink&) = delete;
};

} // namespace multiroom
