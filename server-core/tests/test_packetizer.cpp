// The 72-byte header of section 14, checked field by field against the
// *client's* own AudioPacketReader.
//
// That is what makes this a loopback test rather than a server agreeing with
// itself: the reader here is the one compiled into the real binary, so a magic
// number, a byte order, a CRC or a field offset that is wrong has to be wrong
// in the one place both halves share.

#include "TestSupport.h"

#include "client/OutputClock.h"
#include "client/OutputPacketizer.h"
#include "matp/AudioPacket.h"
#include "matp/Protocol.h"

#include <string>
#include <vector>

using namespace multiroom;

namespace {

constexpr uint64_t kPacketNs = 20000000ULL;

SessionId testSessionId()
{
    return SessionId(0x0123456789abcdefULL, 0xfedcba9876543210ULL);
}

AudioFormat cdFormat()
{
    AudioFormat format;
    format.sampleRate = 44100;
    format.channels = 2;
    format.sampleType = AudioFormat::SampleType::Int16;
    return format;
}

// A payload whose every byte is decided here, so a header that describes it
// wrongly is caught by the contents and not only by the length.
std::vector<std::byte> ramp(uint32_t frames, const AudioFormat& format)
{
    std::vector<std::byte> bytes(static_cast<size_t>(frames) * format.bytesPerFrame());
    for (size_t i = 0; i < bytes.size(); ++i)
        bytes[i] = static_cast<std::byte>(i & 0xFF);
    return bytes;
}

// One packet, read back with the client's parser. Fails the test rather than
// returning a half-decoded one.
matp::AudioPacket parseOne(matp::AudioPacketReader& reader, const std::string& packet)
{
    reader.append(packet.data(), packet.size());

    matp::AudioPacket decoded;
    std::string error;
    const matp::AudioPacketReader::Result result = reader.next(decoded, error);
    if (result != matp::AudioPacketReader::Result::Packet)
        multiroomtest::failAt("the client refused this server's packet: " + error, __FILE__, __LINE__);
    return decoded;
}

} // namespace

MULTIROOM_TEST(packetizer_writes_a_header_the_client_reads_field_by_field)
{
    OutputPacketizer packetizer(testSessionId(), cdFormat());
    const std::vector<std::byte> payload = ramp(882, cdFormat());

    const Result<std::string, PacketizerError> packet =
        packetizer.encode(TimelineNs{12000000000ULL}, kPacketNs, 882, payload);
    CHECK_OK(packet);

    // Section 14's version 2 header is 72 bytes, and the magic is 'MATP'.
    CHECK_EQ(packet->size(), matp::kAudioHeaderBytes + payload.size());

    matp::AudioPacketReader reader;
    const matp::AudioPacket decoded = parseOne(reader, *packet);

    CHECK_EQ(decoded.header.protocolVersion, matp::kProtocolVersion);
    CHECK_EQ(decoded.header.headerSize, uint16_t(matp::kAudioHeaderBytes));
    CHECK_EQ(decoded.header.sessionIdHigh, testSessionId().high());
    CHECK_EQ(decoded.header.sessionIdLow, testSessionId().low());
    CHECK_EQ(decoded.header.sequenceNumber, 0ULL);
    CHECK_EQ(decoded.header.timelineStartNs, 12000000000ULL);
    CHECK_EQ(decoded.header.timelineDurationNs, kPacketNs);
    CHECK(decoded.header.format == cdFormat());
    CHECK_EQ(decoded.header.outputFrameCount, 882u);
    CHECK_EQ(decoded.header.payloadBytes, uint32_t(payload.size()));
    CHECK_EQ(decoded.header.flags, 0u);

    // Section 42 makes the CRC optional and zero means "not supplied". It is
    // computed here, because through a bring-up it is the cheapest detector
    // there is of a framing bug on this side.
    CHECK(decoded.header.headerCrc != 0);

    // And the payload arrived as it was written.
    CHECK_EQ(decoded.payloadBytes, payload.size());
    for (size_t i = 0; i < payload.size(); ++i)
        CHECK_EQ(int(decoded.payload[i]), int(std::to_integer<unsigned char>(payload[i])));
}

MULTIROOM_TEST(packetizer_numbers_a_stream_from_zero_and_never_restarts)
{
    OutputPacketizer packetizer(testSessionId(), cdFormat());
    const std::vector<std::byte> payload = ramp(882, cdFormat());

    matp::AudioPacketReader reader;
    for (uint64_t index = 0; index < 100; ++index)
    {
        CHECK_EQ(packetizer.nextSequence(), index);
        const Result<std::string, PacketizerError> packet =
            packetizer.encode(TimelineNs{index * kPacketNs}, kPacketNs, 882, payload);
        CHECK_OK(packet);
        CHECK_EQ(parseOne(reader, *packet).header.sequenceNumber, index);
    }

    CHECK_EQ(packetizer.packets(), 100ULL);
    CHECK_EQ(packetizer.frames(), 88200ULL);
    CHECK_EQ(packetizer.payloadBytes(), 100ULL * payload.size());
}

MULTIROOM_TEST(packetizer_runs_a_hundred_thousand_packets_with_no_timeline_discontinuity)
{
    // Thirty-three minutes of 44.1 kHz audio at twenty milliseconds a packet,
    // decoded by the parser the real client uses -- which fails a stream whose
    // packets do not begin where the last one ended (sections 14 and 15).
    //
    // The grid is computed from the packet's index, exactly as ClientOutput
    // does it. A sender that added each duration to the last position would
    // pass this too at 44.1 kHz; what it would not survive is the trimmed
    // stream below, where the frame count moves and the duration must not.
    OutputPacketizer packetizer(testSessionId(), cdFormat());
    const std::vector<std::byte> payload = ramp(882, cdFormat());

    matp::AudioPacketReader reader;
    uint64_t expectedNs = 0;

    for (uint64_t index = 0; index < 100000; ++index)
    {
        const uint64_t startNs = index * kPacketNs;
        const Result<std::string, PacketizerError> packet =
            packetizer.encode(TimelineNs{startNs}, kPacketNs, 882, payload);
        CHECK_OK(packet);

        const matp::AudioPacket decoded = parseOne(reader, *packet);
        CHECK_EQ(decoded.header.timelineStartNs, expectedNs);
        expectedNs = decoded.header.timelineStartNs + decoded.header.timelineDurationNs;
    }

    // Two thousand seconds of media, to the nanosecond.
    CHECK_EQ(expectedNs, 2000ULL * kNanosecondsPerSecond);
    CHECK_EQ(packetizer.frames(), 88200000ULL);
}

MULTIROOM_TEST(packetizer_lets_the_frame_count_move_while_the_duration_does_not)
{
    // Section 14's whole reason for carrying both: a room being corrected at
    // +30 ppm is sent 883 frames now and then where 882 is nominal, and every
    // one of those packets still covers exactly 20 ms of the zone's timeline.
    // A receiver that derived the timeline from the frame count would read
    // each of them as a discontinuity and rejoin.
    OutputPacketizer packetizer(testSessionId(), cdFormat());
    OutputClock clock(44100, TimelineNs{0});
    CHECK_OK(clock.setTrim(1.000030, TimelineNs{0}));

    matp::AudioPacketReader reader;
    uint64_t sentFrames = 0;
    uint64_t expectedNs = 0;
    size_t longPackets = 0;

    for (uint64_t index = 1; index <= 4000; ++index)
    {
        const uint64_t startNs = (index - 1) * kPacketNs;
        const Result<OutputFrame, OutputClockError> target =
            clock.frameAt(TimelineNs{index * kPacketNs});
        CHECK_OK(target);

        const uint32_t frames = static_cast<uint32_t>(std::to_underlying(*target) - sentFrames);
        sentFrames += frames;

        const Result<std::string, PacketizerError> packet =
            packetizer.encode(TimelineNs{startNs}, kPacketNs, frames, ramp(frames, cdFormat()));
        CHECK_OK(packet);

        const matp::AudioPacket decoded = parseOne(reader, *packet);
        CHECK_EQ(decoded.header.timelineStartNs, expectedNs);
        CHECK_EQ(decoded.header.timelineDurationNs, kPacketNs);
        CHECK_EQ(decoded.header.outputFrameCount, frames);
        CHECK_EQ(decoded.header.payloadBytes, frames * cdFormat().bytesPerFrame());
        expectedNs = decoded.header.timelineStartNs + decoded.header.timelineDurationNs;

        if (frames != 882)
        {
            CHECK_EQ(frames, 883u);
            ++longPackets;
        }
    }

    // Eighty seconds of timeline, and 105 frames more audio than an
    // uncorrected room would have been sent for it.
    CHECK_EQ(expectedNs, 80ULL * kNanosecondsPerSecond);
    CHECK_EQ(sentFrames, 3528105ULL);
    CHECK_EQ(longPackets, size_t(105));
}

MULTIROOM_TEST(packetizer_carries_every_layout_across_the_wire)
{
    // The two 24-bit layouts are different byte counts for the same values and
    // reading one as the other is noise, so the wire code for each is checked
    // rather than assumed.
    const AudioFormat::SampleType layouts[] = {
        AudioFormat::SampleType::Int16, AudioFormat::SampleType::Int24,
        AudioFormat::SampleType::Int24In32, AudioFormat::SampleType::Int32,
        AudioFormat::SampleType::Float32};

    for (const AudioFormat::SampleType layout : layouts)
    {
        AudioFormat format = cdFormat();
        format.sampleType = layout;

        OutputPacketizer packetizer(testSessionId(), format);
        const std::vector<std::byte> payload = ramp(882, format);

        const Result<std::string, PacketizerError> packet =
            packetizer.encode(TimelineNs{0}, kPacketNs, 882, payload);
        CHECK_OK(packet);

        matp::AudioPacketReader reader;
        const matp::AudioPacket decoded = parseOne(reader, *packet);
        CHECK(decoded.header.format.sampleType == layout);
        CHECK_EQ(decoded.header.payloadBytes, uint32_t(payload.size()));
    }
}

MULTIROOM_TEST(packetizer_refuses_what_the_client_would_refuse)
{
    // Every one of these is a packet the reader on the other end drops the
    // connection over. Refused here, where the fault has a name, rather than
    // on the wire, where it is a room that went quiet.
    OutputPacketizer packetizer(testSessionId(), cdFormat());
    const std::vector<std::byte> payload = ramp(882, cdFormat());

    CHECK_ERR(packetizer.encode(TimelineNs{0}, kPacketNs, 0, {}), PacketizerError::EmptyPacket);
    CHECK_ERR(packetizer.encode(TimelineNs{0}, 0, 882, payload), PacketizerError::EmptyDuration);
    CHECK_ERR(packetizer.encode(TimelineNs{0}, matp::kMaxPacketDurationNs + 1, 882, payload),
              PacketizerError::DurationTooLong);

    // The payload has to be exactly the frames the header claims, in this
    // room's own format: a length that disagrees is a receiver cutting the
    // bytes up wrongly.
    CHECK_ERR(packetizer.encode(TimelineNs{0}, kPacketNs, 881, payload),
              PacketizerError::PayloadMismatch);

    // 882 frames at 44.1 kHz is 20 ms, and no resampling ratio makes it 200.
    CHECK_ERR(packetizer.encode(TimelineNs{0}, 200000000ULL, 882, payload),
              PacketizerError::FrameCountImplausible);

    // A packet that ends past the end of the timeline is one nothing can
    // follow, because the next one begins where this one ended.
    CHECK_ERR(packetizer.encode(TimelineNs{UINT64_MAX - 1}, kPacketNs, 882, payload),
              PacketizerError::TimelineOverflow);

    // Nothing above was sent, so the sequence has not moved.
    CHECK_EQ(packetizer.nextSequence(), 0ULL);
    CHECK_EQ(packetizer.packets(), 0ULL);

    OutputPacketizer unnamed(SessionId(), cdFormat());
    CHECK_ERR(unnamed.encode(TimelineNs{0}, kPacketNs, 882, payload),
              PacketizerError::InvalidSession);

    AudioFormat broken = cdFormat();
    broken.sampleRate = 0;
    OutputPacketizer formatless(testSessionId(), broken);
    CHECK_ERR(formatless.encode(TimelineNs{0}, kPacketNs, 882, payload),
              PacketizerError::InvalidFormat);
}

MULTIROOM_TEST(packetizer_gives_back_the_numbers_of_packets_that_never_reached_a_socket)
{
    // Phase 16, and a defect phase 14 left behind with it. A room that is
    // parked, stranded or reopened in another format drops what it had queued
    // -- and those packets had already been numbered. The client anchors the
    // numbering on the first packet of a connection and reads a gap in it as a
    // server that skipped ahead, which costs the **connection** and not the
    // packet, so the numbers have to go back.
    OutputPacketizer packetizer(testSessionId(), cdFormat());
    const std::vector<std::byte> payload = ramp(882, cdFormat());

    for (uint64_t index = 0; index < 5; ++index)
        CHECK_OK(packetizer.encode(TimelineNs{index * kPacketNs}, kPacketNs, 882, payload));
    CHECK_EQ(packetizer.nextSequence(), 5ULL);

    // Three were built and thrown away: the next packet on the wire carries
    // the number after the last one that actually went out.
    packetizer.rewind(3);
    CHECK_EQ(packetizer.nextSequence(), 2ULL);

    const Result<std::string, PacketizerError> next =
        packetizer.encode(TimelineNs{9 * kPacketNs}, kPacketNs, 882, payload);
    CHECK_OK(next);
    matp::AudioPacketReader reader;
    CHECK_EQ(parseOne(reader, *next).header.sequenceNumber, 2ULL);

    // What this packetizer built is a different question from what a room was
    // sent, so that counter is not wound back with it.
    CHECK_EQ(packetizer.packets(), 6ULL);

    // And more than it ever issued is the whole numbering, not an underflow.
    packetizer.rewind(1000);
    CHECK_EQ(packetizer.nextSequence(), 0ULL);
}

MULTIROOM_TEST(packetizer_flags_the_one_packet_after_a_flush)
{
    // Section 54 (phase 26): the first packet after a flush carries
    // afterFlush, the ones after it do not, and the client's own reader --
    // which refuses every other flag bit -- takes it.
    OutputPacketizer packetizer(testSessionId(), cdFormat());
    const std::vector<std::byte> payload = ramp(882, cdFormat());
    matp::AudioPacketReader reader;

    const auto plain = packetizer.encode(TimelineNs{0}, kPacketNs, 882, payload);
    CHECK_OK(plain);
    CHECK_EQ(parseOne(reader, *plain).header.flags, 0u);

    packetizer.markAfterFlush();
    CHECK(packetizer.afterFlushPending());
    const auto flagged =
        packetizer.encode(TimelineNs{50 * kPacketNs}, kPacketNs, 882, payload);
    CHECK_OK(flagged);
    const matp::AudioPacket first = parseOne(reader, *flagged);
    CHECK_EQ(first.header.flags, matp::kAudioFlagAfterFlush);
    CHECK_EQ(first.header.sequenceNumber, 1ULL);
    CHECK(!packetizer.afterFlushPending());

    const auto after = packetizer.encode(TimelineNs{51 * kPacketNs}, kPacketNs, 882, payload);
    CHECK_OK(after);
    CHECK_EQ(parseOne(reader, *after).header.flags, 0u);

    // A packet that is refused was never built, and does not use the flag up.
    packetizer.markAfterFlush();
    CHECK_ERR(packetizer.encode(TimelineNs{60 * kPacketNs}, kPacketNs, 0, {}),
              PacketizerError::EmptyPacket);
    CHECK(packetizer.afterFlushPending());
}
