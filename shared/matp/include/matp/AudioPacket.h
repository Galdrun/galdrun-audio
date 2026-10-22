#pragma once

// The audio connection's wire format: the binary packet header of MATP
// section 14 and the PCM payload behind it.
//
// Pure, like matp/Protocol.h: it builds and parses bytes and knows nothing
// about sockets, ring buffers or devices. The control connection's JSON
// framing lives next door; this one is separate because it is the only part of
// the protocol that is not JSON and the only part a packet-per-20-ms rate
// makes worth keeping cheap.
//
// Layout, big endian throughout, which is the byte order section 14 asks for
// and the one the control framing already uses:
//
//   offset  size  field
//        0     4  magic ('MATP')
//        4     2  protocolVersion
//        6     2  headerSize
//        8     8  sessionIdHigh
//       16     8  sessionIdLow
//       24     8  sequenceNumber
//       32     8  timelineStartNs
//       40     8  timelineDurationNs
//       48     4  sampleRate
//       52     2  channels
//       54     2  sampleFormat
//       56     4  outputFrameCount
//       60     4  payloadBytes
//       64     4  flags
//       68     4  headerCrc
//       72        payload
//
// Five things section 14 leaves open, and a client cannot:
//
//   * **A packet spans two axes and has to say both.** The zone's timeline is
//     media time in nanoseconds and every room shares it; the payload is PCM
//     frames of one room's own stream, at that room's own rate, after whatever
//     resampling that room is getting (sections 5, 15 and 33). So
//     `timelineStartNs` and `timelineDurationNs` place the packet on the
//     timeline, `outputFrameCount` says how many frames were actually sent
//     here, and `payloadBytes` follows the output count because that is what
//     the payload holds. Timeline progress is `timelineStartNs +
//     timelineDurationNs` and is never computed from the frame count and the
//     rate -- 480 frames at 44,100 Hz is 10,884,353.7 ns, and a receiver that
//     rounded that per packet would build its own drift. matp/TimelineMap.h is
//     what the client converts between the two axes with.
//   * `sampleFormat` is a number on the wire and a name in section 12. The
//     codes below are this client's, assigned in the order of that section's
//     table, for the same reason MessageType's numbers are: the protocol gives
//     none. A code this client does not know is refused rather than guessed --
//     the layout decides how the bytes are read, and s24le3 read as s24le is
//     noise.
//   * `headerCrc` is optional (section 42). Zero means "not supplied"; any
//     other value is verified and a mismatch is treated as broken framing,
//     which is exactly what section 42 wants it for.
//   * `flags` has one bit defined, kAudioFlagAfterFlush. A packet that sets
//     any other is asking for something this client cannot do, and it is
//     refused rather than decoded as if the flag had not been there.
//   * **A packet's duration is bounded** (kMaxPacketDurationNs). Section 15
//     asks for 10-20 ms and says nothing about a ceiling, but the timeline
//     arithmetic downstream multiplies a duration by a frame count, so an
//     unbounded one is an overflow waiting for a hostile or broken sender.

#include "audio/AudioFormat.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace multiroom {
namespace matp {

// 'M' 'A' 'T' 'P'.
const uint32_t kAudioPacketMagic = 0x4D415450u;

// Everything up to and including headerCrc. A server may send a longer header;
// the extra bytes are skipped, which is what headerSize is for.
const size_t kAudioHeaderBytes = 72;

// The CRC covers the header up to but not including headerCrc itself, so a
// longer header from a later protocol version still checks out.
const size_t kAudioHeaderCrcBytes = 68;

// Section 15 recommends 10-20 ms per packet, which is 7,680 bytes at 48 kHz
// stereo 32-bit. Anything near a megabyte is a desync or a hostile length.
const uint32_t kMaxAudioPayloadBytes = 1024 * 1024;

// Fifty times what section 15 recommends per packet. Not a protocol limit: it
// is what keeps `duration * frameCount` inside 64 bits everywhere the mapping
// interpolates, and a sender that wants more than a second of audio in one
// packet has already given up on the 10-20 ms the section asks for.
const uint64_t kMaxPacketDurationNs = 1000000000ULL;

// How far a packet's frame count may disagree with the duration its own sample
// rate implies before the header is refused. Per-client resampling moves the
// two by parts per million (section 28), so a factor of two is not a tolerance
// -- it is the gross plausibility gate DriftEstimator uses for the same
// reason: what it catches is a sender that means something else by one of the
// fields, not a ratio that drifted.
const uint64_t kMaxFrameDurationRatio = 2;

// Bit 0 of `flags`: the first packet after a FLUSH (section 54). It begins at
// the position the server seeked to, and the receiver has to tell that jump
// from a rejoin the moment it reads the header -- a rejoin drops the start
// mapping and a flush keeps it -- while the FLUSH that explains it may still
// be on its way over the control connection.
const uint32_t kAudioFlagAfterFlush = 1u;

struct AudioPacketHeader
{
    uint16_t protocolVersion = 0;
    uint16_t headerSize = 0;

    // Section 13's 128-bit session id. Kept as the two halves it is sent as:
    // the client compares it for change rather than for meaning (see
    // AudioStream), so there is nothing to be gained from a richer type.
    uint64_t sessionIdHigh = 0;
    uint64_t sessionIdLow = 0;

    uint64_t sequenceNumber = 0;

    // Where the packet begins on the zone's media timeline, and how much of
    // that timeline it covers, both in nanoseconds. These are the protocol's
    // numbers: every room's copy of this audio carries the same two, whatever
    // that room's rate and resampling ratio did to the payload.
    uint64_t timelineStartNs = 0;
    uint64_t timelineDurationNs = 0;

    // sampleRate, channels and sampleFormat of the wire header, in the
    // client's own type. What the packet is in, which must be what the client
    // confirmed in STREAM_READY -- and for a bitperfect room that is the
    // source's own format rather than a rate the zone agreed on.
    AudioFormat format;

    // Frames actually in the payload, at the rate above. Internal to the
    // client in every other respect; it travels on the wire only because the
    // receiver has to know how to cut the bytes up.
    uint32_t outputFrameCount = 0;

    uint32_t payloadBytes = 0;
    uint32_t flags = 0;
    uint32_t headerCrc = 0; // 0 means the sender did not supply one
};

// The wire numbers for section 12's sample format names.
uint16_t sampleFormatCode(AudioFormat::SampleType type);
bool parseSampleFormatCode(uint16_t code, AudioFormat::SampleType& out);

// Builds one packet. headerSize, payloadBytes and headerCrc are filled in from
// the payload and the layout above rather than taken from the header, so a
// caller cannot produce a packet that contradicts itself.
std::string encodeAudioPacket(const AudioPacketHeader& header, const void* payload,
                              size_t payloadBytes);

// One decoded packet. The payload points into the reader that produced it and
// stays valid until that reader is next appended to, read from or reset --
// copying it here would put a memcpy of every packet in front of the one the
// ring buffer already does.
struct AudioPacket
{
    AudioPacketHeader header;
    const unsigned char* payload = nullptr;
    size_t payloadBytes = 0;
};

// The receiving half: bytes in, whole packets out. Same shape as FrameReader,
// and for the same reason -- one read is not one packet.
class AudioPacketReader
{
public:
    enum class Result
    {
        Packet,     // out was filled
        Incomplete, // the rest of the packet has not arrived yet
        Invalid     // the stream is out of step; only a new connection fixes it
    };

    void append(const void* data, size_t size);

    Result next(AudioPacket& out, std::string& error);

    void reset();

    size_t buffered() const { return buffer_.size() - consumed_; }

private:
    void compact(size_t incoming);

    // Packets are consumed by moving an offset rather than by erasing from the
    // front: at fifty packets a second an erase would memmove the tail of
    // the buffer that often, for nothing.
    std::string buffer_;
    size_t consumed_ = 0;
};

} // namespace matp
} // namespace multiroom
