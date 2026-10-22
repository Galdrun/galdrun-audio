#include "timeline/SourceTimeline.h"

#include <algorithm>
#include <limits>
#include <string>

namespace multiroom {
namespace {

// The two mappings of core/Units.h report the same two conditions this module
// does, under its own names.
TimelineError fromFrameMath(FrameMathError error)
{
    return error == FrameMathError::InvalidSampleRate ? TimelineError::InvalidSampleRate
                                                      : TimelineError::Overflow;
}

std::string frameText(SourceFrame frame)
{
    return "frame " + std::to_string(std::to_underlying(frame));
}

} // namespace

const char* errorName(TimelineError error)
{
    switch (error)
    {
    case TimelineError::InvalidSampleRate:
        return "InvalidSampleRate";
    case TimelineError::OutOfOrder:
        return "OutOfOrder";
    case TimelineError::Empty:
        return "Empty";
    case TimelineError::NotCovered:
        return "NotCovered";
    case TimelineError::Overflow:
        return "Overflow";
    }
    return "?";
}

// A table that holds no segments could map nothing at all, so zero is read as
// one rather than refused: the constructor has no way to report a failure and
// a silently empty timeline would be worse than a very short history.
SourceTimeline::SourceTimeline(size_t maxSegments)
    : maxSegments_(maxSegments == 0 ? 1 : maxSegments)
{
}

Result<TimelineNs, TimelineError> SourceTimeline::stage(SourceFrame firstFrame,
                                                        uint32_t sampleRate)
{
    if (sampleRate == 0)
        return fail(TimelineError::InvalidSampleRate, frameText(firstFrame));

    const std::lock_guard<std::mutex> lock(mutex_);

    TimelineNs base{};
    if (!segments_.empty())
    {
        const TimelineSegment& previous = segments_.back();
        const uint64_t previousFirst = std::to_underlying(previous.firstFrame);
        const uint64_t first = std::to_underlying(firstFrame);

        // The previous source's length is what this call fixes. One staged at
        // the same frame means the previous source ended before it decoded
        // anything -- stopped or skipped while still opening. It has no
        // length and no position maps to it, so the new source takes its
        // place rather than failing the zone.
        if (first == previousFirst)
        {
            const TimelineNs previousBase = previous.baseNs;
            segments_.back() = TimelineSegment{firstFrame, previousBase, sampleRate};
            return previousBase;
        }
        if (first < previousFirst)
        {
            return fail(TimelineError::OutOfOrder,
                        frameText(firstFrame) + " does not follow " + frameText(previous.firstFrame));
        }

        const auto length = nanosecondsFromFrames(first - previousFirst, previous.sampleRate);
        if (!length)
            return fail(fromFrameMath(length.error().code), describe(length.error()));

        const uint64_t previousBase = std::to_underlying(previous.baseNs);
        if (*length > std::numeric_limits<uint64_t>::max() - previousBase)
            return fail(TimelineError::Overflow, frameText(firstFrame));

        base = TimelineNs{previousBase + *length};
    }

    segments_.push_back(TimelineSegment{firstFrame, base, sampleRate});
    if (segments_.size() > maxSegments_)
    {
        segments_.pop_front();
        ++dropped_;
    }
    return base;
}

Result<TimelineNs, TimelineError> SourceTimeline::stageAt(SourceFrame firstFrame,
                                                          uint32_t sampleRate, TimelineNs base)
{
    if (sampleRate == 0)
        return fail(TimelineError::InvalidSampleRate, frameText(firstFrame));

    const std::lock_guard<std::mutex> lock(mutex_);
    if (!segments_.empty())
    {
        // The same two orders stage() keeps, and nothing else: the frames
        // still follow on, and the base may cut the previous source short but
        // never place anything before it. frameAt() takes the newest segment a
        // position reaches, so a base inside the previous range is a cut.
        const TimelineSegment& previous = segments_.back();
        if (std::to_underlying(firstFrame) < std::to_underlying(previous.firstFrame))
        {
            return fail(TimelineError::OutOfOrder,
                        frameText(firstFrame) + " does not follow " + frameText(previous.firstFrame));
        }
        if (std::to_underlying(base) < std::to_underlying(previous.baseNs))
        {
            return fail(TimelineError::OutOfOrder,
                        std::to_string(std::to_underlying(base)) + " ns is before " +
                            std::to_string(std::to_underlying(previous.baseNs)) + " ns");
        }
        // As in stage(): a previous source with no frames is replaced.
        if (std::to_underlying(firstFrame) == std::to_underlying(previous.firstFrame))
        {
            segments_.back() = TimelineSegment{firstFrame, base, sampleRate};
            return base;
        }
    }

    segments_.push_back(TimelineSegment{firstFrame, base, sampleRate});
    if (segments_.size() > maxSegments_)
    {
        segments_.pop_front();
        ++dropped_;
    }
    return base;
}

Result<TimelineNs, TimelineError> SourceTimeline::timelineAt(SourceFrame frame) const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    if (segments_.empty())
        return fail(TimelineError::Empty, frameText(frame));

    const uint64_t target = std::to_underlying(frame);
    const auto after = std::upper_bound(segments_.begin(), segments_.end(), target,
                                        [](uint64_t value, const TimelineSegment& segment)
                                        { return value < std::to_underlying(segment.firstFrame); });
    if (after == segments_.begin())
    {
        return fail(TimelineError::NotCovered,
                    frameText(frame) + " is before " + frameText(segments_.front().firstFrame));
    }

    const TimelineSegment& segment = *std::prev(after);
    const auto offset = nanosecondsFromFrames(target - std::to_underlying(segment.firstFrame),
                                              segment.sampleRate);
    if (!offset)
        return fail(fromFrameMath(offset.error().code), describe(offset.error()));

    const uint64_t base = std::to_underlying(segment.baseNs);
    if (*offset > std::numeric_limits<uint64_t>::max() - base)
        return fail(TimelineError::Overflow, frameText(frame));

    return TimelineNs{base + *offset};
}

Result<SourceFrame, TimelineError> SourceTimeline::frameAt(TimelineNs position) const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    if (segments_.empty())
        return fail(TimelineError::Empty, std::to_string(std::to_underlying(position)) + " ns");

    const uint64_t target = std::to_underlying(position);
    const auto after = std::upper_bound(segments_.begin(), segments_.end(), target,
                                        [](uint64_t value, const TimelineSegment& segment)
                                        { return value < std::to_underlying(segment.baseNs); });
    if (after == segments_.begin())
    {
        return fail(TimelineError::NotCovered,
                    std::to_string(target) + " ns is before " +
                        std::to_string(std::to_underlying(segments_.front().baseNs)) + " ns");
    }

    const TimelineSegment& segment = *std::prev(after);
    const auto offset =
        framesFromNanoseconds(target - std::to_underlying(segment.baseNs), segment.sampleRate);
    if (!offset)
        return fail(fromFrameMath(offset.error().code), describe(offset.error()));

    const uint64_t first = std::to_underlying(segment.firstFrame);
    if (*offset > std::numeric_limits<uint64_t>::max() - first)
        return fail(TimelineError::Overflow, std::to_string(target) + " ns");

    return SourceFrame{first + *offset};
}

size_t SourceTimeline::segmentCount() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return segments_.size();
}

std::optional<TimelineSegment> SourceTimeline::segment(size_t index) const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    if (index >= segments_.size())
        return std::nullopt;
    return segments_[index];
}

uint64_t SourceTimeline::droppedSegments() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return dropped_;
}

} // namespace multiroom
