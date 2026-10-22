#pragma once

// What one room's audio is, and why -- the answer to "is this room
// bitperfect", as data rather than as a branch somewhere in the output path.
//
// A plan is produced by OutputPlanner, logged once per change, reported by the
// status API and consumed by the processor of phase 7. It carries a *reason*
// as well as a mode because "why is this room not bitperfect" is the most
// common question this server will ever be asked, and an operator who has to
// infer the answer from two formats will infer it wrongly.
//
// Nothing here decides anything. The rules live in OutputPlanner; this header
// is the vocabulary they speak, plus the one predicate -- isValuePreserving --
// that defines what "can play the source exactly" means, because MATP
// section 2 allows repacking and that makes the bitperfect test a property of
// a pair of layouts rather than of one.

#include "audio/AudioFormat.h"
#include "core/Error.h"
#include "matp/Protocol.h"
#include "resampler/IAudioResampler.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace multiroom {

enum class PlanError
{
    // The configured resampler id is not in this build's registry. A build
    // with both resampler options off registers none, and that is where a
    // deployment naming one meets a refused configuration rather than a
    // silent resample.
    UnknownResampler,

    // This room would have to be converted and this server has no converter
    // at all -- a build with both resampler options off, which registers none.
    // That room's error and not the zone's: every room whose device can play
    // the source exactly is still served, which is precisely what such a build
    // is for.
    NoResampler,

    InvalidSourceFormat, // zero rate or zero channels

    // Section 12 forbids it: a client must not be sent audio before it has
    // reported its capabilities, so an empty list is a planning error rather
    // than a room with no options.
    NoFormatsOffered,

    // bitperfect Force, and this device cannot play the source exactly. That
    // room's error and not the zone's -- the fault-isolation table's rule for
    // a client whose converter will not configure applies unchanged.
    BitperfectUnavailable,

    // The client offers nothing with the source's channel count. Nothing in
    // this server changes a channel count, so there is no plan to make.
    ChannelCountNotOffered,

    UnsupportedChannels, // more channels than the configured resampler handles
    UnsupportedRatio     // a rate pair outside the configured resampler's range
};

const char* errorName(PlanError error);

// The room's Audio Control (MATP section 59), the one enum the wire,
// server.json and the control API all spell the same way. There is no drift
// setting: whether a room is corrected follows from this, its zone and its
// device.
//
//   * BitPerfect: bitperfect or nothing. Never resampled, never corrected --
//     not even in a zone shared with other rooms, where it drifts; a client
//     that cannot play the source format exactly is an error for that room,
//     and the client may not scale its samples for volume either.
//   * Auto: bitperfect where the format matches, the room plays alone and its
//     equalizer is off. Resampled where the device cannot play the source,
//     where the room shares its zone, or where its equalizer is on (only the
//     resampled path runs it), and corrected whenever it is resampled.
using matp::AudioControl;
using matp::audioControlName;
using matp::parseAudioControl;

// What corrects a room's drift.
// Whether a room is corrected follows from its Audio Control, its zone and its
// device as below; this is only *how*. ServerResampling varies the converter's
// ratio; ClientHardwareClock leaves the stream at its nominal rate and has the
// client trim its DAC clock, which keeps a bitperfect room bitperfect.
enum class DriftActuator
{
    None,
    ServerResampling,
    ClientHardwareClock
};

// "none" | "resampling" | "hardwareClock", as status spells it.
const char* driftActuatorName(DriftActuator actuator);

// Why a room that is grouped or resampled is not corrected. NotDisabled with
// no actuator is a room alone and bitperfect: nothing to correct.
enum class DriftDisabledReason
{
    NotDisabled,

    // bitperfect Force has already said this room will not be resampled, and
    // correction is resampling (section 28).
    BitperfectForced,

    // The configured implementation takes its ratio once. r8brain is that
    // case, and naming it disables correction for every room in the server --
    // resolved rather than refused, and visible in both the startup log and
    // status, because a fixed-ratio converter is a deliberate choice and
    // refusing to start would make it a two-step one.
    FixedRatioResampler,

    // There is no converter at all, and correction is resampling (section 28).
    // Kept apart from FixedRatioResampler because "the one you chose takes its
    // ratio once" and "this build has none" are different things to tell an
    // operator.
    NoResampler
};

// What the planner reads for one room: the operator's Audio Control, and
// whether the room shares its zone with another room, as the Server's wanted
// membership says. The control API stores both per room (phase 17).
struct RoomAudioSettings
{
    AudioControl audioControl = AudioControl::Auto;
    bool grouped = false;
    // The room's equalizer is on: an Auto room leaves bitperfect for it, a
    // BitPerfect one does not.
    bool equalized = false;
};

// What AUDIO_CAPABILITIES (section 12) tells the planner about one room: the
// formats the currently selected device actually accepted, and a hint.
//
// The device's id and name travel with the session rather than with the plan,
// so they are not here: status reports them, and no rule below reads them.
struct ClientCapabilities
{
    // Only combinations the device accepted, in the order the client reported
    // them -- which is the tie-break where two candidates rank equally.
    std::vector<AudioFormat> formats;

    // A fallback rather than a target. Where the source's own format is in the
    // list, section 2 sends that one instead and this is never consulted.
    uint32_t preferredSampleRate = 0;

    // "exclusive" | "shared", empty when the client did not say. Reported,
    // never planned on: `formats` are already the ones of this mode.
    std::string accessMode;

    // The client reads DRIFT_FEEDBACK, and the trim its session selected and
    // the opened device confirmed -- available only then. A room with one is
    // corrected through it rather than by resampling.
    bool driftFeedback = false;
    matp::HardwareClockCapability hardwareClock;
};

enum class OutputMode
{
    BitPerfect, // no converter exists for this room at all
    Resampled
};

// Why the mode above. Reported in status verbatim.
enum class PlanReason
{
    SourceFormatOffered,  // the device accepts the source's own layout
    SourceFormatRepacked, // the same sample values in a layout the device takes

    FormatNotOffered, // the device cannot play the source at all
    Grouped,          // the room shares its zone and is kept in step
    Equalized,        // the room's equalizer is on, and only a resampled path runs it

    // The room was re-planned on STREAM_READY because the driver granted
    // something other than what SESSION_CONFIG asked for (section 12). The
    // server asked for one thing and was told another, and status says so
    // rather than reporting the plan it did not get.
    DriverGrantedDifferentFormat
};

// The human phrase status and the log line use. Not an enumeration to switch
// on -- the enums above are that.
const char* planReasonText(PlanReason reason);
const char* driftDisabledText(DriftDisabledReason reason);

struct OutputPlan
{
    OutputMode mode = OutputMode::BitPerfect;

    // The zone's, as the decoder produced it.
    AudioFormat sourceFormat;

    // This room's own, as SESSION_CONFIG will name it.
    AudioFormat outputFormat;

    DriftActuator driftActuator = DriftActuator::None;

    // Empty for BitPerfect, because there is no converter: bitperfect is not a
    // ratio of one, it is no resampler at all.
    std::string resamplerId;

    PlanReason reason = PlanReason::SourceFormatOffered;

    // Only meaningful while no actuator corrects the room. NotDisabled then
    // means nobody asked for correction, which is most rooms most of the time.
    DriftDisabledReason driftDisabledBy = DriftDisabledReason::NotDisabled;

    // Corrected by either actuator: what status and the hard resync of
    // section 32 read. Only the server's own converter varies a ratio.
    [[nodiscard]] bool driftCorrected() const noexcept
    {
        return driftActuator != DriftActuator::None;
    }
    [[nodiscard]] bool resamplingCorrected() const noexcept
    {
        return driftActuator == DriftActuator::ServerResampling;
    }

    // What phase 7 hands the converter, derived rather than stored so the
    // drift flag cannot be got wrong in two places. requiresVariableRatio is
    // resamplingCorrected(): SoXR builds its constant-rate engine for a room that is
    // not being corrected and its variable-rate one for a room that is, and
    // the difference is 15.5 dB of noise floor. A planner that left the flag
    // false and then called setRatio would get FixedRatioOnly, which is a
    // planning defect and reads as one.
    //
    // A BitPerfect plan returns a default-constructed config -- an invalid
    // one, because there is nothing to configure.
    [[nodiscard]] ResamplerConfig resamplerConfig() const;
};

// "44100 Hz 2ch int24 -> 48000 Hz 2ch int32: resampled by soxr, drift
// corrected (the client does not offer the source format)"
std::string describePlan(const OutputPlan& plan);

// Whether a source layout reaches a device as `to` with its sample values
// intact -- MATP section 2's bitperfect rule, which is about values and not
// about byte identity.
//
// Repacking is allowed: 24 bits packed into three bytes and the same 24 bits
// in a 32-bit container are the same numbers, and a device that offers only
// one of the two layouts is not thereby disqualified from bitperfect playback.
// Widening is allowed for the same reason -- 16-bit samples in a wider integer
// container are exactly recoverable, and nothing has been decided about them.
//
// Narrowing is not, and neither is any crossing between integer and float: a
// float conversion is a change of representation, and phase 7's direct path is
// permitted a layout change and nothing else. This is the single definition
// both the planner and that path work from.
[[nodiscard]] bool isValuePreserving(AudioFormat::SampleType from, AudioFormat::SampleType to);

} // namespace multiroom
