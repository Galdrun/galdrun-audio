#include "source/http/HttpMediaSource.h"

#include "util/Clock.h"
#include "util/Logger.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <mutex>
#include <thread>
#include <utility>

extern "C" {
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
}

namespace multiroom {
namespace {

std::string avErrorText(int code)
{
    char text[AV_ERROR_MAX_STRING_SIZE] = {};
    if (av_strerror(code, text, sizeof(text)) < 0)
        return "AVERROR " + std::to_string(code);
    return text;
}

bool isHttpUri(const std::string& uri)
{
    return uri.rfind("http://", 0) == 0 || uri.rfind("https://", 0) == 0;
}

void initialiseNetwork()
{
    static std::once_flag once;
    std::call_once(once, [] { avformat_network_init(); });
}

// Whether the FFmpeg this was linked against knows `max_redirects`. It is the
// only thing that stops FFmpeg following a redirect round the origin check
// (connect()), and avio_open2 leaves an option it does not know unused rather
// than refusing it -- so a system FFmpeg without it would follow one silently.
bool refusesRedirects()
{
    const AVClass* http = avio_protocol_get_class("http");
    return http != nullptr &&
           av_opt_find(&http, "max_redirects", nullptr, 0, AV_OPT_SEARCH_FAKE_OBJ) != nullptr;
}

} // namespace

HttpMediaSource::HttpMediaSource(std::string uri, const IClock& clock,
                                 const MediaFetchSettings& settings)
    : uri_(std::move(uri)),
      headers_(settings.token.empty() ? std::string{}
                                      : "Authorization: Bearer " + settings.token + "\r\n"),
      clock_(&clock),
      timeoutNs_(static_cast<uint64_t>(settings.stallTimeoutMs) * 1'000'000ULL),
      retries_(settings.retries)
{
}

HttpMediaSource::~HttpMediaSource()
{
    if (context_ != nullptr)
        avio_closep(&context_);
}

int HttpMediaSource::interrupted(void* opaque)
{
    auto* self = static_cast<HttpMediaSource*>(opaque);
    self->polled_ = true;
    if (self->timeoutNs_ == 0)
        return 0;
    if (self->clock_->monotonicNowNs() < self->deadlineNs_)
        return 0;

    // Remembered because FFmpeg reports an interrupted call as a generic exit
    // code, and "the server went quiet" is a different fault from "the server
    // said no".
    self->timedOut_ = true;
    return 1;
}

void HttpMediaSource::startDeadline() noexcept
{
    timedOut_ = false;
    deadlineNs_ = timeoutNs_ == 0 ? 0 : clock_->monotonicNowNs() + timeoutNs_;
}

bool HttpMediaSource::mayRetry()
{
    if (failures_ >= retries_)
        return false;
    ++failures_;
    std::this_thread::sleep_for(std::chrono::milliseconds(kBackoffStepMs * failures_));
    return true;
}

Result<std::unique_ptr<HttpMediaSource>, MediaSourceError>
HttpMediaSource::open(const std::string& uri, const IClock& clock,
                      const MediaFetchSettings& settings)
{
    if (!isHttpUri(uri))
        return fail(MediaSourceError::InvalidArgument, "not an http or https URI: " + uri);

    initialiseNetwork();
    if (!refusesRedirects())
    {
        return fail(MediaSourceError::OpenFailed,
                    "this server's FFmpeg cannot be told not to follow redirects, so no "
                    "media is fetched with it");
    }

    auto source = std::unique_ptr<HttpMediaSource>(new HttpMediaSource(uri, clock, settings));
    if (auto connected = source->connect(); !connected)
        return std::unexpected(connected.error());

    source->seekable_ = (source->context_->seekable & AVIO_SEEKABLE_NORMAL) != 0;
    const int64_t size = avio_size(source->context_);
    if (size > 0)
        source->size_ = static_cast<uint64_t>(size);

    return source;
}

Status<MediaSourceError> HttpMediaSource::connect()
{
    for (;;)
    {
        // A live deadline before the open, so that the interrupt callback
        // bounds avio_open2's own connect and first read.
        startDeadline();

        AVIOInterruptCB interrupt{};
        interrupt.callback = &HttpMediaSource::interrupted;
        interrupt.opaque = this;

        AVDictionary* options = nullptr;
        // Without this a URI from the control API could name file: or concat:
        // and turn a media request into a read of this server's filesystem.
        av_dict_set(&options, "protocol_whitelist", "http,https,tcp,tls", 0);
        av_dict_set_int(&options, "rw_timeout", static_cast<int64_t>(timeoutNs_ / 1000), 0);
        av_dict_set(&options, "reconnect", "0", 0);
        // FFmpeg follows a redirect on its own, and one to another host would
        // walk round the origin check the control API made (phase 20). The
        // high-end server has no reason to redirect, so none is followed.
        av_dict_set(&options, "max_redirects", "0", 0);
        // Track metadata comes through source.stage; the contract tells the
        // endpoint to ignore this header, and now it is not sent (phase 21).
        av_dict_set(&options, "icy", "0", 0);
        // Phase 23. Kept on the context, so FFmpeg's own seeks reconnect
        // with it too; it logs a request only at debug, which the log bridge
        // never passes on.
        if (!headers_.empty())
            av_dict_set(&options, "headers", headers_.c_str(), 0);
        // A resume. FFmpeg sends `Range: bytes=<offset>-` and fails the open
        // with EIO unless the Content-Range it gets back starts there -- which
        // is how a 200 to a ranged request is refused.
        if (position_ > 0)
            av_dict_set_int(&options, "offset", static_cast<int64_t>(position_), 0);

        AVIOContext* context = nullptr;
        polled_ = false;
        const int code = avio_open2(&context, uri_.c_str(), AVIO_FLAG_READ, &interrupt, &options);
        av_dict_free(&options);

        if (code >= 0)
        {
            context_ = context;
            return {};
        }

        const bool stalled = timedOut_ || code == AVERROR_EXIT || code == AVERROR(ETIMEDOUT);

        // Phase 28: judged by where it failed, not by the error number. An
        // HTTP status comes back as one of FFmpeg's AVERROR_HTTP_ codes, or as
        // a bare EIO for one it has no code for (a redirect, an offset it did
        // not ask for). But a failed name lookup is a bare EIO too, and it
        // fails before FFmpeg has waited on any socket -- so before this
        // callback was ever polled. Everything else -- a refusal, a reset, a
        // connection closed before its status line, whatever number the OS
        // gave it -- had no response behind it and may be gone next time.
        const bool answered = code == AVERROR_HTTP_BAD_REQUEST ||
                              code == AVERROR_HTTP_UNAUTHORIZED ||
                              code == AVERROR_HTTP_FORBIDDEN || code == AVERROR_HTTP_NOT_FOUND ||
                              code == AVERROR_HTTP_TOO_MANY_REQUESTS ||
                              code == AVERROR_HTTP_OTHER_4XX || code == AVERROR_HTTP_SERVER_ERROR ||
                              (code == AVERROR(EIO) && polled_);

        // The contract's transient answers: no response at all, a 5xx, a 429.
        // Every other answer is the same answer next time.
        if (!answered || code == AVERROR_HTTP_SERVER_ERROR ||
            code == AVERROR_HTTP_TOO_MANY_REQUESTS)
        {
            std::string what;
            if (answered)
                what = avErrorText(code);
            else if (timedOut_)
                what = "no response within " + std::to_string(timeoutNs_ / 1'000'000) + " ms";
            else if (code == AVERROR(EIO))
                what = "no connection: the host name did not resolve";
            else
                what = "no response: " + avErrorText(code);
            if (mayRetry())
            {
                MULTIROOM_WARN("{}: {} at byte {}, trying again ({} of {})", uri_, what, position_,
                         failures_, retries_);
                continue;
            }
            if (stalled)
                return fail(MediaSourceError::Timeout, uri_ + ": " + what);
            return fail(MediaSourceError::OpenFailed, uri_ + ": " + what);
        }

        if (code == AVERROR_HTTP_NOT_FOUND)
            return fail(MediaSourceError::NotFound, uri_);
        // A configuration fault rather than this source's: every other source
        // will be refused the same way. Which half of the pair to look at is
        // said; the token itself is not.
        if (code == AVERROR_HTTP_UNAUTHORIZED || code == AVERROR_HTTP_FORBIDDEN)
            return fail(MediaSourceError::AccessDenied,
                        uri_ + ": " + avErrorText(code) +
                            (headers_.empty() ? " (no media.token is configured)"
                                              : " (media.token was refused)"));
        // FFmpeg answers both of these with a bare EIO and nothing else, so
        // the reading an operator most needs is added here rather than left
        // to be guessed.
        if (code == AVERROR(EIO) && position_ > 0)
        {
            return fail(MediaSourceError::OpenFailed,
                        uri_ + ": the resume at byte " + std::to_string(position_) +
                            " was not answered from that byte");
        }
        if (code == AVERROR(EIO))
            return fail(MediaSourceError::OpenFailed,
                        uri_ + ": " + avErrorText(code) + " (or a redirect, which is not followed)");
        return fail(MediaSourceError::OpenFailed, uri_ + ": " + avErrorText(code));
    }
}

Result<size_t, MediaSourceError> HttpMediaSource::read(std::span<std::byte> buffer)
{
    if (context_ == nullptr)
        return fail(MediaSourceError::InvalidArgument, uri_ + ": closed");
    if (buffer.empty())
        return size_t{0};

    const int capacity = static_cast<int>(std::min<size_t>(buffer.size(), INT_MAX));
    for (;;)
    {
        startDeadline();
        const int code =
            avio_read(context_, reinterpret_cast<unsigned char*>(buffer.data()), capacity);

        if (code > 0)
        {
            failures_ = 0;
            position_ += static_cast<uint64_t>(code);
            return static_cast<size_t>(code);
        }

        // The end, unless a length was announced and not delivered: then it
        // is a connection that closed early, and that is a drop.
        const bool shortOfLength = size_ && position_ < *size_;
        if ((code == AVERROR_EOF || code == 0) && !shortOfLength)
            return size_t{0};

        const bool stalled = timedOut_ || code == AVERROR_EXIT || code == AVERROR(ETIMEDOUT);
        const Failure<MediaSourceError> failure =
            stalled ? Failure<MediaSourceError>{MediaSourceError::Timeout, uri_}
                    : Failure<MediaSourceError>{
                          MediaSourceError::ReadFailed,
                          uri_ + " at byte " + std::to_string(position_) + ": " +
                              (code < 0 ? avErrorText(code) : std::string("closed early"))};

        // A stream that cannot be asked for a byte range cannot be resumed:
        // whatever a new request returned would start somewhere else.
        if (!seekable_ || !mayRetry())
            return std::unexpected(failure);

        MULTIROOM_WARN("{}: {}, resuming at byte {} ({} of {})", uri_,
                 stalled ? std::string("stalled") : failure.detail, position_, failures_,
                 retries_);

        if (auto reconnected = reconnect(MediaSourceError::ReadFailed); !reconnected)
            return std::unexpected(reconnected.error());
    }
}

Status<MediaSourceError> HttpMediaSource::reconnect(MediaSourceError failure)
{
    avio_closep(&context_);
    if (auto connected = connect(); !connected)
        return connected;

    const int64_t size = avio_size(context_);
    if (!size_ || size != static_cast<int64_t>(*size_))
    {
        avio_closep(&context_);
        const char* how = failure == MediaSourceError::ReadFailed ? "resumed" : "reopened";
        return fail(failure, uri_ + ": " + how + " at byte " + std::to_string(position_) +
                                 " with a length of " + std::to_string(size) + " instead of " +
                                 std::to_string(size_.value_or(0)));
    }
    return {};
}

Status<MediaSourceError> HttpMediaSource::seek(uint64_t offset)
{
    if (context_ == nullptr)
        return fail(MediaSourceError::InvalidArgument, uri_ + ": closed");
    if (!seekable_)
        return fail(MediaSourceError::NotSeekable, uri_);
    if (size_ && offset > *size_)
        return fail(MediaSourceError::OutOfRange,
                    uri_ + " offset " + std::to_string(offset) + " of " +
                        std::to_string(*size_));

    startDeadline();

    const int64_t landed = avio_seek(context_, static_cast<int64_t>(offset), SEEK_SET);
    if (landed >= 0)
    {
        position_ = static_cast<uint64_t>(landed);
        return {};
    }

    const bool stalled = timedOut_;
    const std::string what = stalled ? std::string("stalled")
                                     : avErrorText(static_cast<int>(landed));

    // A seek past what is buffered is a new request, and it fails the way an
    // open or a read does -- a disk spinning up behind the high-end server is
    // a stall here as often as anywhere. So it is retried like them: a fresh
    // connection asked for the byte the seek was for.
    if (!mayRetry())
    {
        if (stalled)
            return fail(MediaSourceError::Timeout, uri_);
        return fail(MediaSourceError::SeekFailed,
                    uri_ + " offset " + std::to_string(offset) + ": " + what);
    }

    MULTIROOM_WARN("{}: seek to byte {} {}, trying again ({} of {})", uri_, offset, what, failures_,
             retries_);

    position_ = offset;
    return reconnect(MediaSourceError::SeekFailed);
}

} // namespace multiroom
