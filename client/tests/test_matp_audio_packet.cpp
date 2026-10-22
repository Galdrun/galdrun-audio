#include "TestSupport.h"

#include "matp/AudioPacket.h"
#include "matp/Protocol.h"

#include <algorithm>
#include <string>
#include <vector>

using namespace multiroom;

namespace {

AudioFormat format(uint32_t rate, uint16_t channels, AudioFormat::SampleType type)
{
    AudioFormat result;
    result.sampleRate = rate;
    result.channels = channels;
    result.sampleType = type;
    return result;
}

AudioFormat streamFormat()
{
    return format(48000, 2, AudioFormat::SampleType::Int24In32);
}

// What a span of frames at a given rate is worth in media time, computed the
// way MATP section 15 requires a sender to compute it.
uint64_t nominalDurationNs(uint64_t frames, uint32_t rate)
{
    return (frames * 1000000000ULL) / rate;
}

matp::AudioPacketHeader header(uint64_t sequence, uint64_t timelineStartNs,
                               uint32_t outputFrames, uint64_t durationNs = 0,
                               const AudioFormat& fmt = streamFormat())
{
    matp::AudioPacketHeader result;
    result.protocolVersion = static_cast<uint16_t>(matp::kProtocolVersion);
    result.sessionIdHigh = 0x0123456789ABCDEFULL;
    result.sessionIdLow = 0xFEDCBA9876543210ULL;
    result.sequenceNumber = sequence;
    result.timelineStartNs = timelineStartNs;
    result.timelineDurationNs =
        durationNs != 0 ? durationNs : nominalDurationNs(outputFrames, fmt.sampleRate);
    result.format = fmt;
    result.outputFrameCount = outputFrames;
    return result;
}

// A payload whose every byte is a function of its position, so a packet that
// arrives shifted by a byte does not still look correct.
std::vector<unsigned char> payload(uint32_t frameCount, unsigned char seed,
                                   const AudioFormat& fmt = streamFormat())
{
    std::vector<unsigned char> bytes(static_cast<size_t>(frameCount) * fmt.bytesPerFrame());
    for (size_t i = 0; i < bytes.size(); ++i)
        bytes[i] = static_cast<unsigned char>(seed + i);
    return bytes;
}

std::string packet(uint64_t sequence, uint64_t timelineStartNs, uint32_t outputFrames,
                   unsigned char seed = 0, uint64_t durationNs = 0)
{
    const std::vector<unsigned char> bytes = payload(outputFrames, seed);
    return matp::encodeAudioPacket(
        header(sequence, timelineStartNs, outputFrames, durationNs), bytes.data(), bytes.size());
}

uint32_t readUint32(const std::string& data, size_t offset)
{
    uint32_t value = 0;
    for (size_t i = 0; i < 4; ++i)
        value = (value << 8) | static_cast<unsigned char>(data[offset + i]);
    return value;
}

uint64_t readUint64(const std::string& data, size_t offset)
{
    uint64_t value = 0;
    for (size_t i = 0; i < 8; ++i)
        value = (value << 8) | static_cast<unsigned char>(data[offset + i]);
    return value;
}

uint16_t readUint16(const std::string& data, size_t offset)
{
    return static_cast<uint16_t>((static_cast<unsigned char>(data[offset]) << 8) |
                                 static_cast<unsigned char>(data[offset + 1]));
}

void writeUint32(std::string& data, size_t offset, uint32_t value)
{
    for (size_t i = 0; i < 4; ++i)
        data[offset + i] = static_cast<char>((value >> (24 - 8 * i)) & 0xFF);
}

void writeUint64(std::string& data, size_t offset, uint64_t value)
{
    for (size_t i = 0; i < 8; ++i)
        data[offset + i] = static_cast<char>((value >> (56 - 8 * i)) & 0xFF);
}

void writeUint16(std::string& data, size_t offset, uint16_t value)
{
    data[offset] = static_cast<char>((value >> 8) & 0xFF);
    data[offset + 1] = static_cast<char>(value & 0xFF);
}

// Patching a field invalidates the CRC, and a packet refused for its CRC would
// prove nothing about the field. Zeroing it says "not supplied", which section
// 42 allows and which is what leaves the patched field to be judged.
void clearCrc(std::string& data)
{
    writeUint32(data, 68, 0);
}

// The caller owns the reader: a decoded packet's payload points into it and
// lives only as long as it does.
matp::AudioPacketReader::Result parseOne(matp::AudioPacketReader& reader,
                                         const std::string& data, matp::AudioPacket& out,
                                         std::string& error)
{
    reader.append(data.data(), data.size());
    return reader.next(out, error);
}

} // namespace

MULTIROOM_TEST(matp_audio_packet_header_is_big_endian_and_72_bytes)
{
    CHECK_EQ(matp::kAudioHeaderBytes, static_cast<size_t>(72));

    const std::string encoded = packet(7, 12000000000ULL, 2);
    CHECK_EQ(encoded.size(),
             matp::kAudioHeaderBytes + static_cast<size_t>(2) * streamFormat().bytesPerFrame());

    CHECK_EQ(readUint32(encoded, 0), matp::kAudioPacketMagic);
    CHECK_EQ(readUint16(encoded, 4), static_cast<uint16_t>(matp::kProtocolVersion));
    CHECK_EQ(readUint16(encoded, 6), static_cast<uint16_t>(matp::kAudioHeaderBytes));
    CHECK_EQ(readUint64(encoded, 8), 0x0123456789ABCDEFULL);
    CHECK_EQ(readUint64(encoded, 16), 0xFEDCBA9876543210ULL);
    CHECK_EQ(readUint64(encoded, 24), static_cast<uint64_t>(7));
    CHECK_EQ(readUint64(encoded, 32), 12000000000ULL);
    CHECK_EQ(readUint64(encoded, 40), nominalDurationNs(2, 48000));
    CHECK_EQ(readUint32(encoded, 48), static_cast<uint32_t>(48000));
    CHECK_EQ(readUint16(encoded, 52), static_cast<uint16_t>(2));
    CHECK_EQ(readUint16(encoded, 54), matp::sampleFormatCode(AudioFormat::SampleType::Int24In32));
    CHECK_EQ(readUint32(encoded, 56), static_cast<uint32_t>(2));
    CHECK_EQ(readUint32(encoded, 60), static_cast<uint32_t>(2 * 8));
    CHECK_EQ(readUint32(encoded, 64), static_cast<uint32_t>(0));
    CHECK(readUint32(encoded, 68) != 0);
}

MULTIROOM_TEST(matp_audio_packet_sample_format_codes_keep_the_two_24_bit_layouts_apart)
{
    const AudioFormat::SampleType types[] = {
        AudioFormat::SampleType::Int16, AudioFormat::SampleType::Int24,
        AudioFormat::SampleType::Int24In32, AudioFormat::SampleType::Int32,
        AudioFormat::SampleType::Float32};

    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); ++i)
    {
        AudioFormat::SampleType parsed = AudioFormat::SampleType::Int16;
        CHECK(matp::parseSampleFormatCode(matp::sampleFormatCode(types[i]), parsed));
        CHECK(parsed == types[i]);
    }

    CHECK(matp::sampleFormatCode(AudioFormat::SampleType::Int24) !=
          matp::sampleFormatCode(AudioFormat::SampleType::Int24In32));

    AudioFormat::SampleType unused = AudioFormat::SampleType::Int16;
    CHECK(!matp::parseSampleFormatCode(0, unused));
    CHECK(!matp::parseSampleFormatCode(6, unused));
}

MULTIROOM_TEST(matp_audio_packet_round_trips_through_the_reader)
{
    const std::vector<unsigned char> bytes = payload(4, 17);
    const std::string encoded =
        matp::encodeAudioPacket(header(3, 10000000, 4), bytes.data(), bytes.size());

    matp::AudioPacketReader reader;
    reader.append(encoded.data(), encoded.size());

    matp::AudioPacket decoded;
    std::string error;
    CHECK(reader.next(decoded, error) == matp::AudioPacketReader::Result::Packet);
    CHECK_EQ(decoded.header.sequenceNumber, static_cast<uint64_t>(3));
    CHECK_EQ(decoded.header.timelineStartNs, static_cast<uint64_t>(10000000));
    CHECK_EQ(decoded.header.timelineDurationNs, nominalDurationNs(4, 48000));
    CHECK_EQ(decoded.header.outputFrameCount, static_cast<uint32_t>(4));
    CHECK_EQ(decoded.header.sessionIdHigh, 0x0123456789ABCDEFULL);
    CHECK_EQ(decoded.header.sessionIdLow, 0xFEDCBA9876543210ULL);
    CHECK(decoded.header.format == streamFormat());
    CHECK_EQ(decoded.payloadBytes, bytes.size());
    CHECK(std::string(reinterpret_cast<const char*>(decoded.payload), decoded.payloadBytes) ==
          std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));

    CHECK(reader.next(decoded, error) == matp::AudioPacketReader::Result::Incomplete);
}

// The reader compacts before it would grow, which moves the unread tail of a
// packet on almost every read; a stream of packets bigger than one read, cut
// at every offset a real socket produces, must come out whole and in order.
MULTIROOM_TEST(matp_audio_packet_reader_keeps_packets_whole_across_compaction)
{
    const AudioFormat fmt = format(44100, 2, AudioFormat::SampleType::Int32);
    std::string stream;
    std::vector<std::vector<unsigned char>> payloads;
    for (uint64_t i = 0; i < 40; ++i)
    {
        payloads.push_back(payload(882, static_cast<unsigned char>(i), fmt));
        stream += matp::encodeAudioPacket(header(i, i * 20000000ULL, 882, 0, fmt),
                                          payloads.back().data(), payloads.back().size());
    }

    const size_t chunks[] = {6000, 8192, 1460, 7127};
    for (size_t chunk : chunks)
    {
        matp::AudioPacketReader reader;
        matp::AudioPacket decoded;
        std::string error;
        uint64_t expected = 0;
        for (size_t offset = 0; offset < stream.size(); offset += chunk)
        {
            reader.append(stream.data() + offset, std::min(chunk, stream.size() - offset));
            matp::AudioPacketReader::Result result;
            while ((result = reader.next(decoded, error)) ==
                   matp::AudioPacketReader::Result::Packet)
            {
                CHECK_EQ(decoded.header.sequenceNumber, expected);
                CHECK(std::equal(payloads[expected].begin(), payloads[expected].end(),
                                 decoded.payload));
                ++expected;
            }
            CHECK(result == matp::AudioPacketReader::Result::Incomplete);
        }
        CHECK_EQ(expected, static_cast<uint64_t>(payloads.size()));
        CHECK_EQ(reader.buffered(), static_cast<size_t>(0));
    }
}

MULTIROOM_TEST(matp_audio_packet_survives_one_byte_per_read)
{
    const std::string encoded = packet(0, 0, 3, 5);

    matp::AudioPacketReader reader;
    matp::AudioPacket decoded;
    std::string error;

    for (size_t i = 0; i + 1 < encoded.size(); ++i)
    {
        reader.append(encoded.data() + i, 1);
        CHECK(reader.next(decoded, error) == matp::AudioPacketReader::Result::Incomplete);
    }

    reader.append(encoded.data() + encoded.size() - 1, 1);
    CHECK(reader.next(decoded, error) == matp::AudioPacketReader::Result::Packet);
    CHECK_EQ(decoded.header.outputFrameCount, static_cast<uint32_t>(3));
    CHECK_EQ(decoded.payload[0], static_cast<unsigned char>(5));
}

MULTIROOM_TEST(matp_audio_packet_reads_two_packets_out_of_one_read)
{
    const uint64_t first = nominalDurationNs(2, 48000);
    const std::string both = packet(1, 0, 2, 1) + packet(2, first, 2, 100);

    matp::AudioPacketReader reader;
    reader.append(both.data(), both.size());

    matp::AudioPacket one;
    matp::AudioPacket two;
    std::string error;
    CHECK(reader.next(one, error) == matp::AudioPacketReader::Result::Packet);
    CHECK_EQ(one.header.sequenceNumber, static_cast<uint64_t>(1));
    CHECK_EQ(one.payload[0], static_cast<unsigned char>(1));

    CHECK(reader.next(two, error) == matp::AudioPacketReader::Result::Packet);
    CHECK_EQ(two.header.sequenceNumber, static_cast<uint64_t>(2));
    CHECK_EQ(two.header.timelineStartNs, first);
    CHECK_EQ(two.payload[0], static_cast<unsigned char>(100));

    CHECK(reader.next(two, error) == matp::AudioPacketReader::Result::Incomplete);
    CHECK_EQ(reader.buffered(), static_cast<size_t>(0));
}

MULTIROOM_TEST(matp_audio_packet_refuses_a_header_that_is_not_a_packet)
{
    std::string encoded = packet(0, 0, 1);
    writeUint32(encoded, 0, 0x4D415451); // 'MATQ'

    matp::AudioPacket decoded;
    std::string error;
    matp::AudioPacketReader reader;
    CHECK(parseOne(reader, encoded, decoded, error) == matp::AudioPacketReader::Result::Invalid);
    CHECK(!error.empty());
}

MULTIROOM_TEST(matp_audio_packet_refuses_a_header_shorter_than_the_version_2_header)
{
    std::string encoded = packet(0, 0, 1);
    writeUint16(encoded, 6, 68); // the version 1 header's length
    clearCrc(encoded);

    matp::AudioPacket decoded;
    std::string error;
    matp::AudioPacketReader reader;
    CHECK(parseOne(reader, encoded, decoded, error) == matp::AudioPacketReader::Result::Invalid);
}

MULTIROOM_TEST(matp_audio_packet_skips_a_header_from_a_later_version)
{
    // A server that added fields of its own: headerSize says where the payload
    // starts, which is the whole reason the field exists.
    const std::vector<unsigned char> bytes = payload(2, 9);
    std::string encoded = matp::encodeAudioPacket(header(0, 0, 2), bytes.data(), bytes.size());

    const std::string extra(8, '\x7F');
    encoded.insert(matp::kAudioHeaderBytes, extra);
    writeUint16(encoded, 6, static_cast<uint16_t>(matp::kAudioHeaderBytes + extra.size()));
    clearCrc(encoded);

    matp::AudioPacket decoded;
    std::string error;
    matp::AudioPacketReader reader;
    CHECK(parseOne(reader, encoded, decoded, error) == matp::AudioPacketReader::Result::Packet);
    CHECK_EQ(decoded.payloadBytes, bytes.size());
    CHECK_EQ(decoded.payload[0], static_cast<unsigned char>(9));
}

MULTIROOM_TEST(matp_audio_packet_refuses_a_payload_that_contradicts_the_frame_count)
{
    std::string encoded = packet(0, 0, 4);
    writeUint32(encoded, 56, 3); // outputFrameCount, with a payload of four frames
    clearCrc(encoded);

    matp::AudioPacket decoded;
    std::string error;
    matp::AudioPacketReader reader;
    CHECK(parseOne(reader, encoded, decoded, error) == matp::AudioPacketReader::Result::Invalid);
}

MULTIROOM_TEST(matp_audio_packet_refuses_a_sample_format_it_does_not_know)
{
    std::string encoded = packet(0, 0, 1);
    writeUint16(encoded, 54, 99);
    clearCrc(encoded);

    matp::AudioPacket decoded;
    std::string error;
    matp::AudioPacketReader reader;
    CHECK(parseOne(reader, encoded, decoded, error) == matp::AudioPacketReader::Result::Invalid);
}

MULTIROOM_TEST(matp_audio_packet_accepts_after_flush)
{
    matp::AudioPacketHeader flagged = header(7, 5000000ULL, 48);
    flagged.flags = matp::kAudioFlagAfterFlush;
    const std::vector<unsigned char> bytes = payload(48, 3);
    const std::string encoded = matp::encodeAudioPacket(flagged, bytes.data(), bytes.size());

    // Section 54's one flag, carried through with its CRC intact.
    matp::AudioPacket decoded;
    std::string error;
    matp::AudioPacketReader reader;
    CHECK(parseOne(reader, encoded, decoded, error) == matp::AudioPacketReader::Result::Packet);
    CHECK_EQ(decoded.header.flags, matp::kAudioFlagAfterFlush);
    CHECK_EQ(decoded.header.timelineStartNs, static_cast<uint64_t>(5000000ULL));
}

MULTIROOM_TEST(matp_audio_packet_refuses_flags_it_does_not_understand)
{
    // Every bit but afterFlush, alone and beside it.
    const uint32_t refused[] = {2u, 0x80000000u, matp::kAudioFlagAfterFlush | 4u};
    for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); ++i)
    {
        std::string encoded = packet(0, 0, 1);
        writeUint32(encoded, 64, refused[i]);
        clearCrc(encoded);

        matp::AudioPacket decoded;
        std::string error;
        matp::AudioPacketReader reader;
        CHECK(parseOne(reader, encoded, decoded, error) ==
              matp::AudioPacketReader::Result::Invalid);
    }
}

MULTIROOM_TEST(matp_audio_packet_refuses_a_payload_past_the_limit)
{
    std::string encoded = packet(0, 0, 1);
    writeUint32(encoded, 60, matp::kMaxAudioPayloadBytes + 1);
    clearCrc(encoded);

    matp::AudioPacket decoded;
    std::string error;
    // Refused on the header alone: waiting for a megabyte to arrive before
    // deciding is what the limit exists to avoid.
    matp::AudioPacketReader reader;
    CHECK(parseOne(reader, encoded, decoded, error) == matp::AudioPacketReader::Result::Invalid);
}

MULTIROOM_TEST(matp_audio_packet_checks_the_header_crc_when_there_is_one)
{
    std::string encoded = packet(0, 0, 2);
    const uint32_t crc = readUint32(encoded, 68);
    writeUint32(encoded, 68, crc ^ 0xFFu);

    matp::AudioPacket decoded;
    std::string error;
    matp::AudioPacketReader reader;
    CHECK(parseOne(reader, encoded, decoded, error) == matp::AudioPacketReader::Result::Invalid);

    // ...and a sender that supplies none is not refused for it (section 42).
    std::string without = packet(0, 0, 2);
    clearCrc(without);
    matp::AudioPacketReader second;
    CHECK(parseOne(second, without, decoded, error) == matp::AudioPacketReader::Result::Packet);
}

MULTIROOM_TEST(matp_audio_packet_reader_forgets_everything_on_reset)
{
    const std::string encoded = packet(0, 0, 2);
    matp::AudioPacketReader reader;
    reader.append(encoded.data(), encoded.size() - 4);
    CHECK(reader.buffered() != 0);

    reader.reset();
    CHECK_EQ(reader.buffered(), static_cast<size_t>(0));

    // The tail of the previous packet must not be read as the head of the next.
    reader.append(encoded.data(), encoded.size());
    matp::AudioPacket decoded;
    std::string error;
    CHECK(reader.next(decoded, error) == matp::AudioPacketReader::Result::Packet);
}

// The two axes of section 14: one span of timeline, and a frame count that
// belongs to this room alone.
MULTIROOM_TEST(matp_audio_packet_carries_a_timeline_span_and_a_frame_count_apart)
{
    // 20 ms of timeline delivered as 961 frames of a 48 kHz stream, which is
    // what a server correcting this client's clock sends (section 28).
    const std::vector<unsigned char> bytes = payload(961, 3);
    const std::string encoded =
        matp::encodeAudioPacket(header(9, 12000000000ULL, 961, 20000000ULL), bytes.data(),
                                bytes.size());

    matp::AudioPacket decoded;
    std::string error;
    matp::AudioPacketReader reader;
    CHECK(parseOne(reader, encoded, decoded, error) == matp::AudioPacketReader::Result::Packet);
    CHECK_EQ(decoded.header.timelineStartNs, 12000000000ULL);
    CHECK_EQ(decoded.header.timelineDurationNs, 20000000ULL);
    CHECK_EQ(decoded.header.outputFrameCount, static_cast<uint32_t>(961));

    // The payload is sized by the frame count, because that is what is in it.
    CHECK_EQ(decoded.payloadBytes, static_cast<size_t>(961) * streamFormat().bytesPerFrame());
}

// The same 20 ms as the packet above, for a bitperfect 44.1 kHz room: a
// different rate and a different frame count for identical timeline fields.
MULTIROOM_TEST(matp_audio_packet_carries_a_bitperfect_44_1_khz_stream)
{
    const AudioFormat fmt = format(44100, 2, AudioFormat::SampleType::Int24);
    const std::vector<unsigned char> bytes = payload(882, 1, fmt);
    const std::string encoded = matp::encodeAudioPacket(
        header(0, 12000000000ULL, 882, 20000000ULL, fmt), bytes.data(), bytes.size());

    matp::AudioPacket decoded;
    std::string error;
    matp::AudioPacketReader reader;
    CHECK(parseOne(reader, encoded, decoded, error) == matp::AudioPacketReader::Result::Packet);
    CHECK_EQ(decoded.header.timelineDurationNs, 20000000ULL);
    CHECK_EQ(decoded.header.outputFrameCount, static_cast<uint32_t>(882));
    CHECK_EQ(decoded.header.format.sampleRate, static_cast<uint32_t>(44100));
    CHECK(decoded.header.format.sampleType == AudioFormat::SampleType::Int24);
    CHECK_EQ(decoded.payloadBytes, static_cast<size_t>(882) * 6);
}

MULTIROOM_TEST(matp_audio_packet_refuses_a_packet_that_covers_no_timeline_time)
{
    std::string patched = packet(0, 0, 4);
    writeUint64(patched, 40, 0); // timelineDurationNs
    clearCrc(patched);

    matp::AudioPacket decoded;
    std::string error;
    matp::AudioPacketReader reader;
    CHECK(parseOne(reader, patched, decoded, error) == matp::AudioPacketReader::Result::Invalid);
}

MULTIROOM_TEST(matp_audio_packet_refuses_a_span_and_a_frame_count_no_ratio_produces)
{
    // Resampling moves the two by parts per million; ten times apart is a
    // sender that means something else by one of the fields.
    std::string patched = packet(0, 0, 480);
    writeUint64(patched, 40, nominalDurationNs(4800, 48000));
    clearCrc(patched);

    matp::AudioPacket decoded;
    std::string error;
    matp::AudioPacketReader reader;
    CHECK(parseOne(reader, patched, decoded, error) == matp::AudioPacketReader::Result::Invalid);

    // A ratio a correction really could produce is accepted: 20 ms carrying one
    // extra frame.
    const std::string fine = packet(0, 0, 961, 0, 20000000ULL);
    matp::AudioPacketReader second;
    CHECK(parseOne(second, fine, decoded, error) == matp::AudioPacketReader::Result::Packet);
}

// The duration bound exists for the arithmetic downstream, not for the
// protocol: it is what keeps `duration * frameCount` inside 64 bits.
MULTIROOM_TEST(matp_audio_packet_refuses_a_span_longer_than_it_will_map)
{
    // A frame count that matches the duration, so only the bound can refuse it.
    const uint32_t frames = 48000 * 2;
    const std::vector<unsigned char> bytes = payload(frames, 0);
    std::string encoded = matp::encodeAudioPacket(
        header(0, 0, frames, matp::kMaxPacketDurationNs + 1), bytes.data(), bytes.size());

    matp::AudioPacket decoded;
    std::string error;
    matp::AudioPacketReader reader;
    CHECK(parseOne(reader, encoded, decoded, error) == matp::AudioPacketReader::Result::Invalid);
    CHECK(error.find("this client maps in one packet") != std::string::npos);

    // Exactly at the bound is still a packet.
    const std::string atBound = matp::encodeAudioPacket(
        header(0, 0, frames, matp::kMaxPacketDurationNs), bytes.data(), bytes.size());
    matp::AudioPacketReader second;
    CHECK(parseOne(second, atBound, decoded, error) == matp::AudioPacketReader::Result::Packet);
}

// The end of a packet is where the next one begins (section 14), so a span
// that cannot be added up is one nothing can follow.
MULTIROOM_TEST(matp_audio_packet_refuses_a_span_that_ends_past_the_end_of_the_timeline)
{
    std::string patched = packet(0, 0, 4);
    writeUint64(patched, 32, UINT64_MAX - 1000);
    clearCrc(patched);

    matp::AudioPacket decoded;
    std::string error;
    matp::AudioPacketReader reader;
    CHECK(parseOne(reader, patched, decoded, error) == matp::AudioPacketReader::Result::Invalid);
    CHECK(error.find("past the end of the timeline") != std::string::npos);
}
