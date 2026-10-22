#pragma once

// libsoxr behind IAudioResampler.
//
// This header lives beside its source rather than in include/ because of the
// roadmap's phase-3 rule: no library header outside these two directories. The
// rule is what keeps a third implementation cheap, so this file does not
// include soxr.h either -- soxr_t is `struct soxr *`, and the forward
// declaration below is enough to hold one.
//
// Two things about this adapter are decisions rather than transcription.
//
// **It has two engines, and configure() picks between them.** SoXR's
// variable-rate engine works in single precision where its constant-rate
// engine does not: measured on a 1 kHz tone through 44.1 -> 48 kHz, 115.7 dB
// of noise floor against 131.2 dB, a difference of 15.5 dB. So a client whose
// ratio will never move must not pay for one that can, and
// ResamplerConfig::requiresVariableRatio -- phase 6's drift flag, handed
// straight through -- is what says which it is.
//
// The consequence is that setRatio() belongs to the instance, not to the
// implementation: capabilities().variableRatio stays true, because SoXR *can*,
// while an instance configured for a fixed ratio refuses with FixedRatioOnly.
// That is not the silent fallback MATP section 28 forbids -- nothing falls
// back, and the refusal answers a caller that broke the promise it made at
// configure(). Inferring the engine instead, from whether initialRatio
// happened to carry a trim, would have a room change engine mid-deployment the
// first time a planner passed one.
//
// **In variable-rate mode the ratio window is fixed at configure().** SoXR's
// variable-rate engine is created with the *maximum* input/output ratio it
// will ever be asked for and designs its filter for that, so the window has to
// be committed up front. kTrimRange is that window, and it is generous: four
// times the protocol's temporary convergence limit of +/-500 ppm. Widening it
// costs passband -- the anti-alias filter is designed for the most decimation
// the window allows -- which is why it is a window and not simply
// kMinRatio..kMaxRatio. The constant-rate engine has no window, because it has
// nowhere to move.

#include "resampler/IAudioResampler.h"
#include "resampler/IResamplerFactory.h"

#include <cstddef>
#include <memory>

// soxr.h: `typedef struct soxr * soxr_t;`. Declared at global scope, because a
// struct declared inside namespace multiroom would be a different type.
struct soxr;

namespace multiroom {

class SoxrResampler final : public IAudioResampler
{
public:
    // The same limits FakeResampler declares, for the same reasons: wide
    // enough that no test meets them by accident, narrow enough that a
    // misplaced decimal point or an inverted convention is a failure rather
    // than an enormous filter.
    static constexpr uint16_t kMaxChannels = 8;
    static constexpr double kMinRatio = 1.0 / 32.0;
    static constexpr double kMaxRatio = 32.0;

    // How far setRatio() may move from the ratio configure() was given, as a
    // fraction, in variable-rate mode. 0.002 is +/-2000 ppm against a protocol
    // that allows +/-100 ppm in normal correction and +/-500 ppm while
    // converging (MATP section 46).
    static constexpr double kTrimRange = 0.002;

    static const ResamplerCapabilities& staticCapabilities() noexcept;

    SoxrResampler() = default;
    ~SoxrResampler() override;

    const ResamplerCapabilities& capabilities() const noexcept override;
    Status<ResamplerError> configure(const ResamplerConfig& config) override;
    bool configured() const noexcept override { return handle_ != nullptr; }
    double ratio() const noexcept override { return ratio_; }
    Status<ResamplerError> setRatio(double ratio) override;
    Result<ProcessResult, ResamplerError> process(std::span<const double> input,
                                                  std::span<double> output) override;
    void reset() noexcept override;
    double latencyFrames() const noexcept override;

private:
    void destroy() noexcept;

    soxr* handle_ = nullptr;
    uint16_t channels_ = 0;
    double ratio_ = 0.0;

    // Which engine this instance was built with, and therefore whether
    // setRatio() can be honoured at all.
    bool variableRatio_ = false;

    // The centre of the window setRatio() may move within. Kept separately
    // from ratio_, which is wherever drift correction has moved it to.
    double configuredRatio_ = 0.0;
};

class SoxrResamplerFactory final : public IResamplerFactory
{
public:
    const ResamplerCapabilities& capabilities() const noexcept override;
    std::unique_ptr<IAudioResampler> create() const override;
};

} // namespace multiroom
