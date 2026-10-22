#pragma once

// Bring-up path for the playback chain: plays a tone on the selected device so
// it can be verified before the server delivers audio. With throughRingBuffer
// the tone is produced on a second thread and reaches the audio thread through
// AudioRingBuffer, which is the path the server's audio takes as well.
//
// Playback is driven through PlaybackSupervisor, so a device pulled out mid
// tone is reopened rather than ending the run -- which is what makes the tone
// a usable test for phase 8 and not only for phase 3.

#include "app/ClientCommandHandler.h"
#include "control/CommandQueue.h"
#include "control/ControlProtocol.h"

#include <csignal>
#include <cstdint>

namespace multiroom {

// Returns false if the device never came up, or if it was still down when the
// tone's time was up. stopRequested is the process' termination flag: it is
// passed in rather than read from a global here, because the signal handler
// belongs to the process' entry point and not to the tone.
bool playTestTone(ClientRuntime& runtime, CommandQueue& queue, IControlHandler& handler,
                  uint32_t seconds, bool throughRingBuffer,
                  const volatile std::sig_atomic_t& stopRequested);

} // namespace multiroom
