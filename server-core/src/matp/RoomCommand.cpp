#include "matp/RoomCommand.h"

namespace multiroom {

const char* roomCommandKindName(RoomCommandKind kind)
{
    switch (kind)
    {
    case RoomCommandKind::Start:
        return "startAt";
    case RoomCommandKind::Pause:
        return "pauseAt";
    case RoomCommandKind::Stop:
        return "stopAt";
    case RoomCommandKind::Volume:
        return "setVolume";
    case RoomCommandKind::TrackInfo:
        return "trackInfo";
    case RoomCommandKind::TrackBoundary:
        return "trackBoundary";
    case RoomCommandKind::Flush:
        return "flush";
    case RoomCommandKind::Renegotiate:
        return "sessionConfig";
    case RoomCommandKind::WakeStream:
        return "prepareStream";
    case RoomCommandKind::SetAudioDevice:
        return "setAudioDevice";
    case RoomCommandKind::RescanAudio:
        return "rescanAudio";
    case RoomCommandKind::SetInputBindings:
        return "setInputBindings";
    case RoomCommandKind::SetBuffer:
        return "setBuffer";
    case RoomCommandKind::SetVolumeControl:
        return "setVolumeControl";
    case RoomCommandKind::SetRoomName:
        return "setRoomName";
    case RoomCommandKind::SetDriftCorrection:
        return "setDriftCorrection";
    case RoomCommandKind::DriftFeedback:
        return "driftFeedback";
    }
    return "unknown";
}

} // namespace multiroom
