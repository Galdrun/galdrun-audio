#pragma once

#include <cstdint>

namespace multiroom {

// Widens a wrapping 32-bit counter to 64 bits.
//
// Frame counters have to cross a thread boundary: the audio thread increments,
// the control thread reads. A 64-bit atomic would be the obvious answer, but
// std::atomic<uint64_t> is not lock-free on ARMv6 -- it falls back to a lock,
// which has no business in the audio thread. So the counter stays a 32-bit
// atomic and is widened here, on the reading side.
//
// At 48 kHz a 32-bit frame counter wraps after about 24.8 hours, so this is
// correct as long as extend() is called more often than that. Telemetry runs
// every few hundred milliseconds, which leaves five orders of magnitude of
// margin.
//
// Not thread-safe: it belongs to the single reader.
class Wrap32Extender
{
public:
    void reset()
    {
        lastLow_ = 0;
        high_ = 0;
        started_ = false;
    }

    uint64_t extend(uint32_t current)
    {
        if (!started_)
        {
            started_ = true;
        }
        else if (current < lastLow_)
        {
            // The low word went backwards, which for a monotonic counter can
            // only mean it wrapped.
            ++high_;
        }
        lastLow_ = current;
        return (static_cast<uint64_t>(high_) << 32) | static_cast<uint64_t>(current);
    }

private:
    uint32_t lastLow_ = 0;
    uint32_t high_ = 0;
    bool started_ = false;
};

} // namespace multiroom
