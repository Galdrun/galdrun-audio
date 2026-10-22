#include "TestSupport.h"

#include "audio/AudioFormat.h"

using namespace multiroom;

namespace {

AudioFormat makeFormat(uint32_t rate, uint16_t channels, AudioFormat::SampleType type)
{
    AudioFormat format;
    format.sampleRate = rate;
    format.channels = channels;
    format.sampleType = type;
    return format;
}

} // namespace

MULTIROOM_TEST(audio_format_bytes_per_frame_for_all_five_layouts)
{
    using SampleType = AudioFormat::SampleType;

    // The table of MATP section 12, in bytes per *sample*. Int24 and
    // Int24In32 differ here and nowhere else, which is exactly why they are
    // two values and not one.
    CHECK_EQ(AudioFormat::bytesPerSample(SampleType::Int16), 2u);
    CHECK_EQ(AudioFormat::bytesPerSample(SampleType::Int24), 3u);
    CHECK_EQ(AudioFormat::bytesPerSample(SampleType::Int24In32), 4u);
    CHECK_EQ(AudioFormat::bytesPerSample(SampleType::Int32), 4u);
    CHECK_EQ(AudioFormat::bytesPerSample(SampleType::Float32), 4u);

    CHECK_EQ(makeFormat(44100, 2, SampleType::Int16).bytesPerFrame(), 4u);
    CHECK_EQ(makeFormat(44100, 2, SampleType::Int24).bytesPerFrame(), 6u);
    CHECK_EQ(makeFormat(44100, 2, SampleType::Int24In32).bytesPerFrame(), 8u);
    CHECK_EQ(makeFormat(44100, 2, SampleType::Int32).bytesPerFrame(), 8u);
    CHECK_EQ(makeFormat(44100, 2, SampleType::Float32).bytesPerFrame(), 8u);

    // Six channels, to prove the channel count is a factor and not a lookup.
    CHECK_EQ(makeFormat(48000, 6, SampleType::Int24).bytesPerFrame(), 18u);
    CHECK_EQ(makeFormat(48000, 1, SampleType::Int24In32).bytesPerFrame(), 4u);
}

MULTIROOM_TEST(audio_format_valid_bits_separate_the_two_24_bit_layouts)
{
    using SampleType = AudioFormat::SampleType;

    CHECK_EQ(AudioFormat::validBits(SampleType::Int16), 16u);
    CHECK_EQ(AudioFormat::validBits(SampleType::Int24), 24u);
    CHECK_EQ(AudioFormat::validBits(SampleType::Int24In32), 24u);
    CHECK_EQ(AudioFormat::validBits(SampleType::Int32), 32u);
    CHECK_EQ(AudioFormat::validBits(SampleType::Float32), 32u);

    // Same valid bits, different container. Confusing them produces noise.
    CHECK(AudioFormat::bytesPerSample(SampleType::Int24) !=
          AudioFormat::bytesPerSample(SampleType::Int24In32));
    CHECK(makeFormat(44100, 2, SampleType::Int24) != makeFormat(44100, 2, SampleType::Int24In32));
}

MULTIROOM_TEST(audio_format_validity_and_equality)
{
    using SampleType = AudioFormat::SampleType;

    CHECK(!AudioFormat{}.isValid());
    CHECK(!makeFormat(0, 2, SampleType::Int16).isValid());
    CHECK(!makeFormat(44100, 0, SampleType::Int16).isValid());
    CHECK(makeFormat(44100, 2, SampleType::Int16).isValid());

    CHECK(makeFormat(48000, 2, SampleType::Int32) == makeFormat(48000, 2, SampleType::Int32));
    CHECK(makeFormat(48000, 2, SampleType::Int32) != makeFormat(44100, 2, SampleType::Int32));
    CHECK(makeFormat(48000, 2, SampleType::Int32) != makeFormat(48000, 1, SampleType::Int32));
}

MULTIROOM_TEST(audio_format_names_round_trip)
{
    using SampleType = AudioFormat::SampleType;

    const SampleType all[] = {SampleType::Int16, SampleType::Int24, SampleType::Int24In32,
                              SampleType::Int32, SampleType::Float32};
    for (const SampleType type : all)
    {
        SampleType parsed = SampleType::Int16;
        CHECK(parseSampleType(sampleTypeName(type), parsed));
        CHECK(parsed == type);
    }

    SampleType unused = SampleType::Int16;
    CHECK(!parseSampleType("s24le", unused)); // the wire spelling is MATP's, not this type's
    CHECK(!parseSampleType("", unused));
    CHECK(!parseSampleType("Int16", unused));

    CHECK_EQ(describeFormat(makeFormat(48000, 2, SampleType::Int24In32)),
             std::string("48000 Hz 2ch int24in32"));
}

MULTIROOM_TEST(audio_format_widest_sample_type_is_four_bytes)
{
    // Everything that sizes a buffer from "the largest format" depends on this.
    CHECK_EQ(AudioFormat::bytesPerSample(kWidestSampleType), 4u);

    using SampleType = AudioFormat::SampleType;
    const SampleType all[] = {SampleType::Int16, SampleType::Int24, SampleType::Int24In32,
                              SampleType::Int32, SampleType::Float32};
    for (const SampleType type : all)
        CHECK(AudioFormat::bytesPerSample(type) <= AudioFormat::bytesPerSample(kWidestSampleType));
}
