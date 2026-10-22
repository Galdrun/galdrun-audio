#pragma once

// The server's model of one client's clock: `server = offset + client * scale`.
//
// **This exists only because of CLOCK_SYNC_RESULT.** Section 8 has the client
// measure and the server model, and the client sends all four timestamps back
// unfiltered -- including the samples its own model rejected -- precisely so
// that the server can build this. Without it there is no offset correction at
// all (sections 29 to 31): PLAYBACK_REPORT's `localMonotonicTimeNs` is on the
// client's clock, and a server that cannot put it into its own can correct a
// room's *rate* but never its *position*.
//
// Phase 12's ClientSyncController is what consumes it: every PLAYBACK_REPORT
// names an instant on the client's clock, and neither the drift regression nor
// the offset can be computed without putting that instant on this server's. It
// is built here rather than there because it is fed by a message that only
// arrives during the handshake and every five seconds after it, so a server
// that added the model later would have thrown away the first minutes of every
// session's data.
//
// What one CLOCK_SYNC_RESULT gives (section 8's four timestamps):
//
//     C1  the client sent the request
//     S2  this server received it
//     S3  this server answered
//     C4  the client received the answer
//
//     roundTrip  = (C4 - C1) - (S3 - S2)
//     clientMid  = C1 + (C4 - C1) / 2
//     serverMid  = S2 + (S3 - S2) / 2
//
// `clientMid` and `serverMid` are the same instant seen on the two clocks, to
// within half the round trip. That is one (x, y) point, and the model is the
// weighted straight line through them.
//
// Three things this file decides that section 8 does not:
//
//   * **A sample is weighted by its round trip, not counted.** The uncertainty
//     in a point *is* half its round trip, so a 40 ms sample says a fortieth
//     of what a 1 ms one does and averaging them equally would let one
//     congested moment move the whole model. The weight is 1/rtt^2, which is
//     the inverse of that uncertainty squared and therefore the ordinary
//     least-squares weight rather than a tuning knob.
//   * **An outlier is refused before it is weighted**, because a weight cannot
//     express "this did not happen on a network". A round trip more than
//     kOutlierRoundTripFactor times the best one in the window is a queued
//     packet, and the *best* rather than the mean is the reference for the
//     same reason NTP uses it: congestion only ever makes a round trip longer,
//     so the minimum is the closest thing to a true measurement and the mean
//     moves with the very thing being rejected.
//   * **Scale is reported only once it has been measured.** Fitting a slope to
//     twenty samples a second apart measures the sampling jitter, not the
//     oscillator: a 1 ms uncertainty over a 20 s lever arm is 50 ppm of slope
//     error, which is larger than the drift being looked for. Until the
//     accepted samples span kMinScaleSpanNs the line is fitted with its slope
//     pinned to exactly 1 and `hasScale()` is false -- an unmeasured rate is
//     reported as unmeasured, not as 1.000000 that happens to be right.
//
// Bounded, like every other buffer here: kMaxSamples newest samples, and the
// oldest is dropped rather than the window growing for the life of a session.

#include "matp/Protocol.h"

#include <cstddef>
#include <cstdint>
#include <deque>

namespace multiroom {

// One completed exchange, reduced to the point it contributes.
struct ClockSample
{
    uint64_t clientMidNs = 0;
    uint64_t serverMidNs = 0;
    uint64_t roundTripNs = 0;
};

class ClientClockModel
{
public:
    // Twenty is section 46's burst on connect and four minutes of the ongoing
    // five-second exchange. Past that the oldest samples describe an
    // oscillator that has since warmed up.
    static constexpr size_t kMaxSamples = 64;

    // Below this the line is not fitted at all: two points through noise is a
    // slope, not a measurement, and the offset of a single sample is already
    // the best estimate there is.
    static constexpr size_t kMinSamples = 3;

    // A round trip more than three times the best in the window did not
    // measure the network it claims to have crossed.
    static constexpr double kOutlierRoundTripFactor = 3.0;

    // Sixty seconds of lever arm. At a typical 1 ms round trip that puts the
    // slope uncertainty near 1 ppm, which is small against the tens of ppm
    // section 28 is looking for.
    static constexpr uint64_t kMinScaleSpanNs = 60ULL * 1000ULL * 1000ULL * 1000ULL;

    // True when the sample was kept. A rejected one is counted and nothing
    // else: the client sends every exchange it completed, including the ones
    // its own model threw away, so refusing some of them is expected traffic
    // rather than a fault.
    bool addSample(const matp::ClockSyncResult& result);

    void reset();

    // At least kMinSamples have been accepted, so there is a line.
    [[nodiscard]] bool valid() const noexcept { return samples_.size() >= kMinSamples; }

    // Whether the slope has been measured rather than pinned. See above.
    [[nodiscard]] bool hasScale() const noexcept;

    // The client's clock read in this server's time. Zero when there is no
    // model yet, which the caller checks with valid() rather than comparing.
    [[nodiscard]] uint64_t toServerTimeNs(uint64_t clientNs) const;

    // What the model says the two clocks differ by *at the newest sample*.
    // Signed, and necessarily so: a client that booted before this server sees
    // every server timestamp below every client one.
    //
    // An offset is only meaningful at an instant once the scale is not exactly
    // one, which is why this names the instant rather than pretending the
    // difference is a constant.
    [[nodiscard]] int64_t offsetNs() const;

    [[nodiscard]] double scale() const;

    [[nodiscard]] size_t sampleCount() const noexcept { return samples_.size(); }
    [[nodiscard]] uint64_t acceptedCount() const noexcept { return accepted_; }
    [[nodiscard]] uint64_t rejectedCount() const noexcept { return rejected_; }

    // The best round trip in the window, which is both the outlier reference
    // and the honest statement of how good this model can be.
    [[nodiscard]] uint64_t bestRoundTripNs() const noexcept;

    // The span the accepted samples cover on the client's clock.
    [[nodiscard]] uint64_t spanNs() const noexcept;

private:
    struct Fit
    {
        double slope = 1.0;
        double intercept = 0.0; // in the shifted frame, see the source
        bool measuredSlope = false;
    };

    [[nodiscard]] Fit fit() const;

    std::deque<ClockSample> samples_;
    uint64_t accepted_ = 0;
    uint64_t rejected_ = 0;
};

} // namespace multiroom
