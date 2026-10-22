#include "TestSupport.h"

#include "core/Units.h"

#include <limits>
#include <type_traits>
#include <utility>

using namespace multiroom;

MULTIROOM_TEST(units_frames_from_nanoseconds_at_whole_number_rates)
{
    // 48 kHz divides a nanosecond span evenly enough to be checked by hand.
    CHECK_EQ(framesFromNanoseconds(0, 48000).value(), 0u);
    CHECK_EQ(framesFromNanoseconds(kNanosecondsPerSecond, 48000).value(), 48000u);
    CHECK_EQ(framesFromNanoseconds(20'000'000, 48000).value(), 960u);
    CHECK_EQ(framesFromNanoseconds(20'000'000, 96000).value(), 1920u);

    // A packet duration is a duration, never a frame count: one value covers
    // every room, and the frame count differs per rate.
    CHECK_EQ(framesFromNanoseconds(20'000'000, 44100).value(), 882u);
}

MULTIROOM_TEST(units_mapping_is_exact_at_44100_where_a_frame_is_not_whole_nanoseconds)
{
    // One frame at 44.1 kHz is 22675.7369... ns. Neither direction can be
    // lossless on its own, so what is asserted here is the invariant that
    // makes the pair usable: nanoseconds->frames->nanoseconds returns the
    // frame it started from, for every frame.
    constexpr uint32_t kRate = 44100;

    CHECK_EQ(nanosecondsFromFrames(1, kRate).value(), 22676u); // 22675.73..., rounded up
    CHECK_EQ(nanosecondsFromFrames(2, kRate).value(), 45352u); // 45351.47..., rounded up

    // Rounding to nearest would have given 45351 here, which converts back to
    // frame 1 -- the exact failure the ceiling rule exists to prevent.
    CHECK_EQ(framesFromNanoseconds(45351, kRate).value(), 1u);
    CHECK_EQ(framesFromNanoseconds(45352, kRate).value(), 2u);

    for (uint64_t frame = 0; frame < 5000; ++frame)
    {
        const auto ns = nanosecondsFromFrames(frame, kRate);
        CHECK_OK(ns);
        const auto back = framesFromNanoseconds(*ns, kRate);
        CHECK_OK(back);
        CHECK_EQ(*back, frame);
    }

    // And at a rate that divides evenly, and one that does not divide at all.
    for (const uint32_t rate : {8000u, 44100u, 48000u, 96000u, 192000u, 705600u})
    {
        for (const uint64_t frame : {0ULL, 1ULL, 881ULL, 882ULL, 1'000'000ULL,
                                     3'600ULL * rate, 86'400ULL * rate})
        {
            const auto ns = nanosecondsFromFrames(frame, rate);
            CHECK_OK(ns);
            const auto back = framesFromNanoseconds(*ns, rate);
            CHECK_OK(back);
            CHECK_EQ(*back, frame);
        }
    }
}

MULTIROOM_TEST(units_mapping_is_absolute_and_never_accumulated)
{
    // A sender that adds up rounded 20 ms durations drifts away from the true
    // position; the client reads that as discontinuous and rejoins forever.
    // Converting from the origin every time does not drift.
    constexpr uint32_t kRate = 44100;

    // 20 ms happens to be a whole 882 frames at 44.1 kHz, so it hides the
    // problem. 7 ms is 308.7 frames and shows it: the rounded step is 308.
    constexpr uint64_t kStepNs = 7'000'000;
    const uint64_t perStep = framesFromNanoseconds(kStepNs, kRate).value();
    CHECK_EQ(perStep, 308u);

    uint64_t accumulated = 0;
    for (uint64_t step = 1; step <= 100; ++step)
        accumulated += perStep;

    const uint64_t absolute = framesFromNanoseconds(100 * kStepNs, kRate).value();
    CHECK_EQ(absolute, 30'870u);
    CHECK_EQ(accumulated, 30'800u);
    CHECK(absolute > accumulated); // 70 frames, 1.6 ms adrift after 700 ms
}

MULTIROOM_TEST(units_reject_a_zero_sample_rate)
{
    CHECK_ERR(framesFromNanoseconds(1'000'000, 0), FrameMathError::InvalidSampleRate);
    CHECK_ERR(nanosecondsFromFrames(1000, 0), FrameMathError::InvalidSampleRate);
}

MULTIROOM_TEST(units_report_overflow_rather_than_wrapping)
{
    constexpr uint64_t kMax = std::numeric_limits<uint64_t>::max();

    // Every position a 64-bit nanosecond timeline can express converts to
    // frames: 584 years at 768 kHz still fits.
    CHECK_OK(framesFromNanoseconds(kMax, 768000));
    CHECK_EQ(framesFromNanoseconds(kMax, 1).value(), 18'446'744'073ULL);

    // The other direction runs out first, because a frame is worth more than a
    // nanosecond. 2^64 frames at 44.1 kHz is 13 million years.
    CHECK_ERR(nanosecondsFromFrames(kMax, 44100), FrameMathError::Overflow);
    CHECK_OK(nanosecondsFromFrames(kMax / 1'000'000'000ULL * 44100, 44100));
}

MULTIROOM_TEST(units_axes_are_distinct_types)
{
    // The point of the opaque enums: a value is read out deliberately, and the
    // three axes never convert into one another by accident.
    const TimelineNs position{180'000'000'000ULL};
    const SourceFrame source{7'938'000ULL};
    const OutputFrame output{8'640'000ULL};

    CHECK_EQ(std::to_underlying(position), 180'000'000'000ULL);
    CHECK_EQ(std::to_underlying(source), 7'938'000ULL);
    CHECK_EQ(std::to_underlying(output), 8'640'000ULL);

    static_assert(!std::is_convertible_v<SourceFrame, OutputFrame>);
    static_assert(!std::is_convertible_v<TimelineNs, uint64_t>);
    static_assert(!std::is_convertible_v<uint64_t, OutputFrame>);
    static_assert(sizeof(TimelineNs) == sizeof(uint64_t));

    // The same media position on two rooms' own axes: 180 s of a 44.1 kHz
    // source is frame 7,938,000 for the room playing it untouched and frame
    // 8,640,000 for the room resampled to 48 kHz.
    CHECK_EQ(framesFromNanoseconds(std::to_underlying(position), 44100).value(),
             std::to_underlying(source));
    CHECK_EQ(framesFromNanoseconds(std::to_underlying(position), 48000).value(),
             std::to_underlying(output));
}
