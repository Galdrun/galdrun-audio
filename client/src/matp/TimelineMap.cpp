#include "matp/TimelineMap.h"

#include <cstdint>

namespace multiroom {
namespace matp {
namespace {

// floor(value * numerator / denominator), exactly, or false when the result
// does not fit in 64 bits.
//
// Splitting the division out first is exact rather than merely safe: the
// remainder term is the part a plain `value / denominator * numerator` would
// have lost. Both multiplies are checked because the interpolating callers
// pass a value smaller than the denominator and could not overflow, while the
// extrapolating ones pass a distance bounded only by how far ahead a server
// schedules -- and the numbers reach this class straight out of a packet
// header.
bool scaleExact(uint64_t value, uint64_t numerator, uint64_t denominator, uint64_t& out)
{
    if (denominator == 0)
        return false;
    if (numerator == 0)
    {
        out = 0;
        return true;
    }

    const uint64_t whole = value / denominator;
    const uint64_t remainder = value % denominator;

    if (whole > UINT64_MAX / numerator)
        return false;
    const uint64_t scaledWhole = whole * numerator;

    if (remainder > UINT64_MAX / numerator)
        return false;
    const uint64_t scaledRemainder = (remainder * numerator) / denominator;

    if (scaledWhole > UINT64_MAX - scaledRemainder)
        return false;
    out = scaledWhole + scaledRemainder;
    return true;
}

// base + scaleExact(...), so that a caller cannot add an in-range offset to an
// in-range base and land outside the range.
bool offsetBy(uint64_t base, uint64_t value, uint64_t numerator, uint64_t denominator,
              uint64_t& out)
{
    uint64_t scaled = 0;
    if (!scaleExact(value, numerator, denominator, scaled))
        return false;
    if (base > UINT64_MAX - scaled)
        return false;
    out = base + scaled;
    return true;
}

} // namespace

TimelineMap::TimelineMap() : segments_(kTimelineSegments)
{
}

void TimelineMap::clear()
{
    head_ = 0;
    count_ = 0;
}

const TimelineSegment& TimelineMap::at(size_t index) const
{
    return segments_[(head_ + index) % segments_.size()];
}

void TimelineMap::append(const TimelineSegment& segment)
{
    if (segment.streamFrameCount == 0 || segment.timelineDurationNs == 0)
        return; // nothing to interpolate inside; a caller that got here has a bug

    if (count_ < segments_.size())
    {
        segments_[(head_ + count_) % segments_.size()] = segment;
        ++count_;
        return;
    }

    // Full: the oldest segment is overwritten and the head moves on. The audio
    // it described has long been played, and a lookup that wanted it says so.
    segments_[head_] = segment;
    head_ = (head_ + 1) % segments_.size();
}

uint64_t TimelineMap::oldestStreamEnd() const
{
    if (count_ == 0)
        return 0;
    const TimelineSegment& oldest = at(0);
    return oldest.streamStartFrame + oldest.streamFrameCount;
}

uint64_t TimelineMap::nextStreamFrame() const
{
    if (count_ == 0)
        return 0;
    const TimelineSegment& newest = at(count_ - 1);
    return newest.streamStartFrame + newest.streamFrameCount;
}

uint64_t TimelineMap::nextTimelineNs() const
{
    if (count_ == 0)
        return 0;
    const TimelineSegment& newest = at(count_ - 1);
    return newest.timelineStartNs + newest.timelineDurationNs;
}

bool TimelineMap::timelineToStream(uint64_t timelineNs, uint64_t& streamFrame) const
{
    if (count_ == 0)
        return false;

    if (timelineNs < at(0).timelineStartNs)
        return false;

    const TimelineSegment& newest = at(count_ - 1);
    const uint64_t frontierNs = newest.timelineStartNs + newest.timelineDurationNs;
    if (timelineNs >= frontierNs)
    {
        return offsetBy(newest.streamStartFrame + newest.streamFrameCount,
                        timelineNs - frontierNs, newest.streamFrameCount,
                        newest.timelineDurationNs, streamFrame);
    }

    // The last segment that begins at or before the position. Both axes are
    // contiguous and ascending, so this is the one that contains it.
    size_t low = 0;
    size_t high = count_ - 1;
    while (low < high)
    {
        const size_t mid = low + (high - low + 1) / 2;
        if (at(mid).timelineStartNs <= timelineNs)
            low = mid;
        else
            high = mid - 1;
    }

    const TimelineSegment& segment = at(low);
    return offsetBy(segment.streamStartFrame, timelineNs - segment.timelineStartNs,
                    segment.streamFrameCount, segment.timelineDurationNs, streamFrame);
}

bool TimelineMap::streamToTimeline(uint64_t streamFrame, uint64_t& timelineNs) const
{
    if (count_ == 0)
        return false;

    if (streamFrame < at(0).streamStartFrame)
        return false;

    const TimelineSegment& newest = at(count_ - 1);
    const uint64_t frontier = newest.streamStartFrame + newest.streamFrameCount;
    if (streamFrame >= frontier)
    {
        return offsetBy(newest.timelineStartNs + newest.timelineDurationNs,
                        streamFrame - frontier, newest.timelineDurationNs,
                        newest.streamFrameCount, timelineNs);
    }

    size_t low = 0;
    size_t high = count_ - 1;
    while (low < high)
    {
        const size_t mid = low + (high - low + 1) / 2;
        if (at(mid).streamStartFrame <= streamFrame)
            low = mid;
        else
            high = mid - 1;
    }

    const TimelineSegment& segment = at(low);
    return offsetBy(segment.timelineStartNs, streamFrame - segment.streamStartFrame,
                    segment.timelineDurationNs, segment.streamFrameCount, timelineNs);
}

} // namespace matp
} // namespace multiroom
