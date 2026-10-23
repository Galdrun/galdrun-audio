#pragma once

#include "audio/AudioClockSnapshot.h"

#include <cstdint>
#include <vector>

namespace multiroom {

// Measures how fast the sound card's clock actually runs.
//
// It regresses deviceFramePosition against monotonicTimeNs over a rolling
// window. Two points are not enough: scheduler jitter of a hundred
// microseconds is larger than the effect being measured, so a single pair can
// be off by tens of ppm while a regression over hundreds of pairs is not.
//
// This class measures and nothing else. The client never corrects its own
// playback rate -- the result is reported to the server, which owns the
// correction (see docs/protocol/MATP-AudioTransferProtocol.md section 50).
//
// Platform-independent and testable with no sound card present.
class DriftEstimator
{
public:
    struct Settings
    {
        // Rolling window length. Longer resolves smaller drift but reacts
        // more slowly to a clock that changes.
        double windowSeconds = 20.0;

        // Ring capacity, allocated once. When the caller samples faster than
        // capacity/window, the oldest samples fall out early and the effective
        // window is shorter -- never an allocation.
        uint32_t capacity = 512;

        // Below this many samples no estimate is published at all.
        uint32_t minSamples = 8;

        // A sample is rejected when its residual against the current fit
        // exceeds this many standard deviations...
        double outlierSigma = 3.0;
        // ...but never closer than this, so a fit that happens to be perfect
        // does not start rejecting everything. Expressed in seconds of frames
        // (0.001 = 1 ms, which is well under a typical period).
        double outlierFloorSeconds = 0.001;

        // After this many rejections in a row the window is assumed to be
        // stale rather than the samples wrong, and it restarts.
        uint32_t maxConsecutiveRejections = 5;
    };

    // Two constructors rather than a defaulted argument: GCC rejects
    // `= Settings()` for a nested struct with member initializers, because the
    // enclosing class is still incomplete where the argument is parsed.
    DriftEstimator();
    explicit DriftEstimator(const Settings& settings);

    // Drops the window. Call it whenever the frame position may have stepped:
    // a device reopen, a format change, or a rise in the backend's underrun
    // count (on ALSA an xrun drops frames that were already counted).
    void reset();

    // Returns false when the sample was refused: unusable, out of order,
    // implausible, or an outlier against the current fit.
    bool addSample(const AudioClockSnapshot& sample);

    bool hasEstimate() const { return hasEstimate_; }

    // Measured rate in Hz, 0 when there is no estimate yet.
    double estimatedSampleRate() const { return estimatedRate_; }

    // ((measured / nominal) - 1) * 1e6, 0 when there is no estimate.
    double driftPpm() const { return driftPpm_; }

    // 0..1, from how full the window is, how precisely the slope is resolved,
    // and how trustworthy the timestamps are. Low right after a reset.
    double confidence() const { return confidence_; }

    // One standard error of the regression slope, expressed in ppm. This is
    // the honest "how well do we know this" number; confidence() folds it in
    // together with the other two factors.
    double standardErrorPpm() const { return standardErrorPpm_; }

    uint32_t sampleCount() const { return count_; }
    uint64_t rejectedCount() const { return rejected_; }
    double windowSpanSeconds() const;
    AudioClockQuality quality() const { return worstQuality_; }

private:
    struct Sample
    {
        uint64_t timeNs = 0;
        uint64_t frames = 0;
        AudioClockQuality quality = AudioClockQuality::Unavailable;
    };

    void pushSample(const AudioClockSnapshot& sample);
    bool handleRejection(const AudioClockSnapshot& sample);
    void dropExpired(uint64_t newestTimeNs);
    void recompute();
    const Sample& at(uint32_t index) const;
    bool predictResidual(const AudioClockSnapshot& sample, double& residualFrames) const;

    Settings settings_;

    // Fixed-size ring, allocated once in the constructor.
    std::vector<Sample> ring_;
    uint32_t head_ = 0; // index of the oldest sample
    uint32_t count_ = 0;

    uint32_t nominalSampleRate_ = 0;
    AudioClockQuality worstQuality_ = AudioClockQuality::Unavailable; // of the window

    bool hasEstimate_ = false;
    double estimatedRate_ = 0.0;
    double driftPpm_ = 0.0;
    double confidence_ = 0.0;
    double standardErrorPpm_ = 0.0;

    // Regression state kept for the outlier test.
    double slope_ = 0.0;
    double intercept_ = 0.0;
    double residualStdDev_ = 0.0;

    uint32_t consecutiveRejections_ = 0;
    uint64_t rejected_ = 0;
};

} // namespace multiroom
