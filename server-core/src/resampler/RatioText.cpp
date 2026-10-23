#include "resampler/RatioText.h"

#include <cstdio>

namespace multiroom::resampler {

std::string ratioText(double ratio)
{
    char buffer[32];
    int written = std::snprintf(buffer, sizeof(buffer), "%.6f", ratio);
    // snprintf returns the untruncated length; a garbage ratio too wide for
    // fixed notation (1e300 is 300+ digits) is printed in exponent form, which
    // always fits.
    if (written > 0 && static_cast<size_t>(written) >= sizeof(buffer))
        written = std::snprintf(buffer, sizeof(buffer), "%.6g", ratio);
    if (written <= 0 || static_cast<size_t>(written) >= sizeof(buffer))
        return std::string("?");
    return std::string(buffer, static_cast<size_t>(written));
}

} // namespace multiroom::resampler
