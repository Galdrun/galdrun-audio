#include "TestSupport.h"

#include "matp/TimelineMap.h"

#include <cstdint>

using namespace multiroom;

namespace {

matp::TimelineSegment segment(uint64_t streamStartFrame, uint64_t timelineStartNs,
                              uint32_t streamFrameCount, uint64_t timelineDurationNs)
{
    matp::TimelineSegment result;
    result.streamStartFrame = streamStartFrame;
    result.timelineStartNs = timelineStartNs;
    result.streamFrameCount = streamFrameCount;
    result.timelineDurationNs = timelineDurationNs;
    return result;
}

// Where frame `n` of a stream at `rate` falls, measured absolutely from the
// stream's own start. This is the arithmetic MATP section 15 requires of a
// sender: every packet boundary comes from one division of an absolute frame
// position, so nothing accumulates.
uint64_t absoluteNs(uint64_t frames, uint32_t rate)
{
    return (frames * 1000000000ULL) / rate;
}

// A stream of `count` packets of `framesPerPacket` frames at `rate`, placed on
// the timeline from `baseNs` the way a server that does not accumulate
// rounding error places them.
void fillAtRate(matp::TimelineMap& map, uint64_t baseNs, uint32_t rate, uint32_t count,
                uint32_t framesPerPacket)
{
    for (uint32_t i = 0; i < count; ++i)
    {
        const uint64_t firstFrame = static_cast<uint64_t>(i) * framesPerPacket;
        const uint64_t startNs = baseNs + absoluteNs(firstFrame, rate);
        const uint64_t endNs = baseNs + absoluteNs(firstFrame + framesPerPacket, rate);
        map.append(segment(firstFrame, startNs, framesPerPacket, endNs - startNs));
    }
}

// `count` packets each covering `durationNs` of timeline and arriving as
// `streamFrames` frames -- the shape of a stream whose ratio is constant.
void fillAtRatio(matp::TimelineMap& map, uint64_t baseNs, uint32_t count,
                 uint64_t durationNs, uint32_t streamFrames)
{
    for (uint32_t i = 0; i < count; ++i)
        map.append(segment(static_cast<uint64_t>(i) * streamFrames, baseNs + i * durationNs,
                           streamFrames, durationNs));
}

} // namespace

MULTIROOM_TEST(timeline_map_is_empty_until_a_packet_has_arrived)
{
    matp::TimelineMap map;
    CHECK(map.empty());

    uint64_t out = 0;
    CHECK(!map.timelineToStream(0, out));
    CHECK(!map.streamToTimeline(0, out));
}

// 48 kHz is the only rate where 20 ms is a whole number of nanoseconds and a
// whole number of frames at once, which makes it the case that would pass even
// with sloppy arithmetic. It is here as the baseline for the two that follow.
MULTIROOM_TEST(timeline_map_maps_a_48_khz_stream)
{
    matp::TimelineMap map;
    fillAtRate(map, 180000000000ULL, 48000, 4, 960);

    uint64_t stream = 0;
    CHECK(map.timelineToStream(180000000000ULL, stream));
    CHECK_EQ(stream, static_cast<uint64_t>(0));
    CHECK(map.timelineToStream(180020000000ULL, stream));
    CHECK_EQ(stream, static_cast<uint64_t>(960));
    CHECK(map.timelineToStream(180060000000ULL, stream));
    CHECK_EQ(stream, static_cast<uint64_t>(2880));

    uint64_t ns = 0;
    CHECK(map.streamToTimeline(2880, ns));
    CHECK_EQ(ns, 180060000000ULL);

    CHECK_EQ(map.nextStreamFrame(), static_cast<uint64_t>(3840));
    CHECK_EQ(map.nextTimelineNs(), 180080000000ULL);
}

// 44.1 kHz is the rate a bitperfect client exists for, and the one no whole
// number of nanoseconds lines up with: 480 frames is 10,884,353.7 ns. Every
// packet boundary must still convert to the exact frame.
MULTIROOM_TEST(timeline_map_maps_a_bitperfect_44_1_khz_stream)
{
    matp::TimelineMap map;
    const uint64_t base = 1000000000ULL;
    fillAtRate(map, base, 44100, 8, 480);

    for (uint32_t packet = 0; packet <= 8; ++packet)
    {
        const uint64_t frames = static_cast<uint64_t>(packet) * 480;
        uint64_t stream = 0;
        CHECK(map.timelineToStream(base + absoluteNs(frames, 44100), stream));
        CHECK_EQ(stream, frames);
    }

    // Inside a packet, at that packet's own ratio: half of 10,884,353 ns is
    // 240 frames of 44.1 kHz audio.
    uint64_t stream = 0;
    CHECK(map.timelineToStream(base + absoluteNs(480, 44100) + 5442176, stream));
    CHECK_EQ(stream, static_cast<uint64_t>(719)); // 239.99 frames in, floored

    uint64_t ns = 0;
    CHECK(map.streamToTimeline(1440, ns));
    CHECK_EQ(ns, base + absoluteNs(1440, 44100));
}

MULTIROOM_TEST(timeline_map_maps_a_bitperfect_96_khz_stream)
{
    matp::TimelineMap map;
    fillAtRate(map, 0, 96000, 4, 1920);

    uint64_t stream = 0;
    CHECK(map.timelineToStream(20000000, stream));
    CHECK_EQ(stream, static_cast<uint64_t>(1920));
    CHECK(map.timelineToStream(60000000, stream));
    CHECK_EQ(stream, static_cast<uint64_t>(5760));

    // The same span of timeline as the 48 kHz test above, twice the frames --
    // which is the whole reason the timeline is not a frame counter.
    uint64_t ns = 0;
    CHECK(map.streamToTimeline(5760, ns));
    CHECK_EQ(ns, static_cast<uint64_t>(60000000));
}

// The point of the class: two rooms holding the same span of timeline with
// different numbers of frames in it. A deliberately extreme 11:10 ratio rather
// than a realistic handful of ppm -- at 20 ppm an arithmetic slip needs 50,000
// frames to show itself, and a test that takes that long to fail is a test
// that passes by accident.
MULTIROOM_TEST(timeline_map_converts_a_resampled_stream_in_both_directions)
{
    matp::TimelineMap map;
    fillAtRatio(map, 1000, 4, 100, 110); // 110 frames per 100 ns

    uint64_t stream = 0;
    CHECK(map.timelineToStream(1000, stream));
    CHECK_EQ(stream, static_cast<uint64_t>(0));
    CHECK(map.timelineToStream(1100, stream));
    CHECK_EQ(stream, static_cast<uint64_t>(110));
    CHECK(map.timelineToStream(1300, stream));
    CHECK_EQ(stream, static_cast<uint64_t>(330));

    // Inside a segment, at that segment's ratio.
    CHECK(map.timelineToStream(1050, stream));
    CHECK_EQ(stream, static_cast<uint64_t>(55));

    uint64_t ns = 0;
    CHECK(map.streamToTimeline(330, ns));
    CHECK_EQ(ns, static_cast<uint64_t>(1300));
    CHECK(map.streamToTimeline(55, ns));
    CHECK_EQ(ns, static_cast<uint64_t>(1050));
}

// A ratio that changes between packets is the normal case -- the server's
// controller moves it as the room's drift estimate settles -- and each segment
// has to keep its own.
MULTIROOM_TEST(timeline_map_keeps_a_ratio_per_segment)
{
    matp::TimelineMap map;
    map.append(segment(0, 500, 100, 100));   // 1 frame per ns
    map.append(segment(100, 600, 120, 100)); // then 1.2

    uint64_t stream = 0;
    CHECK(map.timelineToStream(550, stream));
    CHECK_EQ(stream, static_cast<uint64_t>(50));
    CHECK(map.timelineToStream(650, stream));
    CHECK_EQ(stream, static_cast<uint64_t>(160));
}

// What drift correction actually looks like on the wire: the same 20 ms of
// timeline, 960 frames in most packets and 961 in the occasional one. The
// timeline must stay continuous across the change.
MULTIROOM_TEST(timeline_map_survives_a_drift_corrected_frame_count)
{
    matp::TimelineMap map;
    uint64_t frames = 0;
    for (uint32_t i = 0; i < 100; ++i)
    {
        const uint32_t count = (i % 20 == 0) ? 961u : 960u;
        map.append(segment(frames, i * 20000000ULL, count, 20000000ULL));
        frames += count;
    }

    // 100 packets of timeline is 2 s whatever the frame counts did.
    CHECK_EQ(map.nextTimelineNs(), 2000000000ULL);
    CHECK_EQ(map.nextStreamFrame(), frames);
    CHECK_EQ(frames, static_cast<uint64_t>(96005)); // five extra frames

    // Every packet boundary still converts exactly, in both directions.
    uint64_t running = 0;
    for (uint32_t i = 0; i < 100; ++i)
    {
        uint64_t stream = 0;
        CHECK(map.timelineToStream(i * 20000000ULL, stream));
        CHECK_EQ(stream, running);

        uint64_t ns = 0;
        CHECK(map.streamToTimeline(running, ns));
        CHECK_EQ(ns, i * 20000000ULL);

        running += (i % 20 == 0) ? 961u : 960u;
    }
}

// The failure the absolute arithmetic of section 15 exists to prevent: a long
// run at a rate that does not divide a nanosecond, converted a packet at a
// time. Every boundary the map still holds has to convert to the frame it was
// built from, however far into the stream it is.
MULTIROOM_TEST(timeline_map_does_not_accumulate_rounding_over_a_long_run)
{
    matp::TimelineMap map;
    const uint32_t packets = 50000; // 480 frames each: about nine minutes
    fillAtRate(map, 0, 44100, packets, 480);

    // Only the last kTimelineSegments packets are still held, which is the
    // bound working rather than a limitation of the arithmetic.
    CHECK_EQ(map.size(), matp::kTimelineSegments);

    const uint64_t totalFrames = static_cast<uint64_t>(packets) * 480;
    CHECK_EQ(map.nextStreamFrame(), totalFrames);
    CHECK_EQ(map.nextTimelineNs(), absoluteNs(totalFrames, 44100));

    for (uint32_t back = 0; back < matp::kTimelineSegments; back += 97)
    {
        const uint64_t frames = totalFrames - static_cast<uint64_t>(back) * 480;
        uint64_t stream = 0;
        CHECK(map.timelineToStream(absoluteNs(frames, 44100), stream));
        CHECK_EQ(stream, frames);
    }

    // And the other direction, which is what a PLAYBACK_REPORT goes through.
    uint64_t ns = 0;
    CHECK(map.streamToTimeline(totalFrames - 480, ns));
    CHECK_EQ(ns, absoluteNs(totalFrames - 480, 44100));
}

// A PAUSE_AT names a position seconds before the audio for it has arrived, so
// a position past the newest segment is answered rather than refused.
MULTIROOM_TEST(timeline_map_extrapolates_past_the_newest_segment)
{
    matp::TimelineMap map;
    fillAtRatio(map, 0, 2, 100, 110);

    uint64_t stream = 0;
    CHECK(map.timelineToStream(300, stream)); // 100 ns past the frontier
    CHECK_EQ(stream, static_cast<uint64_t>(330));

    // The other direction, which is where a room whose buffer ran dry reports
    // an audible position ahead of everything it was given.
    uint64_t ns = 0;
    CHECK(map.streamToTimeline(330, ns));
    CHECK_EQ(ns, static_cast<uint64_t>(300));
}

// The one case no ratio recovers: that audio was played and forgotten.
MULTIROOM_TEST(timeline_map_refuses_a_position_behind_what_it_still_holds)
{
    matp::TimelineMap map;
    fillAtRatio(map, 1000, 2, 100, 100);

    uint64_t out = 0;
    CHECK(!map.timelineToStream(999, out));
    CHECK(map.timelineToStream(1000, out));

    // The same on the stream axis, where the oldest segment does not begin at
    // frame zero.
    matp::TimelineMap moved;
    moved.append(segment(5000, 1000, 100, 100));
    CHECK(!moved.streamToTimeline(4999, out));
    CHECK(moved.streamToTimeline(5000, out));
}

MULTIROOM_TEST(timeline_map_drops_the_oldest_segments_when_it_is_full)
{
    matp::TimelineMap map;
    fillAtRatio(map, 0, static_cast<uint32_t>(matp::kTimelineSegments) + 10, 100, 100);

    CHECK_EQ(map.size(), matp::kTimelineSegments);

    // The first ten packets have been overwritten, so their positions are gone
    // and everything from the eleventh on is still answerable.
    uint64_t out = 0;
    CHECK(!map.timelineToStream(999, out));
    CHECK(map.timelineToStream(1000, out));
    CHECK_EQ(out, static_cast<uint64_t>(1000));
}

MULTIROOM_TEST(timeline_map_clear_forgets_a_timeline_nobody_is_playing)
{
    matp::TimelineMap map;
    fillAtRatio(map, 1000, 4, 100, 100);
    map.clear();

    CHECK(map.empty());
    uint64_t out = 0;
    CHECK(!map.timelineToStream(1000, out));
}

// A segment with a zero on either axis has no ratio, so there is nothing to
// interpolate inside it. AudioStream cannot produce one -- the packet parser
// refuses both -- and the map drops it rather than dividing by it.
MULTIROOM_TEST(timeline_map_ignores_a_segment_with_no_span)
{
    matp::TimelineMap map;
    map.append(segment(0, 1000, 0, 100));
    CHECK(map.empty());
    map.append(segment(0, 1000, 100, 0));
    CHECK(map.empty());
}

// Extrapolating a long way must stay exact rather than overflowing: a stop
// scheduled a minute out is a distance of millions of frames multiplied by a
// ratio.
MULTIROOM_TEST(timeline_map_extrapolates_a_long_way_without_losing_precision)
{
    matp::TimelineMap map;
    map.append(segment(0, 0, 960, 20000000));

    uint64_t stream = 0;
    CHECK(map.timelineToStream(60ULL * 1000000000ULL, stream));
    CHECK_EQ(stream, static_cast<uint64_t>(48000ULL * 60ULL));

    matp::TimelineMap bitperfect;
    bitperfect.append(segment(0, 0, 480, absoluteNs(480, 44100)));
    CHECK(bitperfect.timelineToStream(60ULL * 1000000000ULL, stream));
    // A minute of 44.1 kHz audio, to within the one frame the segment's own
    // 10,884,353 ns of resolution can place it.
    CHECK(stream >= 2645999ULL && stream <= 2646000ULL);
}

// The numbers reach this class straight out of a packet header, so a ratio no
// sender would produce must fail rather than wrap: a wrapped frame position
// would place audio at an arbitrary point in the buffer.
MULTIROOM_TEST(timeline_map_refuses_a_conversion_that_would_overflow)
{
    matp::TimelineMap map;
    // One nanosecond carrying four billion frames: an absurd ratio, and the
    // steepest one the field widths allow.
    map.append(segment(0, 0, 4000000000u, 1));

    uint64_t stream = 0;
    CHECK(!map.timelineToStream(1ULL << 40, stream));

    // Just inside the range still works, so the guard is a limit and not a
    // refusal to extrapolate at all.
    CHECK(map.timelineToStream(1000, stream));
    CHECK_EQ(stream, 4000000000000ULL);

    // The mirror image: one frame carrying a huge span of timeline.
    matp::TimelineMap flat;
    flat.append(segment(0, 0, 1, 1000000000));
    uint64_t ns = 0;
    CHECK(!flat.streamToTimeline(1ULL << 40, ns));
    CHECK(flat.streamToTimeline(100, ns));
    CHECK_EQ(ns, 100000000000ULL);
}

// The extrapolation base itself is near the end of the range, which is the
// case where adding an in-range offset to an in-range base overflows.
MULTIROOM_TEST(timeline_map_refuses_an_extrapolation_past_the_end_of_the_range)
{
    matp::TimelineMap map;
    map.append(segment(UINT64_MAX - 1000, 0, 960, 20000000));

    uint64_t stream = 0;
    CHECK(!map.timelineToStream(60ULL * 1000000000ULL, stream));
    CHECK(map.timelineToStream(0, stream));
    CHECK_EQ(stream, UINT64_MAX - 1000);
}
