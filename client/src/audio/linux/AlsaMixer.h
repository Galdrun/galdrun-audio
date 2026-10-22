#pragma once

#include "audio/HardwareClock.h"
#include "audio/HardwareVolume.h"

#include <memory>
#include <string>

namespace multiroom {

// ALSA's half of audio/HardwareVolume.h: the card's simple mixer, through the
// same alsa-lib the backend already needs. Internal to the audio layer.

// Fills `out` from the mixer of card `cardId` (the "x" of "hw:CARD=x"). False,
// with `out` untouched, when the card has no playback volume element this can
// be sure of: a card with several and none of the usual names falls back to
// the software path rather than guessing which one is the output.
bool detectAlsaHardwareVolume(const std::string& cardId, HardwareVolumeInfo& out);

// Opens the element the scan found for `device`, a "hw:CARD=x,DEV=n" entry.
// Null, with the reason logged, when the mixer or the element is gone.
std::unique_ptr<IHardwareVolume> openAlsaHardwareVolume(const AudioDeviceInfo& device);

// The clock trim the board file names ("hardwareClockControl" in
// /etc/<brand>-board.json), on card `cardId`: an integer control of one
// channel whose range includes 0. False, with `out` untouched, on a board that
// names none -- which is every board not verified to retune cleanly.
bool detectAlsaHardwareClock(const std::string& cardId, HardwareClockInfo& out);

// Opens it for `device`, retaining the control handle for every write.
std::unique_ptr<IHardwareClock> openAlsaHardwareClock(const AudioDeviceInfo& device);

} // namespace multiroom
