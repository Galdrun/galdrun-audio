#include "client/OutputPlanner.h"

#include "client/FormatMatch.h"
#include "resampler/RatioText.h"
#include "util/Logger.h"

#include <cstdint>
#include <string>
#include <utility>

namespace multiroom {
namespace {

std::string describeOffered(const std::vector<AudioFormat>& offered)
{
    std::string out;
    for (const AudioFormat& format : offered)
    {
        if (!out.empty())
            out += ", ";
        out += describeFormat(format);
    }
    return out.empty() ? "nothing" : out;
}

} // namespace

OutputPlanner::OutputPlanner(ResamplerCapabilities capabilities)
    : capabilities_(std::move(capabilities))
{
}

Result<OutputPlanner, PlanError> OutputPlanner::create(const ResamplerRegistry& registry,
                                                       std::string_view resamplerId)
{
    const auto capabilities = registry.capabilities(resamplerId);
    if (!capabilities)
    {
        // The registry's own detail already names the ids this build has,
        // which is what a config naming a resampler a build option left out
        // needs to see.
        return fail(PlanError::UnknownResampler, describe(capabilities.error()));
    }

    // Once, at startup, and the only place a configuration is resolved rather
    // than refused. It earns that by being visible in the log and in status.
    if (!capabilities->variableRatio)
    {
        MULTIROOM_INFO("resampler {} cannot vary a ratio; drift correction is disabled for all rooms",
                 capabilities->id);
    }
    else
    {
        MULTIROOM_INFO("resampler {}", describeCapabilities(*capabilities));
    }

    return OutputPlanner(*capabilities);
}

DriftDisabledReason OutputPlanner::correctionUnavailable() const noexcept
{
    if (capabilities_.id.empty())
        return DriftDisabledReason::NoResampler;
    if (!capabilities_.variableRatio)
        return DriftDisabledReason::FixedRatioResampler;
    return DriftDisabledReason::NotDisabled;
}

OutputPlanner OutputPlanner::withoutResampler()
{
    // An empty id is the discriminator, because a registered implementation
    // always has one. Everything else about these capabilities is a refusal
    // waiting to happen, and planResampled never reaches it.
    MULTIROOM_INFO("no resampler: rooms that cannot play their zone's format exactly are refused");
    return OutputPlanner(ResamplerCapabilities{});
}

Result<OutputPlan, PlanError> OutputPlanner::plan(const AudioFormat& sourceFormat,
                                                  const RoomAudioSettings& settings,
                                                  const ClientCapabilities& capabilities) const
{
    if (!sourceFormat.isValid())
        return fail(PlanError::InvalidSourceFormat, describeFormat(sourceFormat));

    if (capabilities.formats.empty())
        return fail(PlanError::NoFormatsOffered, "the client reported no formats");

    for (const AudioFormat& offered : capabilities.formats)
    {
        if (!offered.isValid())
            return fail(PlanError::NoFormatsOffered,
                        "an offered format names no rate or no channels: " +
                            describeFormat(offered));
    }

    // Whether this server can correct a room at all, and if not, why. A room
    // whose client trims its own clock can be corrected without a converter,
    // and is then corrected through it rather than by resampling
    //.
    const bool hardwareClock = capabilities.driftFeedback && capabilities.hardwareClock.available;
    const DriftDisabledReason unavailable =
        hardwareClock ? DriftDisabledReason::NotDisabled : correctionUnavailable();
    const bool correctable = unavailable == DriftDisabledReason::NotDisabled;
    const DriftActuator actuator = !correctable ? DriftActuator::None
                                   : hardwareClock ? DriftActuator::ClientHardwareClock
                                                   : DriftActuator::ServerResampling;

    const AudioFormat* exact =
        format_match::bestValuePreserving(sourceFormat, capabilities.formats);

    const auto bitperfectPlan = [&](DriftActuator corrected, DriftDisabledReason disabledBy) {
        OutputPlan plan;
        plan.mode = OutputMode::BitPerfect;
        plan.sourceFormat = sourceFormat;
        plan.outputFormat = *exact;
        plan.driftActuator = corrected;
        plan.reason = exact->sampleType == sourceFormat.sampleType
                          ? PlanReason::SourceFormatOffered
                          : PlanReason::SourceFormatRepacked;
        plan.driftDisabledBy = disabledBy;
        return Result<OutputPlan, PlanError>{plan};
    };

    switch (settings.audioControl)
    {
    case AudioControl::BitPerfect:
        if (exact == nullptr)
        {
            // That room's error and not the zone's. A room configured Force on
            // a device that turns out to do 48 kHz only is a configuration
            // mistake, and the useful behaviour is to say so and keep the
            // house playing.
            return fail(PlanError::BitperfectUnavailable,
                        describeFormat(sourceFormat) + " is not playable by this client; it offers " +
                            describeOffered(capabilities.formats));
        }
        // In a shared zone it drifts unless its own clock can be trimmed,
        // which leaves the samples alone: Force is otherwise the one way to
        // opt out of correction.
        if (settings.grouped && hardwareClock)
            return bitperfectPlan(DriftActuator::ClientHardwareClock,
                                  DriftDisabledReason::NotDisabled);
        return bitperfectPlan(DriftActuator::None, settings.grouped
                                                       ? DriftDisabledReason::BitperfectForced
                                                       : DriftDisabledReason::NotDisabled);

    case AudioControl::Auto:
        // Rooms that drift apart are not one zone. A grouped room whose clock
        // is trimmed keeps its samples where it can; otherwise it is
        // resampled to be corrected, and bitperfect gives way to it.
        if (settings.grouped && hardwareClock && exact != nullptr && !settings.equalized)
            return bitperfectPlan(DriftActuator::ClientHardwareClock,
                                  DriftDisabledReason::NotDisabled);
        if (settings.grouped && correctable)
            return planResampled(sourceFormat, capabilities, actuator, PlanReason::Grouped,
                                 DriftDisabledReason::NotDisabled);
        // So does it to an equalizer that is on, which only a resampled path
        // runs. A server with no converter cannot, and stays bitperfect.
        if (settings.equalized && !capabilities_.id.empty())
            return planResampled(sourceFormat, capabilities, actuator, PlanReason::Equalized,
                                 unavailable);
        if (exact != nullptr)
            return bitperfectPlan(DriftActuator::None, settings.grouped
                                                           ? unavailable
                                                           : DriftDisabledReason::NotDisabled);
        break;
    }

    // Everything left over is a room whose device cannot play the source at
    // all. That is the primary reason it is resampled; the converter is there
    // anyway, so it is corrected where it can be.
    return planResampled(sourceFormat, capabilities, actuator, PlanReason::FormatNotOffered,
                         unavailable);
}

Result<OutputPlan, PlanError>
OutputPlanner::planResampled(const AudioFormat& sourceFormat,
                             const ClientCapabilities& capabilities, DriftActuator actuator,
                             PlanReason reason, DriftDisabledReason driftDisabledBy) const
{
    // The one place every resampled plan passes through, and therefore the one
    // place a server with no converter has to say so. It is that room's error
    // and not the zone's: a build with both resampler options off still serves
    // every room whose device can play the source exactly, which is what such
    // a build is for.
    if (capabilities_.id.empty())
    {
        return fail(PlanError::NoResampler,
                    "this server has no resampler; " + describeFormat(sourceFormat) +
                        " is not playable by this client, which offers " +
                        describeOffered(capabilities.formats));
    }

    // The ordered rules of FormatMatch.h. Null is the one case with no answer
    // rather than a worse one: the client offers nothing at this zone's channel
    // count, and nothing in this server changes a channel count.
    const AudioFormat* chosen = format_match::bestFallback(sourceFormat, capabilities);
    if (chosen == nullptr)
    {
        return fail(PlanError::ChannelCountNotOffered,
                    std::to_string(sourceFormat.channels) + " channels; the client offers " +
                        describeOffered(capabilities.formats));
    }

    // Asked of the capabilities and never of an instance: a client that never
    // connects must still be able to invalidate a configuration, and a
    // BitPerfect room must be provably free of a converter.
    if (capabilities_.maxChannels < sourceFormat.channels)
    {
        return fail(PlanError::UnsupportedChannels,
                    capabilities_.id + " handles " + std::to_string(capabilities_.maxChannels) +
                        " channels, this zone has " + std::to_string(sourceFormat.channels));
    }

    const double ratio =
        static_cast<double>(chosen->sampleRate) / static_cast<double>(sourceFormat.sampleRate);
    if (!capabilities_.supportsRatio(ratio))
    {
        return fail(PlanError::UnsupportedRatio,
                    capabilities_.id + " cannot do " + resampler::ratioText(ratio) + "; " +
                        describeCapabilities(capabilities_));
    }

    OutputPlan plan;
    plan.mode = OutputMode::Resampled;
    plan.sourceFormat = sourceFormat;
    plan.outputFormat = *chosen;
    plan.driftActuator = actuator;
    plan.resamplerId = capabilities_.id;
    plan.reason = reason;
    plan.driftDisabledBy =
        actuator != DriftActuator::None ? DriftDisabledReason::NotDisabled : driftDisabledBy;
    return plan;
}

Result<OutputPlan, PlanError>
OutputPlanner::replanForGrantedFormat(const OutputPlan& requested,
                                      const RoomAudioSettings& settings,
                                      const AudioFormat& grantedFormat,
                                      const ClientCapabilities& offered) const
{
    if (grantedFormat == requested.outputFormat)
        return requested;

    // The device opened one format and can be sent no other, so that is the
    // whole of its capability list for this decision. Its rate is also the
    // only sensible preference left.
    ClientCapabilities granted;
    granted.formats.push_back(grantedFormat);
    granted.preferredSampleRate = grantedFormat.sampleRate;
    // The device is the same one, so is its clock: replanning for the format
    // it granted must not quietly move the room back to resampling.
    granted.driftFeedback = offered.driftFeedback;
    granted.hardwareClock = offered.hardwareClock;

    auto replanned = plan(requested.sourceFormat, settings, granted);
    if (!replanned)
        return replanned;

    // The server asked for one thing and was told another, and that is what
    // status reports -- for the usual case, a substituted rate, the plan above
    // has already turned from BitPerfect into Resampled on its own.
    replanned->reason = PlanReason::DriverGrantedDifferentFormat;
    return replanned;
}

} // namespace multiroom
