#pragma once

#include <cstdint>
#include <string>

namespace multiroom {

// Monotonic time base shared by the audio clock, the drift estimator and the
// logger. Never derived from the wall clock, which can jump.
//
// Windows: QueryPerformanceCounter, scaled to nanoseconds without overflow.
// POSIX:   clock_gettime(CLOCK_MONOTONIC).
uint64_t monotonicNowNs();

// Wall clock, only for human-readable output (log lines, cache timestamps).
// Format: 2026-09-12T12:00:00Z
std::string iso8601UtcNow();

// Local wall clock time of day for log lines. Format: 12:00:00.123
std::string localTimeOfDay();

} // namespace multiroom
