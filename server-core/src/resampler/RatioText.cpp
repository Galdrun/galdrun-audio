#include "resampler/RatioText.h"

#include <cstdio>

namespace multiroom::resampler {

std::string ratioText(double ratio)
{
    char buffer[32];
    const int written = std::snprintf(buffer, sizeof(buffer), "%.6f", ratio);
    return written > 0 ? std::string(buffer, static_cast<size_t>(written)) : std::string("?");
}

} // namespace multiroom::resampler
