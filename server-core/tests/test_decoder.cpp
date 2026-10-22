// The FFmpeg decoder, against fixtures this project wrote byte by byte.
//
// Nothing here is registered when MULTIROOM_WITH_FFMPEG is off, exactly as
// test_real_resamplers.cpp registers nothing without a real converter.

#include "TestSupport.h"

#if MULTIROOM_WITH_FFMPEG

#include "AudioFixtures.h"

#include "decode/ffmpeg/FfmpegDecoder.h"
#include "source/FileMediaSource.h"
#include "source/MemoryMediaSource.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace multiroom;

namespace {

// Decodes a whole stream through whatever buffer size is asked for, so the
// same body covers "one big call" and "a hundred small ones".
struct Decoded
{
    SourceStreamInfo info;
    std::vector<int32_t> samples;
    uint64_t frames = 0;
};

Decoded decodeAll(std::unique_ptr<IMediaSource> source, uint32_t framesPerCall)
{
    FfmpegDecoder decoder;
    auto opened = decoder.open(std::move(source));
    CHECK_OK(opened);

    Decoded decoded;
    decoded.info = decoder.info();

    std::vector<std::byte> buffer(static_cast<size_t>(framesPerCall) *
                                  decoded.info.format.bytesPerFrame());

    for (;;)
    {
        auto span = decoder.decode(buffer);
        if (!span)
        {
            CHECK_ERR(span, DecodeError::EndOfStream);
            break;
        }

        // The position a decoder reports is the count of frames before it, and
        // it must never skip: a hole here is a hole in the timeline later.
        CHECK_EQ(static_cast<uint64_t>(span->firstFrame), decoded.frames);
        CHECK_EQ(span->byteCount,
                 static_cast<size_t>(span->frameCount) * decoded.info.format.bytesPerFrame());
        CHECK(span->frameCount > 0);

        auto values = multiroomtest::readSamples(std::span<const std::byte>(buffer).first(span->byteCount),
                                           decoded.info.format.sampleType);
        decoded.samples.insert(decoded.samples.end(), values.begin(), values.end());
        decoded.frames += span->frameCount;
    }

    return decoded;
}

void checkAgainstFixture(const Decoded& decoded, const multiroomtest::PcmFixture& fixture)
{
    CHECK_EQ(decoded.info.format.sampleRate, fixture.sampleRate);
    CHECK_EQ(decoded.info.format.channels, fixture.channels);
    CHECK_EQ(decoded.info.bitsPerRawSample, fixture.bits);
    CHECK_EQ(decoded.frames, static_cast<uint64_t>(fixture.frames));
    CHECK_EQ(decoded.samples.size(), fixture.samples.size());

    const AudioFormat::SampleType expected = fixture.bits == 16
                                                 ? AudioFormat::SampleType::Int16
                                                 : AudioFormat::SampleType::Int24In32;
    CHECK_EQ(static_cast<int>(decoded.info.format.sampleType), static_cast<int>(expected));

    for (size_t index = 0; index < fixture.samples.size(); ++index)
    {
        if (decoded.samples[index] != fixture.samples[index])
        {
            // One failure, naming the sample: a per-sample CHECK_EQ over a
            // second of audio prints tens of thousands of lines.
            CHECK_EQ(multiroomtest::toText(decoded.samples[index]) + " at sample " +
                         std::to_string(index),
                     multiroomtest::toText(fixture.samples[index]) + " at sample " +
                         std::to_string(index));
            return;
        }
    }
}

std::unique_ptr<IMediaSource> memorySource(std::vector<std::byte> bytes, const char* name)
{
    return std::make_unique<MemoryMediaSource>(std::move(bytes), name);
}

// A source that fails part-way through, to prove the decoder reports the
// source's own reason rather than whatever FFmpeg made of it.
class FailingSource final : public IMediaSource
{
public:
    FailingSource(std::vector<std::byte> bytes, size_t failAfter, MediaSourceError error)
        : bytes_(std::move(bytes)), failAfter_(failAfter), error_(error), name_("failing")
    {
    }

    const std::string& describe() const noexcept override { return name_; }
    bool seekable() const noexcept override { return true; }
    std::optional<uint64_t> sizeBytes() const noexcept override { return bytes_.size(); }
    uint64_t position() const noexcept override { return position_; }

    Result<size_t, MediaSourceError> read(std::span<std::byte> buffer) override
    {
        if (served_ >= failAfter_)
            return fail(error_, "the far end went quiet");

        const size_t count =
            std::min(buffer.size(), static_cast<size_t>(bytes_.size() - position_));
        std::memcpy(buffer.data(), bytes_.data() + position_, count);
        position_ += count;
        served_ += count;
        return count;
    }

    Status<MediaSourceError> seek(uint64_t offset) override
    {
        if (offset > bytes_.size())
            return fail(MediaSourceError::OutOfRange, name_);
        position_ = offset;
        return {};
    }

private:
    std::vector<std::byte> bytes_;
    size_t failAfter_ = 0;
    size_t served_ = 0;
    MediaSourceError error_;
    std::string name_;
    uint64_t position_ = 0;
};

} // namespace

// --- the matrix the roadmap asks for ---------------------------------------
//
// 44.1 / 48 / 96 kHz and 16 / 24 bit, in WAV and FLAC, decoded to PCM
// numerically identical to the generator that produced them.

MULTIROOM_TEST(wav_decodes_to_the_samples_that_were_written)
{
    for (uint32_t rate : {44100u, 48000u, 96000u})
    {
        for (uint32_t bits : {16u, 24u})
        {
            const auto fixture = multiroomtest::makeRamp(rate, 2, bits, 5000);
            const auto decoded =
                decodeAll(memorySource(multiroomtest::encodeWav(fixture), "ramp.wav"), 1024);
            checkAgainstFixture(decoded, fixture);
            CHECK_EQ(decoded.info.containerName, std::string("wav"));
        }
    }
}

MULTIROOM_TEST(flac_decodes_to_the_samples_that_were_written)
{
    for (uint32_t rate : {44100u, 48000u, 96000u})
    {
        for (uint32_t bits : {16u, 24u})
        {
            const auto fixture = multiroomtest::makeRamp(rate, 2, bits, 5000);
            const auto decoded =
                decodeAll(memorySource(multiroomtest::encodeFlac(fixture), "ramp.flac"), 1024);
            checkAgainstFixture(decoded, fixture);
            CHECK_EQ(decoded.info.containerName, std::string("flac"));
            CHECK_EQ(decoded.info.codecName, std::string("flac"));
        }
    }
}

// The test the roadmap names outright: the same ramp through two containers,
// compared to each other rather than only to the generator. A 24-bit source
// arrives from FFmpeg as S32 with its valid bits at the top, and whether this
// server shifts them down, leaves them, or shifts the wrong way is the
// difference between bitperfect audio, 48 dB of missing level and noise.
MULTIROOM_TEST(wav_and_flac_of_the_same_24_bit_ramp_agree_sample_for_sample)
{
    const auto fixture = multiroomtest::makeRamp(44100, 2, 24, 4000);

    const auto wav = decodeAll(memorySource(multiroomtest::encodeWav(fixture), "ramp.wav"), 512);
    const auto flac = decodeAll(memorySource(multiroomtest::encodeFlac(fixture), "ramp.flac"), 512);

    CHECK_EQ(static_cast<int>(wav.info.format.sampleType),
             static_cast<int>(AudioFormat::SampleType::Int24In32));
    CHECK_EQ(static_cast<int>(flac.info.format.sampleType),
             static_cast<int>(AudioFormat::SampleType::Int24In32));
    CHECK_EQ(wav.samples.size(), flac.samples.size());

    for (size_t index = 0; index < wav.samples.size(); ++index)
    {
        if (wav.samples[index] != flac.samples[index])
        {
            CHECK_EQ(multiroomtest::toText(wav.samples[index]) + " at sample " + std::to_string(index),
                     multiroomtest::toText(flac.samples[index]) + " at sample " + std::to_string(index));
            return;
        }
    }

    // And both of them are the ramp, right-aligned: the extremes the generator
    // opens with survive the trip, which a shift in either direction would
    // saturate or halve.
    const int32_t max = (1 << 23) - 1;
    CHECK_EQ(wav.samples[0], 0);
    CHECK_EQ(wav.samples[1], 1);
    CHECK_EQ(wav.samples[2], -1);
    CHECK_EQ(wav.samples[3], max);
    CHECK_EQ(wav.samples[4], -max - 1);
}

MULTIROOM_TEST(the_native_format_survives_the_decoder)
{
    // Mono and three channels, to prove the channel count is reported rather
    // than assumed, and that a planar codec is interleaved correctly.
    for (uint16_t channels : {uint16_t{1}, uint16_t{2}, uint16_t{3}})
    {
        const auto fixture = multiroomtest::makeRamp(48000, channels, 24, 2000);
        const auto decoded =
            decodeAll(memorySource(multiroomtest::encodeFlac(fixture), "ramp.flac"), 333);
        checkAgainstFixture(decoded, fixture);
        CHECK_EQ(decoded.info.format.channels, channels);
    }
}

MULTIROOM_TEST(the_container_duration_is_reported_when_it_is_known)
{
    const auto fixture = multiroomtest::makeRamp(44100, 2, 16, 7777);

    FfmpegDecoder decoder;
    CHECK_OK(decoder.open(memorySource(multiroomtest::encodeFlac(fixture), "ramp.flac")));
    CHECK(decoder.info().totalFrames.has_value());
    CHECK_EQ(*decoder.info().totalFrames, 7777u);
}

// A decoded frame is routinely larger than one block, and a decoder that
// dropped the remainder would lose audio at every block boundary. Seven frames
// at a time is the smallest call that is still a whole number of frames.
MULTIROOM_TEST(a_buffer_smaller_than_a_decoded_frame_loses_nothing)
{
    const auto fixture = multiroomtest::makeRamp(44100, 2, 24, 3000);
    const auto reference = decodeAll(memorySource(multiroomtest::encodeFlac(fixture), "a.flac"), 4096);
    const auto chopped = decodeAll(memorySource(multiroomtest::encodeFlac(fixture), "b.flac"), 7);

    CHECK_EQ(chopped.frames, reference.frames);
    CHECK_EQ(chopped.samples.size(), reference.samples.size());
    CHECK(chopped.samples == reference.samples);
}

MULTIROOM_TEST(the_decoder_refuses_what_it_cannot_do_rather_than_doing_nothing)
{
    const auto fixture = multiroomtest::makeRamp(44100, 2, 16, 100);

    {
        FfmpegDecoder decoder;
        std::vector<std::byte> buffer(1024);
        CHECK_ERR(decoder.decode(buffer), DecodeError::NotOpened);
    }
    {
        FfmpegDecoder decoder;
        CHECK_OK(decoder.open(memorySource(multiroomtest::encodeWav(fixture), "ramp.wav")));
        CHECK_ERR(decoder.open(memorySource(multiroomtest::encodeWav(fixture), "again.wav")),
                  DecodeError::AlreadyOpened);

        // Three bytes is less than one frame of 16-bit stereo, and a
        // zero-length read would make the pipeline spin.
        std::vector<std::byte> tiny(3);
        CHECK_ERR(decoder.decode(tiny), DecodeError::BufferTooSmall);
    }
    {
        FfmpegDecoder decoder;
        std::vector<std::byte> noise(4096, std::byte{0x5A});
        CHECK_ERR(decoder.open(memorySource(noise, "noise.bin")), DecodeError::UnknownContainer);
    }
    {
        // A container FFmpeg can identify, with nothing in it this server can
        // play: a RIFF header claiming zero channels.
        auto broken = multiroomtest::encodeWav(fixture);
        broken[22] = std::byte{0};
        broken[23] = std::byte{0};
        FfmpegDecoder decoder;
        auto opened = decoder.open(memorySource(broken, "zero-channels.wav"));
        CHECK(!opened.has_value());
    }
}

MULTIROOM_TEST(a_source_that_fails_is_reported_as_the_source_failing)
{
    const auto fixture = multiroomtest::makeRamp(44100, 2, 24, 200000);
    auto bytes = multiroomtest::encodeFlac(fixture);

    // Enough for avformat to probe the container, then nothing.
    FfmpegDecoder decoder;
    auto opened = decoder.open(std::make_unique<FailingSource>(bytes, 128 * 1024,
                                                               MediaSourceError::Timeout));
    if (!opened)
    {
        CHECK_ERR(opened, DecodeError::SourceFailed);
        CHECK(opened.error().detail.find("Timeout") != std::string::npos);
        return;
    }

    std::vector<std::byte> buffer(4096 * decoder.info().format.bytesPerFrame());
    for (;;)
    {
        auto span = decoder.decode(buffer);
        if (span)
            continue;

        // The reason has to be the source's own, with its message attached --
        // "DecodeFailed: -5" is what this test exists to prevent.
        CHECK_ERR(span, DecodeError::SourceFailed);
        CHECK(span.error().detail.find("Timeout") != std::string::npos);
        CHECK(span.error().detail.find("the far end went quiet") != std::string::npos);
        break;
    }
}

MULTIROOM_TEST(a_file_on_disk_decodes_the_same_as_the_bytes_in_memory)
{
    const auto fixture = multiroomtest::makeRamp(48000, 2, 24, 3000);
    const auto bytes = multiroomtest::encodeFlac(fixture);

    multiroomtest::TempDirectory temp("decoder");
    const std::string path = temp.file("ramp.flac");
    {
        std::ofstream stream(path, std::ios::binary);
        stream.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
    }

    auto source = FileMediaSource::open(path);
    CHECK_OK(source);

    const auto decoded = decodeAll(std::move(*source), 1024);
    checkAgainstFixture(decoded, fixture);
}

#endif // MULTIROOM_WITH_FFMPEG
