// Phases 12 and 13: what a room's sound card is actually doing, measured
// twice, and the correction that follows from it (MATP sections 25, 26 and
// 29 to 31).
//
// Every test here drives a *synthetic* client whose card runs at a known ppm
// and whose reports carry known noise, and then asks whether the drift, the
// offset and the confidence found their way back to the injected truth. An
// assertion that the controller produced *a* number would say nothing about a
// regression, and a sign error in one produces a perfectly healthy-looking
// server that slowly desynchronizes -- which is the whole reason this phase
// exists before anything is corrected.
//
// Nothing here sleeps and nothing here needs a room: the controller holds no
// clock and no socket, so every instant is a parameter.

#include "TestSupport.h"

#include "client/ClientSyncController.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>

using namespace multiroom;

namespace {

constexpr uint64_t kMs = 1000000ULL;
constexpr uint64_t kSecond = kNanosecondsPerSecond;

// Far enough into both clocks that nothing works by accident from an origin of
// zero, and far enough apart that a test confusing the two axes fails loudly.
constexpr uint64_t kClientOriginNs = 400ULL * kSecond;
constexpr int64_t kClockOffsetNs = -37LL * static_cast<int64_t>(kSecond);

// One completed clock exchange over a link of `roundTripNs`, split evenly,
// seen from a client whose clock reads `offsetNs` less than the server's.
matp::ClockSyncResult exchange(uint64_t clientSendNs, int64_t offsetNs, uint64_t roundTripNs)
{
    matp::ClockSyncResult result;
    result.clientSendNs = clientSendNs;
    result.serverReceiveNs =
        static_cast<uint64_t>(static_cast<int64_t>(clientSendNs) + offsetNs) + roundTripNs / 2;
    result.serverSendNs = result.serverReceiveNs;
    result.clientReceiveNs = clientSendNs + roundTripNs;
    return result;
}

// Phase 8's model, fitted well enough to be valid: section 46's burst of
// twenty on connect, a fifth of a second apart, over a 1 ms link.
ClientClockModel mappedClock(int64_t offsetNs = kClockOffsetNs)
{
    ClientClockModel model;
    for (int i = 0; i < 20; ++i)
        model.addSample(exchange(kClientOriginNs + static_cast<uint64_t>(i) * 200 * kMs,
                                 offsetNs, 1 * kMs));
    return model;
}

// A room: one sound card running `ppm` fast against its nominal rate, one
// clock offset from the server's, and one reporting cadence.
//
// It produces PLAYBACK_REPORTs the way the client does -- a frame position and
// the instant it was read -- and the jitter is applied to the *timestamp*
// rather than to the position, because that is where the noise actually is: a
// frame counter is exact and the moment it was read is not.
class Room
{
public:
    explicit Room(uint32_t nominalRateHz, double ppm) : rate_(nominalRateHz), ppm_(ppm) {}

    void setJitter(uint64_t jitterNs) { jitterNs_ = jitterNs; }
    void setQuality(AudioClockQuality quality) { quality_ = quality; }
    void underrun() { ++underruns_; }

    [[nodiscard]] uint32_t rate() const { return rate_; }
    [[nodiscard]] uint64_t clientTimeNs() const { return clientNs_; }

    // Advances `intervalNs` and reports. `audible`, when set, is what this
    // room says was audible at that instant.
    matp::PlaybackReport advance(uint64_t intervalNs)
    {
        clientNs_ += intervalNs;

        const double seconds = static_cast<double>(clientNs_ - kClientOriginNs) * 1e-9;
        const double actualRate = static_cast<double>(rate_) * (1.0 + ppm_ * 1e-6);

        matp::PlaybackReport report;
        report.clockQuality = quality_;
        report.localMonotonicTimeNs = clientNs_ + jitter();
        report.deviceFramePosition = static_cast<uint64_t>(actualRate * seconds + 0.5);
        report.bufferedDurationNs = 1500 * kMs;
        report.underruns = underruns_;
        return report;
    }

private:
    // Deterministic, so a failure is reproducible. A linear congruential step
    // is noise enough to break a fit that only works on evenly spaced points.
    uint64_t jitter()
    {
        if (jitterNs_ == 0)
            return 0;
        seed_ = seed_ * 6364136223846793005ULL + 1442695040888963407ULL;
        return (seed_ >> 33) % jitterNs_;
    }

    uint32_t rate_;
    double ppm_;
    uint64_t clientNs_ = kClientOriginNs;
    uint64_t jitterNs_ = 0;
    uint64_t underruns_ = 0;
    AudioClockQuality quality_ = AudioClockQuality::Hardware;
    uint64_t seed_ = 0x9e3779b97f4a7c15ULL;
};

// The zone's line, as this file uses it: the timeline reads the same as server
// time, so "what should have been audible" at an instant is that instant. It
// is an arbitrary anchor and deliberately so -- the controller compares two
// positions on one axis and has no opinion about where the axis starts.
TimelineNs expectedAt(uint64_t serverTimeNs)
{
    return TimelineNs{serverTimeNs};
}

// Runs `count` reports into the controller with no offset comparison at all,
// which is a zone that has not started: the drift regression runs and the
// offset does not.
void measureDrift(ClientSyncController& controller, Room& room, const ClientClockModel& clock,
                  size_t count, uint64_t intervalNs = 500 * kMs)
{
    for (size_t i = 0; i < count; ++i)
        (void)controller.onPlaybackReport(room.advance(intervalNs), clock, std::nullopt);
}

// The same, on a zone that is playing and a room that is `errorNs` ahead of
// the timeline (negative: behind).
void measureOffset(ClientSyncController& controller, Room& room, const ClientClockModel& clock,
                   size_t count, int64_t errorNs, uint64_t intervalNs = 500 * kMs)
{
    for (size_t i = 0; i < count; ++i)
    {
        matp::PlaybackReport report = room.advance(intervalNs);
        const uint64_t serverNs = clock.toServerTimeNs(report.localMonotonicTimeNs);
        report.hasTimelineNsAudible = true;
        report.timelineNsAudible =
            static_cast<uint64_t>(static_cast<int64_t>(serverNs) + errorNs);
        (void)controller.onPlaybackReport(report, clock, expectedAt(serverNs));
    }
}

// Phase 13, and the only thing here that is a loop rather than a measurement:
// a room whose offset moves at the difference between its own card and the
// correction the server is applying to it.
//
// The arithmetic is section 30's seen from the room. The server sends `trim`
// frames of this room's stream for every frame of timeline and the card plays
// them `1 + cardPpm` too fast, so what is audible advances at
// (1 + cardPpm) / trim timeline-seconds per second -- a difference of
// (cardPpm - appliedPpm) parts per million, integrated. The device's own frame
// counter is not in that loop at all: it counts what the card played, at the
// rate the card runs, whatever the server sends.
//
// Returns where the room ended up, the worst excursion on the way, and how far
// the applied ratio moved over the last fifth of the run -- a controller that
// converges by overshooting, or one that never stops chasing its own input,
// is not one to ship.
struct Convergence
{
    int64_t finalNs = 0;
    int64_t settledStartNs = 0; // where it was when the last fifth began
    int64_t worstNs = 0;
    double appliedPpm = 0.0;
    double lowestSettledPpm = 0.0;
    double highestSettledPpm = 0.0;

    [[nodiscard]] double settledSwingPpm() const
    {
        return highestSettledPpm - lowestSettledPpm;
    }
};

Convergence converge(ClientSyncController& controller, const ClientClockModel& clock,
                     uint32_t rate, double cardPpm, size_t reports, uint64_t jitterNs = 0,
                     uint64_t intervalNs = 500 * kMs,
                     AudioClockQuality quality = AudioClockQuality::Hardware)
{
    Room room(rate, cardPpm);
    room.setJitter(jitterNs);
    room.setQuality(quality);

    Convergence out;
    out.lowestSettledPpm = 1e9;
    out.highestSettledPpm = -1e9;

    const size_t settledFrom = reports - reports / 5;
    int64_t offsetNs = 0;

    for (size_t i = 0; i < reports; ++i)
    {
        matp::PlaybackReport report = room.advance(intervalNs);

        // Two instants, and the difference between them is the whole of what
        // jitter does to a controller. The report was *read* at the room's own
        // instant, which is what its audible position belongs to; the
        // timestamp it carries is that instant plus the noise, which is what
        // the server has to place it by. A test that used one for both would
        // cancel the noise it set out to inject.
        const uint64_t trueServerNs = clock.toServerTimeNs(room.clientTimeNs());
        const uint64_t statedServerNs = clock.toServerTimeNs(report.localMonotonicTimeNs);

        const double slipPpm = cardPpm - out.appliedPpm;
        offsetNs += static_cast<int64_t>(
            std::llround(slipPpm * 1e-6 * static_cast<double>(intervalNs)));

        report.hasTimelineNsAudible = true;
        report.timelineNsAudible =
            static_cast<uint64_t>(static_cast<int64_t>(trueServerNs) + offsetNs);

        (void)controller.onPlaybackReport(report, clock, expectedAt(statedServerNs));
        out.appliedPpm = controller.status().appliedPpm;

        if (std::llabs(offsetNs) > std::llabs(out.worstNs))
            out.worstNs = offsetNs;
        if (i == settledFrom)
            out.settledStartNs = offsetNs;
        if (i >= settledFrom)
        {
            out.lowestSettledPpm = std::min(out.lowestSettledPpm, out.appliedPpm);
            out.highestSettledPpm = std::max(out.highestSettledPpm, out.appliedPpm);
        }
    }

    out.finalNs = offsetNs;
    return out;
}

} // namespace

// --- the clock model is the only way onto the server's axis -------------------

MULTIROOM_TEST(sync_refuses_every_report_until_the_clock_model_is_valid)
{
    // Not a fault and not silence: the client is there and reporting, and the
    // server simply cannot say when. Counted, so that a session which never
    // gets a model is visible rather than merely quiet.
    const ClientClockModel empty;
    ClientSyncController controller(48000);
    Room room(48000, 30.0);

    for (int i = 0; i < 40; ++i)
    {
        CHECK(controller.onPlaybackReport(room.advance(500 * kMs), empty, std::nullopt) ==
              SyncSample::NoClockModel);
    }

    const ClientSyncStatus status = controller.status();
    CHECK(!status.hasDrift);
    CHECK_EQ(status.reports, 40ULL);
    CHECK_EQ(status.accepted, 0ULL);
    CHECK_EQ(status.rejected, 40ULL);
    CHECK_EQ(status.correctionPpm, 0.0);
    CHECK_EQ(status.ratio, 1.0);
}

MULTIROOM_TEST(sync_refuses_a_report_from_a_client_with_no_usable_device_clock)
{
    const ClientClockModel clock = mappedClock();
    ClientSyncController controller(48000);
    Room room(48000, 30.0);

    matp::PlaybackReport report = room.advance(500 * kMs);
    report.clockQuality = AudioClockQuality::Unavailable;

    CHECK(controller.onPlaybackReport(report, clock, std::nullopt) ==
          SyncSample::NoDeviceClock);
    CHECK_EQ(controller.status().rejected, 1ULL);
    CHECK(!controller.status().hasDrift);
}

// --- the drift regression (section 26) ---------------------------------------

MULTIROOM_TEST(sync_publishes_nothing_until_the_window_has_enough_points)
{
    const ClientClockModel clock = mappedClock();
    ClientSyncController controller(48000);
    Room room(48000, 30.0);

    const ClientSyncSettings& settings = controller.settings();
    measureDrift(controller, room, clock, settings.minSamples - 1);
    CHECK(!controller.status().hasDrift);
    CHECK_EQ(controller.status().samples, settings.minSamples - 1);

    measureDrift(controller, room, clock, 1);
    CHECK(controller.status().hasDrift);
}

MULTIROOM_TEST(sync_finds_an_injected_plus_thirty_ppm_card)
{
    const ClientClockModel clock = mappedClock();
    ClientSyncController controller(48000);
    Room room(48000, 30.0);

    measureDrift(controller, room, clock, 40); // one full 20 s window

    const ClientSyncStatus status = controller.status();
    CHECK(status.hasDrift);
    CHECK_NEAR(status.driftPpm, 30.0, 0.5);
    CHECK_NEAR(status.measuredRateHz, 48000.0 * (1.0 + 30e-6), 0.05);
    CHECK(status.standardErrorPpm < 1.0);
    CHECK_EQ(status.nominalRateHz, 48000u);
}

MULTIROOM_TEST(sync_finds_an_injected_minus_twenty_ppm_card)
{
    // Both signs, because a fit that is right only on one of them is a fit
    // with a sign error and a lucky test.
    const ClientClockModel clock = mappedClock();
    ClientSyncController controller(44100);
    Room room(44100, -20.0);

    measureDrift(controller, room, clock, 40);

    const ClientSyncStatus status = controller.status();
    CHECK(status.hasDrift);
    CHECK_NEAR(status.driftPpm, -20.0, 0.5);
}

MULTIROOM_TEST(sync_measures_drift_against_this_rooms_own_rate_and_not_the_zones)
{
    // Section 26: the nominal rate is this client's stream rate. The same card
    // error expressed against the wrong rate is a different ppm, which is the
    // failure a 44.1 kHz room in a 48 kHz house would show.
    const ClientClockModel clock = mappedClock();
    ClientSyncController controller(44100);
    Room room(44100, 25.0);

    measureDrift(controller, room, clock, 40);

    const ClientSyncStatus status = controller.status();
    CHECK_NEAR(status.driftPpm, 25.0, 0.5);
    CHECK_NEAR(status.measuredRateHz, 44100.0 * (1.0 + 25e-6), 0.05);
}

MULTIROOM_TEST(sync_survives_a_millisecond_of_timestamp_noise)
{
    // A millisecond of jitter on a 20 s lever arm is 50 ppm of error in any
    // single pair; the regression is what turns forty of them into a number
    // worth having, and this is the test that says by how much.
    const ClientClockModel clock = mappedClock();
    ClientSyncController controller(48000);
    Room room(48000, 30.0);
    room.setJitter(1 * kMs);

    measureDrift(controller, room, clock, 40);

    const ClientSyncStatus status = controller.status();
    CHECK(status.hasDrift);

    // The tolerance is the fit's own standard error rather than a number
    // chosen to make this pass: a millisecond of jitter over a 20 s lever arm
    // resolves the slope to a handful of ppm, and the controller says so
    // instead of pretending otherwise.
    CHECK(status.standardErrorPpm > 1.0);
    CHECK(std::fabs(status.driftPpm - 30.0) < 3.0 * status.standardErrorPpm);

    // ...and its confidence is discounted by that error: no better than how
    // far the drift stands clear of three of them, which is what phase 13
    // damps its response with.
    const double drift = std::fabs(status.driftPpm);
    CHECK(status.confidence <= drift / (drift + 3.0 * status.standardErrorPpm) + 1e-9);
    CHECK(status.confidence < 0.95);
}

MULTIROOM_TEST(sync_trusts_a_large_drift_through_a_coarse_position_counter)
{
    // A card whose position counter only moves in whole USB frames fits with
    // an error of several ppm, and a drift of 150 ppm still stands far clear
    // of it. Judged on the error alone this room was corrected by a tenth of
    // what it needed and fell 50 ms behind every few minutes.
    const ClientClockModel clock = mappedClock();
    ClientSyncController controller(48000);
    Room room(48000, -150.0);
    room.setJitter(1 * kMs);

    measureDrift(controller, room, clock, 40);

    const ClientSyncStatus status = controller.status();
    CHECK(status.standardErrorPpm > 1.0);
    CHECK(status.confidence > 0.7);
}

MULTIROOM_TEST(sync_confidence_climbs_with_the_window_and_never_exceeds_one)
{
    const ClientClockModel clock = mappedClock();
    ClientSyncController controller(48000);
    Room room(48000, 30.0);

    measureDrift(controller, room, clock, 10); // 5 s of a 20 s window
    const double quarter = controller.status().confidence;
    CHECK(quarter > 0.0);
    CHECK(quarter < 0.5);

    measureDrift(controller, room, clock, 30); // the rest of it
    const double full = controller.status().confidence;
    CHECK(full > quarter);
    CHECK(full > 0.8);
    CHECK(full <= 1.0);
}

MULTIROOM_TEST(sync_discounts_a_room_whose_device_clock_is_only_estimated)
{
    // The client scales its own confidence by where the timestamp came from,
    // and so does this: two derivations that weigh their certainty
    // differently cannot be cross-checked against each other.
    const ClientClockModel clock = mappedClock();
    ClientSyncController controller(48000);
    Room room(48000, 30.0);

    for (int i = 0; i < 40; ++i)
    {
        matp::PlaybackReport report = room.advance(500 * kMs);
        report.clockQuality = AudioClockQuality::Estimated;
        (void)controller.onPlaybackReport(report, clock, std::nullopt);
    }

    const ClientSyncStatus status = controller.status();
    CHECK(status.hasDrift);
    CHECK_NEAR(status.driftPpm, 30.0, 0.5);
    CHECK(status.confidence < 0.5);
    CHECK(status.confidence > 0.2);
}

MULTIROOM_TEST(sync_window_is_bounded_by_its_duration_and_by_its_ceiling)
{
    ClientSyncSettings settings;
    settings.driftWindowNs = 5 * kSecond;
    settings.maxSamples = 12;

    const ClientClockModel clock = mappedClock();
    ClientSyncController controller(48000, settings);
    Room room(48000, 30.0);

    measureDrift(controller, room, clock, 200, 100 * kMs);

    const ClientSyncStatus status = controller.status();
    CHECK(status.samples <= settings.maxSamples);
    CHECK(status.windowSpanNs <= settings.driftWindowNs);
    CHECK(status.hasDrift);
}

// --- what ends a series -------------------------------------------------------

MULTIROOM_TEST(sync_restarts_the_window_at_an_underrun)
{
    // Section 36: the frames a device silenced were counted by the counter
    // this regresses, so a window spanning the gap measures the gap rather
    // than the oscillator.
    const ClientClockModel clock = mappedClock();
    ClientSyncController controller(48000);
    Room room(48000, 30.0);

    measureDrift(controller, room, clock, 40);
    CHECK(controller.status().hasDrift);

    room.underrun();
    CHECK(controller.onPlaybackReport(room.advance(500 * kMs), clock, std::nullopt) ==
          SyncSample::Underrun);

    const ClientSyncStatus status = controller.status();
    CHECK(!status.hasDrift);
    CHECK_EQ(status.samples, size_t(1)); // the position after the gap is good
    CHECK_EQ(status.confidence, 0.0);

    // ...and it comes back, from nothing, without anything being reset by hand.
    measureDrift(controller, room, clock, 40);
    CHECK(controller.status().hasDrift);
    CHECK_NEAR(controller.status().driftPpm, 30.0, 1.0);
}

MULTIROOM_TEST(sync_restarts_the_window_when_the_frame_counter_steps_backwards)
{
    const ClientClockModel clock = mappedClock();
    ClientSyncController controller(48000);
    Room room(48000, 30.0);

    measureDrift(controller, room, clock, 40);

    matp::PlaybackReport reopened = room.advance(500 * kMs);
    reopened.deviceFramePosition = 0; // a device that was reopened under us
    CHECK(controller.onPlaybackReport(reopened, clock, std::nullopt) == SyncSample::Stepped);
    CHECK(!controller.status().hasDrift);
}

MULTIROOM_TEST(sync_reset_ends_the_series_at_a_format_transition)
{
    // Sections 24 and 25: the nominal rate changed and the counter started
    // again, so confidence climbs back from nothing against the new rate.
    const ClientClockModel clock = mappedClock();
    ClientSyncController controller(48000);
    Room room(48000, 30.0);

    measureDrift(controller, room, clock, 40);
    CHECK(controller.status().hasDrift);

    controller.reset(96000);
    const ClientSyncStatus after = controller.status();
    CHECK(!after.hasDrift);
    CHECK_EQ(after.confidence, 0.0);
    CHECK_EQ(after.nominalRateHz, 96000u);
    CHECK_EQ(after.resets, 1ULL);
    CHECK_EQ(after.reports, 40ULL); // a lifetime counter, and not reset

    Room reopened(96000, -15.0);
    measureDrift(controller, reopened, clock, 40);
    CHECK_NEAR(controller.status().driftPpm, -15.0, 0.5);
}

// --- the offset (section 29) ---------------------------------------------------

MULTIROOM_TEST(sync_has_no_offset_until_the_zone_has_a_line_to_compare_against)
{
    const ClientClockModel clock = mappedClock();
    ClientSyncController controller(48000);
    Room room(48000, 30.0);

    measureDrift(controller, room, clock, 40);

    const ClientSyncStatus status = controller.status();
    CHECK(status.hasDrift); // the card's rate is measurable long before this
    CHECK(!status.hasOffset);
}

MULTIROOM_TEST(sync_offset_is_positive_for_a_room_that_is_ahead_of_the_timeline)
{
    const ClientClockModel clock = mappedClock();
    ClientSyncController controller(48000);
    Room room(48000, 0.0);

    measureOffset(controller, room, clock, 10, 4 * static_cast<int64_t>(kMs));

    const ClientSyncStatus status = controller.status();
    CHECK(status.hasOffset);
    CHECK_NEAR(static_cast<double>(status.offsetNs), 4.0e6, 1.0);
}

MULTIROOM_TEST(sync_offset_is_negative_for_a_room_that_is_behind_the_timeline)
{
    const ClientClockModel clock = mappedClock();
    ClientSyncController controller(48000);
    Room room(48000, 0.0);

    measureOffset(controller, room, clock, 10, -7 * static_cast<int64_t>(kMs));

    CHECK_NEAR(static_cast<double>(controller.status().offsetNs), -7.0e6, 1.0);
}

MULTIROOM_TEST(sync_offset_is_computed_through_the_clock_model_and_not_around_it)
{
    // The proof that the conversion is happening at all: the room is exactly
    // on the timeline, and a controller that compared the client's own
    // nanoseconds against the server's would report the whole clock offset as
    // an error of thirty-seven seconds.
    const ClientClockModel clock = mappedClock();
    ClientSyncController controller(48000);
    Room room(48000, 0.0);

    measureOffset(controller, room, clock, 10, 0);

    const ClientSyncStatus status = controller.status();
    CHECK(status.hasOffset);
    CHECK(std::llabs(status.offsetNs) < 100000); // under 100 us, not 37 s
}

// --- the correction, computed and not applied (sections 30 and 31) -------------

MULTIROOM_TEST(sync_gives_a_fast_card_a_ratio_above_one)
{
    // The project's one ratio convention: outputFrames / inputFrames. A card
    // running fast consumes its frames too quickly and therefore needs *more*
    // of them for the same span of timeline. Tested rather than reasoned
    // about, because the failure mode of a sign error here is a healthy server
    // that slowly desynchronizes.
    const ClientClockModel clock = mappedClock();
    ClientSyncController controller(48000);
    Room room(48000, 30.0);

    measureDrift(controller, room, clock, 40);

    const ClientSyncStatus status = controller.status();
    CHECK_NEAR(status.correctionPpm, 30.0, 0.5);
    CHECK(status.ratio > 1.0);

    // The ratio is the *applied* answer: the drift filtered at the confidence
    // that produced it, which a full window of a steady card has caught up.
    CHECK(status.confidence > 0.5);
    CHECK_NEAR(status.appliedPpm, status.correctionPpm, 0.5);
    CHECK_NEAR(status.ratio, 1.0 + status.appliedPpm / 1e6, 1e-12);
}

MULTIROOM_TEST(sync_gives_a_slow_card_a_ratio_below_one)
{
    const ClientClockModel clock = mappedClock();
    ClientSyncController controller(48000);
    Room room(48000, -30.0);

    measureDrift(controller, room, clock, 40);

    const ClientSyncStatus status = controller.status();
    CHECK_NEAR(status.correctionPpm, -30.0, 0.5);
    CHECK(status.ratio < 1.0);
    CHECK_NEAR(status.appliedPpm, status.correctionPpm, 0.5);
    CHECK_NEAR(status.ratio, 1.0 + status.appliedPpm / 1e6, 1e-12);
}

MULTIROOM_TEST(sync_slows_a_room_that_is_ahead_by_asking_for_still_more_frames)
{
    // A room that is ahead has to make the same span of timeline last longer,
    // which in this convention means more output frames for it -- so the
    // offset term adds to the drift term rather than fighting it.
    const ClientClockModel clock = mappedClock();
    ClientSyncController controller(48000);
    Room room(48000, 0.0);

    measureOffset(controller, room, clock, 2, 3 * static_cast<int64_t>(kMs));
    const double ahead = controller.status().correctionPpm;
    CHECK(ahead > 0.0);

    ClientSyncController behind(48000);
    Room other(48000, 0.0);
    measureOffset(behind, other, clock, 2, -3 * static_cast<int64_t>(kMs));
    CHECK(behind.status().correctionPpm < 0.0);
    CHECK_NEAR(behind.status().correctionPpm, -ahead, 1.0);
}

MULTIROOM_TEST(sync_correction_is_clamped_to_the_normal_limit_once_a_room_has_converged)
{
    ClientSyncSettings settings;
    settings.correctionLimitPpm = 100.0;
    settings.convergenceLimitPpm = 500.0;

    const ClientClockModel clock = mappedClock();
    ClientSyncController controller(48000, settings);
    Room room(48000, 0.0);

    // Inside the converged band first, which is what retires the wider limit.
    measureOffset(controller, room, clock, 4, 1 * static_cast<int64_t>(kMs));
    CHECK(!controller.status().converging);

    // Then a gross excursion: 200 ms of offset asks for 2000 ppm.
    measureOffset(controller, room, clock, 4, 200 * static_cast<int64_t>(kMs));
    CHECK_NEAR(controller.status().correctionPpm, 100.0, 1e-9);
}

MULTIROOM_TEST(sync_allows_the_wider_limit_only_while_a_room_is_still_converging)
{
    ClientSyncSettings settings;
    settings.correctionLimitPpm = 100.0;
    settings.convergenceLimitPpm = 500.0;

    const ClientClockModel clock = mappedClock();
    ClientSyncController controller(48000, settings);
    Room room(48000, 0.0);

    CHECK(controller.status().converging);
    measureOffset(controller, room, clock, 4, 200 * static_cast<int64_t>(kMs));
    CHECK(controller.status().converging);
    CHECK_NEAR(controller.status().correctionPpm, 500.0, 1e-9);
}

MULTIROOM_TEST(sync_integral_cannot_wind_up_past_its_own_ceiling)
{
    // A long stretch of the same error must not become a step the moment
    // something can act on it. Ten minutes of a steady 2 ms is what the
    // integral limit exists for.
    ClientSyncSettings settings;
    settings.offsetGainPpmPerMs = 0.0; // the integral alone, so it is visible
    settings.integralLimitPpm = 40.0;

    const ClientClockModel clock = mappedClock();
    ClientSyncController controller(48000, settings);
    Room room(48000, 0.0);

    measureOffset(controller, room, clock, 1200, 2 * static_cast<int64_t>(kMs));

    CHECK_NEAR(controller.status().correctionPpm, 40.0, 1e-9);
}

MULTIROOM_TEST(sync_correction_is_the_sum_of_both_terms)
{
    // Section 30: hardware drift compensation plus a slow offset controller,
    // and one sum rather than two competing outputs.
    ClientSyncSettings settings;
    settings.offsetGainPpmPerMs = 10.0;
    settings.offsetIntegralPpmPerMsSecond = 0.0; // the proportional term alone

    const ClientClockModel clock = mappedClock();
    ClientSyncController controller(48000, settings);
    Room room(48000, 30.0);

    measureDrift(controller, room, clock, 40);
    CHECK_NEAR(controller.status().correctionPpm, 30.0, 0.5);

    measureOffset(controller, room, clock, 1, 2 * static_cast<int64_t>(kMs));
    CHECK_NEAR(controller.status().correctionPpm, 30.0 + 20.0, 0.5);
}

// --- phase 13: how much of the answer a room is actually given ----------------

MULTIROOM_TEST(sync_applies_its_whole_answer_to_a_room_it_is_sure_of)
{
    // A hardware clock, a full window and a clean fit: there is nothing here
    // to be unsure about, so what is applied is what was asked for.
    const ClientClockModel clock = mappedClock();
    ClientSyncController controller(48000);
    Room room(48000, 30.0);

    measureDrift(controller, room, clock, 200);

    const ClientSyncStatus status = controller.status();
    CHECK(status.confidence > 0.85);
    CHECK_NEAR(status.appliedPpm, status.correctionPpm, 0.1);
}

MULTIROOM_TEST(sync_barely_moves_a_room_whose_clock_it_cannot_measure)
{
    // The first two-room run's shared-mode room, which is what this term is
    // for: its answer swung tens of ppm between reports at a confidence of
    // hundredths, and applying a hundredth of a swing is a room drifting
    // quietly rather than a room being pulled about by noise. The confidence
    // is driven down here the way a real one is -- a device whose position is
    // an estimate, and a millisecond of jitter on top of it.
    //
    // Each report moves the applied figure by at most its confidence's share
    // of the way to the new fit, so the applied answer swings less than the
    // fits it follows.
    const ClientClockModel clock = mappedClock();
    ClientSyncController controller(48000);
    Room room(48000, 30.0);
    room.setJitter(4 * kMs);
    room.setQuality(AudioClockQuality::Estimated);

    double previousApplied = 0.0;
    double lowestFit = 1e9;
    double highestFit = -1e9;
    double lowestApplied = 1e9;
    double highestApplied = -1e9;
    for (int i = 0; i < 80; ++i)
    {
        (void)controller.onPlaybackReport(room.advance(500 * kMs), clock, std::nullopt);
        const ClientSyncStatus status = controller.status();
        if (!status.hasDrift)
            continue;

        const double step = status.appliedPpm - previousApplied;
        CHECK(std::fabs(step) <=
              status.confidence * std::fabs(status.driftPpm - previousApplied) + 1e-9);
        previousApplied = status.appliedPpm;

        if (i >= 40)
        {
            lowestFit = std::min(lowestFit, status.driftPpm);
            highestFit = std::max(highestFit, status.driftPpm);
            lowestApplied = std::min(lowestApplied, status.appliedPpm);
            highestApplied = std::max(highestApplied, status.appliedPpm);
        }
    }

    CHECK(controller.status().confidence < 0.4);
    CHECK(highestApplied - lowestApplied < highestFit - lowestFit);
}

MULTIROOM_TEST(sync_applies_a_steady_drift_in_full_even_at_low_confidence)
{
    // Confidence is how fast a fit is taken up, not a discount on it. A card
    // whose timestamps are only estimated never gets past 0.4 confidence, and
    // a steady -144 ppm on it is still corrected by -144 ppm in the end --
    // scaling the output instead would leave ~86 ppm uncorrected for good.
    const ClientClockModel clock = mappedClock();
    ClientSyncController controller(48000);
    Room room(48000, -144.0);
    room.setQuality(AudioClockQuality::Estimated);

    measureDrift(controller, room, clock, 200);

    const ClientSyncStatus status = controller.status();
    CHECK(status.confidence < 0.5);
    CHECK_NEAR(status.driftPpm, -144.0, 0.5);
    CHECK_NEAR(status.appliedPpm, -144.0, 1.0);
    CHECK_NEAR(status.appliedPpm, status.correctionPpm, 0.5);
}

MULTIROOM_TEST(sync_offset_stops_growing_on_a_room_measured_at_low_confidence)
{
    // The regression this guards against: with the answer scaled by
    // confidence, a -144 ppm card at 0.4 confidence slipped by ~86 ppm for as
    // long as it played. Pure drift compensation, so nothing else hides it:
    // once the filter has caught up, the offset must stand still.
    ClientSyncSettings settings;
    settings.offsetGainPpmPerMs = 0.0;
    settings.offsetIntegralPpmPerMsSecond = 0.0;
    settings.correctionLimitPpm = 200.0; // -144 inside the limit, which is kept

    const ClientClockModel clock = mappedClock();
    ClientSyncController controller(48000, settings);

    const Convergence run = converge(controller, clock, 48000, -144.0, 1200, 0, 500 * kMs,
                                     AudioClockQuality::Estimated);

    CHECK(controller.status().confidence < 0.5);
    CHECK_NEAR(run.appliedPpm, -144.0, 1.0);
    CHECK(run.settledSwingPpm() < 1.0);

    // The last two minutes: ~10 ms of slip under the old law, none now.
    CHECK(std::llabs(run.finalNs - run.settledStartNs) < 100000);
}

MULTIROOM_TEST(sync_pulls_a_low_confidence_room_back_onto_the_timeline)
{
    // The same card with the offset controller on: what slipped while the
    // filter caught up is closed, rather than held at a standing error by a
    // drift term that never reaches the card.
    ClientSyncSettings settings;
    settings.correctionLimitPpm = 200.0;

    const ClientClockModel clock = mappedClock();
    ClientSyncController controller(48000, settings);

    const Convergence run = converge(controller, clock, 48000, -144.0, 2400, 0, 500 * kMs,
                                     AudioClockQuality::Estimated);

    CHECK(controller.status().confidence < 0.5);
    CHECK(std::llabs(run.finalNs) < 500000);
    CHECK_NEAR(controller.status().appliedPpm, -144.0, 2.0);
}

MULTIROOM_TEST(sync_applied_correction_still_respects_the_limit)
{
    // The filter changes how the drift term is reached, not how far: a card
    // past the normal limit is held at it once the room has converged.
    ClientSyncSettings settings;
    settings.correctionLimitPpm = 100.0;

    const ClientClockModel clock = mappedClock();
    ClientSyncController controller(48000, settings);
    Room room(48000, -144.0);

    measureOffset(controller, room, clock, 200, 0);
    const ClientSyncStatus status = controller.status();
    CHECK(!status.converging);
    CHECK_NEAR(status.correctionPpm, -100.0, 1e-9);
    CHECK_NEAR(status.appliedPpm, -100.0, 1e-9);
}

MULTIROOM_TEST(sync_applies_nothing_at_all_before_it_has_measured_anything)
{
    // Confidence is zero until the window has points in it, and zero times an
    // offset term is nothing -- which is the answer wanted: a room that has
    // just connected is not one to start pulling on.
    const ClientClockModel clock = mappedClock();
    ClientSyncController controller(48000);
    Room room(48000, 0.0);

    measureOffset(controller, room, clock, 2, 3 * static_cast<int64_t>(kMs));

    const ClientSyncStatus status = controller.status();
    CHECK(!status.hasDrift);
    CHECK(status.correctionPpm > 0.0);
    CHECK_EQ(status.appliedPpm, 0.0);
    CHECK_EQ(status.ratio, 1.0);
}

// --- phase 13: the loop closed (section 30) ------------------------------------

MULTIROOM_TEST(sync_pulls_a_fast_room_back_onto_the_timeline)
{
    // The whole of phase 13 in one assertion, and the one a sign error cannot
    // survive: a +30 ppm room, corrected, ends where the timeline is. Left
    // alone it would be 18 ms away after these ten minutes, and pulled the
    // wrong way it would be 36.
    const ClientClockModel clock = mappedClock();
    ClientSyncController controller(48000);

    const Convergence run = converge(controller, clock, 48000, 30.0, 1200);

    CHECK(std::llabs(run.finalNs) < 200000); // under 200 us of 18 ms
    CHECK(std::llabs(run.worstNs) < 2000000);
    CHECK(run.settledSwingPpm() < 2.0);
    CHECK(controller.status().appliedPpm > 20.0);
    CHECK(controller.status().ratio > 1.0);
}

MULTIROOM_TEST(sync_pushes_a_slow_room_back_onto_the_timeline)
{
    // Both signs, because a controller that is right on one of them is a
    // controller with a sign error and a lucky test.
    const ClientClockModel clock = mappedClock();
    ClientSyncController controller(48000);

    const Convergence run = converge(controller, clock, 48000, -30.0, 1200);

    CHECK(std::llabs(run.finalNs) < 200000);
    CHECK(std::llabs(run.worstNs) < 2000000);
    CHECK(run.settledSwingPpm() < 2.0);
    CHECK(controller.status().appliedPpm < -20.0);
    CHECK(controller.status().ratio < 1.0);
}

MULTIROOM_TEST(sync_converges_without_hunting_on_a_room_that_reports_noisily)
{
    // Section 31's "no fast audible pitch changes", and the reason it is
    // stated as a bound rather than as a straight line: a millisecond of
    // timestamp jitter is what a real report carries, it lands directly on the
    // offset through the clock model, and the proportional term turns it into
    // ppm at ten per millisecond. What has to be true is that the swing stays
    // small enough to be inaudible and that the room still ends up on the
    // timeline -- not that the ratio never moves.
    const ClientClockModel clock = mappedClock();
    ClientSyncController controller(48000);

    // Thirty microseconds, and it is not an arbitrary number: it is the noise
    // that reproduces what a real card reported through a real network in the
    // first two-room run -- +14.49 +/- 0.26 ppm.
    const Convergence run = converge(controller, clock, 48000, 30.0, 1200, 30000ULL);

    // The calibration this test rests on, asserted rather than assumed: the
    // fit's error, which is what the noise sets. The confidence follows from
    // it and from the drift, and a 30 ppm card this well measured is trusted.
    CHECK_NEAR(controller.status().standardErrorPpm, 0.26, 0.07);
    CHECK(controller.status().confidence > 0.9);

    // The room lands on the line, within the bias the noise itself carries.
    CHECK(std::llabs(run.finalNs) < 200000);

    // And the ratio sits still: under 8 ppm peak to peak over the last two
    // minutes, which is a seventieth of a cent, against a protocol that allows
    // a hundred ppm of correction outright. It is a bound rather than a
    // straight line -- every report moves the answer a little, and that is the
    // input moving rather than the controller hunting.
    CHECK(run.settledSwingPpm() < 8.0);
    CHECK(run.lowestSettledPpm > 20.0);
    CHECK(run.highestSettledPpm < 40.0);
}

// --- section 26's cross-check --------------------------------------------------

MULTIROOM_TEST(sync_says_nothing_while_the_two_derivations_agree)
{
    const ClientClockModel clock = mappedClock();
    ClientSyncController controller(48000);
    Room room(48000, 30.0);

    for (int i = 0; i < 40; ++i)
    {
        matp::PlaybackReport report = room.advance(500 * kMs);
        report.hasDrift = true;
        report.measuredDriftPpm = 30.4;
        report.measuredSampleRate = 48000.0 * (1.0 + 30.4e-6);
        report.driftConfidence = 0.94;
        (void)controller.onPlaybackReport(report, clock, std::nullopt);
    }

    const ClientSyncStatus status = controller.status();
    CHECK_EQ(status.disagreements, 0ULL);
    CHECK(status.hasClientDrift);
    CHECK_NEAR(status.clientDriftPpm, 30.4, 1e-9);
    CHECK_NEAR(status.clientConfidence, 0.94, 1e-9);
}

MULTIROOM_TEST(sync_records_a_disagreement_once_per_window_and_corrects_nothing_with_it)
{
    // Section 26: where both sides are confident and they still disagree, it
    // is the clock model between them that is suspect. It is counted and
    // logged; the difference is never fed to a resampler, which is why the
    // correction below still follows the server's own regression.
    const ClientClockModel clock = mappedClock();
    ClientSyncController controller(48000);
    Room room(48000, 30.0);

    for (int i = 0; i < 120; ++i) // 60 s: three drift windows
    {
        matp::PlaybackReport report = room.advance(500 * kMs);
        report.hasDrift = true;
        report.measuredDriftPpm = -45.0; // a different sign and a different size
        report.measuredSampleRate = 48000.0 * (1.0 - 45e-6);
        report.driftConfidence = 0.95;
        (void)controller.onPlaybackReport(report, clock, std::nullopt);
    }

    const ClientSyncStatus status = controller.status();
    CHECK(status.disagreements >= 2ULL);
    CHECK(status.disagreements <= 4ULL); // not one per report
    CHECK_NEAR(status.lastDisagreementPpm, 75.0, 1.0);
    CHECK_NEAR(status.correctionPpm, 30.0, 0.5);
}

MULTIROOM_TEST(sync_compares_the_client_figure_on_this_servers_clock)
{
    // The ESP32 bench run of 2026-10-02: the client said -5.5 ppm against its
    // own clock, the server ran 6.35 ppm faster than that clock, and the
    // server's regression said -11.85 ppm. Both were right, and the warning
    // that came of comparing them as they stood was not.
    constexpr double kSkew = 6.35e-6;
    ClientClockModel clock;
    for (int i = 0; i < 64; ++i) // two seconds apart: a lever arm past kMinScaleSpanNs
    {
        const uint64_t clientNs = kClientOriginNs + static_cast<uint64_t>(i) * 2 * kSecond;
        matp::ClockSyncResult result = exchange(clientNs, kClockOffsetNs, 1 * kMs);
        result.serverReceiveNs += static_cast<uint64_t>(static_cast<double>(clientNs) * kSkew);
        result.serverSendNs = result.serverReceiveNs;
        clock.addSample(result);
    }
    CHECK(clock.hasScale());

    ClientSyncController controller(48000);
    Room room(48000, -5.5);
    for (int i = 0; i < 120; ++i)
    {
        matp::PlaybackReport report = room.advance(500 * kMs);
        report.hasDrift = true;
        report.measuredDriftPpm = -5.5;
        report.measuredSampleRate = 48000.0 * (1.0 - 5.5e-6);
        report.driftConfidence = 0.95;
        (void)controller.onPlaybackReport(report, clock, std::nullopt);
    }

    const ClientSyncStatus status = controller.status();
    CHECK_NEAR(status.driftPpm, -11.85, 0.5);
    CHECK_NEAR(status.clientDriftPpm, -5.5, 1e-9);
    CHECK_EQ(status.disagreements, 0ULL);
}

MULTIROOM_TEST(sync_ignores_a_client_estimate_that_is_not_confident_in_itself)
{
    const ClientClockModel clock = mappedClock();
    ClientSyncController controller(48000);
    Room room(48000, 30.0);

    for (int i = 0; i < 40; ++i)
    {
        matp::PlaybackReport report = room.advance(500 * kMs);
        report.hasDrift = true;
        report.measuredDriftPpm = -45.0;
        report.driftConfidence = 0.1; // a window that has only just restarted
        (void)controller.onPlaybackReport(report, clock, std::nullopt);
    }

    CHECK_EQ(controller.status().disagreements, 0ULL);
}

// --- settings ------------------------------------------------------------------

MULTIROOM_TEST(sync_settings_refuse_a_convergence_limit_below_the_normal_one)
{
    ClientSyncSettings settings;
    CHECK(settings.isValid());

    settings.convergenceLimitPpm = 50.0; // a narrowing written as a widening
    CHECK(!settings.isValid());

    settings = ClientSyncSettings{};
    settings.driftWindowNs = 0;
    CHECK(!settings.isValid());

    settings = ClientSyncSettings{};
    settings.maxSamples = 2;
    CHECK(!settings.isValid());
}

// --- phase 16: what survives a cut, and what a ratio cannot close ------------

MULTIROOM_TEST(sync_carries_a_measured_card_across_a_reconnect)
{
    // Section 43, and the reason phase 16 keeps a room's state at all: the
    // connection went away and the window and the clock model behind it went
    // with it -- and the oscillator did not. A room that comes back is given
    // what it was already being given, at the certainty it was measured with,
    // instead of converging again from nothing.
    const ClientClockModel clock = mappedClock();
    ClientSyncController controller(44100);
    Room room(44100, 30.0);

    measureDrift(controller, room, clock, 40);
    const ClientSyncStatus measured = controller.status();
    CHECK(measured.hasDrift);
    CHECK_NEAR(measured.driftPpm, 30.0, 1.0);
    CHECK(measured.appliedPpm > 20.0);

    controller.carryOver(44100);
    const ClientSyncStatus back = controller.status();

    // Nothing measured survives, and the status is honest about it: an empty
    // window reports an empty window rather than the figure it used to hold.
    CHECK(!back.hasDrift);
    CHECK_EQ(back.samples, size_t{0});
    CHECK_EQ(back.confidence, 0.0);
    CHECK(!back.hasOffset);
    CHECK_EQ(back.resets, 1ULL);
    CHECK_EQ(back.reports, measured.reports); // lifetime counters are not measurements

    // The correction does survive, and it is the same one: the room carries on
    // being pulled at the rate its card was measured to need.
    CHECK_NEAR(back.carriedPpm, measured.appliedPpm, 1e-9);
    CHECK_NEAR(back.appliedPpm, measured.appliedPpm, 1e-9);
    CHECK_NEAR(back.ratio, measured.ratio, 1e-12);

    // A second cut before anything has been measured again keeps it rather
    // than forgetting it: two cut cables in a row are not a reason to forget
    // what this room's card does.
    controller.carryOver(44100);
    CHECK_NEAR(controller.status().carriedPpm, measured.appliedPpm, 1e-9);
    CHECK_EQ(controller.status().resets, 2ULL);

    // And a new measurement takes over from it, at the pace its confidence
    // allows rather than with a step.
    Room slower(44100, -20.0);
    measureDrift(controller, slower, clock, 40);
    const ClientSyncStatus remeasured = controller.status();
    CHECK(remeasured.hasDrift);
    CHECK_NEAR(remeasured.driftPpm, -20.0, 1.0);
    CHECK(remeasured.appliedPpm < 0.0);
}

MULTIROOM_TEST(sync_drops_the_offset_when_a_stream_is_moved_and_keeps_the_card)
{
    // Section 32 from this end. The stream has been put somewhere else, so the
    // offset measured against where it used to be describes a room that no
    // longer exists and the integral behind it an error nobody is going to
    // correct -- and the card it was all fitted on is the same card.
    const ClientClockModel clock = mappedClock();
    ClientSyncController controller(44100);
    Room room(44100, 30.0);

    measureOffset(controller, room, clock, 40, 4 * static_cast<int64_t>(kMs));
    const ClientSyncStatus before = controller.status();
    CHECK(before.hasOffset);
    CHECK(before.hasDrift);
    CHECK(!before.converging); // four milliseconds is a room that is in sync

    controller.clearOffset();
    const ClientSyncStatus after = controller.status();
    CHECK(!after.hasOffset);
    CHECK_EQ(after.offsetNs, int64_t{0});
    CHECK(!after.resyncWanted);

    // It has not converged on the position it is being moved to, whatever it
    // had agreed with the one it was at, so the wider limit is in force again.
    CHECK(after.converging);

    // The card did not move with the stream.
    CHECK(after.hasDrift);
    CHECK_NEAR(after.driftPpm, before.driftPpm, 1e-9);
    CHECK_EQ(after.samples, before.samples);
    CHECK_NEAR(after.correctionPpm, after.driftPpm, 1e-9);
}

MULTIROOM_TEST(sync_asks_for_a_resync_only_when_an_offset_no_ratio_closes_persists)
{
    // Section 32's threshold, and the one thing that keeps it from being a
    // hair trigger: an offset is a step when it is still there three reports
    // later, and a single report that disagrees with the rest is the clock
    // model between two machines hiccupping rather than a room that moved.
    const ClientClockModel clock = mappedClock();
    ClientSyncController controller(44100);
    Room room(44100, 0.0);

    // Four milliseconds is exactly what the controller of section 31 exists
    // for, however long it lasts.
    measureOffset(controller, room, clock, 20, 4 * static_cast<int64_t>(kMs));
    CHECK(controller.status().hasOffset);
    CHECK(!controller.status().resyncWanted);

    // Eighty is not. One report past the threshold is not enough, and neither
    // are two...
    measureOffset(controller, room, clock, 1, 80 * static_cast<int64_t>(kMs));
    CHECK(!controller.status().resyncWanted);
    measureOffset(controller, room, clock, 1, 80 * static_cast<int64_t>(kMs));
    CHECK(!controller.status().resyncWanted);

    // ...and three running are.
    measureOffset(controller, room, clock, 1, 80 * static_cast<int64_t>(kMs));
    CHECK(controller.status().resyncWanted);

    // One report back inside the threshold clears it: a room that is where it
    // should be is not a room to move.
    measureOffset(controller, room, clock, 1, 4 * static_cast<int64_t>(kMs));
    CHECK(!controller.status().resyncWanted);

    // A deployment that wants section 32 off gets it off, however far out a
    // room is -- and still gets the measurement.
    ClientSyncSettings off;
    off.hardResyncOffsetNs = 0;
    CHECK(off.isValid());
    ClientSyncController never(44100, off);
    Room elsewhere(44100, 0.0);
    measureOffset(never, elsewhere, clock, 20, 500 * static_cast<int64_t>(kMs));
    CHECK(never.status().hasOffset);
    CHECK(!never.status().resyncWanted);

    // A threshold with no reports behind it is a resync asked for by nothing.
    ClientSyncSettings unbacked;
    unbacked.hardResyncSamples = 0;
    CHECK(!unbacked.isValid());
}

MULTIROOM_TEST(sync_holds_a_room_with_an_output_latency_at_its_target_rather_than_at_zero)
{
    // Phase 27. A room heard through a receiver is started 80 ms ahead of the
    // line on purpose, so that is where it is in sync: the offset is still
    // reported against the line, and everything that acts on it -- the
    // correction, convergence, section 32's threshold -- acts on its distance
    // from the target.
    const ClientClockModel clock = mappedClock();
    constexpr int64_t kLatencyNs = 80 * static_cast<int64_t>(kMs);

    ClientSyncController controller(48000);
    controller.setTargetOffset(kLatencyNs);
    Room room(48000, 0.0);

    measureOffset(controller, room, clock, 20, kLatencyNs);
    ClientSyncStatus status = controller.status();
    CHECK(status.hasOffset);
    CHECK_EQ(status.offsetNs, kLatencyNs);
    CHECK_EQ(status.targetOffsetNs, kLatencyNs);
    CHECK(!status.resyncWanted);
    CHECK(!status.converging);
    CHECK_NEAR(status.correctionPpm, 0.0, 0.5);

    // Three milliseconds past the target is the same correction as three past
    // zero for a room without one, and the same sign.
    ClientSyncController plain(48000);
    Room other(48000, 0.0);
    measureOffset(plain, other, clock, 2, 3 * static_cast<int64_t>(kMs));
    ClientSyncController shifted(48000);
    shifted.setTargetOffset(kLatencyNs);
    Room third(48000, 0.0);
    measureOffset(shifted, third, clock, 2, kLatencyNs + 3 * static_cast<int64_t>(kMs));
    CHECK(shifted.status().correctionPpm > 0.0);
    CHECK_NEAR(shifted.status().correctionPpm, plain.status().correctionPpm, 0.5);

    // On the line is 80 ms short of the target, which is a step no ratio
    // closes: three reports running ask for a resync.
    measureOffset(controller, room, clock, 3, 0);
    CHECK(controller.status().correctionPpm < 0.0);
    CHECK(controller.status().resyncWanted);

    // A new target is a room about to be started somewhere else, so the offset
    // measured against the old one goes and the card stays.
    controller.setTargetOffset(0);
    status = controller.status();
    CHECK(!status.hasOffset);
    CHECK(!status.resyncWanted);
    CHECK_EQ(status.targetOffsetNs, 0);
    CHECK(status.hasDrift);

    // And a reconnect keeps it, because it is the room's and not the socket's.
    controller.setTargetOffset(kLatencyNs);
    controller.carryOver(48000);
    CHECK_EQ(controller.status().targetOffsetNs, kLatencyNs);
}

namespace {

// A room whose card runs `nativePpm` fast and is trimmed by its client on this
// controller's command: the trim
// slows the card, is programmed in whole ppm with the client's hysteresis,
// and takes effect one report after it was asked for. The line runs on
// server time, so the room's offset is simply how far its audible position
// has run ahead of the server's clock.
struct TrimmedRoom
{
    double nativePpm = 0.0;
    double offsetNs = 0.0;
    int32_t programmedPpm = 0;
    uint64_t serverNs = kClientOriginNs + 10 * kSecond + static_cast<uint64_t>(kClockOffsetNs);
    double frames = 0.0;

    matp::PlaybackReport advance(uint64_t intervalNs)
    {
        const double rate = (1.0 + nativePpm * 1e-6) * (1.0 - programmedPpm * 1e-6);
        serverNs += intervalNs;
        frames += 48000.0 * rate * static_cast<double>(intervalNs) * 1e-9;
        offsetNs += (rate - 1.0) * static_cast<double>(intervalNs);

        matp::PlaybackReport report;
        report.clockQuality = AudioClockQuality::Hardware;
        report.localMonotonicTimeNs =
            static_cast<uint64_t>(static_cast<int64_t>(serverNs) - kClockOffsetNs);
        report.deviceFramePosition = static_cast<uint64_t>(frames);
        report.bufferedDurationNs = 1500 * kMs;
        report.hasTimelineNsAudible = true;
        report.timelineNsAudible =
            static_cast<uint64_t>(static_cast<double>(serverNs) + offsetNs);
        report.hasHardwareClock = true;
        report.hardwareClockPpm = programmedPpm;
        return report;
    }

    void program(double targetPpm)
    {
        if (std::fabs(targetPpm - programmedPpm) >= 0.75)
            programmedPpm = static_cast<int32_t>(std::lround(targetPpm));
    }
};

} // namespace

MULTIROOM_TEST(sync_hardware_clock_learns_the_card_and_holds_it_once_the_residual_is_zero)
{
    const ClientClockModel clock = mappedClock();
    // Both signs, and errors past the resampling loop's 50 ppm integral.
    for (const double nativePpm : {40.0, -40.0, 80.0, -80.0})
    {
        ClientSyncController controller(48000);
        controller.setHardwareClock(true, -500, 500);
        TrimmedRoom room;
        room.nativePpm = nativePpm;
        room.offsetNs = 2.0 * static_cast<double>(kMs);

        double worstLateMs = 0.0;
        int32_t lowest = 1000;
        int32_t highest = -1000;
        for (int i = 0; i < 2400; ++i) // twenty minutes at the client's cadence
        {
            const matp::PlaybackReport report = room.advance(500 * kMs);
            CHECK(controller.onPlaybackReport(report, clock, expectedAt(room.serverNs)) ==
                  SyncSample::Accepted);
            room.program(controller.status().hardwareTargetPpm);
            if (i >= 2160) // the last two minutes
            {
                worstLateMs = std::max(worstLateMs, std::fabs(room.offsetNs) / 1e6);
                lowest = std::min(lowest, room.programmedPpm);
                highest = std::max(highest, room.programmedPpm);
            }
        }

        // In sync to well inside a millisecond, and held there by a trim that
        // is the card's own error -- not dropped because the residual is zero.
        CHECK(worstLateMs < 0.1);
        CHECK(std::abs(room.programmedPpm - static_cast<int32_t>(nativePpm)) <= 1);
        CHECK(highest - lowest <= 1);
        const ClientSyncStatus status = controller.status();
        CHECK(status.hardwareClock);
        CHECK_EQ(status.ratio, 1.0);
        CHECK_EQ(status.appliedPpm, static_cast<double>(room.programmedPpm));
        // What the regression measures is the residual of the trimmed card.
        CHECK(std::fabs(status.driftPpm) < 2.0);
    }
}

namespace {

// One report on a playing line. The report is taken first: the line is read
// at the instant it names.
void feedLine(ClientSyncController& controller, TrimmedRoom& room, const ClientClockModel& clock)
{
    const matp::PlaybackReport report = room.advance(500 * kMs);
    (void)controller.onPlaybackReport(report, clock, expectedAt(room.serverNs));
}

} // namespace

MULTIROOM_TEST(sync_hardware_clock_holds_its_trim_through_gaps_and_reseeds_from_the_client)
{
    const ClientClockModel clock = mappedClock();
    ClientSyncController controller(48000);
    controller.setHardwareClock(true, -500, 500);
    TrimmedRoom room;
    room.nativePpm = 30.0;
    for (int i = 0; i < 1200; ++i)
    {
        feedLine(controller, room, clock);
        room.program(controller.status().hardwareTargetPpm);
    }
    const double learned = controller.status().hardwareTargetPpm;
    CHECK(std::fabs(learned - 30.0) < 1.5);

    // No line to compare against: nothing moves.
    for (int i = 0; i < 20; ++i)
        (void)controller.onPlaybackReport(room.advance(500 * kMs), clock, std::nullopt);
    CHECK_EQ(controller.status().hardwareTargetPpm, learned);

    // A move drops the offset and keeps what was learned about the card.
    controller.clearOffset();
    feedLine(controller, room, clock);
    CHECK(std::fabs(controller.status().hardwareTargetPpm - learned) < 1.0);

    // A reopened device reports nominal, and the loop starts from there
    // rather than from a trim the client no longer has.
    controller.reset(48000);
    room.programmedPpm = 0;
    feedLine(controller, room, clock);
    CHECK(std::fabs(controller.status().hardwareTargetPpm) < 1.0);

    // Back to resampling: the ordinary ratio again.
    controller.setHardwareClock(false, -500, 500);
    CHECK(!controller.status().hardwareClock);
}
