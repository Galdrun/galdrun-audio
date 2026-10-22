#pragma once

// The single place that decides whether a client's audio is touched at all.
//
// Everything about bitperfect playback is here, so that no other component
// ever has to ask the question and no other component can answer it
// differently. The planner is pure: it reads a source format, a room's
// settings and one client's capability list, and returns data. It constructs
// no converter -- not even to ask what one can do -- which is what lets a
// BitPerfect plan be proven to create nothing, and what lets a configuration
// be validated at startup for a client that may never connect.
//
// **No input other than the Audio Control, the grouping and the format may
// turn resampling on.** Not the packet size, not the buffer state, not
// "everything is easier at 48 kHz". The six rows below are the whole rule set:
//
//   audio control | grouped | can play exactly | result
//   BitPerfect    | no      | yes              | BitPerfect, uncorrected
//   BitPerfect    | yes     | yes              | BitPerfect, uncorrected: drifts
//   BitPerfect    | any     | no               | Error for that room
//   Auto          | no      | yes              | BitPerfect, uncorrected
//   Auto          | no      | no               | Resampled, corrected
//   Auto          | yes     | any              | Resampled, corrected
//
// Only the grouped row resamples a room that did not need it, because rooms
// that drift apart are not one zone. Without a variable-ratio resampler
// nothing is corrected, and a grouped Auto room is bitperfect where it can be.
//
// **One implementation for the whole server, named once, and never a mix.**
// Every resampled room in every zone gets the id this planner was built with.
// Picking per room -- the variable-ratio converter for corrected rooms and the
// quieter fixed one for the rest -- is the obvious optimisation and it is
// refused: two rooms playing the same zone would then be converted by two
// different filters, which is a difference nobody can hear in isolation and
// everybody can hear between two rooms of one house.

#include "client/OutputPlan.h"
#include "resampler/ResamplerRegistry.h"

#include <string>
#include <string_view>

namespace multiroom {

class OutputPlanner
{
public:
    // Resolves the configured resampler once. The registry is read here and
    // not kept: the planner needs the implementation's *capabilities*, never
    // an instance of it, so there is no lifetime coupling to a registry that
    // outlives startup anyway.
    //
    // An id that is not in this build is refused, with the ids that are in it
    // named in the detail. A fixed-ratio id is *not* refused -- it is resolved,
    // and logged once here as the roadmap's startup line, because choosing a
    // fixed-ratio converter is choosing rooms that drift and refusing to start
    // would make that a two-step decision.
    [[nodiscard]] static Result<OutputPlanner, PlanError> create(const ResamplerRegistry& registry,
                                                                 std::string_view resamplerId);

    // A server with no converter at all, which is what a build with both
    // resampler options off is: `registerBuiltinResamplers` adds nothing to
    // its registry and there is no id to name. Every bitperfect row of the
    // table above is unchanged; every row that would resample is
    // `NoResampler` for *that room*, and the zone keeps playing for the rooms
    // whose devices match it.
    //
    // It is a planner rather than the absence of one because the alternative
    // is a second place that decides a room's format, and the whole of this
    // class's argument is that there is exactly one. It logs its own startup
    // line for the same reason `create` does.
    [[nodiscard]] static OutputPlanner withoutResampler();

    [[nodiscard]] const std::string& resamplerId() const noexcept { return capabilities_.id; }

    [[nodiscard]] bool hasResampler() const noexcept { return !capabilities_.id.empty(); }

    // What status reports about the one implementation this server converts
    // with, and what the two rules below are decided from.
    [[nodiscard]] const ResamplerCapabilities& resamplerCapabilities() const noexcept
    {
        return capabilities_;
    }

    // Why this server corrects no room, or NotDisabled where it can: there is
    // no converter, or the one it has takes its ratio once.
    [[nodiscard]] DriftDisabledReason correctionUnavailable() const noexcept;

    // One room's plan, from the zone's source format and that client's
    // capabilities. Nothing here mutates; a client with two zones over its
    // lifetime is two calls.
    [[nodiscard]] Result<OutputPlan, PlanError> plan(const AudioFormat& sourceFormat,
                                                     const RoomAudioSettings& settings,
                                                     const ClientCapabilities& capabilities) const;

    // STREAM_READY (section 12): the driver granted a format rather than
    // obeying one. The plan is made again against the single format the device
    // actually opened, because that is now the only format this room can be
    // sent, and the reason becomes "driver granted a different format" so that
    // status reports the plan the server got instead of the one it asked for.
    //
    // A granted format equal to the requested one is not a re-negotiation and
    // returns `requested` unchanged. `offered` is what the first plan was made
    // against; its drift actuator carries over, because the device is the same.
    [[nodiscard]] Result<OutputPlan, PlanError>
    replanForGrantedFormat(const OutputPlan& requested, const RoomAudioSettings& settings,
                           const AudioFormat& grantedFormat,
                           const ClientCapabilities& offered) const;

private:
    explicit OutputPlanner(ResamplerCapabilities capabilities);

    [[nodiscard]] Result<OutputPlan, PlanError> planResampled(const AudioFormat& sourceFormat,
                                                              const ClientCapabilities& capabilities,
                                                              DriftActuator actuator,
                                                              PlanReason reason,
                                                              DriftDisabledReason driftDisabledBy) const;

    ResamplerCapabilities capabilities_;
};

} // namespace multiroom
