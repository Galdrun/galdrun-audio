#pragma once

// The mapping between the zone's media timeline and this client's own stream
// of PCM frames.
//
// The two are not the same axis and must never be assumed to be. The timeline
// is media time in nanoseconds and every room shares it (MATP section 5); a
// stream frame is one sample period of *this* room's audio, at this room's own
// rate, after whatever resampling this room is getting (sections 28 and 33).
// A 20 ms span of timeline is 882 frames for a bitperfect 44.1 kHz room, 960
// for a 48 kHz one, and occasionally 961 for a 48 kHz one that is being
// drift-corrected. The protocol speaks the timeline only: every position in
// every message is nanoseconds, and the frame count is this client's business
// alone.
//
// That leaves the client needing to convert between them, which is what this
// class is. Each audio packet contributes one segment -- where it begins and
// how long it is on both axes -- and a position on either axis is found by
// locating its segment and interpolating inside it. Within a segment the ratio
// is constant by construction, because the ratio is the segment.
//
// Three properties it has to have, all of them from section 33:
//
//   * **Integer arithmetic, absolute rather than accumulated.** A position is
//     computed from its segment's own two endpoints, so nothing rounds twice
//     and nothing accumulates. Every multiply is overflow-checked and a
//     conversion that would not fit fails rather than wrapping: the numbers
//     here are attacker-reachable through the packet header, and a wrapped
//     frame position would place audio somewhere arbitrary.
//   * **Bounded storage and an honest failure.** The ring holds the last
//     kTimelineSegments packets and nothing older; a position that has fallen
//     off the old end cannot be converted and says so rather than being
//     guessed at. With section 15's recommended 10-20 ms packets that is 20-40
//     s of history, more than a typical buffer, but packets smaller than the
//     section asks for (or a buffer near its 11 s limit) reach it. AudioStream
//     therefore waits before an append would drop a segment whose audio is
//     still buffered. One allocation in the constructor, none afterwards.
//   * **Forward extrapolation, never backward invention.** A PAUSE_AT names a
//     position seconds ahead of the audio that has arrived, so both directions
//     extend the newest segment's ratio past the frontier. Over those seconds
//     a ratio that is wrong by parts per million is wrong by a fraction of a
//     frame. Behind the oldest segment nothing is extrapolated at all -- that
//     audio has been played and forgotten.
//
// Not synchronized. It is written by the audio connection thread as packets
// arrive and read by the owner thread; AudioStream owns one and covers it with
// the mutex it already holds for everything else the two threads share. The
// audio thread never touches it -- the render callback compares frame counts
// on one axis and converts nothing.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace multiroom {
namespace matp {

// How many packets of history the mapping keeps. 2048 of them is 20 s at
// section 15's 10 ms floor. A stream that sends smaller packets is paced by
// this instead of the ring buffer (AudioStream::awaitTimelineRoom).
const size_t kTimelineSegments = 2048;

// One packet's contribution: the same span of audio measured on both axes.
struct TimelineSegment
{
    uint64_t timelineStartNs = 0;    // where the packet begins on the timeline
    uint64_t timelineDurationNs = 0; // how much of it the packet covers
    uint64_t streamStartFrame = 0;   // cumulative stream frame it begins at
    uint32_t streamFrameCount = 0;   // frames in the payload
};

class TimelineMap
{
public:
    TimelineMap();

    // Drops everything. Used when a stream is attached and when the server
    // moves the room to another timeline position: segments of a timeline
    // nobody is playing any more would map positions that no longer exist.
    void clear();

    // Appends one packet. Callers keep both axes contiguous -- AudioStream
    // refuses a timeline gap and the ring buffer's push counter cannot skip --
    // which is what lets the lookups below binary search. A segment that
    // covers no time or no frames is dropped: there would be nothing to
    // interpolate inside it, and a zero denominator is not a ratio.
    void append(const TimelineSegment& segment);

    // Timeline -> this room's stream frames, and back. Both interpolate inside
    // the segment that holds the position and extrapolate forward past the
    // newest one at its ratio.
    //
    // Both return false for a position behind the oldest segment, which is the
    // one case no ratio can recover, and for an extrapolation so far out that
    // the arithmetic would not fit in 64 bits -- which is a position nothing
    // will ever play rather than a position to guess at.
    bool timelineToStream(uint64_t timelineNs, uint64_t& streamFrame) const;
    bool streamToTimeline(uint64_t streamFrame, uint64_t& timelineNs) const;

    bool empty() const { return count_ == 0; }
    size_t size() const { return count_; }
    // The next append overwrites the oldest segment.
    bool full() const { return count_ == segments_.size(); }

    // One past the oldest segment's last stream frame, meaningful while the
    // map is not empty. Audio before it may be forgotten; audio after it not.
    uint64_t oldestStreamEnd() const;

    // One past the newest segment on each axis, meaningful while the map is
    // not empty. The two are the frontier the extrapolation above starts at.
    uint64_t nextStreamFrame() const;
    uint64_t nextTimelineNs() const;

private:
    const TimelineSegment& at(size_t index) const;

    // A ring rather than a deque: one allocation in the constructor and none
    // afterwards, on the same thread that pushes audio into the ring buffer.
    std::vector<TimelineSegment> segments_;
    size_t head_ = 0; // index of the oldest segment
    size_t count_ = 0;
};

} // namespace matp
} // namespace multiroom
