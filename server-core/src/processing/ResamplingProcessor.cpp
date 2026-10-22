#include "processing/ResamplingProcessor.h"

#include "resampler/RatioText.h"

#include <algorithm>
#include <string>
#include <utility>

namespace multiroom {

using resampler::ratioText;

ResamplingProcessor::ResamplingProcessor(const OutputPlan& plan,
                                         std::unique_ptr<IAudioResampler> resampler,
                                         DitherMode dither)
    : plan_(plan), resampler_(std::move(resampler)), channels_(plan.outputFormat.channels),
      inputFrameBytes_(plan.sourceFormat.bytesPerFrame()),
      outputFrameBytes_(plan.outputFormat.bytesPerFrame()), ditherMode_(dither),
      inputScratch_(kChunkFrames * plan.outputFormat.channels),
      outputScratch_(kChunkFrames * plan.outputFormat.channels)
{
}

Result<std::unique_ptr<ResamplingProcessor>, ProcessorError>
ResamplingProcessor::create(const OutputPlan& plan, const ResamplerRegistry& registry,
                            DitherMode dither)
{
    if (plan.mode != OutputMode::Resampled)
        return fail(ProcessorError::InvalidPlan, "the resampled path needs a resampled plan");

    if (!plan.sourceFormat.isValid() || !plan.outputFormat.isValid())
        return fail(ProcessorError::InvalidPlan, describePlan(plan));

    // Nothing in this server mixes or reorders channels, and IAudioResampler
    // converts a count to itself, so a plan naming two is one the pipeline
    // cannot execute.
    if (plan.sourceFormat.channels != plan.outputFormat.channels)
    {
        return fail(ProcessorError::InvalidPlan,
                    "no channel count is changed here: " + describeFormat(plan.sourceFormat) +
                        " -> " + describeFormat(plan.outputFormat));
    }

    // A ratio that can never move is no filter at all. The conversion through
    // double still happens -- that is what makes this room resampled -- but
    // the registry is never asked, so nothing is built and nothing is
    // configured.
    const bool needsConverter = plan.sourceFormat.sampleRate != plan.outputFormat.sampleRate ||
                                plan.resamplingCorrected();
    if (!needsConverter)
        return std::unique_ptr<ResamplingProcessor>(
            new ResamplingProcessor(plan, nullptr, dither));

    auto resampler = registry.create(plan.resamplerId);
    if (!resampler)
    {
        // The planner refused an unknown id at startup, so a plan carrying one
        // was built against a different registry than the one handed here.
        return fail(ProcessorError::InvalidPlan, describe(resampler.error()));
    }

    const ResamplerConfig config = plan.resamplerConfig();
    auto configured = (*resampler)->configure(config);
    if (!configured)
    {
        return fail(ProcessorError::ResamplerFailed,
                    plan.resamplerId + " at " + ratioText(config.effectiveRatio()) + ": " +
                        describe(configured.error()));
    }

    return std::unique_ptr<ResamplingProcessor>(
        new ResamplingProcessor(plan, std::move(*resampler), dither));
}

sample_convert::TriangularDither* ResamplingProcessor::dither() noexcept
{
    // Phase 31: only a room below 24 valid bits has a rounding worth breaking
    // up. Float32 is never dithered by fromDouble() in any case.
    const bool narrow = AudioFormat::validBits(plan_.outputFormat.sampleType) < 24;
    return ditherMode_ == DitherMode::TriangularPdf && narrow ? &dither_ : nullptr;
}

double ResamplingProcessor::ratio() const noexcept
{
    return resampler_ ? resampler_->ratio() : 1.0;
}

Status<ProcessorError> ResamplingProcessor::setRatio(double ratio)
{
    // The plan's drift flag is what chose the converter's engine, so a room
    // planned without a correction cannot be given one afterwards.
    if (!plan_.resamplingCorrected() || !resampler_)
    {
        return fail(ProcessorError::RatioNotAdjustable,
                    "this room is not corrected by resampling: " + describePlan(plan_));
    }

    auto status = resampler_->setRatio(ratio);
    if (!status)
        return fail(ProcessorError::ResamplerFailed, describe(status.error()));

    return {};
}

Result<ProcessResult, ProcessorError> ResamplingProcessor::process(std::span<const std::byte> input,
                                                                   std::span<std::byte> output)
{
    if (input.size() % inputFrameBytes_ != 0 || output.size() % outputFrameBytes_ != 0)
    {
        return fail(ProcessorError::MisalignedBuffer,
                    std::to_string(input.size()) + " in / " + std::to_string(output.size()) +
                        " bytes is not a whole number of " + std::to_string(inputFrameBytes_) +
                        " / " + std::to_string(outputFrameBytes_) + "-byte frames");
    }

    const size_t inputFrames = input.size() / inputFrameBytes_;
    const size_t outputFrames = output.size() / outputFrameBytes_;
    const size_t channels = channels_;

    size_t consumed = 0;
    size_t produced = 0;

    while (consumed < inputFrames && produced < outputFrames)
    {
        const size_t inChunk = std::min(kChunkFrames, inputFrames - consumed);
        const size_t outChunk = std::min(kChunkFrames, outputFrames - produced);

        if (!resampler_)
        {
            // No filter: the frames pass through the working format and come
            // back out in the room's own layout, one for one.
            const size_t frames = std::min(inChunk, outChunk);
            sample_convert::toDouble(input.subspan(consumed * inputFrameBytes_),
                                     plan_.sourceFormat.sampleType,
                                     std::span<double>(inputScratch_).first(frames * channels));
            if (equalizer_ != nullptr)
                equalizer_->process(std::span<double>(inputScratch_).first(frames * channels));
            sample_convert::fromDouble(
                std::span<const double>(inputScratch_).first(frames * channels),
                output.subspan(produced * outputFrameBytes_), plan_.outputFormat.sampleType,
                dither());
            consumed += frames;
            produced += frames;
            continue;
        }

        sample_convert::toDouble(input.subspan(consumed * inputFrameBytes_),
                                 plan_.sourceFormat.sampleType,
                                 std::span<double>(inputScratch_).first(inChunk * channels));

        auto result = resampler_->process(
            std::span<const double>(inputScratch_).first(inChunk * channels),
            std::span<double>(outputScratch_).first(outChunk * channels));
        if (!result)
            return fail(ProcessorError::ResamplerFailed, describe(result.error()));

        if (result->outputFramesProduced > 0)
        {
            if (equalizer_ != nullptr)
                equalizer_->process(std::span<double>(outputScratch_)
                                        .first(result->outputFramesProduced * channels));
            sample_convert::fromDouble(
                std::span<const double>(outputScratch_)
                    .first(result->outputFramesProduced * channels),
                output.subspan(produced * outputFrameBytes_), plan_.outputFormat.sampleType,
                dither());
        }

        // A converter that took nothing and gave nothing has been handed less
        // than it needs for one output frame. Looping again would not change
        // that, and the frames it did not take stay the caller's, so the call
        // ends here and the caller re-presents them with more behind them.
        if (result->inputFramesConsumed == 0 && result->outputFramesProduced == 0)
            break;

        consumed += result->inputFramesConsumed;
        produced += result->outputFramesProduced;
    }

    return ProcessResult{consumed, produced};
}

void ResamplingProcessor::reset()
{
    if (resampler_)
        resampler_->reset();
    if (equalizer_ != nullptr)
        equalizer_->reset();
}

double ResamplingProcessor::latencyFrames() const noexcept
{
    return resampler_ ? resampler_->latencyFrames() : 0.0;
}

} // namespace multiroom
