#include "decode/ffmpeg/FfmpegDecoder.h"

#include "util/Logger.h"

#include <algorithm>
#include <bit>
#include <cstdarg>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/log.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
#include <libavutil/samplefmt.h>
}

namespace multiroom {
namespace {

// MATP section 12's layouts are all little-endian, and FFmpeg hands back
// samples in the host's order. Every platform this server targets is
// little-endian; the day one is not, the repacking belongs here, at the edge
// that produced the bytes, and this is where that decision would be forced.
static_assert(std::endian::native == std::endian::little,
              "the decoder passes FFmpeg's host-order samples through as MATP's "
              "little-endian layouts");

std::string avErrorText(int code)
{
    // av_err2str is a macro over a compound literal and does not compile as
    // C++, so the buffer is ours.
    char text[AV_ERROR_MAX_STRING_SIZE] = {};
    if (av_strerror(code, text, sizeof(text)) < 0)
        return "AVERROR " + std::to_string(code);
    return text;
}

// FFmpeg logs to stderr by default, in its own format and with no thread tag.
// Everything it reports that matters is converted into a DecodeError and
// reported by the caller, so this exists to keep the diagnosis available
// without letting a second, unattributable stream onto the console: its lines
// land at debug, below the level a normal run prints.
void logBridge(void* context, int level, const char* format, va_list arguments)
{
    if (level > AV_LOG_WARNING || !log::enabled(LogLevel::Debug))
        return;

    char line[1024] = {};
    int printPrefix = 1;
    if (av_log_format_line2(context, level, format, arguments, line, sizeof(line),
                            &printPrefix) < 0)
        return;

    std::string text(line);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
        text.pop_back();
    if (!text.empty())
        MULTIROOM_DEBUG("ffmpeg: {}", text);
}

void installLogBridge()
{
    static std::once_flag once;
    std::call_once(once, [] { av_log_set_callback(&logBridge); });
}

struct FormatMapping
{
    AudioFormat::SampleType type = AudioFormat::SampleType::Int16;
    int rightShift = 0;
    bool planar = false;
};

// The one place FFmpeg's sample formats become MATP's, and the one place the
// 24-bit question is answered. See the note at the top of the header: FFmpeg
// left-aligns a narrower source inside S32, MATP's s24le is right-aligned in
// four bytes, so the twenty-four-bit case is a shift and not a relabel.
Result<FormatMapping, DecodeError> mapSampleFormat(AVSampleFormat sampleFormat,
                                                   int bitsPerRawSample)
{
    FormatMapping mapping;
    mapping.planar = av_sample_fmt_is_planar(sampleFormat) != 0;

    switch (av_get_packed_sample_fmt(sampleFormat))
    {
    case AV_SAMPLE_FMT_S16:
        mapping.type = AudioFormat::SampleType::Int16;
        return mapping;

    case AV_SAMPLE_FMT_S32:
        if (bitsPerRawSample > 0 && bitsPerRawSample <= 24)
        {
            mapping.type = AudioFormat::SampleType::Int24In32;
            mapping.rightShift = 8;
        }
        else
        {
            mapping.type = AudioFormat::SampleType::Int32;
        }
        return mapping;

    case AV_SAMPLE_FMT_FLT:
        mapping.type = AudioFormat::SampleType::Float32;
        return mapping;

    default:
        break;
    }

    // U8, S64 and DBL have no MATP layout, and converting one would be the
    // value change this layer is forbidden to make.
    const char* name = av_get_sample_fmt_name(sampleFormat);
    return fail(DecodeError::UnsupportedFormat, name != nullptr ? name : "unknown sample format");
}

} // namespace

FfmpegDecoder::~FfmpegDecoder()
{
    close();
}

int FfmpegDecoder::readPacket(void* opaque, uint8_t* buffer, int size)
{
    auto* self = static_cast<FfmpegDecoder*>(opaque);
    if (size <= 0)
        return AVERROR(EINVAL);

    auto result = self->source_->read(
        std::span<std::byte>(reinterpret_cast<std::byte*>(buffer), static_cast<size_t>(size)));
    if (!result)
    {
        // FFmpeg flattens this to one integer on the way back out, so the real
        // reason is kept where open() and decode() can still report it.
        self->sourceFailure_ = result.error();
        return result.error().code == MediaSourceError::Timeout ? AVERROR(ETIMEDOUT)
                                                                : AVERROR(EIO);
    }

    if (*result == 0)
        return AVERROR_EOF;
    return static_cast<int>(*result);
}

int64_t FfmpegDecoder::seekPacket(void* opaque, int64_t offset, int whence)
{
    auto* self = static_cast<FfmpegDecoder*>(opaque);
    const std::optional<uint64_t> size = self->source_->sizeBytes();

    if ((whence & AVSEEK_SIZE) != 0)
        return size ? static_cast<int64_t>(*size) : AVERROR(ENOSYS);

    int64_t target = 0;
    switch (whence & ~AVSEEK_FORCE)
    {
    case SEEK_SET:
        target = offset;
        break;
    case SEEK_CUR:
        target = static_cast<int64_t>(self->source_->position()) + offset;
        break;
    case SEEK_END:
        if (!size)
            return AVERROR(ENOSYS);
        target = static_cast<int64_t>(*size) + offset;
        break;
    default:
        return AVERROR(EINVAL);
    }

    if (target < 0)
        return AVERROR(EINVAL);

    auto status = self->source_->seek(static_cast<uint64_t>(target));
    if (!status)
    {
        self->sourceFailure_ = status.error();
        return AVERROR(EIO);
    }
    return target;
}

Status<DecodeError> FfmpegDecoder::open(std::unique_ptr<IMediaSource> source)
{
    if (format_ != nullptr || source_ != nullptr)
        return fail(DecodeError::AlreadyOpened, info_.containerName);
    if (!source)
        return fail(DecodeError::SourceFailed, "no source");

    installLogBridge();
    source_ = std::move(source);
    const std::string name = source_->describe();

    // Fails the whole open rather than leaving a half-constructed decoder: the
    // roadmap's rule is that a component which cannot do its job says so.
    const auto abandon = [&](DecodeError code, std::string detail) {
        close();
        return fail(code, std::move(detail));
    };

    // A source failure seen inside FFmpeg beats whatever FFmpeg made of it.
    const auto sourceOrElse = [&](DecodeError code, const std::string& detail) {
        if (sourceFailure_)
        {
            const std::string reason = name + ": " + describe(*sourceFailure_);
            return abandon(DecodeError::SourceFailed, reason);
        }
        return abandon(code, detail);
    };

    auto* buffer = static_cast<unsigned char*>(av_malloc(kAvioBufferBytes));
    if (buffer == nullptr)
        return abandon(DecodeError::DecodeFailed, name + ": out of memory");

    avio_ = avio_alloc_context(buffer, kAvioBufferBytes, 0, this, &readPacket, nullptr,
                               source_->seekable() ? &seekPacket : nullptr);
    if (avio_ == nullptr)
    {
        av_free(buffer);
        return abandon(DecodeError::DecodeFailed, name + ": avio_alloc_context");
    }
    avio_->seekable = source_->seekable() ? AVIO_SEEKABLE_NORMAL : 0;

    format_ = avformat_alloc_context();
    if (format_ == nullptr)
        return abandon(DecodeError::DecodeFailed, name + ": avformat_alloc_context");
    format_->pb = avio_;
    format_->flags |= AVFMT_FLAG_CUSTOM_IO;

    int code = avformat_open_input(&format_, nullptr, nullptr, nullptr);
    if (code < 0)
    {
        // On failure avformat_open_input frees the context and nulls the
        // pointer; AVFMT_FLAG_CUSTOM_IO keeps it from touching avio_.
        format_ = nullptr;
        return sourceOrElse(DecodeError::UnknownContainer, name + ": " + avErrorText(code));
    }

    // A source that failed *during* the probe is a failed open even when
    // FFmpeg calls the probe a success: it stops reading at the error with
    // what it has, and the packets it hands back afterwards do not start where
    // the stream does -- measured in phase 22, where a WAV whose connection
    // broke inside the probe decoded without its first 64 KiB.
    code = avformat_find_stream_info(format_, nullptr);
    if (code < 0 || sourceFailure_)
        return sourceOrElse(DecodeError::UnknownContainer, name + ": " + avErrorText(code));

    const AVCodec* decoder = nullptr;
    streamIndex_ = av_find_best_stream(format_, AVMEDIA_TYPE_AUDIO, -1, -1, &decoder, 0);
    if (streamIndex_ < 0)
        return abandon(DecodeError::NoAudioStream, name);
    if (decoder == nullptr)
        return abandon(DecodeError::UnsupportedCodec, name);

    AVStream* stream = format_->streams[streamIndex_];

    codec_ = avcodec_alloc_context3(decoder);
    if (codec_ == nullptr)
        return abandon(DecodeError::DecodeFailed, name + ": avcodec_alloc_context3");

    code = avcodec_parameters_to_context(codec_, stream->codecpar);
    if (code < 0)
        return abandon(DecodeError::DecodeFailed, name + ": " + avErrorText(code));

    code = avcodec_open2(codec_, decoder, nullptr);
    if (code < 0)
    {
        // avcodec_open2 having failed means codec_ is allocated but unusable,
        // and opened() keys on codec_, so it must not survive.
        avcodec_free_context(&codec_);
        return abandon(DecodeError::UnsupportedCodec, name + ": " + avErrorText(code));
    }

    AVSampleFormat sampleFormat = codec_->sample_fmt;
    if (sampleFormat == AV_SAMPLE_FMT_NONE)
        sampleFormat = static_cast<AVSampleFormat>(stream->codecpar->format);

    int bitsPerRawSample = codec_->bits_per_raw_sample;
    if (bitsPerRawSample <= 0)
        bitsPerRawSample = stream->codecpar->bits_per_raw_sample;

    auto mapping = mapSampleFormat(sampleFormat, bitsPerRawSample);
    if (!mapping)
        return abandon(mapping.error().code, name + ": " + mapping.error().detail);

    const int channels = codec_->ch_layout.nb_channels;
    if (channels <= 0 || channels > UINT16_MAX)
        return abandon(DecodeError::InvalidChannels, name + ": " + std::to_string(channels));
    if (codec_->sample_rate <= 0)
        return abandon(DecodeError::UnsupportedFormat,
                       name + ": sample rate " + std::to_string(codec_->sample_rate));

    info_ = SourceStreamInfo{};
    info_.format.sampleRate = static_cast<uint32_t>(codec_->sample_rate);
    info_.format.channels = static_cast<uint16_t>(channels);
    info_.format.sampleType = mapping->type;
    info_.codecName = decoder->name != nullptr ? decoder->name : "";
    info_.containerName = format_->iformat != nullptr && format_->iformat->name != nullptr
                              ? format_->iformat->name
                              : "";
    info_.bitsPerRawSample = bitsPerRawSample > 0 ? static_cast<uint32_t>(bitsPerRawSample)
                                                  : AudioFormat::validBits(mapping->type);

    // Omitted rather than guessed: a container that does not carry a duration
    // is a stream whose length nobody knows yet.
    if (stream->duration > 0 && stream->time_base.num > 0 && stream->time_base.den > 0)
    {
        const int64_t frames =
            av_rescale_q(stream->duration, stream->time_base,
                         AVRational{1, static_cast<int>(info_.format.sampleRate)});
        if (frames > 0)
            info_.totalFrames = static_cast<uint64_t>(frames);
    }

    rightShift_ = mapping->rightShift;
    planar_ = mapping->planar;
    bytesPerSample_ = info_.format.bytesPerSample();
    sampleFormat_ = sampleFormat;

    packet_ = av_packet_alloc();
    frame_ = av_frame_alloc();
    if (packet_ == nullptr || frame_ == nullptr)
        return abandon(DecodeError::DecodeFailed, name + ": out of memory");

    return {};
}

void FfmpegDecoder::close() noexcept
{
    if (frame_ != nullptr)
        av_frame_free(&frame_);
    if (packet_ != nullptr)
        av_packet_free(&packet_);
    if (codec_ != nullptr)
        avcodec_free_context(&codec_);
    if (format_ != nullptr)
        avformat_close_input(&format_);
    if (avio_ != nullptr)
    {
        // FFmpeg may have replaced the buffer this context was given, so the
        // one to free is the one it holds now.
        av_freep(&avio_->buffer);
        avio_context_free(&avio_);
    }
    source_.reset();
    streamIndex_ = -1;
    frameOffset_ = 0;
    frameFrames_ = 0;
}

Status<DecodeError> FfmpegDecoder::fillFrame()
{
    // A source failure that has already been recorded outranks whatever FFmpeg
    // made of it, wherever it surfaces. It rarely surfaces where it happened:
    // a read that fails part-way through a packet is handed to the demuxer as
    // a short buffer, and what comes back is AVERROR_INVALIDDATA from the
    // *decoder* two calls later. Reporting that would tell a zone its file is
    // corrupt when the truth is that the far end went quiet.
    const auto failing = [this](DecodeError code, const std::string& detail) {
        if (sourceFailure_)
        {
            const std::string reason = source_->describe() + ": " + describe(*sourceFailure_);
            sourceFailure_.reset();
            return fail(DecodeError::SourceFailed, reason);
        }
        return fail(code, source_->describe() + ": " + detail);
    };

    for (;;)
    {
        const int received = avcodec_receive_frame(codec_, frame_);
        if (received == 0)
        {
            if (frame_->nb_samples <= 0)
                continue; // a frame carrying only side data

            // emit() reads every frame with the layout the stream opened with.
            // A chained or switching stream that changes it mid-way would be
            // read past its planes, so it ends here instead.
            if (frame_->format != sampleFormat_ ||
                frame_->ch_layout.nb_channels != static_cast<int>(info_.format.channels) ||
                (frame_->sample_rate > 0 &&
                 static_cast<uint32_t>(frame_->sample_rate) != info_.format.sampleRate))
            {
                return failing(DecodeError::DecodeFailed,
                               "format changed mid-stream to " +
                                   std::to_string(frame_->ch_layout.nb_channels) +
                                   " channels at " + std::to_string(frame_->sample_rate) +
                                   " Hz");
            }
            frameOffset_ = 0;
            frameFrames_ = static_cast<uint32_t>(frame_->nb_samples);
            return {};
        }
        if (received == AVERROR_EOF)
        {
            // A source that failed just before the end is not a clean end.
            if (sourceFailure_)
                return failing(DecodeError::DecodeFailed, avErrorText(received));
            return fail(DecodeError::EndOfStream, source_->describe());
        }
        if (received != AVERROR(EAGAIN))
            return failing(DecodeError::DecodeFailed, avErrorText(received));

        const int read = av_read_frame(format_, packet_);
        if (read == AVERROR_EOF)
        {
            // Flush: the codec may still hold frames, and EndOfStream is only
            // true once it has given them all back.
            draining_ = true;
            const int sent = avcodec_send_packet(codec_, nullptr);
            if (sent < 0 && sent != AVERROR_EOF)
                return failing(DecodeError::DecodeFailed, avErrorText(sent));
            continue;
        }
        if (read < 0)
            return failing(DecodeError::DecodeFailed, avErrorText(read));

        if (packet_->stream_index != streamIndex_)
        {
            av_packet_unref(packet_);
            continue;
        }

        const int sent = avcodec_send_packet(codec_, packet_);
        av_packet_unref(packet_);
        if (sent < 0 && sent != AVERROR(EAGAIN))
            return failing(DecodeError::DecodeFailed, avErrorText(sent));
    }
}

void FfmpegDecoder::emit(std::span<std::byte> output, uint32_t frames) const
{
    const size_t channels = info_.format.channels;
    const size_t bytes = bytesPerSample_;

    // The common case: an interleaved codec whose samples need no shift, which
    // is every 16-bit and every float source. One memcpy for the whole span.
    if (!planar_ && rightShift_ == 0)
    {
        const size_t offset = static_cast<size_t>(frameOffset_) * channels * bytes;
        std::memcpy(output.data(), frame_->extended_data[0] + offset,
                    static_cast<size_t>(frames) * channels * bytes);
        return;
    }

    for (uint32_t index = 0; index < frames; ++index)
    {
        const size_t sourceFrame = static_cast<size_t>(frameOffset_) + index;
        for (size_t channel = 0; channel < channels; ++channel)
        {
            const uint8_t* from =
                planar_ ? frame_->extended_data[channel] + sourceFrame * bytes
                        : frame_->extended_data[0] + (sourceFrame * channels + channel) * bytes;
            std::byte* to = output.data() + (static_cast<size_t>(index) * channels + channel) * bytes;

            if (rightShift_ != 0)
            {
                int32_t value = 0;
                std::memcpy(&value, from, sizeof(value));
                value >>= rightShift_; // arithmetic since C++20, which is what sign extension needs
                std::memcpy(to, &value, sizeof(value));
            }
            else
            {
                std::memcpy(to, from, bytes);
            }
        }
    }
}

Status<DecodeError> FfmpegDecoder::seekTo(uint64_t frame)
{
    if (!opened())
        return fail(DecodeError::NotOpened);

    const std::string name = source_->describe();
    if (info_.totalFrames && frame >= *info_.totalFrames)
    {
        return fail(DecodeError::OffsetOutOfRange,
                    name + ": frame " + std::to_string(frame) + " of " +
                        std::to_string(*info_.totalFrames));
    }
    if (!source_->seekable())
        return fail(DecodeError::NotSeekable, name + " cannot seek");

    // Timestamps count from the stream's own start, which is not zero for
    // every container; frame positions count from the first frame decoded.
    const AVStream* stream = format_->streams[streamIndex_];
    const int64_t origin = stream->start_time != AV_NOPTS_VALUE ? stream->start_time : 0;
    const AVRational perFrame{1, static_cast<int>(info_.format.sampleRate)};
    const int64_t target =
        origin + av_rescale_q(static_cast<int64_t>(frame), perFrame, stream->time_base);

    const int sought = av_seek_frame(format_, streamIndex_, target, AVSEEK_FLAG_BACKWARD);
    if (sought < 0 || sourceFailure_)
    {
        if (sourceFailure_)
        {
            const std::string reason = name + ": " + describe(*sourceFailure_);
            sourceFailure_.reset();
            return fail(DecodeError::SourceFailed, reason);
        }
        return fail(DecodeError::DecodeFailed, name + ": seek: " + avErrorText(sought));
    }
    avcodec_flush_buffers(codec_);
    draining_ = false;
    frameOffset_ = 0;
    frameFrames_ = 0;

    // Forward to the frame asked for. Whole decoded frames before it are
    // dropped, and the one it falls in is entered part-way, which is what
    // makes the landing exact wherever the codec's timestamps are.
    for (;;)
    {
        if (auto filled = fillFrame(); !filled)
        {
            if (filled.error().code == DecodeError::EndOfStream)
                return fail(DecodeError::OffsetOutOfRange,
                            name + ": frame " + std::to_string(frame) + " is past the end");
            return filled;
        }

        int64_t pts = frame_->best_effort_timestamp;
        if (pts == AV_NOPTS_VALUE)
            pts = frame_->pts;
        if (pts == AV_NOPTS_VALUE)
            return fail(DecodeError::DecodeFailed, name + ": no timestamp to land a seek on");

        const int64_t at = av_rescale_q(pts - origin, stream->time_base, perFrame);
        if (at + static_cast<int64_t>(frameFrames_) <= static_cast<int64_t>(frame))
            continue;

        // A codec that lands after the frame asked for begins where it
        // landed, and says so by the position of the first frame it returns.
        const uint64_t landed = at > 0 ? static_cast<uint64_t>(at) : 0;
        frameOffset_ = landed >= frame ? 0 : static_cast<uint32_t>(frame - landed);
        nextFrame_ = std::max(landed, frame);
        return {};
    }
}

Result<DecodedSpan, DecodeError> FfmpegDecoder::decode(std::span<std::byte> output)
{
    if (!opened())
        return fail(DecodeError::NotOpened);

    const uint32_t bytesPerFrame = info_.format.bytesPerFrame();
    const size_t capacity = output.size() / bytesPerFrame;
    if (capacity == 0)
        return fail(DecodeError::BufferTooSmall,
                    std::to_string(output.size()) + " bytes for a frame of " +
                        std::to_string(bytesPerFrame));

    if (frameOffset_ >= frameFrames_)
    {
        auto status = fillFrame();
        if (!status)
            return std::unexpected(status.error());
    }

    const uint32_t frames =
        static_cast<uint32_t>(std::min<size_t>(capacity, frameFrames_ - frameOffset_));
    emit(output, frames);

    DecodedSpan span;
    span.firstFrame = static_cast<SourceFrame>(nextFrame_);
    span.frameCount = frames;
    span.byteCount = static_cast<size_t>(frames) * bytesPerFrame;

    nextFrame_ += frames;
    frameOffset_ += frames;
    return span;
}

} // namespace multiroom
