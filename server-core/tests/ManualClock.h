#pragma once

// The test implementation of IClock: time only moves when a test moves it.
//
// This is the whole reason every scheduler and timeout in the server takes a
// clock reference. A twenty-second drift regression window, a five-second
// readiness timeout and a reconnect backoff are all walked here by adding
// numbers, and none of the tests sleep.
//
// Atomic, because the rooms' own threads read it while the test moves it.

#include "util/Clock.h"

#include <atomic>

namespace multiroomtest {

class ManualClock final : public multiroom::IClock
{
public:
    explicit ManualClock(uint64_t startNs = 0) : nowNs_(startNs) {}

    uint64_t monotonicNowNs() const override { return nowNs_.load(); }

    void advanceNs(uint64_t nanoseconds) { nowNs_.fetch_add(nanoseconds); }
    void advanceMs(uint64_t milliseconds) { advanceNs(milliseconds * 1'000'000ULL); }
    void setNs(uint64_t nanoseconds) { nowNs_.store(nanoseconds); }

private:
    std::atomic<uint64_t> nowNs_{0};
};

} // namespace multiroomtest
