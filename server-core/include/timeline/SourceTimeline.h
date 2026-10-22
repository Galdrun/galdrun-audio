#pragma once

// The zone's segment table: which timeline nanosecond a source frame is heard
// at, and the other way round.
//
// The store beside this one (audio/NativePcmStore.h) holds source frames and
// nothing else, because audio that has been decoded cannot be re-anchored
// while its mapping onto the timeline can. This is that mapping, and it is the
// only place in the server -- with client/OutputClock from phase 9 -- where
// one axis of core/Units.h is converted into another.
//
// **One entry per staged source, never one per packet or per block.** A
// segment is a source's first frame, the timeline position that frame is heard
// at, and the rate it was decoded at; everything inside it follows from those
// three by integer arithmetic. A track boundary (MATP section 23) therefore
// adds an entry and re-anchors nothing: the base and the frame position carry
// on, which is exactly what section 15 requires of a sender and what makes
// gapless playback (section 22) a property of the arithmetic rather than a
// feature.
//
// **Absolute, never accumulated.** Every position is computed from its
// segment's base, so no rounding error can build up over a run of any length.
// Adding up per-packet durations is the one implementation section 15 names as
// wrong, and the client reads its result as a discontinuity and rejoins.
//
// **History is bounded and its loss is honest.** A server that runs for a week
// stages thousands of sources, so the table keeps a fixed number of them and
// drops the oldest. A position inside a dropped segment cannot be mapped and
// is reported as NotCovered rather than guessed -- section 33's rule for the
// client's copy of the same problem.
//
// Threading: guarded by its own mutex. The pipeline thread stages a source
// while every client output thread maps positions on it, and the lock is held
// for a binary search over a handful of entries and nothing else.

#include "core/Error.h"
#include "core/Units.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>

namespace multiroom {

enum class TimelineError
{
    InvalidSampleRate, // zero; a format that was never validated
    OutOfOrder,        // a source staged before the previous one's start
    Empty,             // nothing staged yet
    NotCovered,        // before the first frame still held, or before its base
    Overflow           // beyond what 64 bits can express on the other axis
};

const char* errorName(TimelineError error);

// One staged source. The range it covers ends where the next one begins, and
// the newest one is open-ended: audio that has not been decoded yet still has
// a timeline position, which is what lets a PAUSE_AT name a position ahead of
// the frontier (section 33).
struct TimelineSegment
{
    SourceFrame firstFrame{};
    TimelineNs baseNs{}; // where firstFrame is heard
    uint32_t sampleRate = 0;
};

class SourceTimeline
{
public:
    // Sixty-four sources is over three hours of album-length tracks, and the
    // table costs a few hundred bytes. The limit exists so that "memory is
    // bounded" holds for a server that is never restarted, not because anyone
    // is expected to reach it.
    static constexpr size_t kDefaultMaxSegments = 64;

    explicit SourceTimeline(size_t maxSegments = kDefaultMaxSegments);

    SourceTimeline(const SourceTimeline&) = delete;
    SourceTimeline& operator=(const SourceTimeline&) = delete;

    // Appends a source that begins at `firstFrame` of the store's frame
    // numbering. The previous source ends there, which is what fixes its
    // length, and the new base is the previous base plus that length -- the
    // timeline carries on rather than restarting. The first staged source is
    // heard at timeline zero (MATP section 5: a zone's timeline starts at zero
    // when the zone starts playing).
    //
    // A source staged at the previous one's own first frame replaces it: the
    // previous source ended with no frames decoded, so nothing maps to it.
    //
    // Returns the timeline position the source begins at, which is the
    // position a TRACK_BOUNDARY for it carries.
    Result<TimelineNs, TimelineError> stage(SourceFrame firstFrame, uint32_t sampleRate);

    // Phase 26's seek: a source that is heard from `base` rather than from
    // where the previous one would have ended. The timeline still only moves
    // forward -- `base` may not lie before the previous source's own base --
    // but it may lie *inside* the previous source's range, and from `base` on
    // every position maps to the new source's frames. The previous source's
    // frames past that point keep their positions on the other axis and are
    // simply never asked for: a room is moved onto the new source, not walked
    // through the old one.
    Result<TimelineNs, TimelineError> stageAt(SourceFrame firstFrame, uint32_t sampleRate,
                                              TimelineNs base);

    // Where a source frame is heard. Forward extrapolation past the newest
    // segment's decoded audio is deliberate and allowed; a position before the
    // oldest segment still held is NotCovered.
    [[nodiscard]] Result<TimelineNs, TimelineError> timelineAt(SourceFrame frame) const;

    // The frame audible at a timeline position, rounded down: the frame the
    // position falls in.
    [[nodiscard]] Result<SourceFrame, TimelineError> frameAt(TimelineNs position) const;

    size_t segmentCount() const;
    std::optional<TimelineSegment> segment(size_t index) const;

    // How many entries the bound above has cost. The status API reports it
    // rather than leaving an unmappable position unexplained.
    uint64_t droppedSegments() const;

private:
    mutable std::mutex mutex_;
    std::deque<TimelineSegment> segments_; // guarded by mutex_
    uint64_t dropped_ = 0;                 // guarded by mutex_
    size_t maxSegments_ = kDefaultMaxSegments;
};

} // namespace multiroom
