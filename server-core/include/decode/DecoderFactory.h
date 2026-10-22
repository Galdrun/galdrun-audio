#pragma once

// A media URI turned into an opened decoder, and the one place a scheme
// becomes a source.
//
// A zone stages a `mediaUri` and never a file handle: the high-end server
// above this one names what to play, and the pipeline thread opens it when the
// previous source has ended. Something has to turn that string into an
// IMediaSource and an IDecoder, and it cannot be the zone -- `zone/` would
// then see an FFmpeg header, which is the one thing `src/decode/ffmpeg/` and
// `src/source/http/` exist to prevent.
//
// It is an interface rather than a free function because the zone tests need a
// decoder that produces a known signal and counts its own calls. "One decode
// for many rooms" is proved by a counter, and a counter needs somewhere to
// live; a test factory is that somewhere, and it is also what keeps every zone
// test out of the MULTIROOM_WITH_FFMPEG branch.
//
// **Opening is synchronous and may block**, because IMediaSource::read may:
// the pipeline thread is the one that calls this, and a source that can wait
// on a network bounds its own wait and reports Timeout (see IMediaSource).

#include "core/Error.h"
#include "decode/IDecoder.h"
#include "source/MediaOrigin.h"
#include "util/Clock.h"

#include <cstdint>
#include <memory>
#include <string>

namespace multiroom {

class IDecoderFactory
{
public:
    virtual ~IDecoderFactory() = default;

    // Opens `uri` and returns a decoder positioned `startOffsetNs` into it --
    // at the frame that offset falls in, rounded down -- with info() already
    // answerable. A failure names the source rather than the library: a path
    // that does not exist and a container FFmpeg cannot identify send an
    // operator to two different places.
    //
    // Phase 26: an offset is a seek, so a non-zero one on a source that cannot
    // seek is NotSeekable and one at or past a stated length is
    // OffsetOutOfRange. The decoder that comes back numbers its frames from
    // the offset's frame, and info() still describes the whole source.
    [[nodiscard]] virtual Result<std::unique_ptr<IDecoder>, DecodeError>
    open(const std::string& uri, uint64_t startOffsetNs) = 0;

protected:
    IDecoderFactory() = default;
    IDecoderFactory(const IDecoderFactory&) = delete;
    IDecoderFactory& operator=(const IDecoderFactory&) = delete;
};

// The real one: `http://` and `https://` reach HttpMediaSource and are
// decoded by FfmpegDecoder, and everything else -- a path, `file:` -- is
// refused as SourceFailed with the URI named. Since phase 20 the core never
// opens a file it was named: the high-end server serves the local library
// over HTTP like everything else. `clock` is the monotonic clock the HTTP
// source's own deadline reads and must outlive the factory; `fetch` is how
// long that source waits and how often it tries again (phase 22).
//
// In a build without FFmpeg it exists and refuses every URI with NoDecoder,
// rather than not existing: a server that silently played nothing would be the
// half-finished API rule broken in the one place it matters most.
[[nodiscard]] std::unique_ptr<IDecoderFactory> makeMediaDecoderFactory(const IClock& clock, MediaFetchSettings fetch = {});

} // namespace multiroom
