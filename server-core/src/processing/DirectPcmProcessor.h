#pragma once

// The bitperfect path: the room is sent the source's own sample values, and
// the only thing that may happen to them on the way is a change of layout.
//
// No float conversion, no gain, no dither, no DSP, and no resampler with a
// ratio of one -- there is no converter here to have a ratio. Where the
// layouts already agree the whole path is a memcpy from the shared block into
// the packet payload, and phase 9 may skip even that where the block's bytes
// can go to the socket unchanged.
//
// It lives beside its source rather than in include/ because nothing above the
// processing layer constructs one: makeClientAudioProcessor is what turns a
// plan into a path, and IClientAudioProcessor is the only view of the result
// the rest of the server has.

#include "processing/IClientAudioProcessor.h"

#include <memory>

namespace multiroom {

class DirectPcmProcessor final : public IClientAudioProcessor
{
public:
    // Refuses anything that is not a layout change: a plan that is not
    // BitPerfect, a rate or channel count that differs between the two
    // formats, or a pair isValuePreserving rejects.
    [[nodiscard]] static Result<std::unique_ptr<DirectPcmProcessor>, ProcessorError>
    create(const OutputPlan& plan);

    [[nodiscard]] const OutputPlan& plan() const noexcept override { return plan_; }

    // Exactly one, always, and not because a converter was set to it.
    [[nodiscard]] double ratio() const noexcept override { return 1.0; }

    Status<ProcessorError> setRatio(double ratio) override;

    Result<ProcessResult, ProcessorError> process(std::span<const std::byte> input,
                                                  std::span<std::byte> output) override;

    // Nothing is held between calls, so there is nothing to discard. A
    // bitperfect room rejoining costs the packetizer a cursor move and this
    // object nothing at all.
    void reset() override {}

    [[nodiscard]] double latencyFrames() const noexcept override { return 0.0; }

private:
    DirectPcmProcessor(const OutputPlan& plan, uint32_t inputFrameBytes,
                       uint32_t outputFrameBytes) noexcept;

    OutputPlan plan_;
    uint32_t inputFrameBytes_;
    uint32_t outputFrameBytes_;
};

} // namespace multiroom
