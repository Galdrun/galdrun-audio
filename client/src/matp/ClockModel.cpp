#include "matp/ClockModel.h"

#include <cmath>
#include <limits>

namespace multiroom {
namespace matp {
namespace {

// Signed overflow is undefined, and the operands of the mappings below reach
// this class from the server's timestamps, so each step says whether it fit.
bool addInt64(int64_t a, int64_t b, int64_t& out)
{
    if ((b > 0 && a > std::numeric_limits<int64_t>::max() - b) ||
        (b < 0 && a < std::numeric_limits<int64_t>::min() - b))
        return false;
    out = a + b;
    return true;
}

bool subtractInt64(int64_t a, int64_t b, int64_t& out)
{
    if ((b < 0 && a > std::numeric_limits<int64_t>::max() + b) ||
        (b > 0 && a < std::numeric_limits<int64_t>::min() + b))
        return false;
    out = a - b;
    return true;
}

// llround's result is unspecified outside int64's range. The bounds are powers
// of two, so the comparison is exact; NaN fails both.
bool roundToInt64(double value, int64_t& out)
{
    if (!(value >= -9223372036854775808.0 && value < 9223372036854775808.0))
        return false;
    out = std::llround(value);
    return true;
}

} // namespace

ClockModel::ClockModel() : ClockModel(Settings()) {}

ClockModel::ClockModel(const Settings& settings) : settings_(settings)
{
    if (settings_.capacity < 4)
        settings_.capacity = 4;
    if (settings_.minSamples < 2)
        settings_.minSamples = 2;
    if (settings_.minSkewSamples < 3)
        settings_.minSkewSamples = 3;
    if (settings_.windowSeconds <= 0.0)
        settings_.windowSeconds = 300.0;

    // The only allocation this class ever makes.
    ring_.resize(settings_.capacity);
    reset();
}

void ClockModel::reset()
{
    head_ = 0;
    count_ = 0;
    bestRoundTripNs_ = 0;

    hasEstimate_ = false;
    hasSkew_ = false;
    slopeNsPerSecond_ = 0.0;
    interceptNs_ = 0.0;
    residualStdDevNs_ = 0.0;
    consecutiveOutliers_ = 0;
    consecutiveSlow_ = 0;
    // rejected_ deliberately survives a reset: it is a lifetime diagnostic.
}

const ClockModel::Sample& ClockModel::at(uint32_t index) const
{
    return ring_[(head_ + index) % settings_.capacity];
}

double ClockModel::windowSpanSeconds() const
{
    if (count_ < 2)
        return 0.0;
    return static_cast<double>(at(count_ - 1).localNs - at(0).localNs) * 1e-9;
}

uint64_t ClockModel::lastRoundTripNs() const
{
    return count_ == 0 ? 0 : at(count_ - 1).roundTripNs;
}

int64_t ClockModel::offsetNs() const
{
    double offset = 0.0;
    int64_t rounded = 0;
    if (!hasEstimate_ || !predictOffsetNs(at(count_ - 1).localNs, offset) ||
        !roundToInt64(offset, rounded))
        return 0;
    return rounded;
}

double ClockModel::skewPpm() const
{
    // The slope is nanoseconds of offset per second of local time, which is
    // parts per billion; ppm is a thousandth of that.
    return hasSkew_ ? slopeNsPerSecond_ * 1e-3 : 0.0;
}

uint64_t ClockModel::uncertaintyNs() const
{
    if (!hasEstimate_)
        return 0;

    const uint64_t half = bestRoundTripNs_ / 2;
    const double scatter = residualStdDevNs_ > 0.0 ? residualStdDevNs_ : 0.0;
    const uint64_t spread = static_cast<uint64_t>(scatter);
    return half > spread ? half : spread;
}

bool ClockModel::derive(const ClockSyncSample& sample, Sample& out)
{
    // Time has to move forward on both machines, and the reply cannot have
    // been sent before the request arrived.
    if (sample.clientReceiveNs <= sample.clientSendNs)
        return false;
    if (sample.serverSendNs < sample.serverReceiveNs)
        return false;

    const uint64_t elapsedLocal = sample.clientReceiveNs - sample.clientSendNs;
    const uint64_t elapsedServer = sample.serverSendNs - sample.serverReceiveNs;
    if (elapsedServer > elapsedLocal)
        return false; // the server took longer than the whole round trip

    out.roundTripNs = elapsedLocal - elapsedServer;

    // offset = ((S2 - C1) + (S3 - C4)) / 2, written as (S2 - C1) - RTT/2:
    // algebraically the same, but the only large subtraction left is one the
    // two clocks can actually produce. S2 comes off the wire, so an offset
    // that does not fit in 64 signed bits is a hostile or broken server, and
    // refused rather than overflowed.
    const uint64_t distance = sample.serverReceiveNs >= sample.clientSendNs
                                  ? sample.serverReceiveNs - sample.clientSendNs
                                  : sample.clientSendNs - sample.serverReceiveNs;
    if (distance > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
        return false;
    const int64_t outbound = sample.serverReceiveNs >= sample.clientSendNs
                                 ? static_cast<int64_t>(distance)
                                 : -static_cast<int64_t>(distance);
    const int64_t halfRoundTrip = static_cast<int64_t>(out.roundTripNs / 2);
    if (outbound < std::numeric_limits<int64_t>::min() + halfRoundTrip)
        return false;
    out.offsetNs = outbound - halfRoundTrip;

    // recompute() subtracts the offsets of a window from each other, and the
    // first samples of one pass no outlier test. Below 2^62 ns (146 years)
    // apart from zero, any two differ by less than 2^63, so that cannot
    // overflow whatever a server sends.
    const int64_t kMaxOffsetNs = static_cast<int64_t>(1) << 62;
    if (out.offsetNs >= kMaxOffsetNs || out.offsetNs <= -kMaxOffsetNs)
        return false;

    // The instant the offset describes is halfway through the exchange, not
    // either end of it.
    out.localNs = sample.clientSendNs + elapsedLocal / 2;
    return true;
}

uint64_t ClockModel::roundTripLimit() const
{
    const double scaled = static_cast<double>(bestRoundTripNs_) * settings_.roundTripFactor;
    const uint64_t byFactor = static_cast<uint64_t>(scaled);
    const uint64_t byFloor = bestRoundTripNs_ + settings_.roundTripFloorNs;
    return byFactor > byFloor ? byFactor : byFloor;
}

void ClockModel::push(const Sample& entry)
{
    if (count_ < settings_.capacity)
    {
        ring_[(head_ + count_) % settings_.capacity] = entry;
        ++count_;
    }
    else
    {
        // Full: overwrite the oldest and move the window forward.
        ring_[head_] = entry;
        head_ = (head_ + 1) % settings_.capacity;
    }
}

void ClockModel::dropExpired(uint64_t newestLocalNs)
{
    const uint64_t windowNs = static_cast<uint64_t>(settings_.windowSeconds * 1e9);
    while (count_ > 2)
    {
        if (newestLocalNs - at(0).localNs <= windowNs)
            break;
        head_ = (head_ + 1) % settings_.capacity;
        --count_;
    }
}

void ClockModel::updateBestRoundTrip()
{
    bestRoundTripNs_ = 0;
    for (uint32_t i = 0; i < count_; ++i)
    {
        const uint64_t trip = at(i).roundTripNs;
        if (i == 0 || trip < bestRoundTripNs_)
            bestRoundTripNs_ = trip;
    }
}

// The gate in addSample only sees a sample once the window already has a best
// round trip to compare it against, so the first samples of a connection get
// in unfiltered. When a later sample shows what the path can really do, this
// drops the ones that no longer qualify.
//
// The most recent samples are kept whatever their round trip: they are the
// only evidence of what the network is doing now, and a path that has become
// permanently slower has to be allowed to become the new normal rather than
// starving the window.
bool ClockModel::pruneByRoundTrip()
{
    if (count_ <= settings_.minSamples)
        return false;

    const uint64_t limit = roundTripLimit();
    uint32_t kept = 0;
    for (uint32_t i = 0; i < count_; ++i)
    {
        // Copied out before the write below, which may land on the same slot.
        const Sample entry = at(i);
        const bool recent = (count_ - i) <= settings_.minSamples;
        if (!recent && entry.roundTripNs > limit)
            continue;
        ring_[(head_ + kept) % settings_.capacity] = entry;
        ++kept;
    }

    if (kept == count_)
        return false;
    count_ = kept;
    return true;
}

// Counts an outlier and restarts the window once too many have piled up in a
// row. Repeated outliers mean one of the two clocks stepped -- a restarted
// server, a suspended client -- and defending a mapping that no longer holds
// is worse than starting over from the sample that disagrees with it.
//
// Round trips that were simply too long are not counted here: congestion is a
// network event, and throwing away a good mapping because of one would be a
// self-inflicted resynchronization.
//
// Returns true when the sample was taken as the new start of the window.
bool ClockModel::handleOutlier(const Sample& entry)
{
    ++rejected_;
    ++consecutiveOutliers_;
    if (consecutiveOutliers_ < settings_.maxConsecutiveOutliers)
        return false;

    reset();
    push(entry);
    updateBestRoundTrip();
    recompute();
    return true;
}

bool ClockModel::addSample(const ClockSyncSample& sample)
{
    Sample entry;
    if (!derive(sample, entry) || entry.roundTripNs > settings_.maxRoundTripNs)
    {
        ++rejected_;
        return false;
    }

    if (count_ > 0)
    {
        // Out of order means two exchanges were mixed up, not that a clock
        // moved: the midpoint of a later exchange cannot be earlier.
        if (entry.localNs <= at(count_ - 1).localNs)
        {
            ++rejected_;
            return false;
        }

        // Section 8's "prefer low-RTT samples", as a gate rather than a
        // weighting: a sample that spent milliseconds in a queue carries an
        // offset error of up to half of them, and a queue is rarely symmetric.
        // A streak of them is no queue but the path's new normal: let through,
        // or the window would hold on to a round trip that no longer exists
        // and refuse every sample from here on.
        if (entry.roundTripNs <= roundTripLimit())
            consecutiveSlow_ = 0;
        else if (++consecutiveSlow_ <= settings_.maxConsecutiveSlow)
        {
            ++rejected_;
            return false;
        }
    }

    double predicted = 0.0;
    if (predictOffsetNs(entry.localNs, predicted))
    {
        double limit = settings_.outlierSigma * residualStdDevNs_;
        const double floorNs = static_cast<double>(settings_.outlierFloorNs);
        if (limit < floorNs)
            limit = floorNs;

        if (std::fabs(static_cast<double>(entry.offsetNs) - predicted) > limit)
            return handleOutlier(entry);
    }

    consecutiveOutliers_ = 0;
    push(entry);
    dropExpired(entry.localNs);
    updateBestRoundTrip();
    if (pruneByRoundTrip())
        updateBestRoundTrip();
    recompute();
    return true;
}

bool ClockModel::predictOffsetNs(uint64_t localNs, double& offsetNs) const
{
    if (!hasEstimate_)
        return false;

    const Sample& origin = at(0);
    const double x = static_cast<double>(static_cast<int64_t>(localNs) -
                                         static_cast<int64_t>(origin.localNs)) *
                     1e-9;
    offsetNs = static_cast<double>(origin.offsetNs) + interceptNs_ + slopeNsPerSecond_ * x;
    return true;
}

void ClockModel::recompute()
{
    if (count_ < settings_.minSamples)
    {
        hasEstimate_ = false;
        hasSkew_ = false;
        slopeNsPerSecond_ = 0.0;
        interceptNs_ = 0.0;
        residualStdDevNs_ = 0.0;
        return;
    }

    // Everything relative to the oldest sample in the window: absolute
    // nanosecond counts are far past the point where a double still resolves
    // the nanoseconds being measured.
    const Sample& origin = at(0);
    const double n = static_cast<double>(count_);

    double sumX = 0.0;
    double sumY = 0.0;
    double sumXX = 0.0;
    double sumXY = 0.0;

    for (uint32_t i = 0; i < count_; ++i)
    {
        const Sample& entry = at(i);
        const double x = static_cast<double>(entry.localNs - origin.localNs) * 1e-9;
        const double y = static_cast<double>(entry.offsetNs - origin.offsetNs);
        sumX += x;
        sumY += y;
        sumXX += x * x;
        sumXY += x * y;
    }

    const double sxx = sumXX - sumX * sumX / n;
    const double sxy = sumXY - sumX * sumY / n;

    bool skew = count_ >= settings_.minSkewSamples &&
                windowSpanSeconds() >= settings_.minSkewSpanSeconds && sxx > 0.0;

    double slope = 0.0;
    if (skew)
    {
        slope = sxy / sxx;
        if (std::fabs(slope * 1e-3) > settings_.maxSkewPpm)
        {
            // Two oscillators do not differ by this much. Whatever the window
            // is describing, it is not a frequency difference.
            slope = 0.0;
            skew = false;
        }
    }

    // With no skew this collapses to the mean offset, which is exactly the
    // right answer for a window too short to resolve a slope.
    const double intercept = (sumY - slope * sumX) / n;

    double ssr = 0.0;
    for (uint32_t i = 0; i < count_; ++i)
    {
        const Sample& entry = at(i);
        const double x = static_cast<double>(entry.localNs - origin.localNs) * 1e-9;
        const double y = static_cast<double>(entry.offsetNs - origin.offsetNs);
        const double r = y - (slope * x + intercept);
        ssr += r * r;
    }

    const double degreesOfFreedom = n - (skew ? 2.0 : 1.0);
    residualStdDevNs_ = degreesOfFreedom > 0.0 ? std::sqrt(ssr / degreesOfFreedom) : 0.0;

    slopeNsPerSecond_ = slope;
    interceptNs_ = intercept;
    hasSkew_ = skew;
    hasEstimate_ = true;
}

bool ClockModel::serverFromLocal(uint64_t localNs, uint64_t& serverNs) const
{
    double offset = 0.0;
    if (!predictOffsetNs(localNs, offset))
        return false;

    // Integer arithmetic for the absolute value: only the offset itself is
    // small enough to survive a double.
    int64_t rounded = 0;
    int64_t server = 0;
    if (localNs > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ||
        !roundToInt64(offset, rounded) ||
        !addInt64(static_cast<int64_t>(localNs), rounded, server) || server < 0)
        return false;

    serverNs = static_cast<uint64_t>(server);
    return true;
}

bool ClockModel::localFromServer(uint64_t serverNs, uint64_t& localNs) const
{
    if (!hasEstimate_)
        return false;

    const Sample& origin = at(0);
    const int64_t kMax = std::numeric_limits<int64_t>::max();

    // Inverting server(t) = t + offset(t). The only quantity that goes through
    // a double is the elapsed time since the origin, which the window and the
    // scheduling horizon both keep small. Every integer step is checked:
    // serverNs and the offsets come off the wire, and a sum that does not fit
    // is a schedule nobody can keep, refused rather than overflowed.
    int64_t intercept = 0;
    int64_t serverAtOrigin = 0;
    int64_t elapsedNs = 0;
    if (origin.localNs > static_cast<uint64_t>(kMax) || serverNs > static_cast<uint64_t>(kMax) ||
        !roundToInt64(interceptNs_, intercept) ||
        !addInt64(static_cast<int64_t>(origin.localNs), origin.offsetNs, serverAtOrigin) ||
        !addInt64(serverAtOrigin, intercept, serverAtOrigin) ||
        !subtractInt64(static_cast<int64_t>(serverNs), serverAtOrigin, elapsedNs))
        return false;

    const double scale = 1.0 + slopeNsPerSecond_ * 1e-9;
    if (scale <= 0.0)
        return false; // a server clock running backwards is not a mapping

    int64_t localElapsed = 0;
    int64_t local = 0;
    if (!roundToInt64(static_cast<double>(elapsedNs) / scale, localElapsed) ||
        !addInt64(static_cast<int64_t>(origin.localNs), localElapsed, local) || local < 0)
        return false;

    localNs = static_cast<uint64_t>(local);
    return true;
}

} // namespace matp
} // namespace multiroom
