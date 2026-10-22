#pragma once

// A container and a codec turned into the source's own PCM, and nothing else.
//
// **The contract is native PCM.** The source's own sample rate, channel count,
// channel order and sample format come out unchanged; the only thing a decoder
// is allowed to change is a planar layout into an interleaved one, which moves
// bytes without touching a value and is what the wire format needs anyway. No
// resampling, no channel mixing, no format conversion, and in particular no
// libswresample: a decoder that quietly produced 48 kHz float would be how
// this server ends up resampling everything without anybody having decided to,
// and the bitperfect path (phase 6) would have nothing left to be bitperfect
// about.
//
// What to do when the source's format does not fit a room is phase 6's and
// phase 7's question. This layer's job is to report the format honestly and to
// hand over the samples that were in the file.

#include "audio/AudioFormat.h"
#include "core/Error.h"
#include "core/Units.h"
#include "source/IMediaSource.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace multiroom {

enum class DecodeError
{
    AlreadyOpened,
    NotOpened, // decode() before a successful open()

    SourceFailed, // the IMediaSource refused; its own describe() is the detail

    // This build has no decoder at all (MULTIROOM_WITH_FFMPEG off). Refused by
    // name rather than by an empty result, because "nothing plays in this
    // build" and "this file is broken" send an operator to two different
    // places.
    NoDecoder,

    UnknownContainer,  // avformat could not identify the stream at all
    NoAudioStream,     // a container with no audio in it
    UnsupportedCodec,  // no decoder for that codec in this build
    UnsupportedFormat, // a sample layout MATP section 12 cannot carry
    InvalidChannels,   // zero channels, or more than an AudioFormat holds

    BufferTooSmall, // the caller's span does not hold one whole frame
    DecodeFailed,   // the library refused a packet or a frame mid-stream

    // Phase 26: an open that was asked to begin somewhere other than the first
    // frame. A source the media endpoint contract calls non-seekable -- no
    // `Range`, no length -- cannot begin anywhere else, and an offset at or
    // past a length the container states names no frame at all. Both are
    // refused by name because "not possible for this source" and "not a
    // position in it" are answered differently to whoever asked.
    NotSeekable,
    OffsetOutOfRange,

    // The source ran out, cleanly. The only code a zone treats as the end of a
    // segment rather than as a fault, which is why it is a code and not a zero
    // frame count: a decoder legitimately returns no frames from a packet that
    // was only a header, and the two must not look alike.
    EndOfStream
};

const char* errorName(DecodeError error);

// What the container said about the audio, read once at open().
struct SourceStreamInfo
{
    // Native, in every field. This is the format phase 6 plans a room's output
    // against and the format a bitperfect room has to match exactly.
    AudioFormat format;

    // Absent when the container does not say, which is normal for a stream and
    // common for an HTTP source. Omitted rather than reported as zero: a zero
    // here reads as an empty track, and the roadmap's rule is that an unknown
    // value is left out.
    std::optional<uint64_t> totalFrames;

    // For a log line and the status API: "flac", "pcm_s24le", "wav".
    std::string codecName;
    std::string containerName;

    // What the encoder actually used, where the container says so -- 24 for a
    // 24-bit FLAC that arrives in a 32-bit container. Zero when unknown.
    //
    // It is kept beside the format rather than folded into it because the two
    // answer different questions: `format.sampleType` is the byte layout this
    // decoder produces, and this is the resolution that was in the file. They
    // agree for every format this server can emit, and the day they do not is
    // the day the status API has to say so.
    uint32_t bitsPerRawSample = 0;
};

// One call's worth of audio, in the caller's buffer.
struct DecodedSpan
{
    SourceFrame firstFrame; // position in the source, counted from zero
    uint32_t frameCount = 0;
    size_t byteCount = 0;
};

class IDecoder
{
public:
    virtual ~IDecoder() = default;

    // Takes the source over, because the decoder reads from it for as long as
    // it lives and a source shared with anything else would be two cursors on
    // one stream.
    virtual Status<DecodeError> open(std::unique_ptr<IMediaSource> source) = 0;

    [[nodiscard]] virtual bool opened() const noexcept = 0;

    // Valid only after a successful open().
    [[nodiscard]] virtual const SourceStreamInfo& info() const noexcept = 0;

    // Fills as much of `output` as the next decoded audio occupies, and never
    // more than a whole number of frames. A call returns at least one frame or
    // it fails: a decoder that returned zero frames without failing would make
    // the pipeline spin, so the internal packet loop runs until there is audio
    // or the source ends.
    //
    // `output` is an AudioBlock's writable span from phase 5 onwards; here it
    // is whatever the caller allocated. Anything shorter than one frame is
    // BufferTooSmall rather than a silent zero-length read.
    [[nodiscard]] virtual Result<DecodedSpan, DecodeError> decode(std::span<std::byte> output) = 0;

protected:
    IDecoder() = default;
    IDecoder(const IDecoder&) = delete;
    IDecoder& operator=(const IDecoder&) = delete;
};

} // namespace multiroom
