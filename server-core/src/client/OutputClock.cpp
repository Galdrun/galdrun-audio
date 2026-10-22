#include "client/OutputClock.h"

#include <cmath>
#include <limits>

namespace multiroom {
namespace {

// What a drift correction can never be. Section 46's limits are ±100 ppm and
// ±500 ppm while converging, and even soxr's own trim window is ±2000 ppm, so
// anything beyond a factor of two is a controller fault rather than a ratio.
constexpr double kMinTrim = 0.5;
constexpr double kMaxTrim = 2.0;

} // namespace

const char* errorName(OutputClockError error)
{
    switch (error)
    {
    case OutputClockError::InvalidSampleRate:
        return "InvalidSampleRate";
    case OutputClockError::InvalidTrim:
        return "InvalidTrim";
    case OutputClockError::BeforeAnchor:
        return "BeforeAnchor";
    case OutputClockError::Overflow:
        return "Overflow";
    }
    return "Unknown";
}

OutputClock::OutputClock(uint32_t outputRate, TimelineNs origin) noexcept
    : sampleRate_(outputRate), origin_(origin), anchorNs_(origin)
{
}

Result<OutputFrame, OutputClockError> OutputClock::frameAt(TimelineNs position) const
{
    if (sampleRate_ == 0)
        return fail(OutputClockError::InvalidSampleRate, "the output format has no rate");

    const uint64_t at = std::to_underlying(position);
    const uint64_t anchor = std::to_underlying(anchorNs_);
    if (at < anchor)
    {
        return fail(OutputClockError::BeforeAnchor,
                    std::to_string(at) + " ns is before this room's stream, which begins at " +
                        std::to_string(anchor) + " ns");
    }

    const uint64_t spanNs = at - anchor;
    const uint64_t anchorFrame = std::to_underlying(anchorFrame_);

    uint64_t frames = 0;
    if (trim_ == 1.0)
    {
        // The exact path, and the one every room takes until something is
        // being corrected: 20 ms is 882 frames at 44.1 kHz with no remainder
        // to carry, so a stream of any length is integer arithmetic from the
        // anchor.
        const Result<uint64_t, FrameMathError> exact = framesFromNanoseconds(spanNs, sampleRate_);
        if (!exact)
        {
            return fail(OutputClockError::Overflow,
                        "timeline span of " + std::to_string(spanNs) +
                            " ns is more frames than 64 bits hold at " +
                            std::to_string(sampleRate_) + " Hz");
        }
        frames = *exact;
    }
    else
    {
        // A trimmed room. The product is formed as a scaled span rather than
        // as span * rate * trim so that neither factor reaches the point where
        // a double stops counting whole frames: at the ceiling of this
        // server's rates a day of audio is under 1e10 frames, which a double
        // holds exactly.
        // The residual of the last re-anchor rides along: what is counted here
        // is the running total since that anchor, and the total began
        // part-way into a frame.
        const double frameValue = anchorResidual_ + static_cast<double>(spanNs) * 1e-9 *
                                                        static_cast<double>(sampleRate_) * trim_;
        if (!(frameValue >= 0.0) ||
            frameValue >= static_cast<double>(std::numeric_limits<uint64_t>::max() - anchorFrame))
        {
            return fail(OutputClockError::Overflow,
                        "timeline span of " + std::to_string(spanNs) +
                            " ns is more frames than 64 bits hold at " +
                            std::to_string(sampleRate_) + " Hz");
        }
        frames = static_cast<uint64_t>(std::floor(frameValue));
    }

    if (frames > std::numeric_limits<uint64_t>::max() - anchorFrame)
    {
        return fail(OutputClockError::Overflow,
                    "this room's frame counter has run past what 64 bits hold");
    }

    return OutputFrame{anchorFrame + frames};
}

Status<OutputClockError> OutputClock::setTrim(double trim, TimelineNs at)
{
    if (!(trim >= kMinTrim) || !(trim <= kMaxTrim))
    {
        return fail(OutputClockError::InvalidTrim,
                    "a trim of " + std::to_string(trim) + " is outside " +
                        std::to_string(kMinTrim) + " .. " + std::to_string(kMaxTrim));
    }

    // The frames asked for so far are asked for under the old trim. Reading
    // them before the new one is installed is what makes the change a hinge
    // rather than a rewrite of everything already sent.
    const Result<OutputFrame, OutputClockError> anchorFrame = frameAt(at);
    if (!anchorFrame)
        return std::unexpected(anchorFrame.error());

    // And the part of a frame that whole count left behind, which is what a
    // correction arriving twice a second would otherwise throw away twice a
    // second. It is formed from the span since the *current* anchor rather
    // than from the total, so nothing large is ever added to something small.
    //
    // The exact path has no remainder worth carrying: a packet grid of whole
    // milliseconds is a whole number of frames at every rate this server
    // serves, and it is the only thing that re-anchors.
    double residual = anchorResidual_;
    if (trim_ != 1.0)
    {
        const double spanSinceAnchorNs =
            static_cast<double>(std::to_underlying(at) - std::to_underlying(anchorNs_));
        const double advance = anchorResidual_ + spanSinceAnchorNs * 1e-9 *
                                                     static_cast<double>(sampleRate_) * trim_;
        residual = advance - std::floor(advance);
    }

    anchorFrame_ = *anchorFrame;
    anchorResidual_ = residual;
    anchorNs_ = at;
    trim_ = trim;
    return {};
}

} // namespace multiroom
