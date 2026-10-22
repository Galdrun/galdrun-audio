#pragma once

// One room's audio packet stream: the 72-byte header of MATP section 14 in
// front of that room's own PCM, numbered and placed on the zone's timeline.
//
// The bytes themselves are `shared/matp`'s -- the client compiles the same
// encoder and the same reader, so magic, layout, byte order and CRC cannot
// disagree between the two ends. What this class adds is the state one stream
// carries and the encoder deliberately does not: the session id every packet
// of this stream names, the sequence number, and the refusal to emit a packet
// the client's own `AudioPacketReader` would reject.
//
// **The refusals are the point.** The reader on the other end drops the
// connection on a packet that carries no frames, covers no timeline time,
// disagrees with its own payload length or claims a frame count its rate
// cannot account for -- and a server that produced one would find out about it
// as a room that went quiet. Every one of those is checked here, where the
// packet is built and the fault has a name, rather than on the wire.
//
// **`headerCrc` is computed and never zeroed.** Section 42 makes it optional,
// TCP already protects the bytes, and the client verifies it when it is
// non-zero: through a first bring-up it is the cheapest detector there is of a
// framing bug on this side, and it costs sixty-eight bytes of CRC fifty times
// a second.
//
// It holds no clock, no socket and no buffer. What a packet covers is decided
// by client/OutputClock and by whoever owns the audio; this turns that
// decision into bytes.

#include "audio/AudioFormat.h"
#include "core/Error.h"
#include "core/Ids.h"
#include "core/Units.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace multiroom {

enum class PacketizerError
{
    InvalidFormat,    // zero rate or zero channels; a format never validated
    InvalidSession,   // a default-constructed session id: section 13's "no session"
    EmptyPacket,      // no frames, which the client refuses and this will not send
    EmptyDuration,    // a span of no timeline time, which nothing can be placed on
    DurationTooLong,  // past what one packet may cover (kMaxPacketDurationNs)
    PayloadMismatch,  // the payload is not frameCount frames of this format
    PayloadTooLarge,  // past the reader's payload ceiling
    FrameCountImplausible, // frames and duration more than a factor of two apart
    TimelineOverflow  // a packet that ends past the end of the timeline
};

const char* errorName(PacketizerError error);

class OutputPacketizer
{
public:
    // The session id is the one SESSION_CONFIG named, as the 128 bits it was
    // generated from rather than as the hex string that was sent: the client
    // compares the header's id for *change* rather than against the string,
    // which is exactly what one id in two shapes supports (section 13).
    OutputPacketizer(SessionId session, AudioFormat format) noexcept;

    // One packet, ready to write. `payload` is this room's own PCM in this
    // room's own format -- `frameCount` frames of it, exactly.
    //
    // The timeline span is the caller's and is never derived from the frame
    // count: at 44.1 kHz 882 frames are 20,000,000 ns by the zone's reckoning
    // and 19,999,999.99 ns by the payload's, and section 15 is explicit about
    // which of the two a receiver must believe.
    [[nodiscard]] Result<std::string, PacketizerError> encode(TimelineNs start,
                                                              uint64_t durationNs,
                                                              uint32_t frameCount,
                                                              std::span<const std::byte> payload);

    // The number the next packet will carry. A stream numbers from zero and
    // never restarts: the client anchors on the first packet of a connection
    // and only requires the ones after it to follow.
    [[nodiscard]] uint64_t nextSequence() const noexcept { return sequence_; }

    [[nodiscard]] uint64_t packets() const noexcept { return packets_; }
    [[nodiscard]] uint64_t frames() const noexcept { return frames_; }
    [[nodiscard]] uint64_t payloadBytes() const noexcept { return payloadBytes_; }

    [[nodiscard]] const AudioFormat& format() const noexcept { return format_; }
    [[nodiscard]] const SessionId& session() const noexcept { return session_; }

    // Section 24's transition, from this end: this room's device was reopened
    // in another format and the packets that follow it carry that one. The
    // sequence deliberately does **not** restart with the format -- the client
    // anchors on the first packet of a connection and reads a gap in the
    // numbering on one connection as a server that skipped ahead, and the
    // audio connection outlives the device that was reopened on it.
    void setFormat(const AudioFormat& format) noexcept { format_ = format; }

    // Packets that were built and then thrown away before they reached the
    // socket: a room that was parked (section 38), stranded (the decode ran
    // past it) or reopened in another format (section 24) drops what it had
    // queued, and those numbers were never on the wire.
    //
    // It has to be given back, and this is the only place it can be. The
    // client anchors the numbering on the first packet of a connection and
    // reads a gap in it as a server that skipped ahead -- not as a packet to
    // ignore, but as a reason to drop the **connection**. What was dropped is
    // always the newest end of the queue, so the numbers handed back are the
    // last ones issued and nothing has been built past them.
    //
    // `packets()` is not wound back with it: that counter is what this
    // packetizer built, which is a different question from what a room was
    // sent.
    void rewind(uint64_t count) noexcept { sequence_ -= count <= sequence_ ? count : sequence_; }

    // Section 54 (phase 26): the next packet built carries `afterFlush`, and
    // the ones after it do not. It is what lets the receiver tell the jump to
    // the seeked position from a rejoin the moment it reads the header, while
    // the FLUSH that explains it may still be on the control connection. A
    // flagged packet that is dropped before the socket takes it hands its
    // number back and not its flag: whatever moved the stream again is what
    // decides what the next packet is.
    void markAfterFlush() noexcept { afterFlush_ = true; }
    [[nodiscard]] bool afterFlushPending() const noexcept { return afterFlush_; }

private:
    SessionId session_;
    AudioFormat format_;

    uint64_t sequence_ = 0;
    uint64_t packets_ = 0;
    uint64_t frames_ = 0;
    uint64_t payloadBytes_ = 0;
    bool afterFlush_ = false;
};

} // namespace multiroom
