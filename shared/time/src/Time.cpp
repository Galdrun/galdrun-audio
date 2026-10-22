#include "util/Time.h"

#include <cstdio>
#include <ctime>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <sys/time.h>
#endif

namespace multiroom {
namespace {

#if defined(_WIN32)

// QPC ticks -> ns without overflowing on 32-bit builds and without losing the
// sub-tick remainder. Frequency is fixed for the lifetime of the process.
int64_t qpcFrequency()
{
    static const int64_t frequency = []() {
        LARGE_INTEGER f;
        if (!QueryPerformanceFrequency(&f) || f.QuadPart <= 0)
            return static_cast<int64_t>(1);
        return static_cast<int64_t>(f.QuadPart);
    }();
    return frequency;
}

#endif

} // namespace

uint64_t monotonicNowNs()
{
#if defined(_WIN32)
    LARGE_INTEGER counter;
    if (!QueryPerformanceCounter(&counter))
        return 0;
    const int64_t frequency = qpcFrequency();
    const int64_t ticks = static_cast<int64_t>(counter.QuadPart);
    const int64_t seconds = ticks / frequency;
    const int64_t remainder = ticks % frequency;
    return static_cast<uint64_t>(seconds) * 1000000000ULL +
           static_cast<uint64_t>((remainder * 1000000000LL) / frequency);
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
           static_cast<uint64_t>(ts.tv_nsec);
#endif
}

std::string iso8601UtcNow()
{
    const std::time_t now = std::time(nullptr);
    std::tm utc;
#if defined(_WIN32)
    if (gmtime_s(&utc, &now) != 0)
        return std::string();
#else
    if (gmtime_r(&now, &utc) == nullptr)
        return std::string();
#endif
    char buffer[80]; // what GCC counts for six unbounded ints
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                  utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday,
                  utc.tm_hour, utc.tm_min, utc.tm_sec);
    return std::string(buffer);
}

std::string localTimeOfDay()
{
#if defined(_WIN32)
    SYSTEMTIME local;
    GetLocalTime(&local);
    char buffer[24];
    std::snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d.%03d",
                  local.wHour, local.wMinute, local.wSecond, local.wMilliseconds);
    return std::string(buffer);
#else
    struct timeval tv;
    if (gettimeofday(&tv, nullptr) != 0)
        return std::string();
    const std::time_t seconds = static_cast<std::time_t>(tv.tv_sec);
    const int milliseconds = static_cast<int>(tv.tv_usec / 1000);

    std::tm local;
    if (localtime_r(&seconds, &local) == nullptr)
        return std::string();
    char buffer[48]; // what GCC counts for four unbounded ints
    std::snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d.%03d",
                  local.tm_hour, local.tm_min, local.tm_sec, milliseconds);
    return std::string(buffer);
#endif
}

} // namespace multiroom
