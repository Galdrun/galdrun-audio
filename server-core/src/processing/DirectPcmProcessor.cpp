#include "processing/DirectPcmProcessor.h"

#include "processing/SampleConvert.h"

#include <algorithm>
#include <string>

namespace multiroom {

DirectPcmProcessor::DirectPcmProcessor(const OutputPlan& plan, uint32_t inputFrameBytes,
                                       uint32_t outputFrameBytes) noexcept
    : plan_(plan), inputFrameBytes_(inputFrameBytes), outputFrameBytes_(outputFrameBytes)
{
}

Result<std::unique_ptr<DirectPcmProcessor>, ProcessorError>
DirectPcmProcessor::create(const OutputPlan& plan)
{
    if (plan.mode != OutputMode::BitPerfect)
        return fail(ProcessorError::InvalidPlan, "the direct path needs a bitperfect plan");

    if (!plan.sourceFormat.isValid() || !plan.outputFormat.isValid())
        return fail(ProcessorError::InvalidPlan, describePlan(plan));

    // A bitperfect room is never resampled for drift -- the planner says so
    // before a client connects -- and one here would be a plan asking for a
    // correction from a path that holds no converter. Its client trimming its
    // own clock is another matter: that changes no sample.
    if (plan.resamplingCorrected())
        return fail(ProcessorError::InvalidPlan, "a bitperfect plan cannot be resampled for drift");

    if (plan.sourceFormat.sampleRate != plan.outputFormat.sampleRate ||
        plan.sourceFormat.channels != plan.outputFormat.channels)
    {
        return fail(ProcessorError::InvalidPlan,
                    "the direct path changes no rate and no channel count: " +
                        describeFormat(plan.sourceFormat) + " -> " +
                        describeFormat(plan.outputFormat));
    }

    if (!isValuePreserving(plan.sourceFormat.sampleType, plan.outputFormat.sampleType))
    {
        return fail(ProcessorError::UnsupportedConversion,
                    std::string(sampleTypeName(plan.sourceFormat.sampleType)) + " -> " +
                        sampleTypeName(plan.outputFormat.sampleType) +
                        " is not a layout change");
    }

    return std::unique_ptr<DirectPcmProcessor>(new DirectPcmProcessor(
        plan, plan.sourceFormat.bytesPerFrame(), plan.outputFormat.bytesPerFrame()));
}

Status<ProcessorError> DirectPcmProcessor::setRatio(double ratio)
{
    (void)ratio;
    return fail(ProcessorError::RatioNotAdjustable,
                "this room is bitperfect and has no converter to trim");
}

Result<ProcessResult, ProcessorError> DirectPcmProcessor::process(std::span<const std::byte> input,
                                                                  std::span<std::byte> output)
{
    if (input.size() % inputFrameBytes_ != 0 || output.size() % outputFrameBytes_ != 0)
    {
        return fail(ProcessorError::MisalignedBuffer,
                    std::to_string(input.size()) + " in / " + std::to_string(output.size()) +
                        " bytes is not a whole number of " + std::to_string(inputFrameBytes_) +
                        " / " + std::to_string(outputFrameBytes_) + "-byte frames");
    }

    // One frame in, one frame out: there is no state, no lookahead and nothing
    // held back, so the count is simply whichever span runs out first.
    const size_t frames =
        std::min(input.size() / inputFrameBytes_, output.size() / outputFrameBytes_);

    sample_convert::repack(input, plan_.sourceFormat.sampleType, output,
                           plan_.outputFormat.sampleType,
                           frames * plan_.outputFormat.channels);

    return ProcessResult{frames, frames};
}

} // namespace multiroom
