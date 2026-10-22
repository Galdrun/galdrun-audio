#include "audio/DriftEstimator.h"

#include <cmath>

namespace multiroom {
namespace {

double clamp01(double value)
{
    if (value < 0.0)
        return 0.0;
    if (value > 1.0)
        return 1.0;
    return value;
}

// How far the fit can be trusted, from its standard error. Either of two things
// is enough: an error small in absolute terms (1 ppm is already half-useful,
// against a whole-system budget of a few tens), or one small against the drift
// it found. A card with a coarse position counter fits 156 +/- 8.6 ppm --
// noisy, and still 18 sigma from zero. A drift within three errors of zero
// stays near the absolute term, so a fit that is only noise is not trusted.
// The server computes the same figure the same way, so section 26's
// cross-check compares like with like.
double fitPrecision(double driftPpm, double standardErrorPpm)
{
    const double absolute = 1.0 / (1.0 + standardErrorPpm);
    const double magnitude = std::fabs(driftPpm);
    if (magnitude <= 0.0)
        return absolute;
    const double significance = magnitude / (magnitude + 3.0 * standardErrorPpm);
    return absolute > significance ? absolute : significance;
}

// How much the timestamps themselves can be trusted. A driver-side timestamp
// is a real measurement but one step further from the crystal; an estimated
// one is our own clock wearing a disguise.
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
    return static_cast<int>(candidate) < static_cast<int>(current);
}

} // namespace

DriftEstimator::DriftEstimator() : DriftEstimator(Settings()) {}

DriftEstimator::DriftEstimator(const Settings& settings) : settings_(settings)
{
    if (settings_.capacity < 4)
        settings_.capacity = 4;
    if (settings_.minSamples < 3)
        settings_.minSamples = 3;
    if (settings_.windowSeconds <= 0.0)
        settings_.windowSeconds = 20.0;

    // The only allocation this class ever makes.
    ring_.resize(settings_.capacity);
    reset();
}

void DriftEstimator::reset()
{
    head_ = 0;
    count_ = 0;
    nominalSampleRate_ = 0;
    worstQuality_ = AudioClockQuality::Unavailable;

    hasEstimate_ = false;
    estimatedRate_ = 0.0;
    driftPpm_ = 0.0;
    confidence_ = 0.0;
    standardErrorPpm_ = 0.0;

    slope_ = 0.0;
    intercept_ = 0.0;
    residualStdDev_ = 0.0;
    consecutiveRejections_ = 0;
    // rejected_ deliberately survives a reset: it is a lifetime diagnostic.
}

const DriftEstimator::Sample& DriftEstimator::at(uint32_t index) const
{
    return ring_[(head_ + index) % settings_.capacity];
}

double DriftEstimator::windowSpanSeconds() const
{
    if (count_ < 2)
        return 0.0;
    const uint64_t first = at(0).timeNs;
    const uint64_t last = at(count_ - 1).timeNs;
    return static_cast<double>(last - first) * 1e-9;
}

void DriftEstimator::pushSample(const AudioClockSnapshot& sample)
{
    Sample entry;
    entry.timeNs = sample.monotonicTimeNs;
    entry.frames = sample.deviceFramePosition;
    entry.quality = sample.quality;

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

void DriftEstimator::dropExpired(uint64_t newestTimeNs)
{
    const uint64_t windowNs = static_cast<uint64_t>(settings_.windowSeconds * 1e9);
    while (count_ > 2)
    {
        const uint64_t oldest = at(0).timeNs;
        if (newestTimeNs - oldest <= windowNs)
            break;
        head_ = (head_ + 1) % settings_.capacity;
        --count_;
    }
}

bool DriftEstimator::predictResidual(const AudioClockSnapshot& sample,
                                     double& residualFrames) const
{
    if (!hasEstimate_ || count_ < settings_.minSamples)
        return false;

    const Sample& origin = at(0);
    const double x = static_cast<double>(sample.monotonicTimeNs - origin.timeNs) * 1e-9;
    const double y = static_cast<double>(sample.deviceFramePosition - origin.frames);
    residualFrames = y - (slope_ * x + intercept_);
    return true;
}

// Counts a rejection, and restarts the window once too many have piled up in a
// row. Repeated rejections mean the window is describing a clock that no longer
// exists -- a reopened device, an xrun, a format change -- so defending the old
// fit forever would be worse than starting over.
//
// Returns true when the sample was taken as the new start of the window.
bool DriftEstimator::handleRejection(const AudioClockSnapshot& sample)
{
    ++rejected_;
    ++consecutiveRejections_;
    if (consecutiveRejections_ < settings_.maxConsecutiveRejections)
        return false;

    reset();
    nominalSampleRate_ = sample.nominalSampleRate;
    pushSample(sample);
    recompute();
    return true;
}

bool DriftEstimator::addSample(const AudioClockSnapshot& sample)
{
    if (!sample.isValid())
    {
        ++rejected_;
        return false;
    }

    // A format change makes the old window meaningless: it was measuring a
    // different clock configuration.
    if (nominalSampleRate_ != 0 && sample.nominalSampleRate != nominalSampleRate_)
        reset();

    if (count_ > 0)
    {
        const Sample& last = at(count_ - 1);

        // Time and the frame position must both move forward. Either going
        // backwards means the device was reset under us, not that the clock
        // drifted.
        if (sample.monotonicTimeNs <= last.timeNs || sample.deviceFramePosition < last.frames)
            return handleRejection(sample);

        // Gross plausibility: between two samples the device cannot have run
        // at less than half or more than twice its nominal rate. This catches
        // a stalled or restarted position long before the residual test could.
        const double seconds =
            static_cast<double>(sample.monotonicTimeNs - last.timeNs) * 1e-9;
        const double frames = static_cast<double>(sample.deviceFramePosition - last.frames);
        const double nominal = static_cast<double>(sample.nominalSampleRate);
        if (seconds > 0.02) // too short an interval says nothing useful
        {
            const double impliedRate = frames / seconds;
            if (impliedRate < nominal * 0.5 || impliedRate > nominal * 2.0)
                return handleRejection(sample);
        }
    }

    // Residual test against the current fit, once there is one to test against.
    double residual = 0.0;
    if (predictResidual(sample, residual))
    {
        const double floorFrames =
            settings_.outlierFloorSeconds * static_cast<double>(sample.nominalSampleRate);
        double limit = settings_.outlierSigma * residualStdDev_;
        if (limit < floorFrames)
            limit = floorFrames;

        if (std::fabs(residual) > limit)
            return handleRejection(sample);
    }

    consecutiveRejections_ = 0;
    nominalSampleRate_ = sample.nominalSampleRate;
    pushSample(sample);
    dropExpired(sample.monotonicTimeNs);
    recompute();
    return true;
}

void DriftEstimator::recompute()
{
    // Only the samples still in the window: one that expired no longer
    // contributes to the fit, so it no longer lowers its quality either.
    worstQuality_ = AudioClockQuality::Unavailable;
    for (uint32_t i = 0; i < count_; ++i)
    {
        const AudioClockQuality quality = at(i).quality;
        if (worstQuality_ == AudioClockQuality::Unavailable || isWorse(quality, worstQuality_))
            worstQuality_ = quality;
    }

    if (count_ < settings_.minSamples)
    {
        hasEstimate_ = false;
        estimatedRate_ = 0.0;
        driftPpm_ = 0.0;
        confidence_ = 0.0;
        standardErrorPpm_ = 0.0;
        return;
    }

    // Everything is measured relative to the oldest sample in the window.
    // Absolute nanosecond and frame counts are large enough that regressing
    // them directly would lose the very precision being measured.
    const Sample& origin = at(0);
    const double n = static_cast<double>(count_);

    double sumX = 0.0;
    double sumY = 0.0;
    double sumXX = 0.0;
    double sumXY = 0.0;

    for (uint32_t i = 0; i < count_; ++i)
    {
        const Sample& entry = at(i);
        const double x = static_cast<double>(entry.timeNs - origin.timeNs) * 1e-9;
        const double y = static_cast<double>(entry.frames - origin.frames);
        sumX += x;
        sumY += y;
        sumXX += x * x;
        sumXY += x * y;
    }

    // Recomputed in full rather than kept incrementally: with a few hundred
    // samples this is nothing, and subtracting terms as they leave the window
    // loses precision exactly where it matters.
    const double sxx = sumXX - sumX * sumX / n;
    const double sxy = sumXY - sumX * sumY / n;

    if (sxx <= 0.0)
    {
        hasEstimate_ = false;
        confidence_ = 0.0;
        return;
    }

    slope_ = sxy / sxx;
    intercept_ = (sumY - slope_ * sumX) / n;

    double ssr = 0.0;
    for (uint32_t i = 0; i < count_; ++i)
    {
        const Sample& entry = at(i);
        const double x = static_cast<double>(entry.timeNs - origin.timeNs) * 1e-9;
        const double y = static_cast<double>(entry.frames - origin.frames);
        const double r = y - (slope_ * x + intercept_);
        ssr += r * r;
    }

    const double degreesOfFreedom = n - 2.0;
    const double residualVariance = degreesOfFreedom > 0.0 ? ssr / degreesOfFreedom : 0.0;
    residualStdDev_ = std::sqrt(residualVariance);

    estimatedRate_ = slope_;
    hasEstimate_ = true;

    const double nominal = static_cast<double>(nominalSampleRate_);
    driftPpm_ = nominal > 0.0 ? (estimatedRate_ / nominal - 1.0) * 1e6 : 0.0;

    // Standard error of the slope, which is what "how well do we know this"
    // actually means for a regression.
    const double slopeStandardError = std::sqrt(residualVariance / sxx);
    standardErrorPpm_ = nominal > 0.0 ? slopeStandardError / nominal * 1e6 : 0.0;

    // Three independent things have to be true for the number to be worth
    // acting on: enough window, a tight enough fit, and trustworthy
    // timestamps.
    const double fill = clamp01(windowSpanSeconds() / settings_.windowSeconds);
    confidence_ =
        clamp01(fill * fitPrecision(driftPpm_, standardErrorPpm_) * qualityFactor(worstQuality_));
}

} // namespace multiroom
