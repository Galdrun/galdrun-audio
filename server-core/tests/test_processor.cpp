#include "TestSupport.h"

#include "processing/DirectPcmProcessor.h"
#include "processing/IClientAudioProcessor.h"
#include "processing/ResamplingProcessor.h"
#include "resampler/FakeResampler.h"
#include "resampler/ResamplerRegistry.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <vector>

using namespace multiroom;

// The processor is where phase 6's plan stops being data, so the tests below
// are about two claims. The first is that the direct path changes nothing but
// a layout: every allowed pair is checked value by value, with the bytes
// written and read by this file rather than by the code under test, because a
// reader that mirrors the writer's mistake agrees with it. The second is that
// the resampled path loses and duplicates nothing, which is checked against a
// bare FakeResampler driven with the same audio -- the processor may convert
// the samples, but it may not change the converter's accounting.

namespace {

// --- layout encoding, written out here so the test is an oracle --------------

void writeLe(std::vector<std::byte>& out, size_t offset, uint32_t bits, size_t count)
{
    for (size_t i = 0; i < count; ++i)
        out[offset + i] = static_cast<std::byte>((bits >> (8 * i)) & 0xFFu);
}

uint32_t readLe(const std::vector<std::byte>& in, size_t offset, size_t count)
{
    uint32_t bits = 0;
    for (size_t i = 0; i < count; ++i)
        bits |= static_cast<uint32_t>(static_cast<uint8_t>(in[offset + i])) << (8 * i);
    return bits;
}

void putInteger(std::vector<std::byte>& out, size_t index, AudioFormat::SampleType type,
                int32_t value)
{
    const size_t bytes = AudioFormat::bytesPerSample(type);
    writeLe(out, index * bytes, static_cast<uint32_t>(value), bytes);
}

// Sign-extended from the layout's own valid width, so a 24-bit value in four
// bytes reads the same whether the top byte carries the sign or padding.
int32_t getInteger(const std::vector<std::byte>& in, size_t index, AudioFormat::SampleType type)
{
    const size_t bytes = AudioFormat::bytesPerSample(type);
    const uint32_t valid = AudioFormat::validBits(type);
    uint32_t bits = readLe(in, index * bytes, bytes);
    if (valid < 32)
    {
        const uint32_t mask = (1u << valid) - 1;
        bits &= mask;
        if ((bits & (1u << (valid - 1))) != 0)
            bits |= ~mask;
    }
    return static_cast<int32_t>(bits);
}

void putFloat(std::vector<std::byte>& out, size_t index, float value)
{
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    writeLe(out, index * 4, bits, 4);
}

float getFloat(const std::vector<std::byte>& in, size_t index)
{
    const uint32_t bits = readLe(in, index * 4, 4);
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

// --- plans -------------------------------------------------------------------

AudioFormat format(uint32_t rate, uint16_t channels, AudioFormat::SampleType type)
{
    AudioFormat out;
    out.sampleRate = rate;
    out.channels = channels;
    out.sampleType = type;
    return out;
}

OutputPlan bitperfectPlan(const AudioFormat& source, const AudioFormat& output)
{
    OutputPlan plan;
    plan.mode = OutputMode::BitPerfect;
    plan.sourceFormat = source;
    plan.outputFormat = output;
    plan.reason = source.sampleType == output.sampleType ? PlanReason::SourceFormatOffered
                                                         : PlanReason::SourceFormatRepacked;
    return plan;
}

OutputPlan resampledPlan(const AudioFormat& source, const AudioFormat& output, bool corrected)
{
    OutputPlan plan;
    plan.mode = OutputMode::Resampled;
    plan.sourceFormat = source;
    plan.outputFormat = output;
    plan.driftActuator = corrected ? DriftActuator::ServerResampling : DriftActuator::None;
    plan.resamplerId = "fake";
    plan.reason = corrected ? PlanReason::Grouped : PlanReason::FormatNotOffered;
    return plan;
}

// --- a registry that counts what it builds ------------------------------------

class CountingFactory final : public IResamplerFactory
{
public:
    explicit CountingFactory(int* creations) : creations_(creations) {}

    const ResamplerCapabilities& capabilities() const noexcept override
    {
        return FakeResampler::staticCapabilities();
    }

    std::unique_ptr<IAudioResampler> create() const override
    {
        ++(*creations_);
        return std::make_unique<FakeResampler>();
    }

private:
    int* creations_;
};

// The fake converts, so the registry it is in can be used by every case here
// and still count what was built. The server never registers it; that rule is
// tested where registration lives.
void addFake(ResamplerRegistry& registry, int* creations)
{
    CHECK_OK(registry.add(std::make_unique<CountingFactory>(creations)));
}

constexpr uint32_t kRate = 44100;
constexpr uint16_t kChannels = 2;

const AudioFormat::SampleType kIntegerTypes[] = {
    AudioFormat::SampleType::Int16, AudioFormat::SampleType::Int24,
    AudioFormat::SampleType::Int24In32, AudioFormat::SampleType::Int32};

// Zero, both ends of the range and the two values either side of zero, plus
// one value whose bytes differ from each other so that a transposition inside
// a container is visible. Built for the width the layout actually carries.
std::vector<int32_t> testValues(AudioFormat::SampleType type)
{
    const uint32_t bits = AudioFormat::validBits(type);
    const int32_t high = static_cast<int32_t>((1u << (bits - 1)) - 1);
    const int32_t low = -high - 1;
    return {0, 1, -1, high, low, high / 3, low / 3, high / 7};
}

} // namespace

// --- the direct path ---------------------------------------------------------

MULTIROOM_TEST(the_direct_path_preserves_every_value_in_every_allowed_layout_pair)
{
    for (const AudioFormat::SampleType from : kIntegerTypes)
    {
        for (const AudioFormat::SampleType to : kIntegerTypes)
        {
            if (!isValuePreserving(from, to))
                continue;

            const std::vector<int32_t> values = testValues(from);
            const size_t samples = values.size();

            std::vector<std::byte> input(samples * AudioFormat::bytesPerSample(from));
            for (size_t i = 0; i < samples; ++i)
                putInteger(input, i, from, values[i]);

            // One channel, so a sample is a frame and the pair under test is
            // the only variable.
            const OutputPlan plan =
                bitperfectPlan(format(kRate, 1, from), format(kRate, 1, to));
            auto processor = DirectPcmProcessor::create(plan);
            CHECK_OK(processor);

            std::vector<std::byte> output(samples * AudioFormat::bytesPerSample(to));
            auto result = (*processor)->process(input, output);
            CHECK_OK(result);
            CHECK_EQ(result->inputFramesConsumed, samples);
            CHECK_EQ(result->outputFramesProduced, samples);

            // The level is preserved, not the integer: a narrower source moves
            // up into the wider container rather than sitting at the bottom of
            // it, which is what makes the pair reversible.
            const uint32_t shift =
                AudioFormat::validBits(to) - AudioFormat::validBits(from);
            for (size_t i = 0; i < samples; ++i)
            {
                const int32_t expected =
                    static_cast<int32_t>(static_cast<uint32_t>(values[i]) << shift);
                CHECK_EQ(getInteger(output, i, to), expected);
            }
        }
    }
}

MULTIROOM_TEST(a_negative_24_bit_value_repacked_into_four_bytes_carries_its_sign_in_the_top_byte)
{
    const OutputPlan plan =
        bitperfectPlan(format(kRate, 1, AudioFormat::SampleType::Int24),
                       format(kRate, 1, AudioFormat::SampleType::Int24In32));
    auto processor = DirectPcmProcessor::create(plan);
    CHECK_OK(processor);

    std::vector<std::byte> input(3);
    putInteger(input, 0, AudioFormat::SampleType::Int24, -1234567);

    std::vector<std::byte> output(4);
    CHECK_OK((*processor)->process(input, output));

    // The decoder produces Int24In32 by shifting a left-aligned S32 right by
    // eight arithmetically, so the top byte of a negative sample is 0xFF
    // there; the repack has to agree with it or the pair stops being a memcpy
    // when nothing changed.
    CHECK_EQ(static_cast<int>(static_cast<uint8_t>(output[3])), 0xFF);
    CHECK_EQ(getInteger(output, 0, AudioFormat::SampleType::Int24In32), -1234567);

    // And back again, which is the other half of "value preserving".
    const OutputPlan back =
        bitperfectPlan(format(kRate, 1, AudioFormat::SampleType::Int24In32),
                       format(kRate, 1, AudioFormat::SampleType::Int24));
    auto reverse = DirectPcmProcessor::create(back);
    CHECK_OK(reverse);

    std::vector<std::byte> round(3);
    CHECK_OK((*reverse)->process(output, round));
    CHECK_EQ(getInteger(round, 0, AudioFormat::SampleType::Int24), -1234567);
}

MULTIROOM_TEST(the_direct_path_reads_int24in32_padding_of_either_convention)
{
    const OutputPlan plan =
        bitperfectPlan(format(kRate, 1, AudioFormat::SampleType::Int24In32),
                       format(kRate, 1, AudioFormat::SampleType::Int24));
    auto processor = DirectPcmProcessor::create(plan);
    CHECK_OK(processor);

    // The same negative 24-bit value, once sign-filled and once zero-padded.
    // ALSA calls the fourth byte padding and does not say what is in it, so a
    // room's audio must not depend on which the far end chose.
    std::vector<std::byte> input(8);
    writeLe(input, 0, 0xFFED2979u, 4);
    writeLe(input, 4, 0x00ED2979u, 4);

    std::vector<std::byte> output(6);
    CHECK_OK((*processor)->process(input, output));
    CHECK_EQ(getInteger(output, 0, AudioFormat::SampleType::Int24),
             getInteger(output, 1, AudioFormat::SampleType::Int24));
}

MULTIROOM_TEST(identical_layouts_reach_the_room_byte_for_byte)
{
    for (const AudioFormat::SampleType type :
         {AudioFormat::SampleType::Int16, AudioFormat::SampleType::Int24,
          AudioFormat::SampleType::Int24In32, AudioFormat::SampleType::Int32,
          AudioFormat::SampleType::Float32})
    {
        const AudioFormat same = format(kRate, kChannels, type);
        auto processor = DirectPcmProcessor::create(bitperfectPlan(same, same));
        CHECK_OK(processor);

        const size_t frames = 64;
        std::vector<std::byte> input(frames * same.bytesPerFrame());
        for (size_t i = 0; i < input.size(); ++i)
            input[i] = static_cast<std::byte>((i * 37 + 11) & 0xFF);

        std::vector<std::byte> output(input.size());
        auto result = (*processor)->process(input, output);
        CHECK_OK(result);
        CHECK_EQ(result->outputFramesProduced, frames);
        CHECK(std::memcmp(input.data(), output.data(), input.size()) == 0);
    }
}

MULTIROOM_TEST(the_direct_path_refuses_anything_that_is_not_a_layout_change)
{
    const AudioFormat wide = format(kRate, kChannels, AudioFormat::SampleType::Int32);
    const AudioFormat narrow = format(kRate, kChannels, AudioFormat::SampleType::Int16);
    const AudioFormat floating = format(kRate, kChannels, AudioFormat::SampleType::Float32);

    CHECK_ERR(DirectPcmProcessor::create(bitperfectPlan(wide, narrow)),
              ProcessorError::UnsupportedConversion);
    CHECK_ERR(DirectPcmProcessor::create(bitperfectPlan(wide, floating)),
              ProcessorError::UnsupportedConversion);
    CHECK_ERR(DirectPcmProcessor::create(bitperfectPlan(floating, wide)),
              ProcessorError::UnsupportedConversion);

    CHECK_ERR(DirectPcmProcessor::create(
                  bitperfectPlan(wide, format(48000, kChannels, AudioFormat::SampleType::Int32))),
              ProcessorError::InvalidPlan);
    CHECK_ERR(DirectPcmProcessor::create(
                  bitperfectPlan(wide, format(kRate, 1, AudioFormat::SampleType::Int32))),
              ProcessorError::InvalidPlan);
    CHECK_ERR(DirectPcmProcessor::create(resampledPlan(wide, wide, false)),
              ProcessorError::InvalidPlan);

    // A bitperfect plan that claims a correction is not a room, it is a plan
    // asking a path with no converter to trim a ratio.
    OutputPlan corrected = bitperfectPlan(wide, wide);
    corrected.driftActuator = DriftActuator::ServerResampling;
    CHECK_ERR(DirectPcmProcessor::create(corrected), ProcessorError::InvalidPlan);
}

MULTIROOM_TEST(a_bitperfect_room_never_has_a_ratio_to_set)
{
    const AudioFormat same = format(kRate, kChannels, AudioFormat::SampleType::Int24);
    int creations = 0;
    ResamplerRegistry registry;
    addFake(registry, &creations);

    auto processor = makeClientAudioProcessor(bitperfectPlan(same, same), registry);
    CHECK_OK(processor);
    CHECK_EQ((*processor)->ratio(), 1.0);
    CHECK_EQ((*processor)->latencyFrames(), 0.0);
    CHECK_ERR((*processor)->setRatio(1.000030), ProcessorError::RatioNotAdjustable);

    // Criterion 5, where it finally means something: this is the phase that
    // constructs converters, and a bitperfect plan does not reach the
    // registry at all.
    CHECK_EQ(creations, 0);
}

// --- the resampled path -------------------------------------------------------

MULTIROOM_TEST(a_resampled_room_builds_exactly_one_converter_and_a_corrected_one_can_be_trimmed)
{
    int creations = 0;
    ResamplerRegistry registry;
    addFake(registry, &creations);

    const AudioFormat source = format(kRate, kChannels, AudioFormat::SampleType::Int16);
    const AudioFormat output = format(48000, kChannels, AudioFormat::SampleType::Int16);

    auto uncorrected = makeClientAudioProcessor(resampledPlan(source, output, false), registry);
    CHECK_OK(uncorrected);
    CHECK_EQ(creations, 1);
    CHECK_NEAR((*uncorrected)->ratio(), 48000.0 / 44100.0, 1e-12);

    // The engine was chosen by the plan's drift flag, so a trim arriving at a
    // room that was never planned for one is a controller talking to the wrong
    // output rather than a value to accept.
    CHECK_ERR((*uncorrected)->setRatio(1.000030), ProcessorError::RatioNotAdjustable);

    auto corrected = makeClientAudioProcessor(resampledPlan(source, output, true), registry);
    CHECK_OK(corrected);
    CHECK_EQ(creations, 2);
    CHECK_OK((*corrected)->setRatio(48000.0 / 44100.0 * 1.000030));
    CHECK_NEAR((*corrected)->ratio(), 48000.0 / 44100.0 * 1.000030, 1e-12);
}

MULTIROOM_TEST(a_depth_change_at_one_rate_builds_no_converter_and_agrees_with_the_direct_path)
{
    // A resampled plan whose ratio can never move is a change of depth and not
    // a filter. It stays resampled -- the samples are not the source's any
    // more -- but a converter would only add its own passband and latency.
    int creations = 0;
    ResamplerRegistry registry;
    addFake(registry, &creations);

    for (const AudioFormat::SampleType from : kIntegerTypes)
    {
        for (const AudioFormat::SampleType to : kIntegerTypes)
        {
            if (!isValuePreserving(from, to))
                continue;

            const std::vector<int32_t> values = testValues(from);
            const size_t samples = values.size();
            std::vector<std::byte> input(samples * AudioFormat::bytesPerSample(from));
            for (size_t i = 0; i < samples; ++i)
                putInteger(input, i, from, values[i]);

            const AudioFormat source = format(kRate, 1, from);
            const AudioFormat target = format(kRate, 1, to);

            auto direct = makeClientAudioProcessor(bitperfectPlan(source, target), registry);
            CHECK_OK(direct);
            auto converted =
                makeClientAudioProcessor(resampledPlan(source, target, false), registry);
            CHECK_OK(converted);
            CHECK_EQ(creations, 0);
            CHECK_EQ((*converted)->ratio(), 1.0);
            CHECK_EQ((*converted)->latencyFrames(), 0.0);

            std::vector<std::byte> byLayout(samples * AudioFormat::bytesPerSample(to));
            std::vector<std::byte> byDouble(byLayout.size());
            CHECK_OK((*direct)->process(input, byLayout));
            CHECK_OK((*converted)->process(input, byDouble));

            // Normalizing by each layout's own full scale is what makes the
            // two agree: the working double carries a level, not an integer.
            CHECK(std::memcmp(byLayout.data(), byDouble.data(), byLayout.size()) == 0);
        }
    }
}

MULTIROOM_TEST(the_resampled_path_rounds_to_nearest_and_clamps_rather_than_wrapping)
{
    int creations = 0;
    ResamplerRegistry registry;
    addFake(registry, &creations);

    const AudioFormat source = format(kRate, 1, AudioFormat::SampleType::Float32);
    const AudioFormat target = format(kRate, 1, AudioFormat::SampleType::Int16);
    auto processor = makeClientAudioProcessor(resampledPlan(source, target, false), registry);
    CHECK_OK(processor);

    const float inputs[] = {0.0f,
                            1.0f,
                            -1.0f,
                            2.5f,   // well past full scale
                            -2.5f,
                            0.4f / 32768.0f,  // rounds down to zero
                            0.6f / 32768.0f}; // rounds up to one
    const int32_t expected[] = {0, 32767, -32768, 32767, -32768, 0, 1};

    const size_t samples = sizeof(inputs) / sizeof(inputs[0]);
    std::vector<std::byte> input(samples * 4);
    for (size_t i = 0; i < samples; ++i)
        putFloat(input, i, inputs[i]);

    std::vector<std::byte> output(samples * 2);
    CHECK_OK((*processor)->process(input, output));
    for (size_t i = 0; i < samples; ++i)
        CHECK_EQ(getInteger(output, i, AudioFormat::SampleType::Int16), expected[i]);
}

MULTIROOM_TEST(dither_is_off_by_default_and_stays_within_one_step_when_it_is_on)
{
    int creations = 0;
    ResamplerRegistry registry;
    addFake(registry, &creations);

    const AudioFormat source = format(kRate, 1, AudioFormat::SampleType::Float32);
    const AudioFormat target = format(kRate, 1, AudioFormat::SampleType::Int16);
    const OutputPlan plan = resampledPlan(source, target, false);

    // A quarter of a step above zero: undithered it is zero forever, which is
    // exactly the quantization a dither exists to break up.
    const size_t samples = 512;
    std::vector<std::byte> input(samples * 4);
    for (size_t i = 0; i < samples; ++i)
        putFloat(input, i, 0.25f / 32768.0f);

    auto plain = makeClientAudioProcessor(plan, registry);
    CHECK_OK(plain);
    std::vector<std::byte> quiet(samples * 2);
    CHECK_OK((*plain)->process(input, quiet));
    for (size_t i = 0; i < samples; ++i)
        CHECK_EQ(getInteger(quiet, i, AudioFormat::SampleType::Int16), 0);

    auto dithered = makeClientAudioProcessor(plan, registry, DitherMode::TriangularPdf);
    CHECK_OK(dithered);
    std::vector<std::byte> noisy(samples * 2);
    CHECK_OK((*dithered)->process(input, noisy));

    size_t moved = 0;
    for (size_t i = 0; i < samples; ++i)
    {
        const int32_t value = getInteger(noisy, i, AudioFormat::SampleType::Int16);
        CHECK(value >= -1 && value <= 1);
        if (value != 0)
            ++moved;
    }
    CHECK(moved > 0);

    // And still no converter was built for either of them.
    CHECK_EQ(creations, 0);
}

MULTIROOM_TEST(a_misaligned_span_is_refused_by_both_paths)
{
    int creations = 0;
    ResamplerRegistry registry;
    addFake(registry, &creations);

    const AudioFormat source = format(kRate, kChannels, AudioFormat::SampleType::Int24);
    const AudioFormat target = format(48000, kChannels, AudioFormat::SampleType::Int24);

    std::vector<std::byte> odd(7);
    std::vector<std::byte> output(600);

    auto direct = makeClientAudioProcessor(bitperfectPlan(source, source), registry);
    CHECK_OK(direct);
    CHECK_ERR((*direct)->process(odd, output), ProcessorError::MisalignedBuffer);

    auto resampling = makeClientAudioProcessor(resampledPlan(source, target, false), registry);
    CHECK_OK(resampling);
    CHECK_ERR((*resampling)->process(odd, output), ProcessorError::MisalignedBuffer);
}

MULTIROOM_TEST(a_resampled_plan_the_planner_could_not_have_made_is_refused)
{
    int creations = 0;
    ResamplerRegistry registry;
    addFake(registry, &creations);

    const AudioFormat source = format(kRate, 2, AudioFormat::SampleType::Int16);

    // Nothing in this server mixes or reorders channels.
    CHECK_ERR(ResamplingProcessor::create(
                  resampledPlan(source, format(48000, 1, AudioFormat::SampleType::Int16), false),
                  registry, DitherMode::No),
              ProcessorError::InvalidPlan);

    CHECK_ERR(ResamplingProcessor::create(bitperfectPlan(source, source), registry,
                                          DitherMode::No),
              ProcessorError::InvalidPlan);

    // An id that is not in this registry: the planner refuses one at startup,
    // so a plan carrying it was made against a different build.
    OutputPlan stranger = resampledPlan(source, format(48000, 2, AudioFormat::SampleType::Int16),
                                        false);
    stranger.resamplerId = "not-in-this-build";
    CHECK_ERR(makeClientAudioProcessor(stranger, registry), ProcessorError::InvalidPlan);

    // A ratio the implementation cannot express is the converter's own refusal,
    // reported as one rather than as a plan that looked wrong here.
    OutputPlan extreme = resampledPlan(format(1000, 2, AudioFormat::SampleType::Int16),
                                       format(192000, 2, AudioFormat::SampleType::Int16), false);
    CHECK_ERR(makeClientAudioProcessor(extreme, registry), ProcessorError::ResamplerFailed);
    CHECK_EQ(creations, 1); // built, then refused at configure()
}

// --- accounting ---------------------------------------------------------------

namespace {

// 44.1 -> 48 kHz in float at both ends, so the comparison against the bare
// converter is about frames and not about a quantization step.
constexpr uint32_t kSourceRate = 44100;
constexpr uint32_t kOutputRate = 48000;

double signalAt(uint64_t frame, uint16_t channel)
{
    const double phase = static_cast<double>(frame) * 0.0137 + channel * 0.5;
    return 0.8 * std::sin(phase);
}

} // namespace

MULTIROOM_TEST(the_resampled_path_keeps_the_converters_accounting_over_ten_thousand_blocks)
{
    int creations = 0;
    ResamplerRegistry registry;
    addFake(registry, &creations);

    const AudioFormat source = format(kSourceRate, kChannels, AudioFormat::SampleType::Float32);
    const AudioFormat target = format(kOutputRate, kChannels, AudioFormat::SampleType::Float32);
    const OutputPlan plan = resampledPlan(source, target, false);

    auto processor = makeClientAudioProcessor(plan, registry);
    CHECK_OK(processor);

    // The oracle: the same converter, configured the same way, driven with the
    // same audio in doubles. The processor may convert the samples; it may not
    // change what the converter consumed and produced.
    FakeResampler oracle;
    CHECK_OK(oracle.configure(plan.resamplerConfig()));

    constexpr size_t kBlocks = 10000;
    constexpr size_t kBlockFrames = 441; // 10 ms at 44.1 kHz
    constexpr size_t kOutputCapacity = 512;

    std::vector<std::byte> pendingBytes;
    std::vector<double> pendingDoubles;
    std::vector<std::byte> outputBytes(kOutputCapacity * kChannels * 4);
    std::vector<double> outputDoubles(kOutputCapacity * kChannels);

    uint64_t nextFrame = 0;
    uint64_t totalConsumed = 0;
    uint64_t totalProduced = 0;
    double worstError = 0.0;

    for (size_t block = 0; block < kBlocks; ++block)
    {
        const size_t carried = pendingDoubles.size() / kChannels;
        pendingBytes.resize((carried + kBlockFrames) * kChannels * 4);
        pendingDoubles.resize((carried + kBlockFrames) * kChannels);
        for (size_t frame = 0; frame < kBlockFrames; ++frame)
        {
            for (uint16_t channel = 0; channel < kChannels; ++channel)
            {
                const size_t index = (carried + frame) * kChannels + channel;
                const double value = signalAt(nextFrame + frame, channel);
                pendingDoubles[index] = static_cast<double>(static_cast<float>(value));
                putFloat(pendingBytes, index, static_cast<float>(value));
            }
        }
        nextFrame += kBlockFrames;

        auto result = (*processor)->process(pendingBytes, outputBytes);
        CHECK_OK(result);

        auto reference = oracle.process(pendingDoubles, outputDoubles);
        CHECK_OK(reference);
        CHECK_EQ(result->inputFramesConsumed, reference->inputFramesConsumed);
        CHECK_EQ(result->outputFramesProduced, reference->outputFramesProduced);

        const size_t samples = result->outputFramesProduced * kChannels;
        for (size_t i = 0; i < samples; ++i)
        {
            const double delta =
                static_cast<double>(getFloat(outputBytes, i)) - outputDoubles[i];
            worstError = std::max(worstError, std::abs(delta));
        }

        totalConsumed += result->inputFramesConsumed;
        totalProduced += result->outputFramesProduced;

        // Whatever was not consumed stays the caller's and goes in front of
        // the next block, which is the contract this interface inherits.
        const size_t consumedSamples = result->inputFramesConsumed * kChannels;
        pendingDoubles.erase(pendingDoubles.begin(),
                             pendingDoubles.begin() + static_cast<ptrdiff_t>(consumedSamples));
        pendingBytes.erase(pendingBytes.begin(),
                           pendingBytes.begin() + static_cast<ptrdiff_t>(consumedSamples * 4));
    }

    // Float32 at both ends, so the only difference between the two runs is the
    // trip through the narrower type.
    CHECK_NEAR(worstError, 0.0, 1e-6);

    const uint64_t presented = static_cast<uint64_t>(kBlocks) * kBlockFrames;
    CHECK(totalConsumed <= presented);
    CHECK(presented - totalConsumed <= 2); // at most the converter's own lookahead

    // 4.41 M frames in, and one output frame per 1/1.088435 of them. A lost or
    // duplicated frame anywhere in the run moves this count.
    const double expected =
        static_cast<double>(totalConsumed) * kOutputRate / static_cast<double>(kSourceRate);
    CHECK_NEAR(static_cast<double>(totalProduced), expected, 2.0);
}

MULTIROOM_TEST(the_same_stream_in_one_call_and_in_many_produces_the_same_samples)
{
    int creations = 0;
    ResamplerRegistry registry;
    addFake(registry, &creations);

    const AudioFormat source = format(kSourceRate, kChannels, AudioFormat::SampleType::Float32);
    const AudioFormat target = format(kOutputRate, kChannels, AudioFormat::SampleType::Float32);
    const OutputPlan plan = resampledPlan(source, target, false);

    // More than the working chunk, so the loop inside process() runs several
    // times for the single call and once per call for the other.
    constexpr size_t kFrames = 5000;
    std::vector<std::byte> input(kFrames * kChannels * 4);
    for (size_t frame = 0; frame < kFrames; ++frame)
        for (uint16_t channel = 0; channel < kChannels; ++channel)
            putFloat(input, frame * kChannels + channel,
                     static_cast<float>(signalAt(frame, channel)));

    auto single = makeClientAudioProcessor(plan, registry);
    CHECK_OK(single);
    std::vector<std::byte> wholeOutput(kFrames * 2 * kChannels * 4);
    auto once = (*single)->process(input, wholeOutput);
    CHECK_OK(once);

    auto piecemeal = makeClientAudioProcessor(plan, registry);
    CHECK_OK(piecemeal);
    std::vector<std::byte> pieceOutput(wholeOutput.size());

    size_t offset = 0;   // input frames handed over
    size_t produced = 0; // output frames written
    while (offset < kFrames)
    {
        const size_t take = std::min<size_t>(7, kFrames - offset);
        auto result = (*piecemeal)
                          ->process(std::span<const std::byte>(input).subspan(
                                        offset * kChannels * 4, take * kChannels * 4),
                                    std::span<std::byte>(pieceOutput)
                                        .subspan(produced * kChannels * 4, 5 * kChannels * 4));
        CHECK_OK(result);
        if (result->inputFramesConsumed == 0 && result->outputFramesProduced == 0)
        {
            // Five output frames at a time is less than seven input frames
            // produce, so the caller re-presents; a run that makes no progress
            // at all would loop forever and is a defect worth catching.
            CHECK(take < 7);
            break;
        }
        offset += result->inputFramesConsumed;
        produced += result->outputFramesProduced;
    }

    CHECK_EQ(produced, once->outputFramesProduced);

    // Every output frame is the same audio in both runs. Not bit-identical:
    // FakeResampler rebases its interpolation position on each call, so a
    // different call pattern rounds in a different place -- a few ulps, where
    // a frame lost or duplicated at a chunk boundary would be the distance
    // between two neighbouring samples of the signal, four orders of magnitude
    // larger.
    double worstError = 0.0;
    for (size_t i = 0; i < produced * kChannels; ++i)
    {
        worstError = std::max(worstError, std::abs(static_cast<double>(getFloat(wholeOutput, i)) -
                                                   static_cast<double>(getFloat(pieceOutput, i))));
    }
    CHECK_NEAR(worstError, 0.0, 1e-6);
}

MULTIROOM_TEST(switching_modes_mid_stream_loses_and_duplicates_no_frame)
{
    // Phase 15's format transition, seen from here: the room's plan is made
    // again and a new processor takes over at a block boundary. Nothing in the
    // old one is owed to the new one, because neither holds a frame the caller
    // was not told about.
    int creations = 0;
    ResamplerRegistry registry;
    addFake(registry, &creations);

    const AudioFormat source = format(kSourceRate, kChannels, AudioFormat::SampleType::Int24);
    const AudioFormat bitperfect = format(kSourceRate, kChannels, AudioFormat::SampleType::Int24);
    const AudioFormat resampled = format(kOutputRate, kChannels, AudioFormat::SampleType::Int24);

    constexpr size_t kFrames = 2048;
    std::vector<std::byte> input(kFrames * kChannels * 3);
    for (size_t frame = 0; frame < kFrames; ++frame)
        for (uint16_t channel = 0; channel < kChannels; ++channel)
            putInteger(input, frame * kChannels + channel, AudioFormat::SampleType::Int24,
                       static_cast<int32_t>(signalAt(frame, channel) * 8388607.0));

    std::unique_ptr<IClientAudioProcessor> processor;
    {
        auto direct = makeClientAudioProcessor(bitperfectPlan(source, bitperfect), registry);
        CHECK_OK(direct);
        processor = std::move(*direct);
    }

    std::vector<std::byte> output(kFrames * 2 * kChannels * 3);
    size_t consumed = 0;
    size_t produced = 0;

    while (consumed < kFrames / 2)
    {
        auto result = processor->process(
            std::span<const std::byte>(input).subspan(consumed * kChannels * 3,
                                                      256 * kChannels * 3),
            std::span<std::byte>(output).subspan(produced * kChannels * 3, 256 * kChannels * 3));
        CHECK_OK(result);
        consumed += result->inputFramesConsumed;
        produced += result->outputFramesProduced;
    }
    CHECK_EQ(consumed, kFrames / 2);
    CHECK_EQ(produced, kFrames / 2); // the direct path is one for one

    {
        auto next = makeClientAudioProcessor(resampledPlan(source, resampled, false), registry);
        CHECK_OK(next);
        processor = std::move(*next);
    }
    CHECK_EQ(creations, 1);

    const size_t switchedAt = consumed;
    while (consumed < kFrames)
    {
        const size_t take = std::min<size_t>(256, kFrames - consumed);
        auto result = processor->process(
            std::span<const std::byte>(input).subspan(consumed * kChannels * 3,
                                                      take * kChannels * 3),
            std::span<std::byte>(output).subspan(produced * kChannels * 3, 512 * kChannels * 3));
        CHECK_OK(result);

        // A converter holds a frame of lookahead, so the very last input frame
        // is not consumed until more arrives behind it. That is the contract,
        // not a lost frame -- the caller re-presents it -- and it is the one
        // place this loop can stop making progress.
        if (result->inputFramesConsumed == 0 && result->outputFramesProduced == 0)
            break;

        consumed += result->inputFramesConsumed;
        produced += result->outputFramesProduced;
    }

    // Every input frame was accounted for exactly once, and the second half
    // produced what its own ratio asks for rather than restarting a count.
    CHECK(kFrames - consumed <= 1);
    const double secondHalf = static_cast<double>(produced - switchedAt);
    const double expected = static_cast<double>(consumed - switchedAt) * kOutputRate /
                            static_cast<double>(kSourceRate);
    CHECK_NEAR(secondHalf, expected, 2.0);

    // The bitperfect half is still the source's own bytes, which is the point
    // of having changed processor rather than ratio.
    CHECK(std::memcmp(input.data(), output.data(), switchedAt * kChannels * 3) == 0);
}

MULTIROOM_TEST(reset_puts_a_resampled_room_back_where_configure_left_it)
{
    int creations = 0;
    ResamplerRegistry registry;
    addFake(registry, &creations);

    const AudioFormat source = format(kSourceRate, kChannels, AudioFormat::SampleType::Float32);
    const AudioFormat target = format(kOutputRate, kChannels, AudioFormat::SampleType::Float32);
    auto processor = makeClientAudioProcessor(resampledPlan(source, target, false), registry);
    CHECK_OK(processor);

    constexpr size_t kFrames = 1000;
    std::vector<std::byte> input(kFrames * kChannels * 4);
    for (size_t frame = 0; frame < kFrames; ++frame)
        for (uint16_t channel = 0; channel < kChannels; ++channel)
            putFloat(input, frame * kChannels + channel,
                     static_cast<float>(signalAt(frame, channel)));

    std::vector<std::byte> first(kFrames * 2 * kChannels * 4);
    std::vector<std::byte> second(first.size());

    auto once = (*processor)->process(input, first);
    CHECK_OK(once);

    (*processor)->reset();

    auto twice = (*processor)->process(input, second);
    CHECK_OK(twice);
    CHECK_EQ(twice->inputFramesConsumed, once->inputFramesConsumed);
    CHECK_EQ(twice->outputFramesProduced, once->outputFramesProduced);
    CHECK(std::memcmp(first.data(), second.data(),
                      once->outputFramesProduced * kChannels * 4) == 0);
}

// --- the equalizer -----------------------------------------------------------

MULTIROOM_TEST(the_resampled_path_runs_the_rooms_equalizer_and_the_direct_path_never_does)
{
    const AudioFormat floating = format(kRate, 1, AudioFormat::SampleType::Float32);

    EqualizerSettings halve;
    halve.enabled = true;
    halve.preampDb = -6.0206;

    const size_t frames = 64;
    std::vector<std::byte> input(frames * floating.bytesPerFrame());
    for (size_t i = 0; i < frames; ++i)
        putFloat(input, i, 0.5f);

    // Resampled at one rate: no converter, the double path all the same.
    {
        ResamplerRegistry registry;
        int creations = 0;
        addFake(registry, &creations);
        auto processor =
            makeClientAudioProcessor(resampledPlan(floating, floating, false), registry);
        CHECK_OK(processor);

        Equalizer eq;
        eq.setFormat(kRate, 1);
        eq.configure(halve);
        (*processor)->setEqualizer(&eq);

        std::vector<std::byte> output(input.size());
        CHECK_OK((*processor)->process(input, output));
        for (size_t i = 0; i < frames; ++i)
            CHECK_NEAR(getFloat(output, i), 0.25f, 1e-4f);
    }

    // Bitperfect: the same equalizer handed over, and the same bytes out.
    {
        ResamplerRegistry registry;
        auto processor = makeClientAudioProcessor(bitperfectPlan(floating, floating), registry);
        CHECK_OK(processor);

        Equalizer eq;
        eq.setFormat(kRate, 1);
        eq.configure(halve);
        (*processor)->setEqualizer(&eq);

        std::vector<std::byte> output(input.size());
        CHECK_OK((*processor)->process(input, output));
        CHECK(std::memcmp(input.data(), output.data(), input.size()) == 0);
    }
}
