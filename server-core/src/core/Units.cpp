#include "core/Units.h"

#include <limits>

namespace multiroom {
namespace {

constexpr uint64_t kUint64Max = std::numeric_limits<uint64_t>::max();

} // namespace

const char* errorName(FrameMathError error)
{
    switch (error)
    {
    case FrameMathError::InvalidSampleRate:
        return "InvalidSampleRate";
    case FrameMathError::Overflow:
        return "Overflow";
    }
    return "?";
}

Result<uint64_t, FrameMathError> framesFromNanoseconds(uint64_t nanoseconds, uint32_t sampleRate)
{
    if (sampleRate == 0)
        return fail(FrameMathError::InvalidSampleRate, "sample rate is zero");

    // floor(nanoseconds * sampleRate / 1e9) exactly, without a 128-bit
    // intermediate that MSVC does not have: split the position into whole
    // seconds and a remainder below 1e9. The remainder's product is at most
    // (1e9 - 1) * (2^32 - 1), which still fits in 64 bits, so only the whole
    // seconds can overflow.
    const uint64_t seconds = nanoseconds / kNanosecondsPerSecond;
    const uint64_t rest = nanoseconds % kNanosecondsPerSecond;

    const uint64_t rate = sampleRate;
    if (seconds != 0 && rate > kUint64Max / seconds)
        return fail(FrameMathError::Overflow, "timeline position too far from the origin");

    const uint64_t whole = seconds * rate;
    const uint64_t partial = (rest * rate) / kNanosecondsPerSecond;
    if (whole > kUint64Max - partial)
        return fail(FrameMathError::Overflow, "timeline position too far from the origin");

    return whole + partial;
}

Result<uint64_t, FrameMathError> nanosecondsFromFrames(uint64_t frames, uint32_t sampleRate)
{
    if (sampleRate == 0)
        return fail(FrameMathError::InvalidSampleRate, "sample rate is zero");

    // ceil(frames * 1e9 / sampleRate). frames = seconds * rate + rest, and the
    // whole seconds contribute an exact multiple of 1e9, so the rounding only
    // ever applies to the remainder. rest is below 2^32, so rest * 1e9 fits.
    const uint64_t rate = sampleRate;
    const uint64_t seconds = frames / rate;
    const uint64_t rest = frames % rate;

    if (seconds > kUint64Max / kNanosecondsPerSecond)
        return fail(FrameMathError::Overflow, "frame count exceeds a 64-bit nanosecond span");

    const uint64_t whole = seconds * kNanosecondsPerSecond;
    const uint64_t partial = (rest * kNanosecondsPerSecond + rate - 1) / rate;
    if (whole > kUint64Max - partial)
        return fail(FrameMathError::Overflow, "frame count exceeds a 64-bit nanosecond span");

    return whole + partial;
}

} // namespace multiroom
