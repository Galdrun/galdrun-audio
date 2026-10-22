#pragma once

// r8brain-free-src behind IAudioResampler.
//
// The second implementation, and the one that proves the abstraction is real:
// it is unlike SoXR in every way the interface has an opinion about, and none
// of that difference reaches a caller.
//
// **It cannot change its ratio.** r8brain designs its filter chain in its
// constructor from the rate pair it is given, so capabilities().variableRatio
// is false and setRatio() returns FixedRatioOnly. A configuration naming it as
// the drift resampler is refused at startup by phase 2's rule, rather than at
// the moment a room first needs correcting -- which, for a client whose card
// runs +30 ppm, is hours in.
//
// **It is a mono converter.** One CDSPResampler24 per channel, and this
// adapter de-interleaves into them and re-interleaves out. That is also why
// kMaxChannels is a real limit here rather than a formality: each channel is
// its own filter chain and its own buffers.
//
// **It hands back its own output buffer.** Where SoXR fills the caller's span,
// r8brain returns a pointer into an internal buffer and a count, so this
// adapter copies -- and, when the caller's span cannot take it all, holds the
// remainder. That buffer is the one this phase adds, it is bounded by what a
// single kChunkFrames-sized call can produce, and it is drained before any
// further input is consumed.
//
// This header lives beside its source, and does not include an r8brain header:
// the roadmap's rule is that no library header appears outside these two
// directories, and CDSPResampler24 is a complete type only inside Impl.

#include "resampler/IAudioResampler.h"
#include "resampler/IResamplerFactory.h"

#include <cstddef>
#include <memory>

namespace multiroom {

class R8BrainResampler final : public IAudioResampler
{
public:
    // The same limits FakeResampler and SoxrResampler declare, so a
    // configuration that is refused is refused for the same reason whichever
    // implementation a room was given.
    static constexpr uint16_t kMaxChannels = 8;
    static constexpr double kMinRatio = 1.0 / 32.0;
    static constexpr double kMaxRatio = 32.0;

    // The most input frames handed to r8brain in one of its calls. It fixes
    // the size of every internal buffer the library allocates, and therefore
    // the ceiling on the output this adapter may have to hold back. 1024
    // frames is comfortably more than one 20 ms packet at 44.1 kHz and less
    // than one at 96 kHz, so a packet costs one or two calls rather than one
    // allocation sized for the largest format any room might ever use.
    static constexpr size_t kChunkFrames = 1024;

    static const ResamplerCapabilities& staticCapabilities() noexcept;

    R8BrainResampler();
    ~R8BrainResampler() override;

    const ResamplerCapabilities& capabilities() const noexcept override;
    Status<ResamplerError> configure(const ResamplerConfig& config) override;
    bool configured() const noexcept override { return channels_ > 0; }
    double ratio() const noexcept override { return ratio_; }
    Status<ResamplerError> setRatio(double ratio) override;
    Result<ProcessResult, ResamplerError> process(std::span<const double> input,
                                                  std::span<double> output) override;
    void reset() noexcept override;
    double latencyFrames() const noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    uint16_t channels_ = 0;
    double ratio_ = 0.0;
};

class R8BrainResamplerFactory final : public IResamplerFactory
{
public:
    const ResamplerCapabilities& capabilities() const noexcept override;
    std::unique_ptr<IAudioResampler> create() const override;
};

} // namespace multiroom
