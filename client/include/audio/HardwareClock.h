#pragma once

// The room's DAC clock trimmed on the server's command.
//
// The server still decides every correction. While a session has negotiated
// the hardware clock, its DRIFT_FEEDBACK names an absolute trim relative to
// the nominal rate and this executes it; the client runs no control loop of
// its own. The samples are untouched, so a room that is otherwise bitperfect
// stays bitperfect.
//
// The capability is found by the platform backend's scan and kept in
// AudioDeviceInfo::hardwareClock, but only where the board file names the
// control: a control of a similar name on other hardware is not evidence of
// the same sign or of a clean retune. IAudioBackend::openHardwareClock()
// opens none by default.
//
// Control thread only, never the render callback: a write is an I2C transfer.

#include "audio/AudioDeviceInfo.h"
#include "matp/Protocol.h"

#include <cstdint>
#include <string>

namespace multiroom {

class IHardwareClock
{
public:
    virtual ~IHardwareClock() = default;

    // Whole ppm relative to nominal, positive slows; within the range and on
    // the step the scan reported. Read back after the write, which proves the
    // driver took the value and nothing about the frequency.
    virtual bool set(int32_t ppm, std::string& error) = 0;
};

// What the room's drift correction is doing, published by StreamPlayback for
// the control API.
struct DriftCorrectionState
{
    // The actuator the last SESSION_CONFIG selected, resampling where none.
    matp::DriftCorrectionMode selected = matp::DriftCorrectionMode::Resampling;

    // The open device's clock follows the server, and what it was last set to.
    bool hardwareClockActive = false;
    int32_t hardwareClockPpm = 0;
    bool hardwareClockSaturated = false;
    std::string hardwareClockError;

    // The server's last DRIFT_FEEDBACK measurement of this room: the residual
    // of the clock as it runs, against the server's. Absent until it has one.
    bool hasServerDrift = false;
    double serverDriftPpm = 0.0;
    double serverDriftConfidence = 0.0;
};

namespace hardware_clock {

struct Setting
{
    int32_t ppm = 0;
    bool saturated = false; // the target was beyond the device's range
};

// The setting for the server's `targetPpm`: on the device's step, clamped to
// its range, and `currentPpm` unchanged while the target stays within three
// quarters of a step of it -- noise around a step boundary would otherwise
// write the two neighbours in turn.
Setting settingFor(const HardwareClockInfo& info, double targetPpm, int32_t currentPpm);

} // namespace hardware_clock
} // namespace multiroom
