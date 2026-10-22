#include "client/IPacketSink.h"

namespace multiroom {

const char* errorName(PacketSinkError error)
{
    switch (error)
    {
    case PacketSinkError::Closed:
        return "Closed";
    }
    return "Unknown";
}

} // namespace multiroom
