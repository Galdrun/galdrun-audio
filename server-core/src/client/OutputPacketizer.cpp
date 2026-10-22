#include "client/OutputPacketizer.h"

#include "matp/AudioPacket.h"
#include "matp/Protocol.h"

#include <limits>

namespace multiroom {

const char* errorName(PacketizerError error)
{
    switch (error)
    {
    case PacketizerError::InvalidFormat:
        return "InvalidFormat";
    case PacketizerError::InvalidSession:
        return "InvalidSession";
    case PacketizerError::EmptyPacket:
        return "EmptyPacket";
    case PacketizerError::EmptyDuration:
        return "EmptyDuration";
    case PacketizerError::DurationTooLong:
        return "DurationTooLong";
    case PacketizerError::PayloadMismatch:
        return "PayloadMismatch";
    case PacketizerError::PayloadTooLarge:
        return "PayloadTooLarge";
    case PacketizerError::FrameCountImplausible:
        return "FrameCountImplausible";
    case PacketizerError::TimelineOverflow:
        return "TimelineOverflow";
    }
    return "Unknown";
}

OutputPacketizer::OutputPacketizer(SessionId session, AudioFormat format) noexcept
    : session_(session), format_(format)
{
}

Result<std::string, PacketizerError> OutputPacketizer::encode(TimelineNs start,
                                                              uint64_t durationNs,
                                                              uint32_t frameCount,
                                                              std::span<const std::byte> payload)
{
    if (!format_.isValid())
        return fail(PacketizerError::InvalidFormat, describeFormat(format_));
    if (!session_.valid())
    {
        return fail(PacketizerError::InvalidSession,
                    "section 13's all-zero id is not a session to send audio for");
    }
    if (frameCount == 0)
        return fail(PacketizerError::EmptyPacket, "a packet with no frames is not audio");
    if (durationNs == 0)
    {
        return fail(PacketizerError::EmptyDuration,
                    "a packet covering no timeline time cannot be placed on the timeline");
    }
    if (durationNs > matp::kMaxPacketDurationNs)
    {
        return fail(PacketizerError::DurationTooLong,
                    std::to_string(durationNs) + " ns is more than the " +
                        std::to_string(matp::kMaxPacketDurationNs) +
                        " ns a receiver maps in one packet");
    }

    const uint64_t expectedBytes =
        static_cast<uint64_t>(frameCount) * static_cast<uint64_t>(format_.bytesPerFrame());
    if (payload.size() != expectedBytes)
    {
        return fail(PacketizerError::PayloadMismatch,
                    std::to_string(payload.size()) + " byte(s) for " +
                        std::to_string(frameCount) + " frame(s) of " +
                        describeFormat(format_) + ", which is " +
                        std::to_string(expectedBytes));
    }
    if (expectedBytes > matp::kMaxAudioPayloadBytes)
    {
        return fail(PacketizerError::PayloadTooLarge,
                    std::to_string(expectedBytes) + " bytes exceeds the " +
                        std::to_string(matp::kMaxAudioPayloadBytes) + " byte limit");
    }

    // The gross plausibility gate the receiver applies, applied here so that a
    // packet this server would not accept is a failure with a name rather than
    // a room that dropped its connection. Resampling and drift correction move
    // the two by parts per million; a factor of two apart means one of the
    // fields means something else.
    const uint64_t impliedNs =
        (static_cast<uint64_t>(frameCount) * kNanosecondsPerSecond) / format_.sampleRate;
    if (durationNs > impliedNs * matp::kMaxFrameDurationRatio ||
        impliedNs > durationNs * matp::kMaxFrameDurationRatio)
    {
        return fail(PacketizerError::FrameCountImplausible,
                    std::to_string(durationNs) + " ns for " + std::to_string(frameCount) +
                        " frame(s) at " + std::to_string(format_.sampleRate) +
                        " Hz, which no resampling ratio produces");
    }

    const uint64_t startNs = std::to_underlying(start);
    if (startNs > std::numeric_limits<uint64_t>::max() - durationNs)
    {
        // The end of a packet is where the next one begins (section 14), so a
        // span that cannot be added up is one nothing can follow.
        return fail(PacketizerError::TimelineOverflow,
                    "a packet at " + std::to_string(startNs) + " ns for " +
                        std::to_string(durationNs) + " ns ends past the end of the timeline");
    }

    matp::AudioPacketHeader header;
    header.protocolVersion = matp::kProtocolVersion;
    header.sessionIdHigh = session_.high();
    header.sessionIdLow = session_.low();
    header.sequenceNumber = sequence_;
    header.timelineStartNs = startNs;
    header.timelineDurationNs = durationNs;
    header.format = format_;
    header.outputFrameCount = frameCount;
    header.flags = afterFlush_ ? matp::kAudioFlagAfterFlush : 0u;
    // headerSize, payloadBytes and headerCrc are filled in by the encoder from
    // the payload and the layout, so a packet cannot contradict itself.

    std::string packet = matp::encodeAudioPacket(header, payload.data(), payload.size());

    afterFlush_ = false;
    ++sequence_;
    ++packets_;
    frames_ += frameCount;
    payloadBytes_ += payload.size();
    return packet;
}

} // namespace multiroom
