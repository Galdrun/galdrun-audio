#pragma once

// A resampler whose output can be worked out on paper.
//
// It exists so that the pipeline above it can be tested before libsoxr or
// r8brain are built, and -- more usefully -- so the *contract* can be tested
// rather than a library's behaviour. Linear interpolation is a poor converter
// and an excellent oracle: output frame k is the input signal at input
// position k / ratio, so a linear input comes out linear exactly, a ratio of
// one comes out bit-identical, and the frame accounting over any sequence of
// calls can be predicted to the frame.
//
// It is not registered by the server's startup. A room must never be served by
// this; a resampler that names it in a deployment config would be an audible
// mistake, and the way to make that impossible is for production code to never
// put it in the registry.
//
// Its ratio changes take effect on the very next output frame, with no
// transition. A real variable-rate converter moves its ratio smoothly over
// some number of frames, which is what makes its output *not* predictable to
// the frame -- that difference is the point of this class, not an oversight.
//
// It ignores ResamplerConfig::requiresVariableRatio, and setRatio() works
// whatever configure() was told. That is not a shortcut: the flag exists so an
// implementation with two engines can pick one, and this has a single engine
// that interpolates. Refusing a call it can honour would only make the oracle
// harder to write tests against.

#include "resampler/IAudioResampler.h"
#include "resampler/IResamplerFactory.h"

#include <cstddef>
#include <memory>
#include <string>

namespace multiroom {

class FakeResampler final : public IAudioResampler
{
public:
    // Wide enough that no test is testing this limit by accident, narrow
    // enough that UnsupportedChannels is reachable.
    static constexpr uint16_t kMaxChannels = 8;

    // 44.1 kHz to 705.6 kHz and back is a factor of 16; 32 leaves room and
    // still makes a ratio that is nonsense (a misplaced decimal point, an
    // inverted convention on a 10x conversion) a failure rather than an
    // enormous buffer.
    static constexpr double kMinRatio = 1.0 / 32.0;
    static constexpr double kMaxRatio = 32.0;

    static const ResamplerCapabilities& staticCapabilities() noexcept;

    const ResamplerCapabilities& capabilities() const noexcept override;
    Status<ResamplerError> configure(const ResamplerConfig& config) override;
    bool configured() const noexcept override { return channels_ > 0; }
    double ratio() const noexcept override { return ratio_; }
    Status<ResamplerError> setRatio(double ratio) override;
    Result<ProcessResult, ResamplerError> process(std::span<const double> input,
                                                  std::span<double> output) override;
    void reset() noexcept override;
    double latencyFrames() const noexcept override;

    // Totals since configure() or reset(), for the accounting tests: the ratio
    // contract is about a whole stream, and a per-call assertion cannot see it.
    uint64_t inputFramesConsumed() const noexcept { return inputFrames_; }
    uint64_t outputFramesProduced() const noexcept { return outputFrames_; }

private:
    uint16_t channels_ = 0;
    double ratio_ = 0.0;
    double step_ = 0.0; // 1 / ratio_, the input frames one output frame advances

    // Where the next output frame sits, in input frames from the first input
    // frame the caller has not yet been told to drop. Rebased on every call
    // rather than held as an absolute stream position, so it stays small and
    // the interpolation keeps full precision on a stream that runs for days.
    double position_ = 0.0;

    uint64_t inputFrames_ = 0;
    uint64_t outputFrames_ = 0;
};

class FakeResamplerFactory final : public IResamplerFactory
{
public:
    const ResamplerCapabilities& capabilities() const noexcept override;
    std::unique_ptr<IAudioResampler> create() const override;
};

} // namespace multiroom
