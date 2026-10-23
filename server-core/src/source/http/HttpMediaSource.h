#pragma once

// HTTP and HTTPS behind IMediaSource, on FFmpeg's avio.
//
// The double wrap is deliberate, and the roadmap argues it: writing an
// HTTP/1.1 client with ranged requests is a day's work, writing the TLS half
// of an HTTPS one is not, and the high-end server may well hand this one an
// https:// URI. FFmpeg has both already and is a dependency taken anyway, so
// the price is one buffer copy per read. The alternative -- giving the URI
// straight to avformat_open_input -- is rejected because then the decode layer
// knows what a URL is and IMediaSource becomes decoration.
//
// This header lives beside its source, like the resampler adapters, so that no
// FFmpeg header is reachable from include/.
//
// **This is the source that can stall**, and therefore the one that bounds its
// own wait. FFmpeg's AVIOInterruptCB is polled from inside a blocking protocol
// call, so the deadline is enforced where the wait actually happens rather
// than by a caller that is itself blocked. The clock is injected, as
// everywhere else in this server, which is what lets the timeout be tested by
// moving a number.
//
// **A broken fetch is tried again here, and nowhere else** (phase 22). What
// the media endpoint contract calls transient -- a 5xx or a 429 at open, a
// connection that drops or stalls mid-stream, and since phase 28 any failure
// with no HTTP response behind it (refused, reset, a name that does not
// resolve), on open and on resume alike -- is retried up to the
// configured count, with a short backoff, before it becomes the source's
// failure. A mid-stream retry is a new connection asking for
// `Range: bytes=<position>-`, and it is taken only if this source said it was
// seekable: a live stream resumed from anywhere else is a different stream.
// The resume is refused when the answer does not start at that byte -- FFmpeg
// checks the Content-Range itself and fails a 200 -- or reports another
// length, because the bytes after it would not be the bytes the decoder is in
// the middle of. FFmpeg's own `reconnect` stays off: it cannot be bounded by
// our count and does not check the length.
//
// The protocol whitelist is not a formality. Without one, a URI that arrives
// from the control API could name `file:` or `concat:` and turn a media
// request into a read of the server's filesystem.

#include "source/IMediaSource.h"
#include "source/MediaOrigin.h"

#include <memory>

struct AVIOContext;

namespace multiroom {

class IClock;

class HttpMediaSource final : public IMediaSource
{
public:
    // The first retry waits this long, the second twice as long, and so on:
    // short, because a zone that is retrying is a zone whose rooms are
    // draining, and the endpoint answers transient faults that will not clear
    // within a few seconds with 404 (phase 21).
    static constexpr uint64_t kBackoffStepMs = 100;

    // `clock` must outlive the source. It is the same monotonic clock every
    // other timeout in this server reads.
    static Result<std::unique_ptr<HttpMediaSource>, MediaSourceError>
    open(const std::string& uri, const IClock& clock, const MediaFetchSettings& settings = {});

    ~HttpMediaSource() override;

    const std::string& describe() const noexcept override { return uri_; }
    bool seekable() const noexcept override { return seekable_; }
    std::optional<uint64_t> sizeBytes() const noexcept override { return size_; }
    uint64_t position() const noexcept override { return position_; }

    Result<size_t, MediaSourceError> read(std::span<std::byte> buffer) override;
    Status<MediaSourceError> seek(uint64_t offset) override;

private:
    HttpMediaSource(std::string uri, const IClock& clock, const MediaFetchSettings& settings);

    // AVIOInterruptCB: non-zero aborts whatever FFmpeg is waiting on.
    static int interrupted(void* opaque);

    // Restarts the deadline for one call. A timeout is per operation, not per
    // source: a slow but progressing download must not be killed for having
    // taken a long time in total.
    void startDeadline() noexcept;

    // A connection whose body starts at position_, retrying what is transient.
    Status<MediaSourceError> connect();

    // The broken connection replaced by one from position_, refused as
    // `failure` where the resource's length is not the one it was opened with:
    // that is another file, and its bytes are not the rest of this one.
    Status<MediaSourceError> reconnect(MediaSourceError failure);

    // Whether another attempt is allowed; if so, counts it and waits its
    // backoff first.
    bool mayRetry();

    AVIOContext* context_ = nullptr;
    std::string uri_;
    std::string headers_; // phase 23's Authorization line, or empty; never logged
    const IClock* clock_ = nullptr;
    uint64_t timeoutNs_ = 0;
    uint32_t retries_ = 0;
    uint32_t failures_ = 0; // consecutive; reset by a read that returned bytes
    uint64_t deadlineNs_ = 0;
    uint64_t position_ = 0;
    std::optional<uint64_t> size_;
    bool seekable_ = false;
    bool timedOut_ = false;
    bool polled_ = false; // FFmpeg has waited on a socket since the attempt began (phase 28)
};

} // namespace multiroom
