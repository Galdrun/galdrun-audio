#include "decode/DecoderFactory.h"

#if MULTIROOM_WITH_FFMPEG
#include "decode/ffmpeg/FfmpegDecoder.h"
#include "source/http/HttpMediaSource.h"

#include <string_view>
#include <utility>
#endif

namespace multiroom {
namespace {

#if MULTIROOM_WITH_FFMPEG

bool hasPrefix(const std::string& uri, std::string_view prefix)
{
    return uri.size() >= prefix.size() && uri.compare(0, prefix.size(), prefix) == 0;
}

// The only factory a deployment gets, and deliberately the whole of the scheme
// knowledge in this server: a zone stages a string, and this decides what that
// string is.
class MediaDecoderFactory final : public IDecoderFactory
{
public:
    MediaDecoderFactory(const IClock& clock, MediaFetchSettings fetch)
        : clock_(clock), fetch_(fetch)
    {
    }

    Result<std::unique_ptr<IDecoder>, DecodeError> open(const std::string& uri,
                                                        uint64_t startOffsetNs) override
    {
        if (uri.empty())
            return fail(DecodeError::SourceFailed, "a source with no URI");

        // Phase 20: media comes from the high-end server over HTTP and from
        // nowhere else. The control API has already held the URI against the
        // configured origin; this is the line behind it, for a zone staged
        // from anywhere that is not the API.
        if (!hasPrefix(uri, "http://") && !hasPrefix(uri, "https://"))
            return fail(DecodeError::SourceFailed, "not an http(s) URI: " + uri);

        auto opened = HttpMediaSource::open(uri, clock_, fetch_);
        if (!opened)
            return fail(DecodeError::SourceFailed, describe(opened.error()));

        auto decoder = std::make_unique<FfmpegDecoder>();
        if (auto ready = decoder->open(std::move(*opened)); !ready)
            return std::unexpected(ready.error());

        // Phase 26: the offset is a seek inside the opened container, so the
        // format and the length are known before a frame of it is decoded.
        if (startOffsetNs != 0)
        {
            const auto frame =
                framesFromNanoseconds(startOffsetNs, decoder->info().format.sampleRate);
            if (!frame)
                return fail(DecodeError::OffsetOutOfRange, describe(frame.error()));
            if (auto sought = decoder->seekTo(*frame); !sought)
                return std::unexpected(sought.error());
        }

        return std::unique_ptr<IDecoder>(std::move(decoder));
    }

private:
    const IClock& clock_;
    MediaFetchSettings fetch_;
};

#else

// A build without FFmpeg has no container and no codec, so every URI is
// refused by name. It exists rather than not existing because a zone that
// silently played nothing would be rule 6 broken where it matters most.
class MediaDecoderFactory final : public IDecoderFactory
{
public:
    Result<std::unique_ptr<IDecoder>, DecodeError> open(const std::string& uri,
                                                        uint64_t /*startOffsetNs*/) override
    {
        return fail(DecodeError::NoDecoder,
                    "this build has no decoder (MULTIROOM_WITH_FFMPEG is off): " + uri);
    }
};

#endif

} // namespace

std::unique_ptr<IDecoderFactory> makeMediaDecoderFactory([[maybe_unused]] const IClock& clock,
                                                         [[maybe_unused]] MediaFetchSettings fetch)
{
#if MULTIROOM_WITH_FFMPEG
    return std::make_unique<MediaDecoderFactory>(clock, fetch);
#else
    return std::make_unique<MediaDecoderFactory>();
#endif
}

} // namespace multiroom
