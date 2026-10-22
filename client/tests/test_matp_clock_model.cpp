#include "TestSupport.h"

#include "matp/ClockModel.h"

using namespace multiroom;

namespace {

// Deterministic, for the same reason test_drift_estimator.cpp has one: a test
// that fails once every few runs is worse than no test.
class Lcg
{
public:
    explicit Lcg(uint32_t seed) : state_(seed) {}

    // Uniform in [-1, 1].
    double symmetric()
    {
        state_ = state_ * 1664525u + 1013904223u;
        return (static_cast<double>(state_ >> 8) / 8388608.0) - 1.0;
    }

private:
    uint32_t state_;
};

const uint64_t kLocalBaseNs = 900ULL * 1000000000ULL;

// A server whose clock is offset from ours and may run at a slightly different
// rate, plus a network with a one-way delay in each direction. Every exchange
// is built from the local instants it actually happened at, so the sample it
// produces is exactly what a real one would be -- including the offset error
// that an asymmetric path unavoidably introduces.
class Server
{
public:
    Server(int64_t offsetNs, double skewPpm) : offsetNs_(offsetNs), skewPpm_(skewPpm) {}

    void step(int64_t byNs) { offsetNs_ += byNs; }

    uint64_t serverAt(uint64_t localNs) const
    {
        const double elapsed = static_cast<double>(localNs - kLocalBaseNs);
        const int64_t drifted = static_cast<int64_t>(elapsed * skewPpm_ * 1e-6);
        return static_cast<uint64_t>(static_cast<int64_t>(localNs) + offsetNs_ + drifted);
    }

    matp::ClockSyncSample exchange(uint64_t sendNs, uint64_t outboundNs, uint64_t inboundNs,
                                   uint64_t processingNs) const
    {
        const uint64_t arrivedAt = sendNs + outboundNs;
        const uint64_t answeredAt = arrivedAt + processingNs;

        matp::ClockSyncSample sample;
        sample.clientSendNs = sendNs;
        sample.serverReceiveNs = serverAt(arrivedAt);
        sample.serverSendNs = serverAt(answeredAt);
        sample.clientReceiveNs = answeredAt + inboundNs;
        return sample;
    }

private:
    int64_t offsetNs_;
    double skewPpm_;
};

const uint64_t kOneWayNs = 250000;     // 250 us each way: a quiet LAN
const uint64_t kProcessingNs = 120000; // what the server spends answering

} // namespace

MULTIROOM_TEST(clock_model_publishes_nothing_before_it_has_a_window)
{
    matp::ClockModel model;
    Server server(5000000000LL, 0.0);

    CHECK(!model.hasEstimate());

    uint64_t serverNs = 0;
    CHECK(!model.serverFromLocal(kLocalBaseNs, serverNs));
    CHECK(!model.localFromServer(kLocalBaseNs, serverNs));
    CHECK_EQ(model.offsetNs(), static_cast<int64_t>(0));

    // minSamples is 4; three is three.
    for (uint32_t i = 0; i < 3; ++i)
    {
        const uint64_t sendNs = kLocalBaseNs + i * 50000000ULL;
        CHECK(model.addSample(server.exchange(sendNs, kOneWayNs, kOneWayNs, kProcessingNs)));
    }
    CHECK(!model.hasEstimate());

    CHECK(model.addSample(
        server.exchange(kLocalBaseNs + 150000000ULL, kOneWayNs, kOneWayNs, kProcessingNs)));
    CHECK(model.hasEstimate());
}

MULTIROOM_TEST(clock_model_recovers_a_constant_offset_through_jitter)
{
    const int64_t offset = -3200000000LL; // the server booted after we did
    matp::ClockModel model;
    Server server(offset, 0.0);
    Lcg rng(7);

    for (uint32_t i = 0; i < 20; ++i)
    {
        // Symmetric jitter: each direction varies independently around the
        // same mean, which is what a switch under light load looks like.
        const uint64_t outbound =
            kOneWayNs + static_cast<uint64_t>((rng.symmetric() + 1.0) * 50000.0);
        const uint64_t inbound =
            kOneWayNs + static_cast<uint64_t>((rng.symmetric() + 1.0) * 50000.0);
        const uint64_t sendNs = kLocalBaseNs + i * 50000000ULL;
        CHECK(model.addSample(server.exchange(sendNs, outbound, inbound, kProcessingNs)));
    }

    CHECK(model.hasEstimate());
    // Half of the jitter spread is the floor no four-timestamp exchange can
    // get under; 20 us is comfortably inside it.
    CHECK_NEAR(model.offsetNs(), offset, 20000.0);
    CHECK(model.bestRoundTripNs() >= 2 * kOneWayNs);
    CHECK_EQ(model.rejectedCount(), static_cast<uint64_t>(0));

    // Twenty samples over one second say nothing about a rate.
    CHECK(!model.hasSkew());
    CHECK_EQ(model.skewPpm(), 0.0);
}

MULTIROOM_TEST(clock_model_rejects_asymmetrically_delayed_exchanges)
{
    const int64_t offset = 2000000000LL;
    matp::ClockModel model;
    Server server(offset, 0.0);

    // Every fourth request is held up by 20 ms on the way out and nothing on
    // the way back: accepted, it would drag the offset 10 ms off.
    for (uint32_t i = 0; i < 24; ++i)
    {
        const bool delayed = (i % 4) == 3;
        const uint64_t outbound = delayed ? kOneWayNs + 20000000ULL : kOneWayNs;
        const uint64_t sendNs = kLocalBaseNs + i * 50000000ULL;
        const matp::ClockSyncSample sample =
            server.exchange(sendNs, outbound, kOneWayNs, kProcessingNs);
        CHECK_EQ(model.addSample(sample), !delayed);
    }

    CHECK(model.hasEstimate());
    CHECK_EQ(model.rejectedCount(), static_cast<uint64_t>(6));
    CHECK_NEAR(model.offsetNs(), offset, 1000.0);
}

MULTIROOM_TEST(clock_model_follows_a_path_that_became_slower_for_good)
{
    const int64_t offset = 1500000000LL;
    matp::ClockModel model;
    Server server(offset, 0.0);

    for (uint32_t i = 0; i < 20; ++i)
    {
        const uint64_t sendNs = kLocalBaseNs + i * 50000000ULL;
        CHECK(model.addSample(server.exchange(sendNs, 100000, 100000, kProcessingNs)));
    }
    CHECK_NEAR(model.bestRoundTripNs(), 200000.0, 1000.0);

    // From here on every exchange takes 10 ms, far above the 0.2 ms gate. The
    // first few are refused as queueing; the streak is the new normal.
    const uint64_t slowFromNs = kLocalBaseNs + 20 * 50000000ULL;
    uint32_t accepted = 0;
    for (uint32_t i = 0; i < 100; ++i)
    {
        const uint64_t sendNs = slowFromNs + i * 5000000000ULL; // section 46's 5 s
        if (model.addSample(server.exchange(sendNs, 5000000, 5000000, kProcessingNs)))
            ++accepted;
    }

    CHECK_EQ(accepted, 100u - 6u);
    CHECK(model.hasEstimate());
    // The fast samples aged out, and the gate follows the path it has now.
    CHECK_NEAR(model.bestRoundTripNs(), 10000000.0, 1000.0);
    CHECK_NEAR(model.offsetNs(), offset, 1000.0);
}

MULTIROOM_TEST(clock_model_drops_a_delayed_first_exchange_once_it_knows_better)
{
    const int64_t offset = 750000000LL;
    matp::ClockModel model;
    Server server(offset, 0.0);

    // Nothing to compare the first sample against, so it is taken. It is 30 ms
    // late in one direction, which puts its offset 15 ms out.
    CHECK(model.addSample(
        server.exchange(kLocalBaseNs, kOneWayNs + 30000000ULL, kOneWayNs, kProcessingNs)));
    CHECK_NEAR(model.bestRoundTripNs(), 30500000.0, 1000.0);

    for (uint32_t i = 1; i < 8; ++i)
    {
        const uint64_t sendNs = kLocalBaseNs + i * 50000000ULL;
        CHECK(model.addSample(server.exchange(sendNs, kOneWayNs, kOneWayNs, kProcessingNs)));
    }

    // The good samples showed the window what the path can do, and the sample
    // that no longer qualifies was dropped rather than defended.
    CHECK_EQ(model.sampleCount(), static_cast<uint32_t>(7));
    CHECK_NEAR(model.bestRoundTripNs(), 500000.0, 1000.0);
    CHECK_NEAR(model.offsetNs(), offset, 1000.0);
}

MULTIROOM_TEST(clock_model_resolves_skew_only_once_the_window_is_long_enough)
{
    const int64_t offset = 1000000000LL;
    const double skewPpm = 50.0;
    matp::ClockModel model;
    Server server(offset, skewPpm);
    Lcg rng(19);

    // Section 46's connect burst first: twenty exchanges inside a second.
    for (uint32_t i = 0; i < 20; ++i)
    {
        const uint64_t sendNs = kLocalBaseNs + i * 50000000ULL;
        CHECK(model.addSample(server.exchange(sendNs, kOneWayNs, kOneWayNs, kProcessingNs)));
    }
    CHECK(model.hasEstimate());
    CHECK(!model.hasSkew());

    // Then section 46's ongoing interval, for two minutes.
    for (uint32_t i = 1; i <= 24; ++i)
    {
        const uint64_t outbound =
            kOneWayNs + static_cast<uint64_t>((rng.symmetric() + 1.0) * 25000.0);
        const uint64_t inbound =
            kOneWayNs + static_cast<uint64_t>((rng.symmetric() + 1.0) * 25000.0);
        const uint64_t sendNs = kLocalBaseNs + 1000000000ULL + i * 5000000000ULL;
        CHECK(model.addSample(server.exchange(sendNs, outbound, inbound, kProcessingNs)));
    }

    CHECK(model.hasSkew());
    CHECK_NEAR(model.skewPpm(), skewPpm, 2.0);
    CHECK(model.windowSpanSeconds() > 100.0);

    // The offset is still the offset: skew is the rate at which it changes,
    // not a replacement for it.
    const uint64_t newestLocal = kLocalBaseNs + 1000000000ULL + 24 * 5000000000ULL;
    CHECK_NEAR(model.offsetNs(),
               static_cast<double>(server.serverAt(newestLocal)) -
                   static_cast<double>(newestLocal),
               50000.0);
}

MULTIROOM_TEST(clock_model_recovers_from_a_step_in_the_server_clock)
{
    const int64_t offset = 400000000LL;
    matp::ClockModel model;
    Server server(offset, 0.0);

    uint64_t sendNs = kLocalBaseNs;
    for (uint32_t i = 0; i < 12; ++i)
    {
        CHECK(model.addSample(server.exchange(sendNs, kOneWayNs, kOneWayNs, kProcessingNs)));
        sendNs += 50000000ULL;
    }
    CHECK_NEAR(model.offsetNs(), offset, 1000.0);

    // The server process restarted: same machine, a monotonic clock that began
    // again 200 ms earlier.
    server.step(-200000000LL);

    // The first samples after the step are refused -- one measurement cannot
    // outvote a window -- and it takes maxConsecutiveOutliers of them before
    // the model concedes that the window is the thing that is wrong.
    CHECK(!model.addSample(server.exchange(sendNs, kOneWayNs, kOneWayNs, kProcessingNs)));
    sendNs += 50000000ULL;
    CHECK(!model.addSample(server.exchange(sendNs, kOneWayNs, kOneWayNs, kProcessingNs)));
    sendNs += 50000000ULL;
    CHECK(model.addSample(server.exchange(sendNs, kOneWayNs, kOneWayNs, kProcessingNs)));
    sendNs += 50000000ULL;

    // Restarted from that sample, so there is no mapping again until the
    // window has refilled.
    CHECK_EQ(model.sampleCount(), static_cast<uint32_t>(1));
    CHECK(!model.hasEstimate());

    for (uint32_t i = 0; i < 8; ++i)
    {
        CHECK(model.addSample(server.exchange(sendNs, kOneWayNs, kOneWayNs, kProcessingNs)));
        sendNs += 50000000ULL;
    }

    CHECK(model.hasEstimate());
    CHECK_NEAR(model.offsetNs(), offset - 200000000LL, 1000.0);
}

MULTIROOM_TEST(clock_model_converts_in_both_directions)
{
    const int64_t offset = 12000000000LL;
    const double skewPpm = 30.0;
    matp::ClockModel model;
    Server server(offset, skewPpm);

    uint64_t sendNs = kLocalBaseNs;
    for (uint32_t i = 0; i < 40; ++i)
    {
        CHECK(model.addSample(server.exchange(sendNs, kOneWayNs, kOneWayNs, kProcessingNs)));
        sendNs += 5000000000ULL;
    }
    CHECK(model.hasSkew());

    // A START_AT two seconds into the future, which is what phase 12 will do
    // with this.
    const uint64_t local = sendNs;
    uint64_t serverNs = 0;
    CHECK(model.serverFromLocal(local, serverNs));
    CHECK_NEAR(static_cast<double>(serverNs), static_cast<double>(server.serverAt(local)),
               20000.0);

    uint64_t backToLocal = 0;
    CHECK(model.localFromServer(serverNs, backToLocal));
    // The round trip through the model has to be exact to the nanosecond; any
    // slack here is precision lost in the arithmetic, not in the measurement.
    CHECK_NEAR(static_cast<double>(backToLocal), static_cast<double>(local), 1.0);

    uint64_t future = 0;
    CHECK(model.localFromServer(server.serverAt(local + 2000000000ULL), future));
    CHECK_NEAR(static_cast<double>(future), static_cast<double>(local + 2000000000ULL), 20000.0);
}

MULTIROOM_TEST(clock_model_refuses_offsets_whose_difference_could_overflow)
{
    // The window's fit subtracts offsets from each other, and the first
    // samples of a window pass no outlier test. An offset of 2^62 ns or more
    // either way is refused, so no two accepted ones can differ by 2^63.
    const int64_t limit = static_cast<int64_t>(1) << 62;
    const uint64_t far = static_cast<uint64_t>(limit) + kLocalBaseNs + 1000000ULL;
    matp::ClockModel model;

    matp::ClockSyncSample tooFar;
    tooFar.clientSendNs = kLocalBaseNs;
    tooFar.serverReceiveNs = far;
    tooFar.serverSendNs = far + kProcessingNs;
    tooFar.clientReceiveNs = kLocalBaseNs + 2 * kOneWayNs + kProcessingNs;
    CHECK(!model.addSample(tooFar));

    // Just inside the bound is a clock, if an odd one.
    Server server(limit - 1000000000LL, 0.0);
    CHECK(model.addSample(server.exchange(kLocalBaseNs, kOneWayNs, kOneWayNs, kProcessingNs)));
    CHECK_EQ(model.sampleCount(), static_cast<uint32_t>(1));
}

MULTIROOM_TEST(clock_model_refuses_impossible_exchanges)
{
    matp::ClockModel model;
    Server server(0, 0.0);
    const matp::ClockSyncSample good =
        server.exchange(kLocalBaseNs, kOneWayNs, kOneWayNs, kProcessingNs);

    matp::ClockSyncSample backwards = good;
    backwards.clientReceiveNs = good.clientSendNs;
    CHECK(!model.addSample(backwards));

    matp::ClockSyncSample answeredFirst = good;
    answeredFirst.serverSendNs = good.serverReceiveNs - 1;
    CHECK(!model.addSample(answeredFirst));

    // A server that claims to have spent longer on the reply than the whole
    // exchange took here leaves a negative round trip, which is not a slow
    // network but a wrong timestamp.
    matp::ClockSyncSample impossible = good;
    impossible.serverSendNs = good.serverReceiveNs + (good.clientReceiveNs - good.clientSendNs) + 1;
    CHECK(!model.addSample(impossible));

    // A server clock more than 2^63 ns away from this one has no signed
    // offset, and subtracting the two as int64 would overflow.
    matp::ClockSyncSample unreachable = good;
    unreachable.serverReceiveNs = 0xFFFFFFFFFFFFFF00ULL;
    unreachable.serverSendNs = unreachable.serverReceiveNs + kProcessingNs;
    CHECK(!model.addSample(unreachable));

    // Nothing plausible enough to measure came in, so nothing is published.
    CHECK_EQ(model.sampleCount(), static_cast<uint32_t>(0));
    CHECK_EQ(model.rejectedCount(), static_cast<uint64_t>(4));

    CHECK(model.addSample(good));
    CHECK_EQ(model.sampleCount(), static_cast<uint32_t>(1));

    // Two exchanges reported in the wrong order would put a point behind the
    // window's own newest sample.
    CHECK(!model.addSample(good));
    CHECK_EQ(model.sampleCount(), static_cast<uint32_t>(1));
}

MULTIROOM_TEST(clock_model_refuses_a_server_instant_that_does_not_fit)
{
    // A server whose clock has only just started, so its origin is near zero
    // and an instant near the top of the range is an elapsed time that no
    // local clock can be moved by without leaving int64.
    matp::ClockModel model;
    Server server(-static_cast<int64_t>(kLocalBaseNs) + 1000000, 0.0);
    uint64_t sendNs = kLocalBaseNs;
    for (uint32_t i = 0; i < 4; ++i)
    {
        CHECK(model.addSample(server.exchange(sendNs, kOneWayNs, kOneWayNs, kProcessingNs)));
        sendNs += 50000000ULL;
    }
    CHECK(model.hasEstimate());

    uint64_t local = 0;
    CHECK(model.localFromServer(server.serverAt(sendNs), local));
    CHECK(!model.localFromServer(0x7FFFFFFFFFFFFFFFULL, local));
    CHECK(!model.localFromServer(0xFFFFFFFFFFFFFFFFULL, local));
}

MULTIROOM_TEST(clock_model_reset_drops_the_mapping_but_keeps_the_diagnostics)
{
    matp::ClockModel model;
    Server server(1000000LL, 0.0);

    for (uint32_t i = 0; i < 8; ++i)
    {
        const uint64_t sendNs = kLocalBaseNs + i * 50000000ULL;
        CHECK(model.addSample(server.exchange(sendNs, kOneWayNs, kOneWayNs, kProcessingNs)));
    }
    matp::ClockSyncSample rubbish;
    CHECK(!model.addSample(rubbish));
    CHECK(model.hasEstimate());

    model.reset();

    CHECK(!model.hasEstimate());
    CHECK_EQ(model.sampleCount(), static_cast<uint32_t>(0));
    CHECK_EQ(model.bestRoundTripNs(), static_cast<uint64_t>(0));
    CHECK_EQ(model.windowSpanSeconds(), 0.0);
    // A lifetime count, not a per-window one.
    CHECK_EQ(model.rejectedCount(), static_cast<uint64_t>(1));
}
