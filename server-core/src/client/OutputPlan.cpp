#include "client/OutputPlan.h"

namespace multiroom {

const char* errorName(PlanError error)
{
    switch (error)
    {
    case PlanError::UnknownResampler:
        return "UnknownResampler";
    case PlanError::NoResampler:
        return "NoResampler";
    case PlanError::InvalidSourceFormat:
        return "InvalidSourceFormat";
    case PlanError::NoFormatsOffered:
        return "NoFormatsOffered";
    case PlanError::BitperfectUnavailable:
        return "BitperfectUnavailable";
    case PlanError::ChannelCountNotOffered:
        return "ChannelCountNotOffered";
    case PlanError::UnsupportedChannels:
        return "UnsupportedChannels";
    case PlanError::UnsupportedRatio:
        return "UnsupportedRatio";
    }
    return "?";
}

const char* planReasonText(PlanReason reason)
{
    switch (reason)
    {
    case PlanReason::SourceFormatOffered:
        return "the client offers the source format";
    case PlanReason::SourceFormatRepacked:
        return "the client offers the source's sample values in another layout";
    case PlanReason::FormatNotOffered:
        return "the client does not offer the source format";
    case PlanReason::Grouped:
        return "the room shares its zone with other rooms and is drift corrected";
    case PlanReason::Equalized:
        return "the room's equalizer is on";
    case PlanReason::DriverGrantedDifferentFormat:
        return "driver granted a different format";
    }
    return "?";
}

const char* driftActuatorName(DriftActuator actuator)
{
    switch (actuator)
    {
    case DriftActuator::None:
        return "none";
    case DriftActuator::ServerResampling:
        return "resampling";
    case DriftActuator::ClientHardwareClock:
        return "hardwareClock";
    }
    return "?";
}

const char* driftDisabledText(DriftDisabledReason reason)
{
    switch (reason)
    {
    case DriftDisabledReason::NotDisabled:
        return "drift correction is not enabled for this room";
    case DriftDisabledReason::BitperfectForced:
        return "the room's Audio Control is bitPerfect";
    case DriftDisabledReason::FixedRatioResampler:
        return "the configured resampler cannot vary a ratio";
    case DriftDisabledReason::NoResampler:
        return "this server has no resampler";
    }
    return "?";
}

ResamplerConfig OutputPlan::resamplerConfig() const
{
    if (mode != OutputMode::Resampled)
        return {};

    ResamplerConfig config;
    config.inputRate = sourceFormat.sampleRate;
    config.outputRate = outputFormat.sampleRate;
    config.channels = outputFormat.channels;

    // Zero means the nominal outputRate / inputRate, which is every client
    // that is not yet being corrected -- including a corrected one before its
    // first trim arrives from the sync controller of phase 12.
    config.initialRatio = 0.0;

    // A room corrected by its own clock runs the conversion at a fixed ratio.
    config.requiresVariableRatio = resamplingCorrected();
    return config;
}

std::string describePlan(const OutputPlan& plan)
{
    std::string out = describeFormat(plan.sourceFormat) + " -> " +
                      describeFormat(plan.outputFormat) + ": ";

    if (plan.mode == OutputMode::BitPerfect)
    {
        out += "bitperfect";
    }
    else
    {
        out += "resampled by " + plan.resamplerId;
    }

    // Honest reporting: the correction state is never inferred from the mode,
    // because a resampled room is not necessarily a corrected one.
    out += plan.driftActuator == DriftActuator::ClientHardwareClock
               ? ", hardware clock synchronized"
           : plan.driftCorrected() ? ", drift corrected"
                                   : ", uncorrected";

    out += " (";
    out += planReasonText(plan.reason);
    if (!plan.driftCorrected() && plan.driftDisabledBy != DriftDisabledReason::NotDisabled)
    {
        out += "; ";
        out += driftDisabledText(plan.driftDisabledBy);
    }
    out += ")";
    return out;
}

bool isValuePreserving(AudioFormat::SampleType from, AudioFormat::SampleType to)
{
    if (from == to)
        return true;

    // Integer and float never meet. Converting between them is a change of
    // representation rather than of layout, and phase 7's direct path is
    // permitted a layout change and nothing else.
    const bool fromFloat = from == AudioFormat::SampleType::Float32;
    const bool toFloat = to == AudioFormat::SampleType::Float32;
    if (fromFloat || toFloat)
        return false;

    return AudioFormat::validBits(to) >= AudioFormat::validBits(from);
}

} // namespace multiroom
