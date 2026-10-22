#pragma once

// The mapping between the server's monotonic clock and this client's
// monotonicNowNs() (MATP sections 8 and 9).
//
// Fed with the four timestamps of one NTP-like exchange -- C1 request sent,
// S2 request received, S3 reply sent, C4 reply received -- it fits
//
//     server_time = local_time + offset + skew * (local_time - origin)
//
// which is section 9's `offset + client_time * scale` rearranged so the
// arithmetic stays where a double still resolves nanoseconds. Absolute
// monotonic values are large enough that regressing them directly would lose
// exactly the precision being measured, so everything is relative to the
// oldest sample in the window, the same way DriftEstimator works.
//
// This is not sound-card drift, and the two must never be mixed. Drift is the
// sound card's crystal against this machine's CPU clock; it is measured and
// reported, and the server corrects it. This is this machine's CPU clock
// against the server's, and the client uses it itself -- it is how a server
// timestamp in START_AT becomes a local instant to wait for.
//
// Pure and platform-independent: it takes timestamps, never reads a clock and
// opens no socket, so the whole model is testable against synthetic data.

#include <cstdint>
#include <vector>

namespace multiroom {
namespace matp {

// One completed exchange (section 8). All four values are nanoseconds on the
// clock of the machine that took them.
struct ClockSyncSample
{
    uint64_t clientSendNs = 0;    // C1
    uint64_t serverReceiveNs = 0; // S2
    uint64_t serverSendNs = 0;    // S3
    uint64_t clientReceiveNs = 0; // C4
};

class ClockModel
{
public:
    struct Settings
    {
        // Rolling window. Long enough that the skew fit has a baseline worth
        // having at section 46's one sample every five seconds.
        double windowSeconds = 300.0;

        // Ring capacity, allocated once. Sampling faster than capacity/window
        // shortens the effective window; it never allocates.
        uint32_t capacity = 128;

        // Below this, no mapping is published at all.
        uint32_t minSamples = 4;

        // A skew is only published once the window is long enough to resolve
        // one. Twenty samples taken a few milliseconds apart during connect
        // say a great deal about the offset and nothing about the rate, and a
        // slope fitted to them would be noise wearing a number.
        double minSkewSpanSeconds = 30.0;
        uint32_t minSkewSamples = 8;

        // Beyond this the fit is not describing two crystals. Reported as "no
        // skew yet" rather than as a rate nobody's oscillator has.
        double maxSkewPpm = 1000.0;

        // Prefer low round trips (section 8): a sample whose round trip is far
        // above the best one in the window spent that time queued somewhere,
        // and the queue is not symmetric.
        double roundTripFactor = 3.0;
        // ...but never closer to the best than this, so a LAN whose round trip
        // is a quarter of a millisecond does not reject everything.
        uint64_t roundTripFloorNs = 1000000; // 1 ms
        // A round trip this long is not a measurement, it is an incident.
        uint64_t maxRoundTripNs = 1000000000; // 1 s
        // After this many round trips in a row above the gate the path itself
        // has become slower, and they are let in until a fast one returns:
        // the fast samples then age out of the window, and the gate follows.
        uint32_t maxConsecutiveSlow = 6;

        // Residual test against the current fit, with a floor for the same
        // reason DriftEstimator has one.
        double outlierSigma = 4.0;
        uint64_t outlierFloorNs = 2000000; // 2 ms

        // After this many outliers in a row the window is describing a clock
        // that no longer exists -- one side stepped -- and it restarts from
        // the sample that no longer fits.
        uint32_t maxConsecutiveOutliers = 3;
    };

    // Two constructors for the reason DriftEstimator has two.
    ClockModel();
    explicit ClockModel(const Settings& settings);

    // Drops the window. The mapping does not survive the connection it was
    // measured on: a reconnect re-runs clock synchronization (section 43),
    // because the server on the other end may be a restarted process whose
    // monotonic clock started over.
    void reset();

    // Returns false when the sample was refused: structurally impossible, out
    // of order, a round trip too far above the best one seen, or an outlier
    // against the current fit.
    bool addSample(const ClockSyncSample& sample);

    bool hasEstimate() const { return hasEstimate_; }

    // Server time minus local time at the newest sample, from the fit rather
    // than from that one measurement. 0 when there is no estimate.
    int64_t offsetNs() const;

    // How much faster the server's clock runs than this one. Only once the
    // window can resolve it; until then the mapping is offset-only, which is
    // the honest answer rather than a slope through a cluster of points.
    bool hasSkew() const { return hasSkew_; }
    double skewPpm() const;

    // Both directions, false when there is no mapping yet or the result would
    // be a negative timestamp, which no monotonic clock produces.
    bool serverFromLocal(uint64_t localNs, uint64_t& serverNs) const;
    bool localFromServer(uint64_t serverNs, uint64_t& localNs) const;

    // The best and the most recent round trip in the window.
    uint64_t bestRoundTripNs() const { return bestRoundTripNs_; }
    uint64_t lastRoundTripNs() const;

    // How wrong the offset can be, honestly: a four-timestamp exchange cannot
    // resolve an asymmetric path better than half its round trip, and the fit
    // cannot claim to be tighter than its own residuals.
    uint64_t uncertaintyNs() const;

    double residualStdDevNs() const { return residualStdDevNs_; }
    uint32_t sampleCount() const { return count_; }
    // Samples refused on arrival. Samples dropped later, when a better round
    // trip showed the window what "close" looks like, are not counted here.
    uint64_t rejectedCount() const { return rejected_; }
    double windowSpanSeconds() const;

private:
    struct Sample
    {
        uint64_t localNs = 0; // midpoint of C1 and C4: when the offset was true
        int64_t offsetNs = 0;
        uint64_t roundTripNs = 0;
    };

    static bool derive(const ClockSyncSample& sample, Sample& out);

    const Sample& at(uint32_t index) const;
    void push(const Sample& entry);
    bool handleOutlier(const Sample& entry);
    void dropExpired(uint64_t newestLocalNs);
    void updateBestRoundTrip();
    uint64_t roundTripLimit() const;
    bool pruneByRoundTrip();
    void recompute();
    bool predictOffsetNs(uint64_t localNs, double& offsetNs) const;

    Settings settings_;

    // Fixed-size ring, allocated once in the constructor.
    std::vector<Sample> ring_;
    uint32_t head_ = 0; // index of the oldest sample
    uint32_t count_ = 0;

    uint64_t bestRoundTripNs_ = 0;

    bool hasEstimate_ = false;
    bool hasSkew_ = false;

    // The fit, relative to at(0): offset(t) = at(0).offsetNs + intercept +
    // slope * (t - at(0).localNs), with slope in nanoseconds per second.
    double slopeNsPerSecond_ = 0.0;
    double interceptNs_ = 0.0;
    double residualStdDevNs_ = 0.0;

    uint32_t consecutiveOutliers_ = 0;
    uint32_t consecutiveSlow_ = 0;
    uint64_t rejected_ = 0;
};

} // namespace matp
} // namespace multiroom
