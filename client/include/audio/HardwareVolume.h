#pragma once

// The room's volume set on the device instead of on the samples.
//
// SET_VOLUME (MATP section 39) is applied by VolumeControl in the render path
// unless the device has a volume control of its own. Where it has one, the
// gain goes there, VolumeControl stays at unity -- which leaves the sample
// loop untouched -- and the PCM the device receives is what the server sent,
// byte for byte (section 2).
//
// The capability is found by the platform backend's capability scan and kept
// in AudioDeviceInfo::hardwareVolume, so it is cached and reported with the
// formats. The control itself is opened per device by
// IAudioBackend::openHardwareVolume(), which by default opens none: a backend
// that has no implementation yet (WASAPI, I2S) leaves every device on the
// software path. A platform adds hardware volume by filling
// AudioDeviceInfo::hardwareVolume in its scan and overriding that one method.
//
// Everything here runs on the control thread. A hardware change is a single
// step rather than VolumeControl's ramp: a mixer write is one register, and
// whatever smoothing it gets is the driver's.

#include "audio/AudioDeviceInfo.h"
#include "matp/Protocol.h"

#include <memory>
#include <string>

namespace multiroom {

// What the control is set to now, read back from the device: someone else
// (alsamixer, a hardware knob) may have moved it since the room last set it.
struct HardwareVolumeState
{
    double gain = 1.0; // the linear gain the setting amounts to, [0, 1]
    bool muted = false;
    bool hasDecibels = false;
    double db = 0.0;
};

class IHardwareVolume
{
public:
    virtual ~IHardwareVolume() = default;

    // `gain` is linear, as in SET_VOLUME, and is clamped to [0, 1]: 1.0 is
    // 0 dB (or the control's top, when that is below 0 dB) and never above,
    // so a hardware room and a software room at the same gain sound the same.
    virtual bool set(double gain, bool muted, std::string& error) = 0;
    virtual bool read(HardwareVolumeState& out, std::string& error) = 0;

    // The mute alone, leaving the level where it is: Volume Control Disabled
    // still honours a mute. Only for a control with a mute of its own
    // (HardwareVolumeInfo::hasMute).
    virtual bool setMute(bool muted, std::string& error) = 0;
};

// How the room plays SET_VOLUME (MATP section 59): the path resolved from the
// room's Audio Control, the Volume Control the client keeps, and whether the
// device has a hardware control -- never Auto. `reason` says why the path is
// not the one that was asked for; empty where it is.
//
// The rule that decides everything else: under AudioControl::BitPerfect the
// samples are never scaled, so Software is never the answer there -- asked
// for outright or reached by Auto, it is Disabled, with the reason.
struct VolumePath
{
    matp::VolumeControlMode effective = matp::VolumeControlMode::Disabled;
    std::string reason;

    bool operator==(const VolumePath& other) const
    {
        return effective == other.effective && reason == other.reason;
    }
    bool operator!=(const VolumePath& other) const { return !(*this == other); }
};

VolumePath resolveVolumePath(matp::AudioControl audioControl,
                             matp::VolumeControlMode volumeControl, bool hardwareAvailable);

namespace hardware_volume {

// The mapping every implementation shares, so the same gain means the same
// level whatever the control. Platform-independent and tested on its own.

// NaN and anything below 0 are 0; anything above 1 is 1.
double clampGain(double gain);

// The decibel setting for `gain`: 20*log10(gain) below the top, where the top
// is 0 dB or maxDb, whichever is lower, and floored at minDb.
double decibelsForGain(const HardwareVolumeInfo& info, double gain);
double gainForDecibels(const HardwareVolumeInfo& info, double db);

// For a control that reports no decibels: linear across the raw steps.
int64_t stepForGain(const HardwareVolumeInfo& info, double gain);
double gainForStep(const HardwareVolumeInfo& info, int64_t step);

} // namespace hardware_volume
} // namespace multiroom
