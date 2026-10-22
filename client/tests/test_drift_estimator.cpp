#include "TestSupport.h"

#include "audio/DriftEstimator.h"

#include <vector>

using namespace multiroom;

namespace {

// Deterministic generator: a test that fails once every few CI runs because of
// std::random_device is worse than no test.
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

// Simulates a sound card whose crystal runs at realRate while the format says
// nominalRate, sampled at a fixed interval with timestamp jitter.
class ClockSimulator
{
public:
    ClockSimulator(uint32_t nominalRate, double realRate, double jitterSeconds, uint32_t seed)
        : nominalRate_(nominalRate), realRate_(realRate), jitter_(jitterSeconds), rng_(seed)
    {
    }

    AudioClockSnapshot next(double intervalSeconds)
    {
        elapsed_ += intervalSeconds;

        // The device has genuinely played this many frames by now.
        const double frames = elapsed_ * realRate_;

        // The timestamp is what we observe, and observation is late by a
        // random amount: scheduler jitter, interrupt latency, bus delay.
        const double observedSeconds = elapsed_ + rng_.symmetric() * jitter_;

        AudioClockSnapshot snapshot;
        snapshot.deviceFramePosition = static_cast<uint64_t>(frames);
        snapshot.monotonicTimeNs =
            baseNs_ + static_cast<uint64_t>(observedSeconds * 1e9);
        snapshot.nominalSampleRate = nominalRate_;
        snapshot.quality = quality_;
        return snapshot;
    }

    void setQuality(AudioClockQuality quality) { quality_ = quality; }

private:
    uint32_t nominalRate_;
    double realRate_;
    double jitter_;
    Lcg rng_;
    double elapsed_ = 0.0;
    uint64_t baseNs_ = 5000000000ULL; // start away from zero
    AudioClockQuality quality_ = AudioClockQuality::Hardware;
};

DriftEstimator::Settings testSettings()
{
    DriftEstimator::Settings settings;
    settings.windowSeconds = 30.0;
    settings.capacity = 1024;
    return settings;
}

// Feeds `seconds` of samples at `interval` and returns how many were accepted.
uint32_t feed(DriftEstimator& estimator, ClockSimulator& clock, double seconds,
              double interval)
{
    uint32_t accepted = 0;
    const uint32_t steps = static_cast<uint32_t>(seconds / interval);
    for (uint32_t i = 0; i < steps; ++i)
    {
        if (estimator.addSample(clock.next(interval)))
            ++accepted;
    }
    return accepted;
}

} // namespace

MULTIROOM_TEST(drift_recovers_the_specified_case_with_50us_jitter)
{
    // The case from the specification: nominal 48000 Hz, real 48001.2 Hz,
    // which is +25 ppm.
    ClockSimulator clock(48000, 48001.2, 50e-6, 12345);
    DriftEstimator estimator(testSettings());

    feed(estimator, clock, 30.0, 0.05);

    CHECK(estimator.hasEstimate());
    CHECK_NEAR(estimator.driftPpm(), 25.0, 2.0);
    CHECK_NEAR(estimator.estimatedSampleRate(), 48001.2, 0.1);
}

MULTIROOM_TEST(drift_recovers_the_specified_case_with_100us_jitter)
{
    // Twice the jitter must not move the answer, only widen the error bar.
    ClockSimulator clock(48000, 48001.2, 100e-6, 999);
    DriftEstimator estimator(testSettings());

    feed(estimator, clock, 30.0, 0.05);

    CHECK(estimator.hasEstimate());
    CHECK_NEAR(estimator.driftPpm(), 25.0, 3.0);
    // The estimator must know it is less sure than in the 50 us case.
    CHECK(estimator.standardErrorPpm() > 0.0);
    CHECK(estimator.standardErrorPpm() < 3.0);
}

MULTIROOM_TEST(drift_reports_a_larger_error_bar_for_more_jitter)
{
    DriftEstimator quiet(testSettings());
    DriftEstimator noisy(testSettings());

    ClockSimulator quietClock(48000, 48001.2, 20e-6, 7);
    ClockSimulator noisyClock(48000, 48001.2, 400e-6, 7);

    feed(quiet, quietClock, 30.0, 0.05);
    feed(noisy, noisyClock, 30.0, 0.05);

    // Both land on the same answer; only the stated precision differs.
    CHECK_NEAR(quiet.driftPpm(), 25.0, 2.0);
    CHECK_NEAR(noisy.driftPpm(), 25.0, 5.0);
    CHECK(noisy.standardErrorPpm() > quiet.standardErrorPpm());
    CHECK(noisy.confidence() < quiet.confidence());
}

MULTIROOM_TEST(drift_handles_negative_drift)
{
    // A card running slow must come out negative, not as an absolute value.
    ClockSimulator clock(48000, 47998.0, 50e-6, 4242);
    DriftEstimator estimator(testSettings());

    feed(estimator, clock, 30.0, 0.05);

    CHECK(estimator.hasEstimate());
    CHECK_NEAR(estimator.driftPpm(), -41.7, 2.0);
}

MULTIROOM_TEST(drift_reports_zero_for_a_perfect_clock)
{
    ClockSimulator clock(48000, 48000.0, 50e-6, 31337);
    DriftEstimator estimator(testSettings());

    feed(estimator, clock, 30.0, 0.05);

    CHECK(estimator.hasEstimate());
    CHECK_NEAR(estimator.driftPpm(), 0.0, 1.5);
}

MULTIROOM_TEST(drift_needs_samples_before_it_answers)
{
    ClockSimulator clock(48000, 48001.2, 50e-6, 55);
    DriftEstimator estimator(testSettings());

    // Nothing at all.
    CHECK(!estimator.hasEstimate());
    CHECK_EQ(estimator.driftPpm(), 0.0);
    CHECK_EQ(estimator.confidence(), 0.0);

    // Below minSamples there is still no answer, rather than a wild one.
    for (uint32_t i = 0; i < 4; ++i)
        estimator.addSample(clock.next(0.05));
    CHECK(!estimator.hasEstimate());

    feed(estimator, clock, 5.0, 0.05);
    CHECK(estimator.hasEstimate());
}

MULTIROOM_TEST(drift_confidence_grows_as_the_window_fills)
{
    ClockSimulator clock(48000, 48001.2, 50e-6, 616);
    DriftEstimator estimator(testSettings()); // 30 s window

    feed(estimator, clock, 3.0, 0.05);
    const double early = estimator.confidence();

    feed(estimator, clock, 27.0, 0.05);
    const double full = estimator.confidence();

    CHECK(early > 0.0);
    CHECK(full > early);
    CHECK(full <= 1.0);
    CHECK_NEAR(estimator.windowSpanSeconds(), 30.0, 1.0);
}

MULTIROOM_TEST(drift_confidence_follows_clock_quality)
{
    DriftEstimator hardware(testSettings());
    DriftEstimator estimated(testSettings());

    ClockSimulator hardwareClock(48000, 48001.2, 50e-6, 11);
    ClockSimulator estimatedClock(48000, 48001.2, 50e-6, 11);
    estimatedClock.setQuality(AudioClockQuality::Estimated);

    feed(hardware, hardwareClock, 30.0, 0.05);
    feed(estimated, estimatedClock, 30.0, 0.05);

    // Same numbers, different trust: a timestamp we derived ourselves cannot
    // count as much as one the hardware gave us.
    CHECK_NEAR(estimated.driftPpm(), hardware.driftPpm(), 0.001);
    CHECK(estimated.confidence() < hardware.confidence());
    CHECK(estimated.quality() == AudioClockQuality::Estimated);
}

MULTIROOM_TEST(drift_quality_recovers_once_a_worse_sample_expires)
{
    DriftEstimator estimator(testSettings()); // 30 s window
    ClockSimulator clock(48000, 48001.2, 50e-6, 21);

    clock.setQuality(AudioClockQuality::Estimated);
    feed(estimator, clock, 0.05, 0.05);
    clock.setQuality(AudioClockQuality::Driver);
    feed(estimator, clock, 60.0, 0.05);

    DriftEstimator reference(testSettings());
    ClockSimulator referenceClock(48000, 48001.2, 50e-6, 21);
    referenceClock.setQuality(AudioClockQuality::Driver);
    feed(reference, referenceClock, 60.05, 0.05);

    CHECK(estimator.quality() == AudioClockQuality::Driver);
    CHECK_NEAR(estimator.confidence(), reference.confidence(), 0.05);
}

MULTIROOM_TEST(drift_rejects_invalid_and_out_of_order_samples)
{
    ClockSimulator clock(48000, 48001.2, 50e-6, 22);
    DriftEstimator estimator(testSettings());
    feed(estimator, clock, 10.0, 0.05);

    const double before = estimator.driftPpm();
    const uint32_t countBefore = estimator.sampleCount();

    // A snapshot with no usable clock.
    AudioClockSnapshot invalid;
    CHECK(!estimator.addSample(invalid));

    // Time running backwards.
    AudioClockSnapshot backwards = clock.next(0.05);
    backwards.monotonicTimeNs -= 5000000000ULL;
    CHECK(!estimator.addSample(backwards));

    // Frame position running backwards.
    AudioClockSnapshot rewound = clock.next(0.05);
    rewound.deviceFramePosition = 1;
    CHECK(!estimator.addSample(rewound));

    CHECK_EQ(estimator.sampleCount(), countBefore);
    CHECK_NEAR(estimator.driftPpm(), before, 0.001);
    CHECK(estimator.rejectedCount() >= 3);
}

MULTIROOM_TEST(drift_survives_a_single_wild_outlier)
{
    ClockSimulator clock(48000, 48001.2, 50e-6, 8080);
    DriftEstimator estimator(testSettings());
    feed(estimator, clock, 20.0, 0.05);

    const double before = estimator.driftPpm();

    // One sample whose position is a quarter second off -- the shape of a
    // dropped buffer, not of scheduler jitter.
    AudioClockSnapshot wild = clock.next(0.05);
    wild.deviceFramePosition += 12000;
    CHECK(!estimator.addSample(wild));

    feed(estimator, clock, 5.0, 0.05);

    // The bad sample must not have moved the answer.
    CHECK_NEAR(estimator.driftPpm(), before, 2.0);
    CHECK_NEAR(estimator.driftPpm(), 25.0, 2.0);
}

MULTIROOM_TEST(drift_restarts_after_a_persistent_step)
{
    // A step that stays -- an xrun on ALSA loses frames permanently, so every
    // later sample is offset. The estimator must stop rejecting and rebuild
    // instead of defending a window that describes a clock that is gone.
    DriftEstimator::Settings settings = testSettings();
    settings.maxConsecutiveRejections = 5;
    DriftEstimator estimator(settings);

    ClockSimulator clock(48000, 48001.2, 50e-6, 4711);
    feed(estimator, clock, 20.0, 0.05);
    CHECK(estimator.hasEstimate());

    // Every sample from here on carries the same large offset.
    uint32_t acceptedDuringStep = 0;
    for (uint32_t i = 0; i < 8; ++i)
    {
        AudioClockSnapshot stepped = clock.next(0.05);
        stepped.deviceFramePosition += 48000; // a whole second of frames
        if (estimator.addSample(stepped))
            ++acceptedDuringStep;
    }

    // It gave up defending the old fit and started a new window.
    CHECK(acceptedDuringStep >= 1);
    CHECK(estimator.sampleCount() < 100);

    // And it converges again on the same physical clock.
    for (uint32_t i = 0; i < 600; ++i)
    {
        AudioClockSnapshot stepped = clock.next(0.05);
        stepped.deviceFramePosition += 48000;
        estimator.addSample(stepped);
    }
    CHECK(estimator.hasEstimate());
    CHECK_NEAR(estimator.driftPpm(), 25.0, 3.0);
}

MULTIROOM_TEST(drift_reset_clears_the_estimate)
{
    ClockSimulator clock(48000, 48001.2, 50e-6, 5150);
    DriftEstimator estimator(testSettings());
    feed(estimator, clock, 20.0, 0.05);
    CHECK(estimator.hasEstimate());

    estimator.reset();
    CHECK(!estimator.hasEstimate());
    CHECK_EQ(estimator.sampleCount(), static_cast<uint32_t>(0));
    CHECK_EQ(estimator.confidence(), 0.0);
    CHECK_EQ(estimator.driftPpm(), 0.0);

    // It must be usable again straight away.
    feed(estimator, clock, 20.0, 0.05);
    CHECK(estimator.hasEstimate());
    CHECK_NEAR(estimator.driftPpm(), 25.0, 3.0);
}

MULTIROOM_TEST(drift_resets_when_the_sample_rate_changes)
{
    DriftEstimator estimator(testSettings());

    ClockSimulator at48k(48000, 48001.2, 50e-6, 1);
    feed(estimator, at48k, 20.0, 0.05);
    CHECK(estimator.hasEstimate());

    // A reopen at a different rate is a different measurement entirely.
    ClockSimulator at44k(44100, 44100.5, 50e-6, 2);
    estimator.addSample(at44k.next(0.05));
    CHECK(!estimator.hasEstimate());

    feed(estimator, at44k, 20.0, 0.05);
    CHECK(estimator.hasEstimate());
    CHECK_NEAR(estimator.driftPpm(), 11.3, 3.0);
}

MULTIROOM_TEST(drift_window_is_bounded_and_allocation_free)
{
    DriftEstimator::Settings settings;
    settings.windowSeconds = 10.0;
    settings.capacity = 128;
    DriftEstimator estimator(settings);

    ClockSimulator clock(48000, 48001.2, 50e-6, 77);

    // Far more samples than the ring holds, and far longer than the window.
    feed(estimator, clock, 300.0, 0.05);

    // Neither the ring nor the window may have grown.
    CHECK(estimator.sampleCount() <= 128);
    CHECK(estimator.windowSpanSeconds() <= 10.5);
    CHECK(estimator.hasEstimate());

    // A shorter effective window still has to produce the right answer.
    CHECK_NEAR(estimator.driftPpm(), 25.0, 6.0);
}

MULTIROOM_TEST(drift_tracks_a_long_run_at_realistic_sampling)
{
    // What phase 13 will actually do: a snapshot every 500 ms for an hour.
    DriftEstimator::Settings settings;
    settings.windowSeconds = 20.0;
    settings.capacity = 256;
    DriftEstimator estimator(settings);

    ClockSimulator clock(48000, 48000.49, 80e-6, 909); // the +10.2 ppm measured here
    feed(estimator, clock, 3600.0, 0.5);

    CHECK(estimator.hasEstimate());
    CHECK_NEAR(estimator.driftPpm(), 10.2, 4.0);
    CHECK(estimator.confidence() > 0.0);
}
