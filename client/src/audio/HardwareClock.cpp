#include "audio/HardwareClock.h"

#include <cmath>

namespace multiroom {
namespace hardware_clock {

Setting settingFor(const HardwareClockInfo& info, double targetPpm, int32_t currentPpm)
{
    Setting setting;
    setting.ppm = currentPpm;
    if (!std::isfinite(targetPpm))
        return setting;

    double target = targetPpm;
    if (target < info.minPpm)
    {
        target = info.minPpm;
        setting.saturated = true;
    }
    else if (target > info.maxPpm)
    {
        target = info.maxPpm;
        setting.saturated = true;
    }

    const double step = info.stepPpm > 0 ? static_cast<double>(info.stepPpm) : 1.0;
    if (std::fabs(target - currentPpm) < 0.75 * step)
        return setting;

    double ppm = std::round(target / step) * step;
    if (ppm < info.minPpm)
        ppm += step;
    if (ppm > info.maxPpm)
        ppm -= step;
    setting.ppm = static_cast<int32_t>(ppm);
    return setting;
}

} // namespace hardware_clock
} // namespace multiroom
