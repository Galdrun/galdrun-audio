#include "audio/IAudioBackend.h"

namespace multiroom {

const char* backendStateName(BackendState state)
{
    switch (state)
    {
    case BackendState::Closed:
        return "closed";
    case BackendState::Open:
        return "open";
    case BackendState::Running:
        return "running";
    case BackendState::Failed:
        return "failed";
    }
    return "unknown";
}

} // namespace multiroom
