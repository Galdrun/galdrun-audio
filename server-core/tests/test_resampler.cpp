#include "TestSupport.h"

#include "resampler/FakeResampler.h"

#include <cmath>
#include <vector>

using namespace multiroom;

namespace {

constexpr double kPi = 3.14159265358979323846;

ResamplerConfig config(uint32_t inputRate, uint32_t outputRate, uint16_t channels,
                       double initialRatio = 0.0)
{
    ResamplerConfig cfg;
    cfg.inputRate = inputRate;
    cfg.outputRate = outputRate;
    cfg.channels = channels;
    cfg.initialRatio = initialRatio;
    return cfg;
}

// A mono ramp, so linear interpolation is exact and every output sample is the
// input position it was taken from.
std::vector<double> ramp(size_t frames)
{
    std::vector<double> out(frames);
    for (size_t i = 0; i < frames; ++i)
        out[i] = static_cast<double>(i);
    return out;
}

} // namespace

MULTIROOM_TEST(resampler_config_ratio_is_output_over_input)
{
    // The one convention, stated in the roadmap and required to be documented
    // by MATP section 30. Getting it inverted sounds exactly like a correct
    // implementation running backwards, so it is asserted rather than assumed.
    CHECK_NEAR(config(44100, 48000, 2).nominalRatio(), 48000.0 / 44100.0, 1e-12);
    CHECK(config(44100, 48000, 2).nominalRatio() > 1.0);
    CHECK(config(48000, 44100, 2).nominalRatio() < 1.0);

    // An explicit ratio wins over the rates, because that is where the drift
    // trim lives; zero means "no trim".
    CHECK_NEAR(config(48000, 48000, 2).effectiveRatio(), 1.0, 1e-12);
    CHECK_NEAR(config(48000, 48000, 2, 1.000030).effectiveRatio(), 1.000030, 1e-12);
}

MULTIROOM_TEST(resampler_fake_declares_what_it_can_do)
{
    const ResamplerCapabilities& caps = FakeResampler::staticCapabilities();
    CHECK_EQ(caps.id, std::string("fake"));
    CHECK(caps.variableRatio);
    CHECK(caps.supportsRatio(1.0));
    CHECK(!caps.supportsRatio(64.0));
    CHECK(!caps.supportsRatio(0.0));
    CHECK(describeCapabilities(caps).find("fake") == 0);
}

MULTIROOM_TEST(resampler_fake_refuses_what_it_cannot_do_at_configure)
{
    FakeResampler resampler;
    CHECK(!resampler.configured());
    CHECK_EQ(resampler.ratio(), 0.0);
    CHECK_EQ(resampler.latencyFrames(), 0.0);

    CHECK_ERR(resampler.configure(config(0, 48000, 2)), ResamplerError::InvalidConfig);
    CHECK_ERR(resampler.configure(config(48000, 0, 2)), ResamplerError::InvalidConfig);
    CHECK_ERR(resampler.configure(config(48000, 48000, 0)), ResamplerError::InvalidConfig);

    // A capability mismatch is refused here and never mid-stream -- the
    // protocol is explicit that there is no silent fallback.
    CHECK_ERR(resampler.configure(config(48000, 48000, FakeResampler::kMaxChannels + 1)),
              ResamplerError::UnsupportedChannels);
    CHECK_ERR(resampler.configure(config(8000, 705600, 2)), ResamplerError::UnsupportedRatio);
    CHECK_ERR(resampler.configure(config(705600, 8000, 2)), ResamplerError::UnsupportedRatio);

    // And a refused configure leaves it unusable rather than half-configured.
    CHECK(!resampler.configured());
    std::vector<double> out(4);
    CHECK_ERR(resampler.process({}, out), ResamplerError::NotConfigured);
    CHECK_ERR(resampler.setRatio(1.0), ResamplerError::NotConfigured);
}

MULTIROOM_TEST(resampler_fake_at_ratio_one_is_an_identity)
{
    // Not the bitperfect path -- that is phase 6 refusing to build a resampler
    // at all -- but if a ratio of one lost or altered a frame here, nothing
    // built on top of the frame accounting would be trustworthy.
    FakeResampler resampler;
    CHECK_OK(resampler.configure(config(48000, 48000, 2)));
    CHECK(resampler.configured());
    CHECK_NEAR(resampler.ratio(), 1.0, 1e-12);

    const std::vector<double> input{1.0, -1.0, 2.0, -2.0, 3.0, -3.0, 4.0, -4.0};
    std::vector<double> output(input.size(), 0.0);

    const auto result = resampler.process(input, output);
    CHECK_OK(result);
    CHECK_EQ(result->inputFramesConsumed, size_t{4});
    CHECK_EQ(result->outputFramesProduced, size_t{4});
    for (size_t i = 0; i < input.size(); ++i)
        CHECK_EQ(output[i], input[i]);
}

MULTIROOM_TEST(resampler_fake_fixed_ratio_over_a_known_signal)
{
    // Output frame k is the input at position k / ratio. A ramp makes that
    // readable: at a ratio of 2 the output is the half-integers.
    FakeResampler resampler;
    CHECK_OK(resampler.configure(config(24000, 48000, 1)));
    CHECK_NEAR(resampler.ratio(), 2.0, 1e-12);

    const std::vector<double> input = ramp(8);
    std::vector<double> output(32, -1.0);

    const auto result = resampler.process(input, output);
    CHECK_OK(result);

    // Frames 0..7 are available. Output frame 14 lands exactly on input 7 and
    // needs no frame after it, so it is produced; frame 15 would sit at 7.5
    // and is not. The last input frame is therefore read but not consumed --
    // the caller re-presents it, and that one frame is the whole of the
    // resampler's latency.
    CHECK_EQ(result->outputFramesProduced, size_t{15});
    CHECK_EQ(result->inputFramesConsumed, size_t{7});
    for (size_t k = 0; k < result->outputFramesProduced; ++k)
        CHECK_NEAR(output[k], static_cast<double>(k) / 2.0, 1e-12);
}

MULTIROOM_TEST(resampler_fake_keeps_a_sine_where_it_was)
{
    // The ramp says the accounting is right; this says the samples are audio
    // and not an index. A 50 Hz tone is far enough below the interpolator's
    // usable band that its own poor quality does not enter into it.
    FakeResampler resampler;
    CHECK_OK(resampler.configure(config(24000, 48000, 1)));

    std::vector<double> sine(2400);
    for (size_t i = 0; i < sine.size(); ++i)
        sine[i] = std::sin(2.0 * kPi * 50.0 * static_cast<double>(i) / 24000.0);

    std::vector<double> output(8192, 0.0);
    const auto result = resampler.process(sine, output);
    CHECK_OK(result);
    CHECK(result->outputFramesProduced > 4700);
    for (size_t k = 0; k < result->outputFramesProduced; ++k)
    {
        const double expected = std::sin(2.0 * kPi * 50.0 * static_cast<double>(k) / 48000.0);
        CHECK_NEAR(output[k], expected, 1e-4);
    }
}

MULTIROOM_TEST(resampler_fake_push_pull_never_assumes_out_equals_in_times_ratio)
{
    // The contract that keeps a stream from developing a hole every few
    // seconds: a call is told what it consumed and what it produced, and the
    // caller re-presents the rest. Here the output view is deliberately far
    // too small for the input.
    FakeResampler resampler;
    CHECK_OK(resampler.configure(config(44100, 48000, 2)));

    constexpr size_t kTotalInputFrames = 4410;
    std::vector<double> input(kTotalInputFrames * 2);
    for (size_t i = 0; i < kTotalInputFrames; ++i)
    {
        input[i * 2] = static_cast<double>(i);
        input[i * 2 + 1] = -static_cast<double>(i);
    }

    std::vector<double> output(7 * 2); // seven frames at a time, a hostile number
    size_t offset = 0;
    size_t produced = 0;
    size_t calls = 0;
    while (offset < kTotalInputFrames && calls < 10000)
    {
        const std::span<const double> remaining(input.data() + offset * 2,
                                                (kTotalInputFrames - offset) * 2);
        const auto result = resampler.process(remaining, output);
        CHECK_OK(result);
        if (result->inputFramesConsumed == 0 && result->outputFramesProduced == 0)
            break;

        // Every produced frame is the ramp read at its own input position, so
        // a duplicated or skipped frame across a call boundary is visible.
        for (size_t k = 0; k < result->outputFramesProduced; ++k)
        {
            const double expected = static_cast<double>(produced + k) * 44100.0 / 48000.0;
            CHECK_NEAR(output[k * 2], expected, 1e-9);
            CHECK_NEAR(output[k * 2 + 1], -expected, 1e-9);
        }

        offset += result->inputFramesConsumed;
        produced += result->outputFramesProduced;
        ++calls;
    }

    CHECK_EQ(produced, static_cast<size_t>(resampler.outputFramesProduced()));
    CHECK_EQ(offset, static_cast<size_t>(resampler.inputFramesConsumed()));

    // 4410 input frames at 44.1 -> 48 kHz is 100 ms, which is 4800 output
    // frames of timeline. 4799 arrive: the last one needs an input frame that
    // has not been handed over yet, and holding it back rather than
    // extrapolating is what a converter does at the end of every buffer.
    // Splitting the input into 630 seven-frame calls changes none of that,
    // which is the claim this test is making.
    CHECK_EQ(produced, size_t{4799});
    CHECK(calls > 600);
}

MULTIROOM_TEST(resampler_ratio_convention_a_fast_client_gets_more_frames)
{
    // The roadmap's assertion, verbatim: a ratio of 1.000030 over one second
    // of 48 kHz input produces 48,001 or 48,002 frames, not 47,998. A client
    // whose card runs +30 ppm fast consumes frames too quickly and needs
    // *more* of them for the same span of timeline (MATP section 28).
    FakeResampler resampler;
    CHECK_OK(resampler.configure(config(48000, 48000, 1, 1.000030)));

    const std::vector<double> input = ramp(48000);
    std::vector<double> output(49152, 0.0);

    const auto result = resampler.process(input, output);
    CHECK_OK(result);
    CHECK(result->outputFramesProduced >= 48001);
    CHECK(result->outputFramesProduced <= 48002);

    // The inverse convention would have produced fewer, which is the failure
    // this test exists to catch: a room converging away from the timeline.
    FakeResampler slow;
    CHECK_OK(slow.configure(config(48000, 48000, 1, 1.0 / 1.000030)));
    const auto slowResult = slow.process(input, output);
    CHECK_OK(slowResult);
    CHECK(slowResult->outputFramesProduced < 48000);
}

MULTIROOM_TEST(resampler_fake_variable_ratio_changes_mid_stream_exactly)
{
    // The drift path in miniature. The fake applies a new ratio to the next
    // output frame with no transition, so the position of every frame after
    // the change is arithmetic rather than an approximation.
    FakeResampler resampler;
    CHECK_OK(resampler.configure(config(48000, 48000, 1)));

    const std::vector<double> input = ramp(1000);
    std::vector<double> output(64, 0.0);

    const auto first = resampler.process(std::span<const double>(input.data(), 100), output);
    CHECK_OK(first);
    CHECK_EQ(first->outputFramesProduced, size_t{64});
    for (size_t k = 0; k < 64; ++k)
        CHECK_NEAR(output[k], static_cast<double>(k), 1e-12);

    // Halve the ratio. The stream position is preserved, so the next output
    // frame continues from where the last one left off, two input frames on.
    CHECK_OK(resampler.setRatio(0.5));
    CHECK_NEAR(resampler.ratio(), 0.5, 1e-12);

    const size_t consumed = first->inputFramesConsumed;
    const auto second = resampler.process(
        std::span<const double>(input.data() + consumed, 100 - consumed), output);
    CHECK_OK(second);
    CHECK(second->outputFramesProduced > 0);
    for (size_t k = 0; k < second->outputFramesProduced; ++k)
        CHECK_NEAR(output[k], 64.0 + static_cast<double>(k) * 2.0, 1e-12);

    // Out of range is refused and the working ratio is left alone.
    CHECK_ERR(resampler.setRatio(100.0), ResamplerError::UnsupportedRatio);
    CHECK_NEAR(resampler.ratio(), 0.5, 1e-12);
}

MULTIROOM_TEST(resampler_fake_reset_clears_state_but_keeps_the_configuration)
{
    FakeResampler resampler;
    CHECK_OK(resampler.configure(config(24000, 48000, 1)));

    const std::vector<double> input = ramp(64);
    std::vector<double> first(128, 0.0);
    const auto before = resampler.process(input, first);
    CHECK_OK(before);
    CHECK(resampler.outputFramesProduced() > 0);

    resampler.reset();
    CHECK(resampler.configured());
    CHECK_NEAR(resampler.ratio(), 2.0, 1e-12);
    CHECK_EQ(resampler.inputFramesConsumed(), uint64_t{0});
    CHECK_EQ(resampler.outputFramesProduced(), uint64_t{0});

    // The same input through a reset resampler is the same output, frame for
    // frame -- which is what "the filter history is gone" has to mean.
    std::vector<double> second(128, 0.0);
    const auto after = resampler.process(input, second);
    CHECK_OK(after);
    CHECK_EQ(after->inputFramesConsumed, before->inputFramesConsumed);
    CHECK_EQ(after->outputFramesProduced, before->outputFramesProduced);
    CHECK(second == first);
}

MULTIROOM_TEST(resampler_config_carries_the_callers_need_for_a_variable_ratio)
{
    // Two different questions, and the interface keeps them apart:
    // ResamplerCapabilities::variableRatio is what an implementation can do,
    // ResamplerConfig::requiresVariableRatio is what this client needs. False
    // by default, because most rooms are not being drift-corrected.
    ResamplerConfig cfg = config(44100, 48000, 2);
    CHECK(!cfg.requiresVariableRatio);
    CHECK(cfg.isValid());

    cfg.requiresVariableRatio = true;
    CHECK(cfg.isValid());

    // The fake has one engine and honours setRatio either way -- an
    // implementation with two is free to build the fixed one and refuse, which
    // is what SoxrResampler does and what test_real_resamplers.cpp covers.
    FakeResampler resampler;
    CHECK_OK(resampler.configure(config(44100, 48000, 2)));
    CHECK_OK(resampler.setRatio(resampler.ratio() * 1.000030));
}

MULTIROOM_TEST(resampler_fake_reports_latency_in_output_frames)
{
    // In output frames and a double, because phase 11 places a scheduled start
    // with it and rounding here throws away the part that matters.
    FakeResampler resampler;
    CHECK_OK(resampler.configure(config(24000, 48000, 1)));
    CHECK_NEAR(resampler.latencyFrames(), 2.0, 1e-12);

    CHECK_OK(resampler.setRatio(1.000030));
    CHECK_NEAR(resampler.latencyFrames(), 1.000030, 1e-12);
    CHECK(resampler.latencyFrames() != 1.0); // the ppm survives, unrounded
}

MULTIROOM_TEST(resampler_fake_rejects_a_span_that_is_not_whole_frames)
{
    FakeResampler resampler;
    CHECK_OK(resampler.configure(config(48000, 48000, 2)));

    const std::vector<double> odd(5, 0.0);
    std::vector<double> output(8, 0.0);
    CHECK_ERR(resampler.process(odd, output), ResamplerError::MisalignedBuffer);

    const std::vector<double> input(8, 0.0);
    std::vector<double> oddOutput(7, 0.0);
    CHECK_ERR(resampler.process(input, oddOutput), ResamplerError::MisalignedBuffer);
}

MULTIROOM_TEST(resampler_fake_handles_an_empty_call)
{
    // A zone with nothing decoded yet calls this. It must be a no-op, not a
    // failure and not a frame of silence inserted into the stream.
    FakeResampler resampler;
    CHECK_OK(resampler.configure(config(48000, 48000, 2)));

    std::vector<double> output(8, 0.0);
    const auto result = resampler.process({}, output);
    CHECK_OK(result);
    CHECK_EQ(result->inputFramesConsumed, size_t{0});
    CHECK_EQ(result->outputFramesProduced, size_t{0});
}

MULTIROOM_TEST(resampler_fake_keeps_channels_apart)
{
    // Interleaved, and an off-by-one in the stride is a channel swap that a
    // mono test cannot see.
    FakeResampler resampler;
    CHECK_OK(resampler.configure(config(24000, 48000, 3)));

    std::vector<double> input(8 * 3);
    for (size_t frame = 0; frame < 8; ++frame)
    {
        input[frame * 3] = static_cast<double>(frame);
        input[frame * 3 + 1] = 100.0 + static_cast<double>(frame);
        input[frame * 3 + 2] = 200.0 + static_cast<double>(frame);
    }

    std::vector<double> output(64 * 3, 0.0);
    const auto result = resampler.process(input, output);
    CHECK_OK(result);
    CHECK_EQ(result->outputFramesProduced, size_t{15});
    for (size_t k = 0; k < result->outputFramesProduced; ++k)
    {
        const double position = static_cast<double>(k) / 2.0;
        CHECK_NEAR(output[k * 3], position, 1e-12);
        CHECK_NEAR(output[k * 3 + 1], 100.0 + position, 1e-12);
        CHECK_NEAR(output[k * 3 + 2], 200.0 + position, 1e-12);
    }
}

MULTIROOM_TEST(resampler_fake_reconfigure_re_points_an_existing_path)
{
    // The format transition of phase 15: the same output path is re-pointed at
    // a new rate rather than torn down, so configure() has to be callable
    // twice and has to leave nothing of the first stream behind.
    FakeResampler resampler;
    CHECK_OK(resampler.configure(config(24000, 48000, 1)));

    const std::vector<double> input = ramp(64);
    std::vector<double> output(256, 0.0);
    CHECK_OK(resampler.process(input, output));
    CHECK(resampler.inputFramesConsumed() > 0);

    CHECK_OK(resampler.configure(config(48000, 96000, 2)));
    CHECK_NEAR(resampler.ratio(), 2.0, 1e-12);
    CHECK_EQ(resampler.inputFramesConsumed(), uint64_t{0});
    CHECK_EQ(resampler.outputFramesProduced(), uint64_t{0});

    // Two channels now, so the previous mono span is no longer well formed.
    const std::vector<double> odd(9, 0.0);
    CHECK_ERR(resampler.process(odd, output), ResamplerError::MisalignedBuffer);
}
