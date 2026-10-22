#pragma once

// One message a zone has decided to send to one room, on its way to the thread
// that owns that room's control connection.
//
// A zone knows which room, which position and which instant; putting the bytes
// on a socket belongs to ControlServer and its one thread, and the two meet at
// a bounded mailbox. Phase 11 carried a START_AT across that boundary and
// nothing else. Phase 14 adds section 37's pause, section 38's stop, section
// 39's volume and the two metadata messages of sections 23 and 44 -- five more
// message types making the same journey, which is what turns "the START_AT
// mailbox" into this.
//
// It is a kind beside five payloads rather than a variant, and the reason is
// PAUSE_AT and STOP_AT: they carry the same two numbers (`matp::TransportAt`)
// and differ only in what a room does when one arrives, so a variant could not
// tell them apart and would need a tag of its own beside it. One tag, written
// once.

#include "core/Ids.h"
#include "matp/Input.h"
#include "matp/Protocol.h"

namespace multiroom {

enum class RoomCommandKind
{
    Start,        // section 20
    Pause,        // section 37
    Stop,         // section 38
    Volume,       // section 39
    TrackInfo,     // sections 17 and 44: what is playing now
    TrackBoundary, // section 23: where the next track begins
    Flush,         // section 54: drop what the room holds from a position on (phase 26)

    // Section 12, from this end: choose the room's format again (phase 17).
    // It carries no payload -- the new format is asked of the room's zone
    // when the session writes it, not decided by whoever queued this.
    Renegotiate,
    WakeStream,

    // Section 56: play on another sound card from now on. Sent by the control
    // API, never by a zone; the client's answer is section 12's re-negotiation.
    SetAudioDevice,

    // Section 56: scan the sound cards again. The control API's, like the one
    // above, and without a payload.
    RescanAudio,

    // Section 57: rebind some gestures of one of the room's inputs. The
    // control API's; the client persists it and reports its inputs again.
    SetInputBindings,

    // Section 58: the room's prebuffer target. The control API's; the client
    // persists it and answers with BUFFER_CONFIG.
    SetBuffer,

    // Section 59: how the room plays its volume. The control API's; the client
    // persists it and answers with VOLUME_CONFIG.
    SetVolumeControl,

    // Section 60: the room's name. The control API's; the client persists it
    // and answers with ROOM_NAME.
    SetRoomName,
    SetDriftCorrection,
    DriftFeedback // the sync controller's measurement and hardware-clock command
};

const char* roomCommandKindName(RoomCommandKind kind);

struct RoomCommand
{
    RoomCommandKind kind = RoomCommandKind::Start;
    ClientId client;

    // Exactly one of these is read, and which one is `kind`. The session id of
    // whichever it is stays empty here: the room's session is the one field of
    // every one of these messages that neither the zone nor the scheduler
    // owns, and it is filled where the session is known.
    matp::StartAt start;          // Start
    matp::TransportAt at;         // Pause, Stop, Flush
    matp::SetVolume volume;       // Volume
    matp::TrackInfo info;         // TrackInfo
    matp::TrackBoundary boundary; // TrackBoundary
    matp::SetAudioDevice audioDevice; // SetAudioDevice
    matp::SetInputBindings inputBindings; // SetInputBindings
    matp::SetBuffer buffer;               // SetBuffer
    matp::SetVolumeControl volumeControl; // SetVolumeControl
    matp::SetRoomName roomName;           // SetRoomName
    matp::SetDriftCorrection driftCorrection; // SetDriftCorrection
    matp::DriftFeedback driftFeedback;        // DriftFeedback
};

} // namespace multiroom
