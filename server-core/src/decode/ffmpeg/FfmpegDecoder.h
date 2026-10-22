#pragma once

// libavformat and libavcodec behind IDecoder.
//
// This header lives beside its source rather than in include/ for the same
// reason SoxrResampler.h does: no FFmpeg header outside this directory. The
// forward declarations below are the whole of what a caller needs to hold one
// of these, and they are what keeps an AVFrame from turning up in a zone.
//
// Three things here are decisions rather than transcription.
//
// **The source is reached through a custom AVIOContext.** libavformat is never
// told what it is reading from, which is what makes MemoryMediaSource a
// complete test rig -- a decoder test needs no file, no socket and no fixture
// checked in. Handing a URI straight to avformat_open_input would be shorter
// and would make IMediaSource decoration.
//
// **A 24-bit source comes out as Int24In32, after a shift.** FFmpeg hands back
// 24-bit FLAC and 24-bit WAV as AV_SAMPLE_FMT_S32 with bits_per_raw_sample 24
// and the twenty-four valid bits at the *top* of the container -- it
// left-shifts by (32 - bps) so that its output is one fixed-point scale
// regardless of the source's depth. MATP section 12 has no such layout: its
// s24le is ALSA's S24_LE, twenty-four valid bits at the bottom of four. So the
// adapter shifts right by eight, arithmetically, and reports Int24In32. Which
// way that shift goes is the difference between bitperfect audio, noise and
// 48 dB of missing level, which is why it is not asserted here but measured in
// the phase-4 fixture test against a ramp this project wrote byte by byte.
//
// **No libswresample.** The roadmap forbids it in the decode path and the only
// thing this adapter does to a sample is that shift and, for a planar codec,
// the interleave. Both are layout changes. Rate, channel count and channel
// order arrive as the file had them.

#include "decode/IDecoder.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

struct AVCodecContext;
struct AVFormatContext;
struct AVFrame;
struct AVIOContext;
struct AVPacket;

namespace multiroom {

class FfmpegDecoder final : public IDecoder
{
public:
    // What avformat probes and buffers through. 32 kB is FFmpeg's own default
    // for a custom context and is about one and a half seconds of the largest
    // format this server allows -- large enough that probing a container does
    // not turn into a hundred source reads, small enough not to matter beside
    // a zone's block pool.
    static constexpr int kAvioBufferBytes = 32 * 1024;

    FfmpegDecoder() = default;
    ~FfmpegDecoder() override;

    Status<DecodeError> open(std::unique_ptr<IMediaSource> source) override;
    bool opened() const noexcept override { return codec_ != nullptr; }
    const SourceStreamInfo& info() const noexcept override { return info_; }
    Result<DecodedSpan, DecodeError> decode(std::span<std::byte> output) override;

    // Phase 26: the next decode() begins at `frame` of the source. FFmpeg
    // seeks to the nearest point at or before it and the frames in between are
    // decoded and thrown away, so a PCM or FLAC source lands on the exact
    // sample and a lossy one as close as its codec allows. Called once, after
    // open() and before the first decode(); a source that cannot seek is
    // NotSeekable, and a frame at or past a stated length OffsetOutOfRange.
    Status<DecodeError> seekTo(uint64_t frame);

private:
    // The AVIOContext trampolines. They are the only place the source is
    // touched from inside FFmpeg, and the only place an AVERROR is invented
    // rather than converted.
    static int readPacket(void* opaque, uint8_t* buffer, int size);
    static int64_t seekPacket(void* opaque, int64_t offset, int whence);

    void close() noexcept;

    // Reads packets and feeds the codec until a frame is available. Returns
    // EndOfStream once the source and the codec's own buffers are both empty.
    Status<DecodeError> fillFrame();

    // Copies `frames` frames of the held AVFrame, starting at frameOffset_,
    // into `output` as interleaved native PCM.
    void emit(std::span<std::byte> output, uint32_t frames) const;

    std::unique_ptr<IMediaSource> source_;

    AVIOContext* avio_ = nullptr;
    AVFormatContext* format_ = nullptr;
    AVCodecContext* codec_ = nullptr;
    AVPacket* packet_ = nullptr;
    AVFrame* frame_ = nullptr;

    int streamIndex_ = -1;

    SourceStreamInfo info_;

    // The position of the next frame of the source. Counted rather than taken
    // from a container timestamp, because the count is exact and a timestamp
    // is not; seekTo() is the one place a timestamp is read, once, to find
    // where the count starts.
    uint64_t nextFrame_ = 0;

    // How much of frame_ has already been handed out. A decoded frame is
    // routinely larger than one block, so a decode() call that cannot take all
    // of it leaves the rest here rather than dropping it.
    uint32_t frameOffset_ = 0;
    uint32_t frameFrames_ = 0;

    // Set once av_read_frame has reported the end and the codec has been told
    // to drain, so the loop does not ask the source for more.
    bool draining_ = false;

    // What the source said when a read inside FFmpeg failed. FFmpeg flattens
    // every error into one integer on the way back out, so the real reason is
    // kept here and reported instead of "DecodeFailed: -5".
    std::optional<Failure<MediaSourceError>> sourceFailure_;

    // Shift applied to every sample of an S32 stream whose source was narrower
    // than 32 bits; see the note at the top. Zero for everything else.
    int rightShift_ = 0;

    // Whether the codec hands back one plane per channel.
    bool planar_ = false;

    uint32_t bytesPerSample_ = 0;

    // The AVSampleFormat the layout above was derived from.
    int sampleFormat_ = -1;
};

} // namespace multiroom
