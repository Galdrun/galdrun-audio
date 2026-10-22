#pragma once

// The converted path: input sample type -> double -> resampler -> output
// sample type, converted once at each end and never in the middle.
//
// The working format is double and the conversion into and out of it happens
// here rather than in the adapters, so libsoxr, r8brain and whatever comes
// third all see the same interleaved spans (phase 3's rule). The rounding at
// the output end is to nearest, with a clamp, and dither is off unless a
// deployment asks for it.
//
// **The working buffers are a fixed chunk, not the caller's span.** A room's
// packet is 20 ms and a zone's block is whatever the codec produced, so
// sizing the scratch from either would put an unbounded buffer in the one
// component that runs per room: kChunkFrames caps it at a known number of
// bytes and the loop below runs as many times as the spans need. The memory
// rule holds without a failure path, because running the loop again is always
// available.
//
// **A resampled room whose ratio can never move has no converter.** A plan
// with equal rates and no drift correction is a change of depth or of
// representation -- float into an integer room, 24-bit into a 16-bit one --
// which is a value change and therefore not the direct path's, but which does
// not need a filter either. Building one would add its passband ripple and its
// latency to a room that asked for neither. The plan still says resampled,
// because the samples really are not the source's any more.

#include "processing/IClientAudioProcessor.h"
#include "processing/SampleConvert.h"

#include <memory>
#include <vector>

namespace multiroom {

class ResamplingProcessor final : public IClientAudioProcessor
{
public:
    // 1024 frames of up to 8 channels is 64 KB of double per direction, which
    // is small enough to ignore per room and large enough that the per-call
    // overhead of a converter disappears. r8brain's own chunk is the same
    // size, for the same reason.
    static constexpr size_t kChunkFrames = 1024;

    // Builds and configures the converter the plan names -- the only place in
    // the server that does. Refuses a plan that is not Resampled, one whose
    // formats are invalid or whose channel counts differ, and reports the
    // converter's own failure where configure() refuses the ratio or the
    // channel count.
    [[nodiscard]] static Result<std::unique_ptr<ResamplingProcessor>, ProcessorError>
    create(const OutputPlan& plan, const ResamplerRegistry& registry, DitherMode dither);

    [[nodiscard]] const OutputPlan& plan() const noexcept override { return plan_; }

    [[nodiscard]] double ratio() const noexcept override;

    Status<ProcessorError> setRatio(double ratio) override;

    Result<ProcessResult, ProcessorError> process(std::span<const std::byte> input,
                                                  std::span<std::byte> output) override;

    void reset() override;

    [[nodiscard]] double latencyFrames() const noexcept override;

    void setEqualizer(Equalizer* equalizer) noexcept override { equalizer_ = equalizer; }

private:
    ResamplingProcessor(const OutputPlan& plan, std::unique_ptr<IAudioResampler> resampler,
                        DitherMode dither);

    [[nodiscard]] sample_convert::TriangularDither* dither() noexcept;

    OutputPlan plan_;

    // Null where the ratio is fixed at one; see the note above.
    std::unique_ptr<IAudioResampler> resampler_;

    // Not owned; run on what the converter produced, never on its input,
    // because input it did not consume is presented again.
    Equalizer* equalizer_ = nullptr;

    uint16_t channels_;
    uint32_t inputFrameBytes_;
    uint32_t outputFrameBytes_;

    DitherMode ditherMode_;
    sample_convert::TriangularDither dither_;

    std::vector<double> inputScratch_;
    std::vector<double> outputScratch_;
};

} // namespace multiroom
