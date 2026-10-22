// The second of the two mapping components: the zone's timeline against one
// room's own output frames.
//
// What is asserted here is the running target, because that is what the
// packetizer's frame count is a difference of. A per-packet ratio and a
// running target agree at 1.0 and disagree by a frame every twenty-third
// packet at +30 ppm, and the difference between them is whether a drift
// correction ever arrives.

#include "TestSupport.h"

#include "client/OutputClock.h"

#include <vector>

using namespace multiroom;

namespace {

constexpr uint64_t kPacketNs = 20000000ULL; // section 46's 20 ms

uint64_t frameAt(const OutputClock& clock, uint64_t ns)
{
    const Result<OutputFrame, OutputClockError> frame = clock.frameAt(TimelineNs{ns});
    CHECK_OK(frame);
    return std::to_underlying(*frame);
}

// The frame counts a 20 ms grid asks for, packet by packet, exactly as
// ClientOutput derives them.
std::vector<uint64_t> packetCounts(OutputClock& clock, uint64_t originNs, size_t packets)
{
    std::vector<uint64_t> counts;
    uint64_t sent = 0;
    for (size_t index = 1; index <= packets; ++index)
    {
        const uint64_t target = frameAt(clock, originNs + index * kPacketNs);
        counts.push_back(target - sent);
        sent = target;
    }
    return counts;
}

} // namespace

MULTIROOM_TEST(output_clock_asks_for_the_same_count_every_packet_at_every_rate)
{
    // A packet duration is a duration and never a frame count: one value
    // covers every room, and at an untrimmed ratio every one of them divides
    // exactly.
    const struct
    {
        uint32_t rate;
        uint64_t frames;
    } rooms[] = {{44100, 882}, {48000, 960}, {96000, 1920}, {192000, 3840}};

    for (const auto& room : rooms)
    {
        OutputClock clock(room.rate, TimelineNs{0});
        const std::vector<uint64_t> counts = packetCounts(clock, 0, 500);
        for (const uint64_t count : counts)
            CHECK_EQ(count, room.frames);
    }
}

MULTIROOM_TEST(output_clock_maps_absolutely_rather_than_by_accumulation)
{
    // Four hours at 44.1 kHz, read from the anchor rather than added up. An
    // implementation that accumulated per packet would be short by whatever
    // its rounding lost, seven hundred and twenty thousand times over.
    OutputClock clock(44100, TimelineNs{0});

    const uint64_t fourHoursNs = 4ULL * 3600ULL * kNanosecondsPerSecond;
    CHECK_EQ(frameAt(clock, fourHoursNs), 4ULL * 3600ULL * 44100ULL);

    // And the same answer whichever position is asked first.
    CHECK_EQ(frameAt(clock, kPacketNs), 882ULL);
    CHECK_EQ(frameAt(clock, fourHoursNs), 4ULL * 3600ULL * 44100ULL);
}

MULTIROOM_TEST(output_clock_counts_from_the_position_a_room_joined_at)
{
    // Section 44: a room that joins an hour into a zone's timeline begins at
    // its own frame zero, and its packets are numbered from there.
    const uint64_t joinNs = 3600ULL * kNanosecondsPerSecond;
    OutputClock clock(44100, TimelineNs{joinNs});

    CHECK_EQ(std::to_underlying(clock.origin()), joinNs);
    CHECK_EQ(frameAt(clock, joinNs), 0ULL);
    CHECK_EQ(frameAt(clock, joinNs + kPacketNs), 882ULL);

    // A position before that is not a smaller number; it is not a number.
    CHECK_ERR(clock.frameAt(TimelineNs{joinNs - 1}), OutputClockError::BeforeAnchor);
}

MULTIROOM_TEST(output_clock_puts_a_trimmed_rooms_extra_frame_where_a_resampler_would)
{
    // +30 ppm, section 28's order of magnitude. At 44.1 kHz that is 0.02646
    // extra frames per 20 ms packet, so the room needs one more frame roughly
    // every thirty-eighth packet -- and a per-packet ratio would round every
    // one of them away.
    OutputClock clock(44100, TimelineNs{0});
    CHECK_OK(clock.setTrim(1.000030, TimelineNs{0}));
    CHECK_EQ(clock.trim(), 1.000030);

    const std::vector<uint64_t> counts = packetCounts(clock, 0, 2000);

    size_t longPackets = 0;
    for (const uint64_t count : counts)
    {
        CHECK(count == 882 || count == 883);
        if (count == 883)
            ++longPackets;
    }

    // Forty seconds of audio at +30 ppm is 1,764,052 frames rather than
    // 1,764,000: fifty-two extra, each of them in a packet of its own.
    uint64_t total = 0;
    for (const uint64_t count : counts)
        total += count;
    CHECK_EQ(total, 1764052ULL);
    CHECK_EQ(longPackets, size_t(52));

    // And the room is exactly where the target says it is, not where the sum
    // of a rounded ratio would have left it.
    CHECK_EQ(frameAt(clock, 2000 * kPacketNs), total);
}

MULTIROOM_TEST(output_clock_re_anchors_rather_than_rewriting_what_was_already_sent)
{
    // A correction applies from the instant it is made. Applying it from the
    // origin instead would ask a room for a frame count it has already been
    // given a different answer to, which the client reads as a stream that
    // jumped.
    OutputClock clock(48000, TimelineNs{0});

    const uint64_t oneSecond = kNanosecondsPerSecond;
    CHECK_EQ(frameAt(clock, oneSecond), 48000ULL);

    CHECK_OK(clock.setTrim(1.0005, TimelineNs{oneSecond}));
    CHECK_EQ(std::to_underlying(clock.anchor()), oneSecond);

    // The first second is untouched, and the second one is 24 frames longer.
    CHECK_EQ(frameAt(clock, oneSecond), 48000ULL);
    CHECK_EQ(frameAt(clock, 2 * oneSecond), 48000ULL + 48024ULL);
}

MULTIROOM_TEST(output_clock_re_anchoring_twice_a_second_loses_nothing)
{
    // Phase 13's correction arrives with every playback report, which is twice
    // a second and therefore every twenty-fifth packet. Each one re-anchors,
    // and the running total at an anchor is not a whole number of frames -- so
    // an anchor that kept only the whole part would throw away half a frame
    // twice a second, which at 44.1 kHz is 23 ppm against a correction of 30.
    //
    // The same trim, installed once and installed eighty times, has to produce
    // the same stream.
    OutputClock once(44100, TimelineNs{0});
    CHECK_OK(once.setTrim(1.000030, TimelineNs{0}));

    // The same loop ClientOutput runs: forty seconds of packets, with the
    // correction re-installed at every twenty-fifth boundary.
    OutputClock often(44100, TimelineNs{0});
    uint64_t sent = 0;
    size_t longPackets = 0;
    for (uint64_t index = 1; index <= 2000; ++index)
    {
        if ((index - 1) % 25 == 0)
            CHECK_OK(often.setTrim(1.000030, TimelineNs{(index - 1) * kPacketNs}));

        const uint64_t target = frameAt(often, index * kPacketNs);
        const uint64_t count = target - sent;

        // Still a stream of whole packets, rather than one that stands still
        // and then catches up in a step at every anchor.
        CHECK(count == 882 || count == 883);
        if (count == 883)
            ++longPackets;
        sent = target;
    }

    CHECK_EQ(sent, frameAt(once, 2000 * kPacketNs));
    CHECK_EQ(sent, 1764052ULL);
    CHECK_EQ(longPackets, size_t(52));
}

MULTIROOM_TEST(output_clock_refuses_what_no_correction_could_be)
{
    OutputClock clock(48000, TimelineNs{0});

    CHECK_ERR(clock.setTrim(0.0, TimelineNs{0}), OutputClockError::InvalidTrim);
    CHECK_ERR(clock.setTrim(-1.0, TimelineNs{0}), OutputClockError::InvalidTrim);
    CHECK_ERR(clock.setTrim(3.0, TimelineNs{0}), OutputClockError::InvalidTrim);
    CHECK_EQ(clock.trim(), 1.0);

    // Backwards is refused for the same reason a position before the anchor
    // is: the frames between the two have already been asked for.
    CHECK_OK(clock.setTrim(1.0001, TimelineNs{kNanosecondsPerSecond}));
    CHECK_ERR(clock.setTrim(1.0002, TimelineNs{0}), OutputClockError::BeforeAnchor);

    OutputClock rateless(0, TimelineNs{0});
    CHECK_ERR(rateless.frameAt(TimelineNs{kPacketNs}), OutputClockError::InvalidSampleRate);
}
