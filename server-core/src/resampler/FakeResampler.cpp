#include "resampler/FakeResampler.h"

#include "resampler/RatioText.h"

#include <algorithm>
#include <string>

namespace multiroom {

using resampler::ratioText;

const ResamplerCapabilities& FakeResampler::staticCapabilities() noexcept
{
    static const ResamplerCapabilities caps = [] {
        ResamplerCapabilities c;
        c.id = "fake";
        c.variableRatio = true;
        c.maxChannels = kMaxChannels;
        c.minRatio = kMinRatio;
        c.maxRatio = kMaxRatio;
        return c;
    }();
    return caps;
}

const ResamplerCapabilities& FakeResampler::capabilities() const noexcept
{
    return staticCapabilities();
}

Status<ResamplerError> FakeResampler::configure(const ResamplerConfig& config)
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
                    std::to_string(config.channels) + " channels, fake handles " +
                        std::to_string(kMaxChannels));
    }

    const double ratio = config.effectiveRatio();
    if (!staticCapabilities().supportsRatio(ratio))
    {
        return fail(ResamplerError::UnsupportedRatio,
                    ratioText(ratio) + " outside " + ratioText(kMinRatio) + ".." +
                        ratioText(kMaxRatio));
    }

    channels_ = config.channels;
    ratio_ = ratio;
    step_ = 1.0 / ratio;
    reset();
    return {};
}

Status<ResamplerError> FakeResampler::setRatio(double ratio)
{
    if (!configured())
        return fail(ResamplerError::NotConfigured, "setRatio before configure");
    if (!staticCapabilities().supportsRatio(ratio))
    {
        return fail(ResamplerError::UnsupportedRatio,
                    ratioText(ratio) + " outside " + ratioText(kMinRatio) + ".." +
                        ratioText(kMaxRatio));
    }

    // No transition and no reset: the stream position is preserved and the new
    // ratio applies from the next output frame. That is what makes a mid-stream
    // change predictable to the frame.
    ratio_ = ratio;
    step_ = 1.0 / ratio;
    return {};
}

Result<ProcessResult, ResamplerError> FakeResampler::process(std::span<const double> input,
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

    const size_t inFrames = input.size() / channels;
    const size_t outFrames = output.size() / channels;

    size_t produced = 0;
    while (produced < outFrames)
    {
        const size_t index = static_cast<size_t>(position_); // position_ >= 0, so this floors
        const double fraction = position_ - static_cast<double>(index);

        // Interpolating needs the frame after this one; landing exactly on a
        // frame does not. Treating those two cases the same would hold a frame
        // back at a ratio of one and stop the path being an identity.
        const size_t needed = fraction > 0.0 ? index + 2 : index + 1;
        if (needed > inFrames)
            break;

        for (size_t channel = 0; channel < channels; ++channel)
        {
            const double a = input[index * channels + channel];
            const double b = fraction > 0.0 ? input[(index + 1) * channels + channel] : a;
            output[produced * channels + channel] = a + (b - a) * fraction;
        }

        ++produced;
        position_ += step_;
    }

    // Everything before the next frame needed is the caller's to drop; the
    // rest it re-presents next time. Reporting more than that would make the
    // resampler responsible for buffering it, and a buffer here is a buffer in
    // every implementation that follows.
    const size_t consumed = std::min(static_cast<size_t>(position_), inFrames);
    position_ -= static_cast<double>(consumed);

    inputFrames_ += consumed;
    outputFrames_ += produced;
    return ProcessResult{consumed, produced};
}

void FakeResampler::reset() noexcept
{
    position_ = 0.0;
    inputFrames_ = 0;
    outputFrames_ = 0;
}

double FakeResampler::latencyFrames() const noexcept
{
    if (!configured())
        return 0.0;

    // One input frame of lookahead -- the frame after the interpolation point,
    // which is never consumed until the output has passed it -- expressed in
    // output frames, which is what the interface promises and what moves with
    // the ratio.
    return ratio_;
}

const ResamplerCapabilities& FakeResamplerFactory::capabilities() const noexcept
{
    return FakeResampler::staticCapabilities();
}

std::unique_ptr<IAudioResampler> FakeResamplerFactory::create() const
{
    return std::make_unique<FakeResampler>();
}

} // namespace multiroom
