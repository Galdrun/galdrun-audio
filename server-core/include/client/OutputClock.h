#pragma once

// The zone's timeline against one room's own output frames, and the second of
// the two places in this server where one axis of core/Units.h becomes
// another.
//
// timeline/SourceTimeline is the first and answers a question about the
// *source*: which nanosecond a decoded frame is heard at. This one answers a
// question about one *client*: how many frames of that room's own stream ought
// to have been produced by a given position on the timeline. The two are not
// the same mapping and they are not the same rate -- a 44.1 kHz source served
// to a 48 kHz room has one of each -- which is why they are two components and
// two types (MATP sections 33 and 34).
//
// **A running target, not a per-packet ratio.** The frame count of a packet is
//
//     count = frameAt(packetEndNs) - framesAlreadySent
//
// and never `duration * rate * ratio` worked out afresh. A room being
// corrected at +30 ppm needs one extra frame roughly every 23rd packet at
// 44.1 kHz; a per-packet ratio rounds that extra frame away fifty times a
// second and the correction never arrives. Deriving the count from a target
// puts it exactly where a resampler would have put it, and it is the whole
// reason section 14 carries a frame count *and* a timeline span rather than
// letting a receiver derive one from the other.
//
// **Absolute, never accumulated**, for the reason section 15 gives: every
// position is computed from an anchor rather than added to the last one, so a
// stream of any length carries no accumulated rounding at all.
//
// **A re-anchor carries the fraction of a frame the last one left over.** A
// correction arrives with every playback report, which is twice a second, and
// re-anchoring is what keeps the frames already asked for asked for. But the
// running total at that instant is not a whole number of frames -- at +30 ppm
// and 20 ms packets it gains one every thirty-eighth packet -- so an anchor
// that kept only the whole part would throw half a frame away twice a second,
// which at 44.1 kHz is a systematic -23 ppm against a correction of +30. The
// remainder is therefore kept beside the anchor, and a corrected stream counts
// the same frames whether its trim was installed once or a thousand times.
//
// **The trim is the drift correction, and it is not the resampling ratio.** A
// room resampled from 44.1 to 48 kHz has that conversion in its *rate*, and
// its trim is exactly 1. The trim is what section 28's correction moves, and
// while it is 1 the arithmetic here is integer and exact -- 882 frames per
// 20 ms at 44.1 kHz, every packet, with no rounding to accumulate.
//
// Threading: one clock belongs to one client output thread, like the processor
// beside it, and nothing here is synchronized.

#include "core/Error.h"
#include "core/Units.h"

#include <cstdint>

namespace multiroom {

enum class OutputClockError
{
    InvalidSampleRate, // zero; a format that was never validated

    // Outside 0.5 .. 2.0, which no drift correction reaches: section 46's
    // limits are ±100 ppm normally and ±500 ppm while converging. A value
    // this far out is a controller fault or an inverted convention, and
    // resampling a room by it would be audible rather than subtle.
    InvalidTrim,

    // A position before the clock's anchor. The mapping is forward only: a
    // room's stream begins where it begins, and a frame count for a position
    // before that is not a smaller number, it is not a number.
    BeforeAnchor,

    Overflow // beyond what 64 bits can express on the other axis
};

const char* errorName(OutputClockError error);

class OutputClock
{
public:
    // `origin` is the timeline position this room's output frame 0 sits at --
    // where the stream begins, which for a late join (section 44) is a
    // position well past the zone's own zero.
    OutputClock(uint32_t outputRate, TimelineNs origin) noexcept;

    // How many frames of this room's stream the timeline has asked for by
    // `position`, counted from the origin. The frame count of a packet is the
    // difference of two of these and never anything else.
    [[nodiscard]] Result<OutputFrame, OutputClockError> frameAt(TimelineNs position) const;

    // Section 28's correction, applied from `at` onwards. The clock re-anchors
    // rather than re-scaling from the origin: the frames already sent were
    // sent, and a new ratio applied retroactively would ask the room for a
    // frame count it has already been given a different answer to.
    //
    // `at` must be at or after the current anchor, for the same reason.
    Status<OutputClockError> setTrim(double trim, TimelineNs at);

    [[nodiscard]] double trim() const noexcept { return trim_; }
    [[nodiscard]] TimelineNs origin() const noexcept { return origin_; }
    [[nodiscard]] TimelineNs anchor() const noexcept { return anchorNs_; }
    [[nodiscard]] uint32_t sampleRate() const noexcept { return sampleRate_; }

private:
    uint32_t sampleRate_ = 0;
    TimelineNs origin_{};

    // Where the current trim took effect, how many whole frames had been asked
    // for by then, and how much of the next one had. For a stream that has
    // never been corrected these are the origin, zero and zero.
    TimelineNs anchorNs_{};
    OutputFrame anchorFrame_{};
    double anchorResidual_ = 0.0; // always in [0, 1)
    double trim_ = 1.0;
};

} // namespace multiroom
