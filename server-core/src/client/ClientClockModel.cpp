#include "client/ClientClockModel.h"

#include <algorithm>
#include <cmath>

namespace multiroom {
namespace {

// Half of a nanosecond difference, without leaving unsigned arithmetic.
uint64_t midpoint(uint64_t first, uint64_t last)
{
    return first + (last - first) / 2;
}

// The zero a fit is done around. Nanoseconds since boot are of the order of
// 1e15 and a double carries 53 bits, so x*x in a least-squares sum would lose
// the microseconds the whole exercise is about. Every sum below is therefore
// in offsets from the oldest sample, where the numbers are seconds rather than
// decades, and the frame is undone in toServerTimeNs.
struct Origin
{
    uint64_t client = 0;
    uint64_t server = 0;
};

} // namespace

bool ClientClockModel::addSample(const matp::ClockSyncResult& result)
{
    // The parser already refused a clock that ran backwards within itself, so
    // both of these subtractions are defined. They are repeated rather than
    // assumed because a sample may reach this model from somewhere that did
    // not come through a socket -- a test, or phase 16's stored history.
    if (result.clientReceiveNs < result.clientSendNs ||
        result.serverSendNs < result.serverReceiveNs)
    {
        ++rejected_;
        return false;
    }

    const uint64_t elapsedOnClient = result.clientReceiveNs - result.clientSendNs;
    const uint64_t spentOnServer = result.serverSendNs - result.serverReceiveNs;
    if (elapsedOnClient < spentOnServer)
    {
        // The exchange took less time on the client than the server spent
        // inside it. One of the two clocks is not monotonic, and no weight
        // expresses that.
        ++rejected_;
        return false;
    }

    ClockSample sample;
    sample.roundTripNs = elapsedOnClient - spentOnServer;
    sample.clientMidNs = midpoint(result.clientSendNs, result.clientReceiveNs);
    sample.serverMidNs = midpoint(result.serverReceiveNs, result.serverSendNs);

    // Congestion only ever lengthens a round trip, so the best one in the
    // window is the closest thing to a true measurement. A sample far above it
    // spent its time in a queue, and where it spent that time decides which
    // way the midpoint is wrong -- which is exactly what cannot be recovered.
    const uint64_t best = bestRoundTripNs();
    if (!samples_.empty() && best > 0 &&
        static_cast<double>(sample.roundTripNs) >
            kOutlierRoundTripFactor * static_cast<double>(best))
    {
        ++rejected_;
        return false;
    }

    if (samples_.size() == kMaxSamples)
        samples_.pop_front();
    samples_.push_back(sample);
    ++accepted_;
    return true;
}

void ClientClockModel::reset()
{
    samples_.clear();
    accepted_ = 0;
    rejected_ = 0;
}

uint64_t ClientClockModel::bestRoundTripNs() const noexcept
{
    uint64_t best = 0;
    bool any = false;
    for (const ClockSample& sample : samples_)
    {
        if (!any || sample.roundTripNs < best)
        {
            best = sample.roundTripNs;
            any = true;
        }
    }
    return best;
}

uint64_t ClientClockModel::spanNs() const noexcept
{
    if (samples_.size() < 2)
        return 0;
    // The deque is in arrival order and a monotonic clock does not go
    // backwards, so the ends are the extremes.
    return samples_.back().clientMidNs - samples_.front().clientMidNs;
}

bool ClientClockModel::hasScale() const noexcept
{
    return valid() && spanNs() >= kMinScaleSpanNs;
}

ClientClockModel::Fit ClientClockModel::fit() const
{
    Fit result;
    if (samples_.empty())
        return result;

    const Origin origin{samples_.front().clientMidNs, samples_.front().serverMidNs};

    double sumWeight = 0.0;
    double sumX = 0.0;
    double sumY = 0.0;
    double sumXX = 0.0;
    double sumXY = 0.0;

    for (const ClockSample& sample : samples_)
    {
        // 1/rtt^2: the inverse square of the sample's own uncertainty, which
        // is what makes this ordinary weighted least squares rather than a
        // tuned heuristic. A round trip of zero is possible on a loopback
        // measured with a coarse clock; one nanosecond is the floor, so that
        // it cannot become an infinite weight.
        const double rtt = std::max(1.0, static_cast<double>(sample.roundTripNs));
        const double weight = 1.0 / (rtt * rtt);

        const double x = static_cast<double>(sample.clientMidNs) -
                         static_cast<double>(origin.client);
        const double y = static_cast<double>(sample.serverMidNs) -
                         static_cast<double>(origin.server);

        sumWeight += weight;
        sumX += weight * x;
        sumY += weight * y;
        sumXX += weight * x * x;
        sumXY += weight * x * y;
    }

    if (sumWeight <= 0.0)
        return result;

    if (hasScale())
    {
        const double denominator = sumWeight * sumXX - sumX * sumX;
        // Zero denominator means every accepted sample landed on one instant,
        // which hasScale() should already have excluded; the guard is here
        // because a divide by it would be a silent NaN through the whole
        // model rather than a fit that declined to be made.
        if (denominator != 0.0)
        {
            result.slope = (sumWeight * sumXY - sumX * sumY) / denominator;
            result.measuredSlope = true;
        }
    }

    // With the slope pinned, this is the weighted mean difference -- which is
    // the right estimator for an offset measured repeatedly, and is what the
    // general form collapses to when the slope is not free.
    result.intercept = (sumY - result.slope * sumX) / sumWeight;
    return result;
}

uint64_t ClientClockModel::toServerTimeNs(uint64_t clientNs) const
{
    if (!valid())
        return 0;

    const Origin origin{samples_.front().clientMidNs, samples_.front().serverMidNs};
    const Fit line = fit();

    const double x = static_cast<double>(clientNs) - static_cast<double>(origin.client);
    const double y = line.intercept + line.slope * x;
    const double absolute = static_cast<double>(origin.server) + y;

    if (absolute <= 0.0)
        return 0;
    return static_cast<uint64_t>(absolute + 0.5);
}

int64_t ClientClockModel::offsetNs() const
{
    if (!valid())
        return 0;

    // At the newest sample, because an offset is only a constant while the
    // scale is exactly one.
    const uint64_t clientNs = samples_.back().clientMidNs;
    const uint64_t serverNs = toServerTimeNs(clientNs);
    return static_cast<int64_t>(serverNs) - static_cast<int64_t>(clientNs);
}

double ClientClockModel::scale() const
{
    if (!valid())
        return 1.0;
    return fit().slope;
}

} // namespace multiroom
