#include "processing/IClientAudioProcessor.h"

#include "processing/DirectPcmProcessor.h"
#include "processing/ResamplingProcessor.h"

namespace multiroom {

const char* errorName(ProcessorError error)
{
    switch (error)
    {
    case ProcessorError::InvalidPlan:
        return "InvalidPlan";
    case ProcessorError::UnsupportedConversion:
        return "UnsupportedConversion";
    case ProcessorError::MisalignedBuffer:
        return "MisalignedBuffer";
    case ProcessorError::RatioNotAdjustable:
        return "RatioNotAdjustable";
    case ProcessorError::ResamplerFailed:
        return "ResamplerFailed";
    }
    return "?";
}

const char* ditherModeName(DitherMode mode)
{
    switch (mode)
    {
    case DitherMode::No:
        return "off";
    case DitherMode::TriangularPdf:
        return "tpdf";
    }
    return "off";
}

bool parseDitherMode(std::string_view text, DitherMode& out)
{
    for (const DitherMode mode : {DitherMode::No, DitherMode::TriangularPdf})
    {
        if (text == ditherModeName(mode))
        {
            out = mode;
            return true;
        }
    }
    return false;
}

Result<std::unique_ptr<IClientAudioProcessor>, ProcessorError>
makeClientAudioProcessor(const OutputPlan& plan, const ResamplerRegistry& registry,
                         DitherMode dither)
{
    if (plan.mode == OutputMode::BitPerfect)
    {
        // The registry is not consulted, not even to ask what an
        // implementation can do. That is the whole of criterion 5: a
        // bitperfect room constructs no converter because there is no call
        // here that could.
        auto direct = DirectPcmProcessor::create(plan);
        if (!direct)
            return std::unexpected(direct.error());
        return std::unique_ptr<IClientAudioProcessor>(std::move(*direct));
    }

    auto resampling = ResamplingProcessor::create(plan, registry, dither);
    if (!resampling)
        return std::unexpected(resampling.error());
    return std::unique_ptr<IClientAudioProcessor>(std::move(*resampling));
}

} // namespace multiroom
