#include "audio/HardwareVolume.h"

#include <cmath>

namespace multiroom {

VolumePath resolveVolumePath(matp::AudioControl audioControl,
                             matp::VolumeControlMode volumeControl, bool hardwareAvailable)
{
    using matp::VolumeControlMode;
    const bool strict = audioControl == matp::AudioControl::BitPerfect;

    VolumePath path;
    switch (volumeControl)
    {
    case VolumeControlMode::Hardware:
        if (hardwareAvailable)
            path.effective = VolumeControlMode::Hardware;
        else
            path.reason = "the device has no hardware volume control";
        break;
    case VolumeControlMode::Software:
        if (!strict)
            path.effective = VolumeControlMode::Software;
        else
            path.reason = "software volume is not allowed while Audio Control is bitPerfect";
        break;
    case VolumeControlMode::Disabled:
        break;
    case VolumeControlMode::Auto:
        if (hardwareAvailable)
            path.effective = VolumeControlMode::Hardware;
        else if (!strict)
            path.effective = VolumeControlMode::Software;
        else
            path.reason = "the device has no hardware volume control, and bitPerfect allows "
                          "no software volume";
        break;
    }
    return path;
}

namespace hardware_volume {
namespace {

// 0 dB where the control reaches it: a room at gain 1.0 is not amplified, the
// same as in the software path, which has no headroom to amplify into.
double topDecibels(const HardwareVolumeInfo& info)
{
    return info.maxDb < 0.0 ? info.maxDb : 0.0;
}

} // namespace

double clampGain(double gain)
{
    // NaN fails both comparisons and ends up at 0, the quiet end.
    if (!(gain > 0.0))
        return 0.0;
    return gain < 1.0 ? gain : 1.0;
}

double decibelsForGain(const HardwareVolumeInfo& info, double gain)
{
    gain = clampGain(gain);
    if (gain == 0.0)
        return info.minDb;
    const double db = topDecibels(info) + 20.0 * std::log10(gain);
    return db > info.minDb ? db : info.minDb;
}

double gainForDecibels(const HardwareVolumeInfo& info, double db)
{
    // ALSA reports a muted minimum as a huge negative number; pow() takes it
    // to 0, which is what it is.
    return clampGain(std::pow(10.0, (db - topDecibels(info)) / 20.0));
}

int64_t stepForGain(const HardwareVolumeInfo& info, double gain)
{
    const double span = static_cast<double>(info.maxStep - info.minStep);
    return info.minStep + static_cast<int64_t>(std::llround(clampGain(gain) * span));
}

double gainForStep(const HardwareVolumeInfo& info, int64_t step)
{
    if (info.maxStep <= info.minStep)
        return 1.0;
    return clampGain(static_cast<double>(step - info.minStep) /
                     static_cast<double>(info.maxStep - info.minStep));
}

} // namespace hardware_volume
} // namespace multiroom
