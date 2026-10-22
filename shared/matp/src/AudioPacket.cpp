#include "matp/AudioPacket.h"

namespace multiroom {
namespace matp {
namespace {

void appendUint16(std::string& out, uint16_t value)
{
    out.push_back(static_cast<char>((value >> 8) & 0xFF));
    out.push_back(static_cast<char>(value & 0xFF));
}

void appendUint32(std::string& out, uint32_t value)
{
    for (int shift = 24; shift >= 0; shift -= 8)
        out.push_back(static_cast<char>((value >> shift) & 0xFF));
}

void appendUint64(std::string& out, uint64_t value)
{
    for (int shift = 56; shift >= 0; shift -= 8)
        out.push_back(static_cast<char>((value >> shift) & 0xFF));
}

uint16_t readUint16(const unsigned char* bytes)
{
    return static_cast<uint16_t>((static_cast<uint16_t>(bytes[0]) << 8) | bytes[1]);
}

uint32_t readUint32(const unsigned char* bytes)
{
    uint32_t value = 0;
    for (size_t i = 0; i < 4; ++i)
        value = (value << 8) | bytes[i];
    return value;
}

uint64_t readUint64(const unsigned char* bytes)
{
    uint64_t value = 0;
    for (size_t i = 0; i < 8; ++i)
        value = (value << 8) | bytes[i];
    return value;
}

// CRC-32 as in IEEE 802.3: reflected, polynomial 0xEDB88320, pre- and
// post-inverted. Computed a bit at a time rather than from a table -- sixty
// bytes fifty times a second is nothing even on the ARMv6 target, and a
// kilobyte of static table is not free there.
uint32_t crc32(const unsigned char* data, size_t size)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i)
    {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return crc ^ 0xFFFFFFFFu;
}

// Compacting costs a memmove, so it happens when there is enough behind the
// read offset to be worth moving out of the way, not after every packet.
const size_t kCompactThresholdBytes = 64 * 1024;

// Room for a partial packet plus one read, reserved once and kept by
// compacting before the buffer would grow. Under 16 KB on purpose: an ESP32
// puts every allocation above CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL (16 KB) in
// PSRAM, where each byte of the stream would be written and read back once
// more through a cache it shares with the code. Larger packets still work;
// the buffer then simply grows.
const size_t kReservedBytes = 16000;

} // namespace

uint16_t sampleFormatCode(AudioFormat::SampleType type)
{
    switch (type)
    {
    case AudioFormat::SampleType::Int16:
        return 1;
    case AudioFormat::SampleType::Int24:
        return 2;
    case AudioFormat::SampleType::Int24In32:
        return 3;
    case AudioFormat::SampleType::Int32:
        return 4;
    case AudioFormat::SampleType::Float32:
        return 5;
    }
    return 0;
}

bool parseSampleFormatCode(uint16_t code, AudioFormat::SampleType& out)
{
    const AudioFormat::SampleType known[] = {
        AudioFormat::SampleType::Int16, AudioFormat::SampleType::Int24,
        AudioFormat::SampleType::Int24In32, AudioFormat::SampleType::Int32,
        AudioFormat::SampleType::Float32};
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); ++i)
    {
        if (code == sampleFormatCode(known[i]))
        {
            out = known[i];
            return true;
        }
    }
    return false;
}

std::string encodeAudioPacket(const AudioPacketHeader& header, const void* payload,
                              size_t payloadBytes)
{
    std::string packet;
    packet.reserve(kAudioHeaderBytes + payloadBytes);

    appendUint32(packet, kAudioPacketMagic);
    appendUint16(packet, header.protocolVersion);
    appendUint16(packet, static_cast<uint16_t>(kAudioHeaderBytes));
    appendUint64(packet, header.sessionIdHigh);
    appendUint64(packet, header.sessionIdLow);
    appendUint64(packet, header.sequenceNumber);
    appendUint64(packet, header.timelineStartNs);
    // Filled in from the header rather than from the payload, unlike
    // payloadBytes below: how much of the timeline a packet covers is the
    // sender's to say and nothing about the bytes reveals it.
    appendUint64(packet, header.timelineDurationNs);
    appendUint32(packet, header.format.sampleRate);
    appendUint16(packet, header.format.channels);
    appendUint16(packet, sampleFormatCode(header.format.sampleType));
    appendUint32(packet, header.outputFrameCount);
    appendUint32(packet, static_cast<uint32_t>(payloadBytes));
    appendUint32(packet, header.flags);

    // Over everything written so far, which is the header up to but not
    // including this field.
    appendUint32(packet,
                 crc32(reinterpret_cast<const unsigned char*>(packet.data()), packet.size()));

    if (payload != nullptr && payloadBytes != 0)
        packet.append(static_cast<const char*>(payload), payloadBytes);
    return packet;
}

void AudioPacketReader::append(const void* data, size_t size)
{
    if (data == nullptr || size == 0)
        return;
    if (buffer_.capacity() < kReservedBytes)
        buffer_.reserve(kReservedBytes);
    compact(size);
    buffer_.append(static_cast<const char*>(data), size);
}

void AudioPacketReader::reset()
{
    buffer_.clear();
    consumed_ = 0;
}

void AudioPacketReader::compact(size_t incoming)
{
    if (consumed_ == 0)
        return;
    // Growing would copy everything anyway; moving only the unread tail
    // instead keeps the buffer where it was allocated.
    const bool wouldGrow = buffer_.size() + incoming > buffer_.capacity();
    if (!wouldGrow && consumed_ < kCompactThresholdBytes && consumed_ < buffer_.size())
        return;

    buffer_.erase(0, consumed_);
    consumed_ = 0;
}

AudioPacketReader::Result AudioPacketReader::next(AudioPacket& out, std::string& error)
{
    const size_t available = buffer_.size() - consumed_;
    if (available < kAudioHeaderBytes)
        return Result::Incomplete;

    const unsigned char* header =
        reinterpret_cast<const unsigned char*>(buffer_.data()) + consumed_;

    if (readUint32(header) != kAudioPacketMagic)
    {
        error = "audio packet does not begin with the MATP magic";
        return Result::Invalid;
    }

    AudioPacketHeader parsed;
    parsed.protocolVersion = readUint16(header + 4);
    parsed.headerSize = readUint16(header + 6);
    if (parsed.headerSize < kAudioHeaderBytes)
    {
        error = "audio packet header of " + std::to_string(parsed.headerSize) +
                " bytes is shorter than the version 2 header";
        return Result::Invalid;
    }

    parsed.sessionIdHigh = readUint64(header + 8);
    parsed.sessionIdLow = readUint64(header + 16);
    parsed.sequenceNumber = readUint64(header + 24);
    parsed.timelineStartNs = readUint64(header + 32);
    parsed.timelineDurationNs = readUint64(header + 40);
    parsed.format.sampleRate = readUint32(header + 48);
    parsed.format.channels = readUint16(header + 52);
    const uint16_t formatCode = readUint16(header + 54);
    parsed.outputFrameCount = readUint32(header + 56);
    parsed.payloadBytes = readUint32(header + 60);
    parsed.flags = readUint32(header + 64);
    parsed.headerCrc = readUint32(header + 68);

    if (parsed.headerCrc != 0 && parsed.headerCrc != crc32(header, kAudioHeaderCrcBytes))
    {
        // Section 42: TCP already protects the bytes on the wire, so what this
        // catches is a parser bug or a framing slip -- which is exactly what
        // the section keeps it for.
        error = "audio packet header CRC does not match";
        return Result::Invalid;
    }

    if (parsed.payloadBytes > kMaxAudioPayloadBytes)
    {
        error = "audio packet payload of " + std::to_string(parsed.payloadBytes) +
                " bytes exceeds the " + std::to_string(kMaxAudioPayloadBytes) + " byte limit";
        return Result::Invalid;
    }

    // Judged before the payload is waited for: a header this client cannot
    // honour does not become acceptable by having its payload turn up.
    if (!parseSampleFormatCode(formatCode, parsed.format.sampleType))
    {
        error = "audio packet names sample format " + std::to_string(formatCode) +
                ", which this client does not know";
        return Result::Invalid;
    }
    if ((parsed.flags & ~kAudioFlagAfterFlush) != 0)
    {
        error = "audio packet sets flags " + std::to_string(parsed.flags) +
                ", of which only afterFlush is defined in protocol version 2";
        return Result::Invalid;
    }
    if (!parsed.format.isValid() || parsed.outputFrameCount == 0)
    {
        error = "audio packet carries no frames";
        return Result::Invalid;
    }
    if (parsed.timelineDurationNs == 0)
    {
        // A packet that covers no media time cannot be placed on the timeline,
        // and the timeline is the only axis the protocol has. Audio that
        // belongs nowhere is worse than no audio.
        error = "audio packet covers no timeline time";
        return Result::Invalid;
    }
    if (parsed.timelineDurationNs > kMaxPacketDurationNs)
    {
        error = "audio packet covers " + std::to_string(parsed.timelineDurationNs) +
                " ns, more than the " + std::to_string(kMaxPacketDurationNs) +
                " ns this client maps in one packet";
        return Result::Invalid;
    }
    if (parsed.timelineStartNs > UINT64_MAX - parsed.timelineDurationNs)
    {
        // The end of the packet is where the next one begins (section 14), so
        // a span that cannot be added up is one nothing can be placed after.
        error = "audio packet ends past the end of the timeline";
        return Result::Invalid;
    }

    // What the payload's own rate says the packet should be worth in media
    // time. Computed from the frame count rather than the other way round so
    // that a hostile sampleRate cannot overflow the multiply.
    const uint64_t impliedDurationNs =
        (static_cast<uint64_t>(parsed.outputFrameCount) * 1000000000ULL) /
        parsed.format.sampleRate;
    if (parsed.timelineDurationNs > impliedDurationNs * kMaxFrameDurationRatio ||
        impliedDurationNs > parsed.timelineDurationNs * kMaxFrameDurationRatio)
    {
        // Resampling this room moves the two by parts per million (section
        // 28), and a bitperfect room's agree exactly. A factor of two apart is
        // a sender that means something else by one of the fields, and
        // guessing which would put the whole room on the wrong part of the
        // timeline.
        error = "audio packet claims " + std::to_string(parsed.timelineDurationNs) +
                " ns for " + std::to_string(parsed.outputFrameCount) + " frame(s) at " +
                std::to_string(parsed.format.sampleRate) +
                " Hz, which no resampling ratio produces";
        return Result::Invalid;
    }
    if (parsed.payloadBytes !=
        static_cast<uint64_t>(parsed.outputFrameCount) * parsed.format.bytesPerFrame())
    {
        error = "audio packet says " + std::to_string(parsed.outputFrameCount) +
                " output frame(s) but " + std::to_string(parsed.payloadBytes) +
                " payload byte(s)";
        return Result::Invalid;
    }

    const size_t total = static_cast<size_t>(parsed.headerSize) + parsed.payloadBytes;
    if (available < total)
        return Result::Incomplete;

    out.header = parsed;
    out.payload = header + parsed.headerSize;
    out.payloadBytes = parsed.payloadBytes;
    consumed_ += total;
    return Result::Packet;
}

} // namespace matp
} // namespace multiroom
