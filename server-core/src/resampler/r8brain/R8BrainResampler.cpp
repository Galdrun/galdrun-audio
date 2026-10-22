#include "resampler/r8brain/R8BrainResampler.h"

#include "resampler/RatioText.h"

#include <algorithm>
#include <string>
#include <vector>

// The only translation unit in the server that sees an r8brain header.
//
// r8brain is a C++98-era header-only library and does not compile clean under
// /W4 -Wall -Wextra -Wpedantic; multiroom_set_warnings() applies to our targets,
// so the include has to be fenced rather than the flags relaxed. Patching the
// vendored tree instead would turn a dependency into a fork at the first
// upstream update.
//
// NOMINMAX because r8bbase.h reaches for <Windows.h> to get a mutex, and
// Windows.h defines min and max as macros unless told not to, which breaks
// std::min at every call site in this file.
#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif

#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wold-style-cast"
#endif

#include <CDSPResampler.h>

#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace multiroom {
namespace {

using resampler::ratioText;

// 24-bit is the widest sample type MATP carries (section 12), and r8brain's
// own guidance is 6.02 * bits + 40 dB of stopband attenuation, which is what
// CDSPResampler24 is preset to. The 16-bit variant would be cheaper and would
// put the converter's noise floor above a 24-bit source's own.
using Converter = r8b::CDSPResampler24;

} // namespace

// One mono converter per channel, plus the buffers that turn this server's
// interleaved spans into the mono calls r8brain takes.
struct R8BrainResampler::Impl
{
    std::vector<std::unique_ptr<Converter>> converters;

    // One channel's worth of input, de-interleaved. r8brain takes a non-const
    // pointer and is free to work in place, so the caller's span is copied
    // here rather than cast.
    std::vector<double> channelInput;

    // What one chunk produced, interleaved, and how much of it the caller's
    // span could not take. r8brain returns as many frames as it has rather
    // than as many as were asked for, so this is where the difference waits.
    std::vector<double> chunkOutput;
    size_t pendingFrames = 0;
    size_t pendingOffsetFrames = 0;
};

const ResamplerCapabilities& R8BrainResampler::staticCapabilities() noexcept
{
    static const ResamplerCapabilities caps = [] {
        ResamplerCapabilities c;
        c.id = "r8brain";
        c.variableRatio = false;
        c.maxChannels = kMaxChannels;
        c.minRatio = kMinRatio;
        c.maxRatio = kMaxRatio;
        return c;
    }();
    return caps;
}

const ResamplerCapabilities& R8BrainResampler::capabilities() const noexcept
{
    return staticCapabilities();
}

R8BrainResampler::R8BrainResampler() : impl_(std::make_unique<Impl>()) {}

// Out of line because Impl is only complete here.
R8BrainResampler::~R8BrainResampler() = default;

Status<ResamplerError> R8BrainResampler::configure(const ResamplerConfig& config)
{
    if (!config.isValid())
    {
        return fail(ResamplerError::InvalidConfig,
                    std::to_string(config.inputRate) + " -> " + std::to_string(config.outputRate) +
                        " Hz, " + std::to_string(config.channels) + "ch, ratio " +
                        ratioText(config.initialRatio));
    }
    if (config.channels > kMaxChannels)
    {
        return fail(ResamplerError::UnsupportedChannels,
                    std::to_string(config.channels) + " channels, r8brain is built for " +
                        std::to_string(kMaxChannels));
    }

    // The refusal the whole capability model exists to make happen here rather
    // than hours in. A planner is expected to have read capabilities() and
    // never reached this line, but a configuration that names r8brain for a
    // drift-corrected room has to fail at startup either way -- the
    // alternative is a converter that accepts the room and then cannot correct
    // it when its card turns out to run +30 ppm fast.
    if (config.requiresVariableRatio)
    {
        return fail(ResamplerError::FixedRatioOnly,
                    "r8brain builds its filter chain at configure() and cannot be the drift "
                    "resampler");
    }

    const double ratio = config.effectiveRatio();
    if (!staticCapabilities().supportsRatio(ratio))
    {
        return fail(ResamplerError::UnsupportedRatio,
                    ratioText(ratio) + " outside " + ratioText(kMinRatio) + ".." +
                        ratioText(kMaxRatio));
    }

    // A failed configure() must not leave the previous format running, and
    // there is nothing below this point that can fail, so the old state goes
    // first.
    impl_->converters.clear();
    impl_->pendingFrames = 0;
    impl_->pendingOffsetFrames = 0;
    channels_ = 0;
    ratio_ = 0.0;

    // The real input rate, not a normalised 1.0, because r8brain recognises a
    // power-of-two rate pair and takes a much cheaper path for it -- 44.1 to
    // 88.2 kHz is two halfband stages and no fractional interpolator at all.
    // The destination is derived from the ratio rather than from outputRate so
    // that a configured trim is honoured, and it is exactly outputRate
    // whenever there is none.
    const double inputRate = static_cast<double>(config.inputRate);
    const double outputRate = inputRate * ratio;

    impl_->converters.reserve(config.channels);
    for (uint16_t channel = 0; channel < config.channels; ++channel)
    {
        impl_->converters.push_back(
            std::make_unique<Converter>(inputRate, outputRate, static_cast<int>(kChunkFrames)));
    }

    const size_t maxOutputFrames =
        static_cast<size_t>(impl_->converters.front()->getMaxOutLen(static_cast<int>(kChunkFrames)));

    impl_->channelInput.assign(kChunkFrames, 0.0);
    impl_->chunkOutput.assign(maxOutputFrames * config.channels, 0.0);

    channels_ = config.channels;
    ratio_ = ratio;
    return {};
}

Status<ResamplerError> R8BrainResampler::setRatio(double ratio)
{
    // Not NotConfigured first, even when it is also unconfigured: the caller
    // that reaches here has a capability model saying this cannot happen, and
    // the useful answer is always the one that names the implementation.
    return fail(ResamplerError::FixedRatioOnly,
                "r8brain builds its filter chain at configure(), so " + ratioText(ratio) +
                    " cannot replace " + ratioText(ratio_) + " mid-stream");
}

Result<ProcessResult, ResamplerError> R8BrainResampler::process(std::span<const double> input,
                                                                std::span<double> output)
{
    if (!configured())
        return fail(ResamplerError::NotConfigured, "process before configure");

    const size_t channels = channels_;
    if (input.size() % channels != 0 || output.size() % channels != 0)
    {
        return fail(ResamplerError::MisalignedBuffer,
                    std::to_string(input.size()) + " in / " + std::to_string(output.size()) +
                        " out samples is not a whole number of " + std::to_string(channels) +
                        "-channel frames");
    }

    const size_t inputFrames = input.size() / channels;
    const size_t outputFrames = output.size() / channels;

    size_t consumed = 0;
    size_t produced = 0;

    // What the last call could not hand over goes first, in order. Anything
    // else would reorder the stream.
    if (impl_->pendingFrames > 0)
    {
        const size_t taken = std::min(impl_->pendingFrames, outputFrames);
        const double* from = impl_->chunkOutput.data() + impl_->pendingOffsetFrames * channels;
        std::copy(from, from + taken * channels, output.data());
        impl_->pendingFrames -= taken;
        impl_->pendingOffsetFrames += taken;
        produced = taken;
    }

    // Input is only fed once nothing is held back, so the buffer above can
    // never grow past one chunk's output.
    while (impl_->pendingFrames == 0 && produced < outputFrames && consumed < inputFrames)
    {
        const size_t chunk = std::min(kChunkFrames, inputFrames - consumed);

        int chunkFrames = 0;
        for (size_t channel = 0; channel < channels; ++channel)
        {
            for (size_t frame = 0; frame < chunk; ++frame)
                impl_->channelInput[frame] = input[(consumed + frame) * channels + channel];

            double* converted = nullptr;
            const int count = impl_->converters[channel]->process(impl_->channelInput.data(),
                                                                  static_cast<int>(chunk),
                                                                  converted);

            // Every channel is the same filter chain fed the same number of
            // frames, so they agree by construction; disagreeing would mean
            // one channel's audio silently sliding against the others.
            if (channel == 0)
                chunkFrames = count;

            // The returned pointer is into that converter's own buffer and
            // stays valid only until its next call, so it is copied now rather
            // than remembered.
            for (int frame = 0; frame < count; ++frame)
            {
                impl_->chunkOutput[static_cast<size_t>(frame) * channels + channel] =
                    converted[frame];
            }
        }

        consumed += chunk;

        const size_t available = static_cast<size_t>(chunkFrames);
        const size_t taken = std::min(available, outputFrames - produced);
        std::copy(impl_->chunkOutput.data(), impl_->chunkOutput.data() + taken * channels,
                  output.data() + produced * channels);
        produced += taken;

        impl_->pendingOffsetFrames = taken;
        impl_->pendingFrames = available - taken;
    }

    return ProcessResult{consumed, produced};
}

void R8BrainResampler::reset() noexcept
{
    for (auto& converter : impl_->converters)
        converter->clear();

    impl_->pendingFrames = 0;
    impl_->pendingOffsetFrames = 0;
}

double R8BrainResampler::latencyFrames() const noexcept
{
    if (!configured())
        return 0.0;

    // The interface asks for the signal held inside the converter, in output
    // frames, and r8brain's own getLatency() and getLatencyFrac() are not it:
    // a CDSPResampler removes its whole-sample latency internally, so
    // getLatency() reports zero and getLatencyFrac() only the sub-frame
    // remainder. Reporting that would claim a converter holding roughly 1850
    // output frames of a 44.1 -> 48 kHz stream holds none.
    //
    // getInputRequiredForOutput(1) is the honest number: how much input has to
    // go in before one frame comes out. It is in input frames, so it is scaled
    // to output frames here, which is also what makes it move with the ratio.
    const int inputBeforeOutput = impl_->converters.front()->getInputRequiredForOutput(1);
    return static_cast<double>(inputBeforeOutput) * ratio_;
}

const ResamplerCapabilities& R8BrainResamplerFactory::capabilities() const noexcept
{
    return R8BrainResampler::staticCapabilities();
}

std::unique_ptr<IAudioResampler> R8BrainResamplerFactory::create() const
{
    return std::make_unique<R8BrainResampler>();
}

} // namespace multiroom
