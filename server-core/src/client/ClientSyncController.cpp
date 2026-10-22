#include "client/ClientSyncController.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace multiroom {
namespace {

constexpr double kNanosecondsPerMillisecond = 1000000.0;

double clamp01(double value)
{
    if (value < 0.0)
        return 0.0;
    if (value > 1.0)
        return 1.0;
    return value;
}

// How far the fit can be trusted, from its standard error. Either of two things
// is enough: an error small in absolute terms (1 ppm is already half-useful),
// or one small against the drift it found. A card with a coarse position
// counter fits 156 +/- 8.6 ppm -- noisy, and still 18 sigma from zero; judged
// on the 8.6 alone it would be corrected by a tenth and resynced every few
// minutes. A drift within three errors of zero stays near the absolute term,
// so a fit that is only noise is still not acted on. The client computes the
// same figure the same way (DriftEstimator.cpp).
double fitPrecision(double driftPpm, double standardErrorPpm)
{
    const double absolute = 1.0 / (1.0 + standardErrorPpm);
    const double magnitude = std::fabs(driftPpm);
    if (magnitude <= 0.0)
        return absolute;
    const double significance = magnitude / (magnitude + 3.0 * standardErrorPpm);
    return absolute > significance ? absolute : significance;
}

double clampTo(double value, double limit)
{
    if (value > limit)
        return limit;
    if (value < -limit)
        return -limit;
    return value;
}

// How far a device's own timestamps can be trusted. The client weighs the same
// four answers the same way, and the cross-check of section 26 is only
// meaningful while both sides do.
double qualityFactor(AudioClockQuality quality)
{
    switch (quality)
    {
    case AudioClockQuality::Hardware:
        return 1.0;
    case AudioClockQuality::Driver:
        return 0.8;
    case AudioClockQuality::Estimated:
        return 0.4;
    case AudioClockQuality::Unavailable:
        return 0.0;
    }
    return 0.0;
}

bool isWorse(AudioClockQuality candidate, AudioClockQuality current)
{
    return std::to_underlying(candidate) < std::to_underlying(current);
}

} // namespace

const char* syncSampleName(SyncSample sample)
{
    switch (sample)
    {
    case SyncSample::Accepted:
        return "accepted";
    case SyncSample::NoClockModel:
        return "no clock model";
    case SyncSample::NoDeviceClock:
        return "no device clock";
    case SyncSample::Underrun:
        return "underrun";
    case SyncSample::Stepped:
        return "stepped";
    }
    return "unknown";
}

bool ClientSyncSettings::isValid() const
{
    if (driftWindowNs == 0)
        return false;
    if (minSamples < 3 || maxSamples < minSamples)
        return false;
    if (offsetGainPpmPerMs < 0.0 || offsetIntegralPpmPerMsSecond < 0.0)
        return false;
    if (integralLimitPpm < 0.0)
        return false;
    if (correctionLimitPpm <= 0.0 || convergenceLimitPpm < correctionLimitPpm)
        return false;
    if (crossCheckPpm <= 0.0 || crossCheckConfidence < 0.0 || crossCheckConfidence > 1.0)
        return false;

    // Zero is section 32 turned off; any other threshold needs at least one
    // report behind it, because a resync asked for by nothing is a room moved
    // by nothing.
    if (hardResyncOffsetNs != 0 && hardResyncSamples == 0)
        return false;
    if (hardwareGainPpmPerMs < 0.0 || hardwareIntegralPpmPerMsSecond < 0.0 ||
        !(hardwareSlewPpmPerSecond > 0.0))
        return false;
    return true;
}

// ---------------------------------------------------------------------------

ClientSyncController::ClientSyncController(uint32_t nominalRateHz, ClientSyncSettings settings)
    : settings_(settings), nominalRateHz_(nominalRateHz)
{
}

void ClientSyncController::reset(uint32_t nominalRateHz)
{
    nominalRateHz_ = nominalRateHz;
    restart();
    ++resets_;
}

void ClientSyncController::carryOver(uint32_t nominalRateHz)
{
    // Read before the restart that drops it. A room that had been measured is
    // carried on its measurement; one that had itself been carried keeps what
    // it was carrying, because two reconnects in a row are not a reason to
    // forget what this card does.
    // What is carried is the filtered figure the room was actually being
    // given, so a reconnect does not step the ratio to the last raw fit.
    const double carried = hasDrift_ ? filteredDriftPpm_ : carriedPpm_;
    const double certainty = hasDrift_ ? confidence_ : carriedConfidence_;

    nominalRateHz_ = nominalRateHz;
    restart();
    ++resets_;

    carriedPpm_ = carried;
    carriedConfidence_ = certainty;
    filteredDriftPpm_ = carried;
    recomputeCorrection();
}

void ClientSyncController::clearOffset()
{
    hasOffset_ = false;
    offsetNs_ = 0;
    offsetAtNs_ = 0;
    recentOffsets_ = 0;
    integralMsSeconds_ = 0.0;
    beyondOffset_ = 0;
    resyncWanted_ = false;

    // The hardware integral is the card's rate, which a move does not change;
    // only the interval it is integrated over starts again.
    hardwareAtNs_ = 0;

    // A room whose stream has just been moved has not converged on the
    // position it is being moved to, whatever it had agreed with the one it
    // was at -- so the wider of section 31's two limits is in force again
    // while it settles.
    converged_ = false;
    recomputeCorrection();
}

void ClientSyncController::setTargetOffset(int64_t targetNs)
{
    if (targetNs == targetOffsetNs_)
        return;
    targetOffsetNs_ = targetNs;
    clearOffset();
}

void ClientSyncController::setHardwareClock(bool enabled, int32_t minPpm, int32_t maxPpm)
{
    hardwareMinPpm_ = minPpm;
    hardwareMaxPpm_ = maxPpm;
    if (enabled == hardware_)
        return;
    hardware_ = enabled;
    hardwareSeeded_ = false;
    hardwareAtNs_ = 0;
    hardwareTargetPpm_ = 0.0;
    hardwareIntegralPpm_ = 0.0;
    hasClientClock_ = false;
    recomputeCorrection();
}

void ClientSyncController::restart()
{
    window_.clear();
    worstQuality_ = AudioClockQuality::Unavailable;

    hasDrift_ = false;
    measuredRateHz_ = 0.0;
    driftPpm_ = 0.0;
    standardErrorPpm_ = 0.0;
    confidence_ = 0.0;
    filteredDriftPpm_ = 0.0;

    hasOffset_ = false;
    offsetNs_ = 0;
    offsetAtNs_ = 0;
    recentOffsets_ = 0;
    integralMsSeconds_ = 0.0;

    // A room whose series has just ended has to converge again before the
    // normal limit binds: the device counter restarted, so whatever agreement
    // it had reached says nothing about where it is now.
    converged_ = false;
    correctionPpm_ = 0.0;
    appliedPpm_ = 0.0;
    carriedPpm_ = 0.0;
    carriedConfidence_ = 0.0;
    beyondOffset_ = 0;
    resyncWanted_ = false;

    hasClientDrift_ = false;
    clientDriftPpm_ = 0.0;
    clientConfidence_ = 0.0;

    // A new series on the client's side -- an underrun, a reopened device, a
    // reconnect -- is reseeded from the trim it reports keeping: unchanged
    // across an underrun, nominal after a reopen.
    hardwareSeeded_ = false;
    hardwareAtNs_ = 0;
}

SyncSample ClientSyncController::onPlaybackReport(const matp::PlaybackReport& report,
                                                  const ClientClockModel& clock,
                                                  std::optional<TimelineNs> expectedAudible)
{
    ++reports_;

    // Section 25: a client with no usable clock still reports, and says so.
    // Its position fields are present and worthless, which is not the same as
    // a report that never arrived -- so it is counted and dropped rather than
    // treated as silence.
    if (report.clockQuality == AudioClockQuality::Unavailable)
    {
        ++rejected_;
        return SyncSample::NoDeviceClock;
    }

    // Everything below happens on the server's axis, and there is no way onto
    // it before phase 8's model is fitted. A room reporting into a model that
    // is still filling is the first few hundred milliseconds of every session.
    if (!clock.valid())
    {
        ++rejected_;
        return SyncSample::NoClockModel;
    }

    const uint64_t serverTimeNs = clock.toServerTimeNs(report.localMonotonicTimeNs);

    // Section 36. The frames a device silenced were counted by the same
    // counter this regresses, so a window spanning the gap measures the gap.
    // The report is kept as the new origin, because the position *after* an
    // underrun is perfectly good -- it is the span across it that is not.
    const bool underran = hasUnderruns_ && report.underruns > lastUnderruns_;
    lastUnderruns_ = report.underruns;
    hasUnderruns_ = true;
    if (underran)
    {
        restart();
        window_.push_back(Sample{serverTimeNs, report.deviceFramePosition});
        worstQuality_ = report.clockQuality;
        ++rejected_;
        return SyncSample::Underrun;
    }

    // Either going backwards means the device was reopened or the clock model
    // moved under a sample, not that an oscillator drifted.
    if (!window_.empty())
    {
        const Sample& last = window_.back();
        if (serverTimeNs <= last.serverTimeNs || report.deviceFramePosition < last.frames)
        {
            restart();
            window_.push_back(Sample{serverTimeNs, report.deviceFramePosition});
            worstQuality_ = report.clockQuality;
            ++rejected_;
            return SyncSample::Stepped;
        }
    }

    window_.push_back(Sample{serverTimeNs, report.deviceFramePosition});
    if (window_.size() == 1 || isWorse(report.clockQuality, worstQuality_))
        worstQuality_ = report.clockQuality;
    trimWindow();

    recomputeDrift();
    updateOffset(report, serverTimeNs, expectedAudible);
    crossCheck(report, serverTimeNs, clock.scale());
    if (hardware_)
        recomputeHardware(report, serverTimeNs);
    else
        recomputeCorrection();

    ++accepted_;
    return SyncSample::Accepted;
}

void ClientSyncController::trimWindow()
{
    const uint64_t newest = window_.back().serverTimeNs;
    while (window_.size() > 2 && newest - window_.front().serverTimeNs > settings_.driftWindowNs)
        window_.pop_front();
    while (window_.size() > settings_.maxSamples)
        window_.pop_front();
}

void ClientSyncController::recomputeDrift()
{
    if (window_.size() < settings_.minSamples || nominalRateHz_ == 0)
    {
        hasDrift_ = false;
        measuredRateHz_ = 0.0;
        driftPpm_ = 0.0;
        standardErrorPpm_ = 0.0;
        confidence_ = 0.0;
        return;
    }

    // Relative to the oldest point in the window, for the reason the client's
    // estimator gives: absolute nanosecond and frame counts are large enough
    // that regressing them directly loses the very precision being measured.
    const Sample& origin = window_.front();
    const double n = static_cast<double>(window_.size());

    double sumX = 0.0;
    double sumY = 0.0;
    double sumXX = 0.0;
    double sumXY = 0.0;
    for (const Sample& sample : window_)
    {
        const double x = static_cast<double>(sample.serverTimeNs - origin.serverTimeNs) * 1e-9;
        const double y = static_cast<double>(sample.frames - origin.frames);
        sumX += x;
        sumY += y;
        sumXX += x * x;
        sumXY += x * y;
    }

    const double sxx = sumXX - sumX * sumX / n;
    const double sxy = sumXY - sumX * sumY / n;
    if (sxx <= 0.0)
    {
        hasDrift_ = false;
        confidence_ = 0.0;
        return;
    }

    const double slope = sxy / sxx;
    const double intercept = (sumY - slope * sumX) / n;

    double ssr = 0.0;
    for (const Sample& sample : window_)
    {
        const double x = static_cast<double>(sample.serverTimeNs - origin.serverTimeNs) * 1e-9;
        const double y = static_cast<double>(sample.frames - origin.frames);
        const double residual = y - (slope * x + intercept);
        ssr += residual * residual;
    }

    const double degreesOfFreedom = n - 2.0;
    const double residualVariance = degreesOfFreedom > 0.0 ? ssr / degreesOfFreedom : 0.0;

    const double nominal = static_cast<double>(nominalRateHz_);
    measuredRateHz_ = slope;
    driftPpm_ = (measuredRateHz_ / nominal - 1.0) * 1e6;
    standardErrorPpm_ = std::sqrt(residualVariance / sxx) / nominal * 1e6;
    hasDrift_ = true;

    // The client's three factors, for the reason the header gives: a
    // cross-check between two confidences measured differently compares
    // nothing.
    const double spanSeconds =
        static_cast<double>(window_.back().serverTimeNs - origin.serverTimeNs) * 1e-9;
    const double windowSeconds = static_cast<double>(settings_.driftWindowNs) * 1e-9;
    const double fill = clamp01(spanSeconds / windowSeconds);
    confidence_ =
        clamp01(fill * fitPrecision(driftPpm_, standardErrorPpm_) * qualityFactor(worstQuality_));

    // Confidence is the filter's gain, not a factor on its output: a noisy fit
    // moves the filtered figure a little per report, and a steady one -- at any
    // confidence above zero -- is reached in full. Scaling the output instead
    // left (1 - confidence) of the drift uncorrected for good, and the offset
    // grew by exactly that for as long as the room played.
    filteredDriftPpm_ += confidence_ * (driftPpm_ - filteredDriftPpm_);
}

void ClientSyncController::updateOffset(const matp::PlaybackReport& report,
                                        uint64_t serverTimeNs,
                                        std::optional<TimelineNs> expectedAudible)
{
    // Both halves are needed and neither is always there: a zone that has not
    // started has no line to be compared against, and a room whose card has
    // not reached the scheduled frame has no audible position to compare
    // (section 25). Until then the drift regression above runs on its own.
    if (!report.hasTimelineNsAudible || !expectedAudible.has_value())
        return;

    const int64_t audible = static_cast<int64_t>(report.timelineNsAudible);
    const int64_t expected = static_cast<int64_t>(std::to_underlying(*expectedAudible));
    const int64_t offsetNs = audible - expected;

    // Section 31's integral, accumulated over the interval the two reports
    // actually span. A gap longer than the drift window is a room that was not
    // reporting rather than a room that was wrong for that long, so the step
    // is capped at the window instead of being trusted.
    if (hasOffset_ && serverTimeNs > offsetAtNs_)
    {
        uint64_t elapsedNs = serverTimeNs - offsetAtNs_;
        if (elapsedNs > settings_.driftWindowNs)
            elapsedNs = settings_.driftWindowNs;

        const double offsetMs =
            static_cast<double>(offsetNs_ - targetOffsetNs_) / kNanosecondsPerMillisecond;
        integralMsSeconds_ += offsetMs * (static_cast<double>(elapsedNs) * 1e-9);

        if (settings_.offsetIntegralPpmPerMsSecond > 0.0)
        {
            const double limit =
                settings_.integralLimitPpm / settings_.offsetIntegralPpmPerMsSecond;
            integralMsSeconds_ = clampTo(integralMsSeconds_, limit);
        }
        else
        {
            integralMsSeconds_ = 0.0;
        }
    }

    hasOffset_ = true;
    offsetNs_ = offsetNs;
    offsetAtNs_ = serverTimeNs;
    recentOffsetsNs_[recentOffsets_ % recentOffsetsNs_.size()] = offsetNs;
    ++recentOffsets_;

    // Phase 27: a room with an output latency is in sync at its target, not
    // at zero, so everything below measures from there.
    const uint64_t errorNs = static_cast<uint64_t>(std::llabs(offsetNs - targetOffsetNs_));
    if (errorNs <= settings_.convergedOffsetNs)
        converged_ = true;

    // Section 32. The count is of reports running rather than of reports,
    // because one report that disagrees with the rest is the clock model
    // between this server and that room hiccupping, and moving a room's whole
    // stream over one of those would be worse than the error.
    if (settings_.hardResyncOffsetNs == 0 || errorNs < settings_.hardResyncOffsetNs)
    {
        beyondOffset_ = 0;
        resyncWanted_ = false;
        return;
    }
    if (beyondOffset_ < settings_.hardResyncSamples)
        ++beyondOffset_;
    resyncWanted_ = beyondOffset_ >= settings_.hardResyncSamples;
}

void ClientSyncController::recomputeCorrection()
{
    // The hardware loop has its own answer; this one would resample.
    if (hardware_)
    {
        correctionPpm_ = hardwareTargetPpm_;
        appliedPpm_ = hasClientClock_ ? clientClockPpm_ : 0.0;
        return;
    }

    // Section 30: one sum of two terms. The drift term is the steady one and
    // the offset controller is the transient one, and both are expressed in
    // the project's own ratio convention rather than in either library's.
    // Phase 16's one addition: a room that has just reconnected or reopened
    // its device has nothing measured yet, and starting it at 1.0 would make
    // every reconnect a fresh convergence through an offset the card is still
    // opening. What carryOver() kept stands in until the regression fills
    // again -- and is replaced by it, not averaged with it.
    const bool carrying = !hasDrift_ && carriedPpm_ != 0.0;
    const double drift = hasDrift_ ? driftPpm_ : (carrying ? carriedPpm_ : 0.0);

    double offsetTerm = 0.0;
    if (hasOffset_)
    {
        const double offsetMs =
            static_cast<double>(offsetNs_ - targetOffsetNs_) / kNanosecondsPerMillisecond;
        offsetTerm += settings_.offsetGainPpmPerMs * offsetMs;
        offsetTerm += clampTo(settings_.offsetIntegralPpmPerMsSecond * integralMsSeconds_,
                              settings_.integralLimitPpm);
    }

    const double limit = converged_ ? settings_.correctionLimitPpm : settings_.convergenceLimitPpm;
    correctionPpm_ = clampTo(drift + offsetTerm, limit);

    // Phase 13's one addition to the control law, and the reason the two
    // numbers are kept apart: a room that cannot be measured is not corrected
    // by a guess, it is corrected a little at a time. The shared-mode room of
    // the first two-room run is the case this exists for -- its answer swung
    // +/-35 ppm between reports at a confidence of 0.01, and following a
    // hundredth of each swing is a room drifting quietly rather than a room
    // being pulled about by noise.
    //
    // The drift term is the filtered one (recomputeDrift()), where confidence
    // sets how fast a new fit is taken up rather than how much of it is ever
    // applied -- so a steady card is corrected in full at any confidence.
    //
    // The offset term is scaled by confidence. It is the transient one, so
    // that only slows how fast an offset is closed and leaves nothing behind
    // once the drift term is right; and a room whose timestamps are worthless
    // reported the audible position it is built from as well.
    //
    // A room that is being carried is scaled by the certainty its figure was
    // measured with instead, because the window that produced `confidence_` is
    // empty. It ends by itself: the moment the regression fills, `hasDrift_`
    // is true and this is the ordinary line again.
    const double certainty = carrying ? carriedConfidence_ : confidence_;
    appliedPpm_ = clampTo(filteredDriftPpm_ + offsetTerm * certainty, limit);
}

void ClientSyncController::recomputeHardware(const matp::PlaybackReport& report,
                                             uint64_t serverTimeNs)
{
    if (report.hasHardwareClock)
    {
        hasClientClock_ = true;
        clientClockPpm_ = report.hardwareClockPpm;
        clientClockSaturated_ = report.hardwareClockSaturated;
    }
    if (!hardwareSeeded_)
    {
        // Where the clock is, by the client's word: a reopened device is back
        // at nominal and says so, a trim kept through an underrun is kept here.
        hardwareIntegralPpm_ = hasClientClock_ ? clientClockPpm_ : 0.0;
        hardwareTargetPpm_ = hardwareIntegralPpm_;
        hardwareSeeded_ = true;
    }

    // Only a fresh offset moves anything. Without one -- no line yet, no
    // audible position, a gap -- the trim and what it learned are held.
    if (hasOffset_ && offsetAtNs_ == serverTimeNs)
    {
        const double limit =
            converged_ ? settings_.correctionLimitPpm : settings_.convergenceLimitPpm;
        const double low = hardwareMinPpm_ > -limit ? hardwareMinPpm_ : -limit;
        const double high = hardwareMaxPpm_ < limit ? hardwareMaxPpm_ : limit;

        uint64_t elapsedNs = hardwareAtNs_ != 0 && serverTimeNs > hardwareAtNs_
                                 ? serverTimeNs - hardwareAtNs_
                                 : 0;
        if (elapsedNs > settings_.driftWindowNs)
            elapsedNs = settings_.driftWindowNs;
        hardwareAtNs_ = serverTimeNs;
        const double elapsed = static_cast<double>(elapsedNs) * 1e-9;

        // Positive is a room ahead of the line, and a positive trim slows it.
        const double errorMs =
            static_cast<double>(offsetNs_ - targetOffsetNs_) / kNanosecondsPerMillisecond;
        const double proportional = settings_.hardwareGainPpmPerMs * errorMs;
        double integral =
            hardwareIntegralPpm_ + settings_.hardwareIntegralPpmPerMsSecond * errorMs * elapsed;
        integral = integral < low ? low : (integral > high ? high : integral);

        double command = proportional + integral;
        command = command < low ? low : (command > high ? high : command);
        const double step = settings_.hardwareSlewPpmPerSecond * elapsed;
        const double slewed = command > hardwareTargetPpm_ + step   ? hardwareTargetPpm_ + step
                              : command < hardwareTargetPpm_ - step ? hardwareTargetPpm_ - step
                                                                    : command;

        // Anti-windup: a command held at a limit or by the slew does not keep
        // learning in the direction it cannot follow.
        const bool held = slewed != proportional + integral;
        if (!held || (errorMs > 0.0) != (slewed < proportional + integral))
            hardwareIntegralPpm_ = integral;
        hardwareTargetPpm_ = slewed;
    }

    correctionPpm_ = hardwareTargetPpm_;
    appliedPpm_ = hasClientClock_ ? clientClockPpm_ : 0.0;
}

void ClientSyncController::crossCheck(const matp::PlaybackReport& report, uint64_t serverTimeNs,
                                      double scale)
{
    hasClientDrift_ = report.hasDrift;
    clientDriftPpm_ = report.measuredDriftPpm;
    clientConfidence_ = report.driftConfidence;

    if (!hasDrift_ || !report.hasDrift)
        return;
    if (confidence_ < settings_.crossCheckConfidence ||
        report.driftConfidence < settings_.crossCheckConfidence)
        return;

    // The client's figure is its card against its own clock and this
    // regression's is the card against this server's, so the two differ by
    // the skew between the clocks -- a -5.5 ppm card on a client clock the
    // server runs 6.3 ppm faster than is -11.8 ppm here, and both are right.
    // The skew is the model's scale (`server = offset + client * scale`).
    const double clientOnServerPpm =
        scale > 0.0 ? ((1.0 + report.measuredDriftPpm * 1e-6) / scale - 1.0) * 1e6
                    : report.measuredDriftPpm;
    const double difference = driftPpm_ - clientOnServerPpm;
    if (std::fabs(difference) <= settings_.crossCheckPpm)
        return;

    // Once per window: a clock model that is wrong stays wrong, and an
    // operator needs to be told that once rather than twice a second.
    if (hasDisagreementInstant_ && serverTimeNs - lastDisagreementAtNs_ < settings_.driftWindowNs)
        return;

    lastDisagreementAtNs_ = serverTimeNs;
    hasDisagreementInstant_ = true;
    lastDisagreementPpm_ = difference;
    ++disagreements_;
}

std::optional<int64_t> ClientSyncController::steadyOffsetNs() const
{
    constexpr int64_t kAgreeNs = 5LL * 1000000LL;
    if (recentOffsets_ < recentOffsetsNs_.size())
        return std::nullopt;

    std::array<int64_t, kSteadyOffsetReports> sorted = recentOffsetsNs_;
    std::sort(sorted.begin(), sorted.end());
    if (sorted.back() - sorted.front() > kAgreeNs)
        return std::nullopt;
    return sorted[sorted.size() / 2];
}

ClientSyncStatus ClientSyncController::status() const
{
    ClientSyncStatus out;
    out.hasDrift = hasDrift_;
    out.measuredRateHz = measuredRateHz_;
    out.driftPpm = driftPpm_;
    out.standardErrorPpm = standardErrorPpm_;
    out.confidence = confidence_;
    out.samples = window_.size();
    out.windowSpanNs = window_.size() < 2
                           ? 0
                           : window_.back().serverTimeNs - window_.front().serverTimeNs;
    out.clockQuality = worstQuality_;

    out.hasClientDrift = hasClientDrift_;
    out.clientDriftPpm = clientDriftPpm_;
    out.clientConfidence = clientConfidence_;

    out.hasOffset = hasOffset_;
    out.offsetNs = offsetNs_;
    out.targetOffsetNs = targetOffsetNs_;

    out.correctionPpm = correctionPpm_;
    out.appliedPpm = appliedPpm_;
    out.ratio = hardware_ ? 1.0 : 1.0 + appliedPpm_ / 1e6;
    out.hardwareClock = hardware_;
    out.hardwareTargetPpm = hardwareTargetPpm_;
    out.hasClientClock = hasClientClock_;
    out.clientClockPpm = clientClockPpm_;
    out.clientClockSaturated = clientClockSaturated_;
    out.converging = !converged_;
    out.resyncWanted = resyncWanted_;
    out.carriedPpm = carriedPpm_;

    out.nominalRateHz = nominalRateHz_;
    out.reports = reports_;
    out.accepted = accepted_;
    out.rejected = rejected_;
    out.resets = resets_;
    out.disagreements = disagreements_;
    out.lastDisagreementPpm = lastDisagreementPpm_;
    return out;
}

} // namespace multiroom
