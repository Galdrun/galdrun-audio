#include "resampler/IAudioResampler.h"

namespace multiroom {

const char* errorName(ResamplerError error)
{
    switch (error)
    {
    case ResamplerError::UnknownId:
        return "UnknownId";
    case ResamplerError::DuplicateId:
        return "DuplicateId";
    case ResamplerError::InvalidConfig:
        return "InvalidConfig";
    case ResamplerError::UnsupportedChannels:
        return "UnsupportedChannels";
    case ResamplerError::UnsupportedRatio:
        return "UnsupportedRatio";
    case ResamplerError::FixedRatioOnly:
        return "FixedRatioOnly";
    case ResamplerError::NotConfigured:
        return "NotConfigured";
    case ResamplerError::MisalignedBuffer:
        return "MisalignedBuffer";
    case ResamplerError::LibraryFailure:
        return "LibraryFailure";
    }
    return "?";
}

bool ResamplerCapabilities::supportsRatio(double value) const noexcept
{
    return value >= minRatio && value <= maxRatio;
}

std::string describeCapabilities(const ResamplerCapabilities& capabilities)
{
    return capabilities.id + " (" +
           (capabilities.variableRatio ? "variable ratio" : "fixed ratio") + ", <=" +
           std::to_string(capabilities.maxChannels) + "ch, " +
           std::to_string(capabilities.minRatio) + ".." +
           std::to_string(capabilities.maxRatio) + ")";
}

bool ResamplerConfig::isValid() const noexcept
{
    return inputRate > 0 && outputRate > 0 && channels > 0 && initialRatio >= 0.0;
}

double ResamplerConfig::nominalRatio() const noexcept
{
    if (inputRate == 0 || outputRate == 0)
        return 0.0;
    return static_cast<double>(outputRate) / static_cast<double>(inputRate);
}

double ResamplerConfig::effectiveRatio() const noexcept
{
    return initialRatio > 0.0 ? initialRatio : nominalRatio();
}

} // namespace multiroom
