// The server's model of one client's clock (MATP sections 8, 9 and 29 to 31).
//
// Every test here injects a known offset and a known skew and checks that the
// model finds them, because that is the only way to test a fit: an assertion
// that it produced *a* number says nothing.

#include "TestSupport.h"

#include "client/ClientClockModel.h"

#include <cmath>

using namespace multiroom;

namespace {

// One exchange, seen from a client whose clock reads `offsetNs` less than the
// server's and runs `scale` times as fast, over a link of `roundTripNs` split
// evenly in both directions.
//
// `scale` is the *server's* seconds per client second, which is the direction
// the model reports: a client card running fast reads more of its own
// nanoseconds per server nanosecond, so its scale is below one.
matp::ClockSyncResult exchange(uint64_t clientSendNs, int64_t offsetNs, double scale,
                               uint64_t roundTripNs, uint64_t serverWorkNs,
                               uint64_t clientOriginNs = 0)
{
    const double elapsedOnClient = static_cast<double>(clientSendNs - clientOriginNs);
    const double serverAtSend = static_cast<double>(clientOriginNs) +
                                static_cast<double>(offsetNs) + elapsedOnClient * scale;

    matp::ClockSyncResult result;
    result.clientSendNs = clientSendNs;
    result.serverReceiveNs =
        static_cast<uint64_t>(serverAtSend + static_cast<double>(roundTripNs) / 2.0);
    result.serverSendNs = result.serverReceiveNs + serverWorkNs;
    result.clientReceiveNs = clientSendNs + roundTripNs + serverWorkNs;
    return result;
}

} // namespace

MULTIROOM_TEST(clock_model_is_invalid_until_it_has_enough_samples)
{
    ClientClockModel model;
    CHECK(!model.valid());
    CHECK(!model.hasScale());
    CHECK_EQ(model.toServerTimeNs(1000), 0ULL);
    CHECK_EQ(model.offsetNs(), int64_t(0));

    uint64_t clientNs = 1000000000ULL;
    for (size_t i = 0; i + 1 < ClientClockModel::kMinSamples; ++i)
    {
        CHECK(model.addSample(exchange(clientNs, 500000000LL, 1.0, 1000000, 50000)));
        clientNs += 1000000000ULL;
        CHECK(!model.valid());
    }

    CHECK(model.addSample(exchange(clientNs, 500000000LL, 1.0, 1000000, 50000)));
    CHECK(model.valid());
}

MULTIROOM_TEST(clock_model_finds_an_injected_offset_within_half_the_round_trip)
{
    // The uncertainty in a midpoint *is* half the round trip; a model that
    // claimed better would be claiming to know which direction was slower.
    constexpr int64_t kOffsetNs = -4000000000LL; // the server booted first
    constexpr uint64_t kRoundTripNs = 2000000ULL;

    ClientClockModel model;
    uint64_t clientNs = 900000000000ULL;
    for (int i = 0; i < 20; ++i)
    {
        CHECK(model.addSample(exchange(clientNs, kOffsetNs, 1.0, kRoundTripNs, 40000)));
        clientNs += 5000000000ULL;
    }

    CHECK(model.valid());
    CHECK_EQ(model.acceptedCount(), 20ULL);
    CHECK_NEAR(static_cast<double>(model.offsetNs()), static_cast<double>(kOffsetNs),
               static_cast<double>(kRoundTripNs) / 2.0);
}

MULTIROOM_TEST(clock_model_finds_a_skewed_client_clock)
{
    // The done-when of phase 8: a deliberately skewed client clock produces a
    // scale away from 1.0. 200 ppm is a bad but entirely real crystal.
    constexpr double kScale = 1.0002;

    ClientClockModel model;
    uint64_t clientNs = 1000000000ULL;
    for (int i = 0; i < 40; ++i)
    {
        CHECK(model.addSample(exchange(clientNs, 0, kScale, 500000, 20000, 1000000000ULL)));
        clientNs += 5000000000ULL; // 195 s of lever arm
    }

    CHECK(model.valid());
    CHECK(model.hasScale());
    CHECK(model.spanNs() >= ClientClockModel::kMinScaleSpanNs);
    // Within 5 ppm of the injected skew, from a 0.5 ms round trip over 195 s.
    CHECK_NEAR(model.scale(), kScale, 5e-6);
    // And well away from 1.0, which is what the roadmap asks to see.
    CHECK(std::fabs(model.scale() - 1.0) > 1e-4);
}

MULTIROOM_TEST(clock_model_reports_no_scale_before_it_has_a_lever_arm)
{
    // Fitting a slope to twenty samples a second apart measures the sampling
    // jitter, not the oscillator. An unmeasured rate is reported as
    // unmeasured, not as a 1.000000 that happens to be right.
    ClientClockModel model;
    uint64_t clientNs = 1000000000ULL;
    for (int i = 0; i < 20; ++i)
    {
        CHECK(model.addSample(exchange(clientNs, 1000000000LL, 1.0002, 500000, 20000,
                                       1000000000ULL)));
        clientNs += 1000000000ULL; // 19 s, short of the 60 s minimum
    }

    CHECK(model.valid());
    CHECK(!model.hasScale());
    CHECK_EQ(model.scale(), 1.0);

    // The offset is still measured. With the slope pinned it is the window's
    // *mean* difference rather than the difference at the newest sample, which
    // is the right estimator for a constant measured repeatedly -- and for a
    // clock that is in fact drifting, it lags the present by half the drift
    // the window covers. Here that is 19 s at 200 ppm, so the mean sits
    // 1.9 ms below the 3.8 ms the last sample showed.
    //
    // That lag is the whole reason the scale is worth measuring once there is
    // a lever arm for it, and it is why hasScale() is reported rather than
    // hidden: a caller that treats this as an instantaneous offset needs to
    // know which of the two it has been given.
    CHECK_NEAR(static_cast<double>(model.offsetNs()), 1000000000.0 + 9500000000.0 * 0.0002,
               50000.0);
}

MULTIROOM_TEST(clock_model_rejects_a_round_trip_that_sat_in_a_queue)
{
    // Congestion only ever lengthens a round trip, so the best one in the
    // window is the reference. A sample far above it spent its time in a queue
    // and there is no way to know which direction paid for it.
    ClientClockModel model;
    uint64_t clientNs = 1000000000ULL;
    for (int i = 0; i < 10; ++i)
    {
        CHECK(model.addSample(exchange(clientNs, 2000000000LL, 1.0, 1000000, 30000)));
        clientNs += 1000000000ULL;
    }
    CHECK_EQ(model.rejectedCount(), 0ULL);

    // Ten times the best round trip in the window.
    CHECK(!model.addSample(exchange(clientNs, 2000000000LL, 1.0, 10000000, 30000)));
    CHECK_EQ(model.rejectedCount(), 1ULL);
    CHECK_EQ(model.acceptedCount(), 10ULL);
    CHECK_EQ(model.sampleCount(), size_t(10));

    // One just inside the factor is kept: this is a gate against a queue, not
    // a tolerance on the network.
    clientNs += 1000000000ULL;
    CHECK(model.addSample(exchange(clientNs, 2000000000LL, 1.0, 2500000, 30000)));
    CHECK_EQ(model.acceptedCount(), 11ULL);
}

MULTIROOM_TEST(clock_model_rejects_a_clock_that_contradicts_itself)
{
    ClientClockModel model;

    // C4 before C1.
    matp::ClockSyncResult backwardsClient;
    backwardsClient.clientSendNs = 2000;
    backwardsClient.serverReceiveNs = 1000;
    backwardsClient.serverSendNs = 1100;
    backwardsClient.clientReceiveNs = 1000;
    CHECK(!model.addSample(backwardsClient));

    // S3 before S2.
    matp::ClockSyncResult backwardsServer;
    backwardsServer.clientSendNs = 1000;
    backwardsServer.serverReceiveNs = 1100;
    backwardsServer.serverSendNs = 1000;
    backwardsServer.clientReceiveNs = 2000;
    CHECK(!model.addSample(backwardsServer));

    // An exchange that took less time on the client than the server spent
    // inside it: one of the two clocks is not monotonic.
    matp::ClockSyncResult impossible;
    impossible.clientSendNs = 1000;
    impossible.serverReceiveNs = 5000;
    impossible.serverSendNs = 9000; // 4000 ns of server work
    impossible.clientReceiveNs = 2000; // but only 1000 ns elapsed on the client
    CHECK(!model.addSample(impossible));

    CHECK_EQ(model.rejectedCount(), 3ULL);
    CHECK(!model.valid());
}

MULTIROOM_TEST(clock_model_keeps_a_bounded_window)
{
    // Memory is bounded here like everywhere else, and the oldest samples
    // describe an oscillator that has since warmed up.
    ClientClockModel model;
    uint64_t clientNs = 1000000000ULL;
    for (size_t i = 0; i < ClientClockModel::kMaxSamples * 2; ++i)
    {
        CHECK(model.addSample(exchange(clientNs, 1000000000LL, 1.0, 1000000, 30000)));
        clientNs += 1000000000ULL;
    }

    CHECK_EQ(model.sampleCount(), ClientClockModel::kMaxSamples);
    CHECK_EQ(model.acceptedCount(), ClientClockModel::kMaxSamples * 2);
}

MULTIROOM_TEST(clock_model_converts_a_client_instant_into_server_time)
{
    // This is what phase 12 will do with it: PLAYBACK_REPORT's
    // localMonotonicTimeNs is on the client's clock, and the offset half of
    // the correction is comparing it against the server's.
    constexpr int64_t kOffsetNs = 7000000000LL;

    ClientClockModel model;
    uint64_t clientNs = 1000000000ULL;
    for (int i = 0; i < 30; ++i)
    {
        CHECK(model.addSample(exchange(clientNs, kOffsetNs, 1.0, 400000, 20000,
                                       1000000000ULL)));
        clientNs += 5000000000ULL;
    }

    const uint64_t someClientInstant = 50000000000ULL;
    const uint64_t asServerTime = model.toServerTimeNs(someClientInstant);
    CHECK_NEAR(static_cast<double>(asServerTime),
               static_cast<double>(someClientInstant) + static_cast<double>(kOffsetNs),
               200000.0);
}

MULTIROOM_TEST(clock_model_resets_to_nothing)
{
    // Section 43: the mapping is rebuilt from scratch on every connection.
    ClientClockModel model;
    uint64_t clientNs = 1000000000ULL;
    for (int i = 0; i < 5; ++i)
    {
        CHECK(model.addSample(exchange(clientNs, 1000000000LL, 1.0, 1000000, 30000)));
        clientNs += 1000000000ULL;
    }
    CHECK(model.valid());

    model.reset();
    CHECK(!model.valid());
    CHECK_EQ(model.sampleCount(), size_t(0));
    CHECK_EQ(model.acceptedCount(), 0ULL);
    CHECK_EQ(model.rejectedCount(), 0ULL);
}
