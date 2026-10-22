#pragma once

// A seekable stream of bytes, with nothing said about where they come from.
//
// This interface exists so that libavformat never learns whether it is reading
// a file, a socket or a std::vector. The decoder reaches its bytes through a
// custom AVIOContext wrapped around one of these, which is what lets every
// decoder test run against MemoryMediaSource with no I/O at all -- no fixture
// on disk, no network, no fixed port.
//
// It is a *byte* interface on purpose. Nothing here knows about containers,
// codecs, frames or the timeline; a source that understood any of those would
// be a second demuxer, and there is a very good one in the dependency list
// already.
//
// **Stalling.** A zone's pipeline thread blocks inside read(), so a source
// that waits forever stops that zone forever. An implementation that can block
// on anything other than local I/O therefore bounds its own wait and reports
// Timeout; since phase 22 that ends one source with a reason, and its zone
// moves on to the next. The bound belongs to the implementation
// rather than to this call because only the implementation knows what it is
// waiting on and how to interrupt it -- FileMediaSource has no interruptible
// wait to bound, and HttpMediaSource has FFmpeg's interrupt callback.

#include "core/Error.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace multiroom {

enum class MediaSourceError
{
    InvalidArgument, // an empty path or URI, or a seek on a closed source
    NotFound,        // no such file, or a server that said so
    AccessDenied,
    OpenFailed,  // anything else that stopped the source from opening
    ReadFailed,  // the underlying read reported an error
    SeekFailed,  // the underlying seek reported an error
    NotSeekable, // seek() on a stream that cannot go backwards
    OutOfRange,  // a seek past the end of a source whose size is known
    Timeout      // the implementation's own deadline, not the caller's
};

const char* errorName(MediaSourceError error);

class IMediaSource
{
public:
    virtual ~IMediaSource() = default;

    // The path or URI, for a log line and for the reason a zone stopped. A
    // failure has to be attributable to one source by name.
    [[nodiscard]] virtual const std::string& describe() const noexcept = 0;

    [[nodiscard]] virtual bool seekable() const noexcept = 0;

    // Absent when the length is genuinely not known -- an HTTP response
    // without Content-Length, say. Never zero as a stand-in for unknown:
    // avformat reads a missing size as a stream it may not seek in, and a
    // reported zero as an empty file.
    [[nodiscard]] virtual std::optional<uint64_t> sizeBytes() const noexcept = 0;

    [[nodiscard]] virtual uint64_t position() const noexcept = 0;

    // Fills at most buffer.size() bytes and reports how many. A short read is
    // normal and is not the end of the stream; **zero is** the end, and it is
    // the only way the end is reported, because a container that ends where
    // the caller expected it to is not a failure.
    [[nodiscard]] virtual Result<size_t, MediaSourceError> read(std::span<std::byte> buffer) = 0;

    // Absolute, from the start of the source. Seeking exactly to the end is
    // allowed and leaves the next read at the end of the stream; past it is
    // OutOfRange where the size is known.
    [[nodiscard]] virtual Status<MediaSourceError> seek(uint64_t offset) = 0;

protected:
    IMediaSource() = default;
    IMediaSource(const IMediaSource&) = delete;
    IMediaSource& operator=(const IMediaSource&) = delete;
};

} // namespace multiroom
