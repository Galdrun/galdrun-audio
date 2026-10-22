#include "TestSupport.h"

#include "timeline/SourceTimeline.h"

#include <utility>

using namespace multiroom;

namespace {

constexpr uint32_t kCd = 44100;
constexpr uint64_t kTrackFrames = 44100ULL * 180; // three minutes at 44.1 kHz
constexpr uint64_t kTrackNs = 180ULL * kNanosecondsPerSecond;

uint64_t ns(const Result<TimelineNs, TimelineError>& position)
{
    CHECK_OK(position);
    return std::to_underlying(*position);
}

uint64_t frame(const Result<SourceFrame, TimelineError>& position)
{
    CHECK_OK(position);
    return std::to_underlying(*position);
}

} // namespace

MULTIROOM_TEST(source_timeline_maps_one_source_absolutely)
{
    SourceTimeline timeline;
    CHECK_EQ(timeline.segmentCount(), size_t{0});

    // Nothing staged: a position is unanswerable rather than zero.
    CHECK_ERR(timeline.timelineAt(SourceFrame{0}), TimelineError::Empty);
    CHECK_ERR(timeline.frameAt(TimelineNs{0}), TimelineError::Empty);

    // A zone's timeline starts at zero when the zone starts playing.
    auto base = timeline.stage(SourceFrame{0}, kCd);
    CHECK_OK(base);
    CHECK_EQ(std::to_underlying(*base), uint64_t{0});
    CHECK_EQ(timeline.segmentCount(), size_t{1});

    CHECK_EQ(ns(timeline.timelineAt(SourceFrame{0})), uint64_t{0});
    CHECK_EQ(ns(timeline.timelineAt(SourceFrame{44100})), kNanosecondsPerSecond);
    CHECK_EQ(ns(timeline.timelineAt(SourceFrame{882})), uint64_t{20000000}); // one packet

    // Four hours in, and still exact: every position is computed from the
    // segment's base rather than accumulated packet by packet, which is the
    // one implementation MATP section 15 names as wrong.
    CHECK_EQ(ns(timeline.timelineAt(SourceFrame{44100ULL * 3600 * 4})),
             4ULL * 3600 * kNanosecondsPerSecond);

    CHECK_EQ(frame(timeline.frameAt(TimelineNs{0})), uint64_t{0});
    CHECK_EQ(frame(timeline.frameAt(TimelineNs{kNanosecondsPerSecond})), uint64_t{44100});
}

MULTIROOM_TEST(source_timeline_round_trips_every_frame_at_44_1_khz)
{
    SourceTimeline timeline;
    CHECK_OK(timeline.stage(SourceFrame{0}, kCd));

    // 22675.7... ns to a frame, so no rounding is lossless both ways. The
    // pairing in core/Units.h is exact in this direction, and the segment's
    // base must not spoil it.
    for (uint64_t f = 0; f < 5000; ++f)
    {
        const uint64_t position = ns(timeline.timelineAt(SourceFrame{f}));
        CHECK_EQ(frame(timeline.frameAt(TimelineNs{position})), f);
    }
}

MULTIROOM_TEST(source_timeline_carries_the_base_across_a_track_boundary)
{
    SourceTimeline timeline;
    CHECK_OK(timeline.stage(SourceFrame{0}, kCd));

    // The second track begins where the first one's audio ended, in the
    // store's own frame numbering. The timeline is not reset and the audio is
    // not re-anchored: section 22's gapless playback is this arithmetic.
    auto boundary = timeline.stage(SourceFrame{kTrackFrames}, kCd);
    CHECK_OK(boundary);
    CHECK_EQ(std::to_underlying(*boundary), kTrackNs);
    CHECK_EQ(timeline.segmentCount(), size_t{2});

    // The last frame of track A and the first of track B are consecutive on
    // the timeline as well as in the store -- the boundary is a position, not
    // a discontinuity.
    const uint64_t lastOfA = ns(timeline.timelineAt(SourceFrame{kTrackFrames - 1}));
    CHECK_EQ(ns(timeline.timelineAt(SourceFrame{kTrackFrames})), kTrackNs);
    CHECK_EQ(kTrackNs - lastOfA, uint64_t{22675}); // under one 44.1 kHz frame
    CHECK_EQ(ns(timeline.timelineAt(SourceFrame{kTrackFrames + 44100})),
             kTrackNs + kNanosecondsPerSecond);

    CHECK_EQ(frame(timeline.frameAt(TimelineNs{kTrackNs})), kTrackFrames);
    CHECK_EQ(frame(timeline.frameAt(TimelineNs{kTrackNs - 1})), kTrackFrames - 1);

    // A packet run across the boundary is contiguous by construction: every
    // packet's end is the next packet's start, on both sides of it.
    const uint64_t firstPacket = (kTrackFrames / 882) - 3;
    uint64_t previousEnd = ns(timeline.timelineAt(SourceFrame{firstPacket * 882}));
    for (uint64_t packet = firstPacket; packet < firstPacket + 6; ++packet)
    {
        const uint64_t start = ns(timeline.timelineAt(SourceFrame{packet * 882}));
        const uint64_t end = ns(timeline.timelineAt(SourceFrame{(packet + 1) * 882}));
        CHECK_EQ(start, previousEnd);
        CHECK(end > start);
        previousEnd = end;
    }
}

MULTIROOM_TEST(source_timeline_maps_a_source_at_another_rate)
{
    SourceTimeline timeline;
    CHECK_OK(timeline.stage(SourceFrame{0}, kCd));

    // 44.1 kHz then 96 kHz: the timeline is the one axis both are named on,
    // and neither rate appears in a position.
    auto base = timeline.stage(SourceFrame{kTrackFrames}, 96000);
    CHECK_OK(base);
    CHECK_EQ(std::to_underlying(*base), kTrackNs);

    CHECK_EQ(ns(timeline.timelineAt(SourceFrame{kTrackFrames + 96000})),
             kTrackNs + kNanosecondsPerSecond);
    CHECK_EQ(frame(timeline.frameAt(TimelineNs{kTrackNs + kNanosecondsPerSecond})),
             kTrackFrames + 96000);

    // Forward extrapolation past the decoded audio is deliberate: a PAUSE_AT
    // names a position seconds ahead of the frontier (section 33).
    CHECK_EQ(ns(timeline.timelineAt(SourceFrame{kTrackFrames + 96000ULL * 3600})),
             kTrackNs + 3600ULL * kNanosecondsPerSecond);

    auto segment = timeline.segment(1);
    CHECK(segment.has_value());
    CHECK_EQ(segment->sampleRate, 96000u);
    CHECK_EQ(std::to_underlying(segment->firstFrame), kTrackFrames);
    CHECK_EQ(std::to_underlying(segment->baseNs), kTrackNs);
    CHECK(!timeline.segment(2).has_value());
}

MULTIROOM_TEST(source_timeline_refuses_a_source_that_does_not_follow)
{
    SourceTimeline timeline;
    CHECK_ERR(timeline.stage(SourceFrame{0}, 0), TimelineError::InvalidSampleRate);
    CHECK_EQ(timeline.segmentCount(), size_t{0});

    CHECK_OK(timeline.stage(SourceFrame{1000}, kCd));

    CHECK_ERR(timeline.stage(SourceFrame{999}, kCd), TimelineError::OutOfOrder);
    CHECK_EQ(timeline.segmentCount(), size_t{1});

    // The first staged source is heard at zero wherever its frames begin.
    CHECK_EQ(ns(timeline.timelineAt(SourceFrame{1000})), uint64_t{0});
    CHECK_ERR(timeline.timelineAt(SourceFrame{999}), TimelineError::NotCovered);
}

MULTIROOM_TEST(source_timeline_bounds_its_history_and_says_what_it_lost)
{
    SourceTimeline timeline(4);

    for (uint64_t track = 0; track < 10; ++track)
        CHECK_OK(timeline.stage(SourceFrame{track * kTrackFrames}, kCd));

    // A server that is never restarted stages thousands of sources, so the
    // table is bounded like every other buffer here.
    CHECK_EQ(timeline.segmentCount(), size_t{4});
    CHECK_EQ(timeline.droppedSegments(), uint64_t{6});

    auto oldest = timeline.segment(0);
    CHECK(oldest.has_value());
    CHECK_EQ(std::to_underlying(oldest->firstFrame), 6 * kTrackFrames);
    CHECK_EQ(std::to_underlying(oldest->baseNs), 6 * kTrackNs);

    // What is still held maps exactly; what was dropped is reported as lost
    // rather than guessed, which is section 33's rule for the client's copy of
    // the same problem.
    CHECK_EQ(ns(timeline.timelineAt(SourceFrame{6 * kTrackFrames})), 6 * kTrackNs);
    CHECK_EQ(ns(timeline.timelineAt(SourceFrame{9 * kTrackFrames + 44100})),
             9 * kTrackNs + kNanosecondsPerSecond);
    CHECK_ERR(timeline.timelineAt(SourceFrame{6 * kTrackFrames - 1}), TimelineError::NotCovered);
    CHECK_ERR(timeline.frameAt(TimelineNs{6 * kTrackNs - 1}), TimelineError::NotCovered);
}

MULTIROOM_TEST(source_timeline_stages_a_seek_inside_the_previous_source_and_never_before_it)
{
    // Phase 26. Ten seconds of one source decoded, and the same source staged
    // again from store frame 441000 to be heard from 2 s: from there on the
    // positions are the new frames', and before it still the old ones'.
    SourceTimeline timeline;
    CHECK_OK(timeline.stage(SourceFrame{0}, kCd));
    CHECK_EQ(ns(timeline.stageAt(SourceFrame{441000}, kCd, TimelineNs{2 * kNanosecondsPerSecond})),
             2 * kNanosecondsPerSecond);

    CHECK_EQ(frame(timeline.frameAt(TimelineNs{2 * kNanosecondsPerSecond})), uint64_t{441000});
    CHECK_EQ(frame(timeline.frameAt(TimelineNs{3 * kNanosecondsPerSecond})), uint64_t{485100});
    CHECK_EQ(frame(timeline.frameAt(TimelineNs{kNanosecondsPerSecond})), uint64_t{44100});
    CHECK_EQ(ns(timeline.timelineAt(SourceFrame{441000})), 2 * kNanosecondsPerSecond);

    // The next source follows the seeked one, not the audio it cut short.
    CHECK_EQ(ns(timeline.stage(SourceFrame{441000 + 44100}, kCd)), 3 * kNanosecondsPerSecond);

    // The timeline only moves forward, and the frames still follow on.
    CHECK_ERR(timeline.stageAt(SourceFrame{600000}, kCd, TimelineNs{kNanosecondsPerSecond}),
              TimelineError::OutOfOrder);
    CHECK_ERR(timeline.stageAt(SourceFrame{485099}, kCd, TimelineNs{4 * kNanosecondsPerSecond}),
              TimelineError::OutOfOrder);
    CHECK_ERR(timeline.stageAt(SourceFrame{600000}, 0, TimelineNs{4 * kNanosecondsPerSecond}),
              TimelineError::InvalidSampleRate);
    CHECK_EQ(timeline.segmentCount(), size_t{3});
}

MULTIROOM_TEST(source_timeline_replaces_a_source_that_decoded_nothing)
{
    // A source stopped or skipped before it decoded a frame leaves the store's
    // next frame where it began; the next source takes its place instead of
    // failing the zone ("frame N does not follow frame N").
    SourceTimeline timeline;
    CHECK_OK(timeline.stage(SourceFrame{0}, kCd));
    CHECK_EQ(ns(timeline.stage(SourceFrame{kTrackFrames}, kCd)), kTrackNs);
    CHECK_EQ(ns(timeline.stage(SourceFrame{kTrackFrames}, 48000)), kTrackNs);
    CHECK_EQ(timeline.segmentCount(), size_t{2});
    CHECK_EQ(timeline.droppedSegments(), uint64_t{0});

    // The replacement's own rate maps what follows.
    CHECK_EQ(ns(timeline.timelineAt(SourceFrame{kTrackFrames + 48000})),
             kTrackNs + kNanosecondsPerSecond);

    // The same for a seek, which still may not move the timeline back.
    CHECK_ERR(timeline.stageAt(SourceFrame{kTrackFrames}, kCd, TimelineNs{kTrackNs - 1}),
              TimelineError::OutOfOrder);
    CHECK_EQ(ns(timeline.stageAt(SourceFrame{kTrackFrames}, kCd,
                                 TimelineNs{kTrackNs + kNanosecondsPerSecond})),
             kTrackNs + kNanosecondsPerSecond);
    CHECK_EQ(timeline.segmentCount(), size_t{2});
    CHECK_EQ(frame(timeline.frameAt(TimelineNs{kTrackNs + kNanosecondsPerSecond})),
             kTrackFrames);
}
