#include "audio/AudioClockSnapshot.h"

#include <cstddef>

namespace multiroom {

const char* audioClockQualityName(AudioClockQuality quality)
{
    switch (quality)
    {
    case AudioClockQuality::Unavailable:
        return "unavailable";
    case AudioClockQuality::Estimated:
        return "estimated";
    case AudioClockQuality::Driver:
        return "driver";
    case AudioClockQuality::Hardware:
        return "hardware";
    }
    return "unknown";
}

bool parseAudioClockQuality(const std::string& text, AudioClockQuality& out)
{
    const AudioClockQuality known[] = {AudioClockQuality::Unavailable,
                                       AudioClockQuality::Estimated,
                                       AudioClockQuality::Driver,
                                       AudioClockQuality::Hardware};
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); ++i)
    {
        if (text == audioClockQualityName(known[i]))
        {
            out = known[i];
            return true;
        }
    }
    return false;
}

} // namespace multiroom
