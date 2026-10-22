#pragma once

// Time is injected, everywhere, without exception.
//
// Every scheduler, controller and timeout in this server takes its time from
// an IClock reference rather than calling a global. That is what lets the
// scheduled-start barrier, the drift regression window, the readiness timeout
// and the reconnect backoff be tested by moving a number instead of sleeping
// through them -- a twenty-second regression window is otherwise a
// twenty-second test.
//
// The clock is monotonic and never derived from the wall clock, which can jump
// backwards. Human-readable timestamps are a separate, log-only concern: they
// are in shared/time's util/Time.h, beside the free monotonicNowNs() that
// SystemClock wraps.

#include <cstdint>

namespace multiroom {

class IClock
{
public:
    virtual ~IClock() = default;

    // Nanoseconds since an unspecified but fixed origin. Strictly
    // non-decreasing.
    virtual uint64_t monotonicNowNs() const = 0;
};

// The real clock: shared/time's monotonicNowNs(), the client's time base.
class SystemClock final : public IClock
{
public:
    uint64_t monotonicNowNs() const override;
};

} // namespace multiroom
