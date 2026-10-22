#pragma once

// One room's sync state: how fast its sound card actually runs, how far its
// playback has slipped from the zone's timeline, and the resampling ratio that
// would put it back.
//
// Phase 12 computed all three and connected none of them. **Phase 13 connects
// the last one**, and adds the term that phase reserved for it: each new drift
// fit is taken up at the confidence that produced it, so a room whose clock
// cannot be measured is nudged rather than yanked -- and a steady one still
// ends up corrected in full. What leaves here is `appliedPpm`, and
// `correctionPpm` beside it is what this controller would ask for if it were
// certain -- two numbers rather than one because an operator reading a room
// that is drifting needs to know whether the controller is unwilling or merely
// unsure.
//
// The input is PLAYBACK_REPORT (section 25) and nothing else. One report
// carries three things this class uses:
//
//     deviceFramePosition     this room's own hardware frame counter
//     localMonotonicTimeNs    the instant it was read, on the *client's* clock
//     timelineNsAudible       where that put the room on the zone's timeline
//
// Five decisions the protocol leaves open.
//
//   * **The offset is computed in server time.** The two position fields are
//     on the client's own axes, so every report is put through phase 8's
//     ClientClockModel before anything is done with it. What *was* audible in
//     the room is then compared against what the zone's timeline says *should*
//     have been audible at that same server instant (section 20's running
//     line). There is no other definition of the offset, and no shortcut that
//     avoids the clock model -- which is why a room whose model is not valid
//     yet contributes nothing rather than contributing a guess.
//
//   * **The client's own drift figure is an input, not the answer.** The
//     server keeps its own regression of deviceFramePosition against server
//     time, exactly as section 26 describes, and compares the two. The
//     client measures its card against its *own* clock, so its figure is put
//     on this server's clock first, through the model's scale -- the same
//     scale the server's regression was measured through, and exactly 1
//     until it has been measured. Two independent derivations agreeing is the
//     cross-check that section asks for; a disagreement is recorded and logged rather than averaged away,
//     because where the two disagree it is the clock model between them that
//     is suspect and not the sound card.
//
//   * **An underrun invalidates the window.** `underruns` counts *events*, and
//     the frames a device silenced are frames its own position counter already
//     counted -- so a regression spanning the gap measures the gap. Any report
//     whose count has risen restarts this window, exactly as the client
//     restarts its own.
//
//   * **A format transition ends the series.** The nominal rate changed and
//     the device's counter started again (sections 24 and 25), so reset() is
//     called with the new rate and confidence climbs back from nothing.
//
//   * **A reconnect ends it too, and carries the answer over.** Phase 16: the
//     window was mapped through a clock model that went with the connection,
//     and a device that was reopened counts from zero again -- so nothing
//     measured survives carryOver(). What does survive is the correction this
//     room was already being given, because an oscillator does not change with
//     a socket, and a room that had to converge again from 1.0 after every cut
//     would spend its evening converging. It is replaced by the first real
//     measurement rather than averaged with it.
//
//   * **Confidence is measured the same way the client measures it** -- window
//     fill, the standard error of the fitted slope, and how much the device's
//     own timestamps can be trusted. Not because that formula is canonical,
//     but because the cross-check above is only meaningful when the two sides
//     express their certainty in the same terms. The extra uncertainty this
//     side carries -- the clock model's own -- needs no term of its own: a
//     noisy mapping widens the residuals, which is exactly what the standard
//     error already measures.
//
// The sign convention is the project's one ratio convention and is tested
// rather than reasoned about: ratio = outputFrames / inputFrames, so a card
// running fast needs *more* output frames for the same span of timeline and
// gets a ratio above one. A room that is ahead of the timeline is slowed by
// being given still more frames for that span, so the offset term adds.
//
// **No lock of its own.** One of these belongs to one room and is called from
// whatever owns that room -- the zone, under the zone's mutex.

#include "audio/AudioClockSnapshot.h"
#include "client/ClientClockModel.h"
#include "core/Units.h"
#include "matp/Protocol.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>

namespace multiroom {

// Why one report did or did not move the model.
//
// Not an error enum: a report this controller cannot use is ordinary traffic
// -- a device that has not started, a clock model that is still filling, a
// room that just underran -- and none of it is a fault of the room or of the
// connection. It is returned so that the caller can log it and the tests can
// name it.
enum class SyncSample
{
    Accepted,      // it moved the regression
    NoClockModel,  // phase 8's model is not valid yet: there is no server instant
    NoDeviceClock, // clockQuality "unavailable": the position is worthless
    Underrun,      // section 36: the device silenced frames it had counted
    Stepped        // the clock or the frame counter went backwards
};

const char* syncSampleName(SyncSample sample);

struct ClientSyncSettings
{
    // Section 46's drift regression window. Section 26 asks for "10-30
    // seconds" and 46 names 20, which at the client's 500 ms reporting
    // interval is forty points -- enough for a slope, short enough to follow
    // an oscillator that warms up.
    uint64_t driftWindowNs = 20ULL * kNanosecondsPerSecond;

    // Bounded, like every other buffer here. A client is free to report four
    // times a second (section 25's lower bound is 250 ms), which is 80 points
    // in the window; the ceiling is what stops a client reporting far faster
    // than that from growing this one.
    size_t maxSamples = 256;

    // Below this many points no estimate is published at all. Eight is the
    // client's own floor, for the same reason: a slope through three points of
    // scheduler jitter is jitter.
    size_t minSamples = 8;

    // Section 31's PI controller, in the units the numbers are actually in:
    // ppm of ratio per millisecond of offset, and per millisecond-second of
    // accumulated offset. A 4 ms offset therefore asks for 40 ppm, which
    // closes it in about a hundred seconds -- slow enough that section 31's
    // "no fast audible pitch changes" is met by construction (100 ppm is under
    // a fifth of a cent), and fast enough that a room does not spend an
    // evening converging.
    //
    // **Starting values, to be replaced by a measurement.** Nothing has been
    // corrected with them yet; phase 13's milestone is what settles them.
    double offsetGainPpmPerMs = 10.0;
    double offsetIntegralPpmPerMsSecond = 0.5;

    // The integral term's own ceiling, so that a long stretch of reports the
    // controller could not act on cannot wind it up into a step when it can.
    double integralLimitPpm = 50.0;

    // Section 31's two limits. The wider one applies only while a series is
    // still converging for the first time -- after a start, a reset or a
    // format transition -- and the room has never yet been inside
    // `convergedOffsetNs`. Once it has, this room is a room that is in sync,
    // and an excursion past the normal limit is a fault to be seen rather than
    // a licence to pull harder.
    double correctionLimitPpm = 100.0;
    double convergenceLimitPpm = 500.0;
    uint64_t convergedOffsetNs = 5ULL * 1000000ULL;

    // Section 26's cross-check. Where both sides are confident and they still
    // disagree by more than this, the clock model between them is suspect and
    // the difference is logged rather than fed to a resampler.
    double crossCheckPpm = 5.0;
    double crossCheckConfidence = 0.7;

    // Section 32's hard resync, as the offset no ratio is going to close. 50 ms
    // is the wide end of that section's example range, and deliberately the
    // wide end: the controller above closes a 4 ms offset in about a hundred
    // seconds, so an offset anywhere near this one arrived at once. It is a
    // step -- a stall, a seek, a reconnect, an evicted store -- and never
    // accumulated drift, because a room that is allowed to drift is a room
    // nobody is correcting and the caller does not offer it a resync at all
    // (phase 13's rule, kept where the plan is). Zero disables it.
    uint64_t hardResyncOffsetNs = 50ULL * 1000000ULL;

    // Consecutive accepted reports past that offset before one is asked for.
    // A single report is a clock model that hiccupped; three running, half a
    // second apart, is a room that is somewhere else.
    uint32_t hardResyncSamples = 3;

    // The hardware clock's loop.
    // Trimming the card makes the drift regression a measurement of the
    // residual, which is not a rate to command, so this is section 31's
    // offset PI alone: its integral learns the card's own error and holds it
    // when the residual reaches zero. One ppm moves the offset one
    // microsecond a second, so these put the loop's natural period near three
    // minutes with a damping of about 0.8. Starting points, to be confirmed
    // by measurement on the hardware.
    double hardwareGainPpmPerMs = 50.0;
    double hardwareIntegralPpmPerMsSecond = 1.0;

    // How fast the commanded trim may move. A conservative limit until
    // listening tests on the hardware say otherwise.
    double hardwareSlewPpmPerSecond = 20.0;

    [[nodiscard]] bool isValid() const;
};

// One room's sync state as a value, so that a caller holding the zone's mutex
// can answer a status question after letting go of it.
struct ClientSyncStatus
{
    // --- the server's own regression (section 26) ----------------------------
    bool hasDrift = false;
    double measuredRateHz = 0.0;
    double driftPpm = 0.0;
    double standardErrorPpm = 0.0;
    double confidence = 0.0;
    size_t samples = 0;
    uint64_t windowSpanNs = 0;
    // The worst clock quality in the window, the one the fit rests on.
    AudioClockQuality clockQuality = AudioClockQuality::Unavailable;

    // --- what the client derived from the same positions --------------------
    bool hasClientDrift = false;
    double clientDriftPpm = 0.0;
    double clientConfidence = 0.0;

    // --- the position error (section 29) -------------------------------------
    //
    // Signed, and necessarily so: positive is a room that is ahead of the
    // zone's timeline. Absent until the zone is running and the room has an
    // audible position on it.
    bool hasOffset = false;
    int64_t offsetNs = 0;

    // Where the offset is held (phase 27): the room's output latency, and
    // zero for a room without one.
    int64_t targetOffsetNs = 0;

    // --- the answer, and how much of it a room is actually given -------------
    //
    // `correctionPpm` is what the controller asks for from the latest fit;
    // `appliedPpm` is what a room is given: the drift filtered at the
    // confidence above, plus the offset term scaled by it -- phase 13's "low
    // confidence means do less, not guess", as a slower uptake rather than a
    // permanent discount. The gap between them closes as a steady fit is taken
    // up, and is the measurement's own doubt made readable meanwhile.
    double correctionPpm = 0.0;
    double appliedPpm = 0.0;
    double ratio = 1.0;      // 1 + appliedPpm / 1e6, the project's convention
    bool converging = false; // the wider of section 31's two limits is in force

    // Section 32: the offset has been past `hardResyncOffsetNs` for
    // `hardResyncSamples` reports running, so no ratio is going to close it.
    // A request and not an action -- this class moves nothing, and whether a
    // room may be resynced at all is a property of its plan rather than of its
    // measurements.
    bool resyncWanted = false;

    // What the correction started from rather than what has been measured
    // since: the figure carryOver() kept across a reconnect or a device
    // reopen. Zero for a room whose series has never been interrupted, and
    // stops being used the moment `hasDrift` is true again.
    //
    // While it is in use -- non-zero, with `hasDrift` false -- `appliedPpm` is
    // scaled by the certainty that figure was *measured* with rather than by
    // `confidence` above, which is the empty window's and is zero. That is the
    // whole of "a reconnect costs nothing": the room carries on being given
    // what it was being given, and the moment `hasDrift` is true again the
    // measurement replaces it.
    double carriedPpm = 0.0;

    // --- the hardware clock ----------------------------------------------------
    //
    // While the room is corrected through its client's clock: the absolute
    // trim relative to nominal this controller commands (positive slows), and
    // what the client last reported having programmed. `correctionPpm` is
    // then the command, `appliedPpm` the client's figure, and `ratio` 1.0:
    // nothing on this server is resampled for it.
    bool hardwareClock = false;
    double hardwareTargetPpm = 0.0;
    bool hasClientClock = false;
    int32_t clientClockPpm = 0;
    bool clientClockSaturated = false;

    // --- counters -------------------------------------------------------------
    uint32_t nominalRateHz = 0;
    uint64_t reports = 0;
    uint64_t accepted = 0;
    uint64_t rejected = 0;
    uint64_t resets = 0;

    // Section 26's disagreement, counted at most once per drift window so that
    // a persistent one is a line an operator reads rather than two a second.
    // The difference is the server's figure less the client's, the client's
    // put on this server's clock first.
    uint64_t disagreements = 0;
    double lastDisagreementPpm = 0.0;
};

class ClientSyncController
{
public:
    explicit ClientSyncController(uint32_t nominalRateHz, ClientSyncSettings settings = {});

    // One PLAYBACK_REPORT, and the two things it cannot be read without:
    // phase 8's model of this client's clock, and what the zone says should
    // have been audible at the instant the report names. The second is absent
    // on a zone that has not started, which costs the offset and leaves the
    // drift regression alone -- a card's rate can be measured long before
    // anything of the timeline is audible.
    SyncSample onPlaybackReport(const matp::PlaybackReport& report,
                                const ClientClockModel& clock,
                                std::optional<TimelineNs> expectedAudible);

    // Sections 24 and 25: this room's stream format changed, so the nominal
    // rate is new and the device's frame counter started again. Everything
    // measured is dropped; the counters are lifetime figures and are not.
    void reset(uint32_t nominalRateHz);

    // Section 43: the same room, on a new connection or on a device that has
    // been reopened. Everything measured goes -- the clock model this window
    // was mapped through went with the connection, and a reopened device
    // counts from zero again -- but what this room's card was measured to be
    // doing is carried forward as the correction it starts from, because an
    // oscillator does not change with a socket. That is the whole difference
    // between this and reset(), and it is the difference between a reconnect
    // costing nothing and costing a fresh convergence.
    void carryOver(uint32_t nominalRateHz);

    // Section 32: this room's stream has been moved to another timeline
    // position, so the offset measured against the old one describes a room
    // that no longer exists and the integral behind it an error nobody is
    // going to correct. The drift regression is deliberately not touched: it
    // is the same card, and server time did not stop.
    void clearOffset();

    // Phase 27: the offset this room is held at instead of zero. A room heard
    // through a receiver's DSP is started that much ahead of the line, and
    // correcting it back to zero would undo exactly that. Everything that
    // acts on the offset -- the correction, convergence, section 32's
    // threshold -- acts on its distance from this target; the offset itself
    // is still reported against the line. A change clears the offset, as a
    // move does, because the room is about to be started somewhere else.
    void setTargetOffset(int64_t targetNs);

    // The actuator this room is corrected through, and the trim range its
    // device reported. A change of actuator starts the hardware loop over; it
    // is seeded from what the client reports having programmed, so the state
    // here agrees with the trim the client actually kept.
    void setHardwareClock(bool enabled, int32_t minPpm, int32_t maxPpm);

    [[nodiscard]] ClientSyncStatus status() const;

    // Where this room is heard against the line, as its last
    // `kSteadyOffsetReports` accepted reports agree on it: their median, and
    // nothing while there are fewer of them since the offset was last cleared
    // or while they are more than 5 ms apart. The figure a line is moved by
    // (Zone, a room joining one that plays alone) and therefore not one
    // report, which can be the clock model between two machines hiccupping.
    static constexpr size_t kSteadyOffsetReports = 5;
    [[nodiscard]] std::optional<int64_t> steadyOffsetNs() const;

    [[nodiscard]] const ClientSyncSettings& settings() const noexcept { return settings_; }

private:
    struct Sample
    {
        uint64_t serverTimeNs = 0;
        uint64_t frames = 0;
    };

    // Drops the window and the controller's own state, keeping the lifetime
    // counters. Used by reset() and by every discontinuity that makes the
    // window describe a device that no longer exists.
    void restart();

    void trimWindow();
    void recomputeDrift();
    void updateOffset(const matp::PlaybackReport& report, uint64_t serverTimeNs,
                      std::optional<TimelineNs> expectedAudible);
    void recomputeCorrection();
    void recomputeHardware(const matp::PlaybackReport& report, uint64_t serverTimeNs);
    void crossCheck(const matp::PlaybackReport& report, uint64_t serverTimeNs, double scale);

    ClientSyncSettings settings_;
    uint32_t nominalRateHz_ = 0;

    std::deque<Sample> window_;
    AudioClockQuality worstQuality_ = AudioClockQuality::Unavailable;
    uint64_t lastUnderruns_ = 0;
    bool hasUnderruns_ = false;

    bool hasDrift_ = false;
    double measuredRateHz_ = 0.0;
    double driftPpm_ = 0.0;
    double standardErrorPpm_ = 0.0;
    double confidence_ = 0.0;
    // The drift term actually applied: each fit taken up at its confidence.
    double filteredDriftPpm_ = 0.0;

    bool hasOffset_ = false;
    int64_t offsetNs_ = 0;
    std::array<int64_t, kSteadyOffsetReports> recentOffsetsNs_{};
    size_t recentOffsets_ = 0; // how many of them are real; the next is written at % size
    uint64_t offsetAtNs_ = 0; // the server instant the offset above was measured at
    int64_t targetOffsetNs_ = 0;
    double integralMsSeconds_ = 0.0;
    bool converged_ = false;

    double correctionPpm_ = 0.0;
    double appliedPpm_ = 0.0;

    // What carryOver() kept, standing in for the drift term until this room
    // has been measured again, and how many accepted reports running have
    // been past section 32's threshold.
    double carriedPpm_ = 0.0;
    double carriedConfidence_ = 0.0;
    uint32_t beyondOffset_ = 0;
    bool resyncWanted_ = false;

    bool hasClientDrift_ = false;
    double clientDriftPpm_ = 0.0;
    double clientConfidence_ = 0.0;

    uint64_t reports_ = 0;
    uint64_t accepted_ = 0;
    uint64_t rejected_ = 0;
    uint64_t resets_ = 0;
    uint64_t disagreements_ = 0;
    double lastDisagreementPpm_ = 0.0;
    uint64_t lastDisagreementAtNs_ = 0;
    bool hasDisagreementInstant_ = false;

    bool hardware_ = false;
    double hardwareMinPpm_ = 0.0;
    double hardwareMaxPpm_ = 0.0;
    bool hardwareSeeded_ = false;
    double hardwareIntegralPpm_ = 0.0;
    double hardwareTargetPpm_ = 0.0;
    uint64_t hardwareAtNs_ = 0; // the last offset integrated, 0 for none
    bool hasClientClock_ = false;
    int32_t clientClockPpm_ = 0;
    bool clientClockSaturated_ = false;
};

} // namespace multiroom
