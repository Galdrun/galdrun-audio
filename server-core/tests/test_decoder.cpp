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
#include <filesystem>
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

// The bytes in memory, remembering the furthest one read: what an open costs
// a source that has to fetch each byte it is asked for.
class FurthestReadSource final : public IMediaSource
{
public:
    explicit FurthestReadSource(std::vector<std::byte> bytes)
        : bytes_(std::move(bytes)), name_("furthest")
    {
    }

    const std::string& describe() const noexcept override { return name_; }
    bool seekable() const noexcept override { return true; }
    std::optional<uint64_t> sizeBytes() const noexcept override { return bytes_.size(); }
    uint64_t position() const noexcept override { return position_; }
    uint64_t furthest() const noexcept { return furthest_; }

    Result<size_t, MediaSourceError> read(std::span<std::byte> buffer) override
    {
        const size_t count =
            std::min(buffer.size(), static_cast<size_t>(bytes_.size() - position_));
        std::memcpy(buffer.data(), bytes_.data() + position_, count);
        position_ += count;
        furthest_ = std::max(furthest_, position_);
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
    std::string name_;
    uint64_t position_ = 0;
    uint64_t furthest_ = 0;
};

// `count` frames from where the decoder is, as samples.
std::vector<int32_t> decodeFrames(FfmpegDecoder& decoder, uint64_t expectedFirst, uint32_t count)
{
    const AudioFormat format = decoder.info().format;
    std::vector<std::byte> buffer(static_cast<size_t>(count) * format.bytesPerFrame());
    std::vector<int32_t> samples;
    bool first = true;
    while (samples.size() < static_cast<size_t>(count) * format.channels)
    {
        auto span = decoder.decode(buffer);
        CHECK_OK(span);
        if (!span)
            break;
        if (first)
            CHECK_EQ(static_cast<uint64_t>(span->firstFrame), expectedFirst);
        first = false;
        auto values = multiroomtest::readSamples(std::span<const std::byte>(buffer).first(span->byteCount),
                                                 format.sampleType);
        samples.insert(samples.end(), values.begin(), values.end());
    }
    samples.resize(std::min(samples.size(), static_cast<size_t>(count) * format.channels));
    return samples;
}

std::vector<int32_t> fixtureFrames(const multiroomtest::PcmFixture& fixture, uint64_t from, uint32_t count)
{
    const auto begin = fixture.samples.begin() + static_cast<std::ptrdiff_t>(from * fixture.channels);
    return {begin, begin + static_cast<std::ptrdiff_t>(static_cast<size_t>(count) * fixture.channels)};
}

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

// --- fragmented MP4, as streaming services send FLAC -------------------------
//
// 30 fragments of four 4096-frame blocks, with no index: FFmpeg's MP4
// demuxer would read every fragment's header to build one before the first
// sample, which over HTTP is most of the file fetched before playback.

namespace {

constexpr uint32_t kFragmentBlock = 4096;
constexpr uint32_t kBlocksPerFragment = 4;
constexpr uint32_t kFragments = 30;

multiroomtest::PcmFixture fragmentedFixture()
{
    return multiroomtest::makeRamp(44100, 2, 16, kFragmentBlock * kBlocksPerFragment * kFragments);
}

std::vector<std::byte> fragmentedMp4(const multiroomtest::PcmFixture& fixture)
{
    return multiroomtest::encodeFragmentedMp4Flac(fixture, kFragmentBlock, kBlocksPerFragment);
}

} // namespace

MULTIROOM_TEST(a_fragmented_mp4_decodes_to_the_samples_that_were_written)
{
    const auto fixture = fragmentedFixture();
    const auto decoded = decodeAll(memorySource(fragmentedMp4(fixture), "ramp.mp4"), 1000);
    checkAgainstFixture(decoded, fixture);
    CHECK_EQ(decoded.info.containerName, std::string("mov,mp4,m4a,3gp,3g2,mj2"));
    CHECK_EQ(decoded.info.codecName, std::string("flac"));
}

MULTIROOM_TEST(a_fragmented_mp4_opens_from_its_start_without_reading_every_fragment)
{
    const auto fixture = fragmentedFixture();
    const auto bytes = fragmentedMp4(fixture);
    const uint64_t size = bytes.size();
    auto source = std::make_unique<FurthestReadSource>(bytes);
    const FurthestReadSource& seen = *source;

    FfmpegDecoder decoder;
    CHECK_OK(decoder.open(std::move(source)));

    // Read as far as the first fragments, not to the last moof.
    CHECK(seen.furthest() < size / 4);
    // Its length is unknown until played: the headers say 0, and the
    // fragments read so far are not the track.
    CHECK(!decoder.info().totalFrames.has_value());
}

MULTIROOM_TEST(a_fragmented_mp4_seeks_exactly_past_the_fragments_it_has_read_and_back)
{
    const auto fixture = fragmentedFixture();
    FfmpegDecoder decoder;
    CHECK_OK(decoder.open(memorySource(fragmentedMp4(fixture), "ramp.mp4")));

    const uint64_t far = static_cast<uint64_t>(kFragmentBlock) * kBlocksPerFragment * 25 + 1234;
    CHECK_OK(decoder.seekTo(far));
    CHECK(decodeFrames(decoder, far, 3000) == fixtureFrames(fixture, far, 3000));

    // Back into what has been read, and into the first fragment.
    const uint64_t back = static_cast<uint64_t>(kFragmentBlock) * kBlocksPerFragment * 3 + 77;
    CHECK_OK(decoder.seekTo(back));
    CHECK(decodeFrames(decoder, back, 3000) == fixtureFrames(fixture, back, 3000));
    CHECK_OK(decoder.seekTo(5));
    CHECK(decodeFrames(decoder, 5, 100) == fixtureFrames(fixture, 5, 100));

    CHECK_ERR(decoder.seekTo(fixture.frames + 10), DecodeError::OffsetOutOfRange);
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

MULTIROOM_TEST(a_concat_list_in_the_media_cannot_open_a_local_file)
{
    // Every byte is the source's: a playlist inside it naming a file next to
    // the process must not be read from disk.
    const auto fixture = multiroomtest::makeRamp(44100, 2, 16, 4410);
    const auto wav = multiroomtest::encodeWav(fixture);
    const std::string name = "galdrun-concat-probe.wav";
    const std::filesystem::path local = std::filesystem::current_path() / name;
    {
        std::ofstream stream(local, std::ios::binary);
        stream.write(reinterpret_cast<const char*>(wav.data()),
                     static_cast<std::streamsize>(wav.size()));
    }

    const std::string list = "ffconcat version 1.0\nfile '" + name + "'\n";
    std::vector<std::byte> bytes(list.size());
    std::memcpy(bytes.data(), list.data(), list.size());

    uint64_t frames = 0;
    {
        FfmpegDecoder decoder;
        if (decoder.open(memorySource(std::move(bytes), "list")))
        {
            std::vector<std::byte> buffer(4096);
            while (auto span = decoder.decode(buffer))
                frames += span->frameCount;
        }
    }
    std::error_code ignored;
    std::filesystem::remove(local, ignored);
    CHECK_EQ(frames, uint64_t{0});
}

#endif // MULTIROOM_WITH_FFMPEG
