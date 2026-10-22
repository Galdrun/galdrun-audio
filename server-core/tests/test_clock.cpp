#include "ManualClock.h"
#include "TestSupport.h"

#include "core/Units.h"
#include "util/Clock.h"
#include "util/Time.h"

#include <thread>

using namespace multiroom;

namespace {

// What every timed component in this server will look like: it is handed a
// clock and never calls one of its own.
class Timeout
{
public:
    Timeout(const IClock& clock, uint64_t durationNs)
        : clock_(clock), deadlineNs_(clock.monotonicNowNs() + durationNs)
    {
    }

    bool expired() const { return clock_.monotonicNowNs() >= deadlineNs_; }

private:
    const IClock& clock_;
    uint64_t deadlineNs_;
};

} // namespace

MULTIROOM_TEST(manual_clock_only_moves_when_a_test_moves_it)
{
    multiroomtest::ManualClock clock(1000);

    CHECK_EQ(clock.monotonicNowNs(), uint64_t{1000});
    CHECK_EQ(clock.monotonicNowNs(), uint64_t{1000});

    clock.advanceNs(500);
    CHECK_EQ(clock.monotonicNowNs(), uint64_t{1500});

    clock.advanceMs(2);
    CHECK_EQ(clock.monotonicNowNs(), uint64_t{2'001'500});

    clock.setNs(0);
    CHECK_EQ(clock.monotonicNowNs(), uint64_t{0});
}

MULTIROOM_TEST(an_injected_clock_walks_a_timeout_without_sleeping)
{
    // MATP section 46's client ready timeout is five seconds. Nothing here
    // waits five seconds, and that is the reason time is injected at all.
    multiroomtest::ManualClock clock;
    const Timeout ready(clock, 5 * kNanosecondsPerSecond);

    CHECK(!ready.expired());

    clock.advanceMs(4999);
    CHECK(!ready.expired());

    clock.advanceMs(1);
    CHECK(ready.expired());

    // And the twenty-second drift regression window of the same section.
    multiroomtest::ManualClock slow;
    const Timeout window(slow, 20 * kNanosecondsPerSecond);
    slow.advanceMs(19'999);
    CHECK(!window.expired());
    slow.advanceMs(1);
    CHECK(window.expired());
}

MULTIROOM_TEST(system_clock_is_monotonic_and_in_nanoseconds)
{
    const SystemClock clock;

    const uint64_t first = clock.monotonicNowNs();
    CHECK(first > 0);

    uint64_t previous = first;
    for (int i = 0; i < 1000; ++i)
    {
        const uint64_t now = clock.monotonicNowNs();
        CHECK(now >= previous);
        previous = now;
    }

    // A short sleep has to show up as roughly that many nanoseconds. The upper
    // bound is generous because a scheduler is allowed to oversleep; the point
    // is the unit, not the precision.
    const uint64_t before = clock.monotonicNowNs();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const uint64_t elapsed = clock.monotonicNowNs() - before;

    CHECK(elapsed >= 10'000'000ULL);
    CHECK(elapsed < 5ULL * kNanosecondsPerSecond);
}

MULTIROOM_TEST(wall_clock_helpers_are_for_humans_only)
{
    const std::string time = localTimeOfDay();
    CHECK_EQ(time.size(), size_t{12}); // 12:00:00.123
    CHECK_EQ(time[2], ':');
    CHECK_EQ(time[5], ':');
    CHECK_EQ(time[8], '.');

    const std::string utc = iso8601UtcNow();
    CHECK_EQ(utc.size(), size_t{20}); // 2026-09-17T12:00:00Z
    CHECK_EQ(utc[4], '-');
    CHECK_EQ(utc[10], 'T');
    CHECK_EQ(utc[19], 'Z');
}
