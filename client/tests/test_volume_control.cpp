#include "TestSupport.h"

#include "audio/VolumeControl.h"

#include <cstring>
#include <thread>
#include <vector>

using namespace multiroom;

namespace {

AudioFormat stereo(AudioFormat::SampleType type, uint32_t rate = 48000)
{
    AudioFormat format;
    format.sampleRate = rate;
    format.channels = 2;
    format.sampleType = type;
    return format;
}

// One period of a constant sample value, so a gain is readable directly off
// the result rather than inferred from a waveform.
std::vector<unsigned char> constantInt16(uint32_t frames, int16_t value)
{
    std::vector<unsigned char> block(static_cast<size_t>(frames) * 4);
    for (uint32_t i = 0; i < frames * 2; ++i)
        std::memcpy(block.data() + static_cast<size_t>(i) * 2, &value, 2);
    return block;
}

int16_t int16At(const std::vector<unsigned char>& block, uint32_t sample)
{
    int16_t value = 0;
    std::memcpy(&value, block.data() + static_cast<size_t>(sample) * 2, 2);
    return value;
}

int32_t int32At(const std::vector<unsigned char>& block, uint32_t sample)
{
    int32_t value = 0;
    std::memcpy(&value, block.data() + static_cast<size_t>(sample) * 4, 4);
    return value;
}

// The ramp is 15 ms, which is 720 frames at 48 kHz. A period longer than that
// therefore contains the whole of it and ends at the target gain.
const uint32_t kPastTheRamp = 1024;

} // namespace

MULTIROOM_TEST(volume_control_leaves_full_volume_untouched)
{
    VolumeControl volume;
    volume.prepare(stereo(AudioFormat::SampleType::Int16));

    std::vector<unsigned char> block = constantInt16(64, 1000);
    volume.process(block.data(), 64, 0);

    // Not "close to 1000": a gain of one must not put a rounding error into
    // every sample of a stream nobody asked to change.
    for (uint32_t i = 0; i < 128; ++i)
        CHECK_EQ(int16At(block, i), static_cast<int16_t>(1000));
    CHECK_NEAR(volume.gain(), 1.0, 1e-12);
    CHECK(!volume.muted());
}

MULTIROOM_TEST(volume_control_ramps_into_a_new_gain_rather_than_stepping)
{
    VolumeControl volume;
    volume.prepare(stereo(AudioFormat::SampleType::Int16));
    volume.set(0.5, false);

    // The first period is the ramp: it starts at the old gain and has not
    // reached the new one, which is what keeps the change from being a click.
    std::vector<unsigned char> first = constantInt16(64, 10000);
    volume.process(first.data(), 64, 0);
    CHECK(int16At(first, 0) > 9900);
    CHECK(int16At(first, 126) < int16At(first, 0));
    CHECK(int16At(first, 126) > 5000);

    // Monotone across the whole period, frame by frame: a ramp that went up
    // and down again would be inaudible in the endpoints and audible in the
    // sound.
    for (uint32_t frame = 1; frame < 64; ++frame)
        CHECK(int16At(first, frame * 2) <= int16At(first, (frame - 1) * 2));

    // Both channels of a frame get the same gain; a ramp per sample instead of
    // per frame would pan the image while it ran.
    for (uint32_t frame = 0; frame < 64; ++frame)
        CHECK_EQ(int16At(first, frame * 2), int16At(first, frame * 2 + 1));

    std::vector<unsigned char> rest = constantInt16(kPastTheRamp, 10000);
    volume.process(rest.data(), kPastTheRamp, 64);
    CHECK_EQ(int16At(rest, 2 * kPastTheRamp - 1), static_cast<int16_t>(5000));
    CHECK_NEAR(volume.appliedGain(), 0.5, 1e-9);
}

MULTIROOM_TEST(volume_control_mutes_and_comes_back_to_the_volume_it_had)
{
    VolumeControl volume;
    volume.prepare(stereo(AudioFormat::SampleType::Int16));
    volume.set(0.25, false);

    std::vector<unsigned char> settle = constantInt16(kPastTheRamp, 8000);
    volume.process(settle.data(), kPastTheRamp, 0);
    CHECK_EQ(int16At(settle, 2 * kPastTheRamp - 1), static_cast<int16_t>(2000));

    // Muting keeps the gain, which is what makes the mute reversible without
    // the server having to send the volume again.
    volume.set(volume.gain(), true);
    std::vector<unsigned char> muted = constantInt16(kPastTheRamp, 8000);
    volume.process(muted.data(), kPastTheRamp, kPastTheRamp);
    CHECK_EQ(int16At(muted, 2 * kPastTheRamp - 1), static_cast<int16_t>(0));
    CHECK(volume.muted());
    CHECK_NEAR(volume.gain(), 0.25, 1e-12);

    volume.set(volume.gain(), false);
    std::vector<unsigned char> back = constantInt16(kPastTheRamp, 8000);
    volume.process(back.data(), kPastTheRamp, 2 * kPastTheRamp);
    CHECK_EQ(int16At(back, 2 * kPastTheRamp - 1), static_cast<int16_t>(2000));
}

MULTIROOM_TEST(volume_control_applies_a_scheduled_change_on_its_own_frame)
{
    VolumeControl volume;
    volume.prepare(stereo(AudioFormat::SampleType::Int16));
    volume.setAt(0.0, false, 2000);
    CHECK(volume.isScheduled());
    CHECK_EQ(volume.scheduledDeviceFrame(), static_cast<uint64_t>(2000));

    // A period that ends before the scheduled frame is untouched.
    std::vector<unsigned char> before = constantInt16(1024, 10000);
    volume.process(before.data(), 1024, 0);
    CHECK_EQ(int16At(before, 2047), static_cast<int16_t>(10000));

    // The period the frame falls into changes exactly there, not at its start:
    // frame 2000 is the 976th frame of a period that began at 1024. The ramp
    // begins at the gain the room already had, so that frame is the last one
    // still at full volume and the fall starts immediately after it.
    std::vector<unsigned char> across = constantInt16(1024, 10000);
    volume.process(across.data(), 1024, 1024);
    CHECK_EQ(int16At(across, 975 * 2), static_cast<int16_t>(10000));
    CHECK_EQ(int16At(across, 976 * 2), static_cast<int16_t>(10000));
    CHECK(int16At(across, 977 * 2) < 10000);
    CHECK(int16At(across, 1023 * 2) < 9500);
}

MULTIROOM_TEST(volume_control_scales_every_sample_layout)
{
    // Int16 is covered above; these are the three that differ in how a sample
    // is loaded and stored, and getting one wrong costs 48 dB rather than
    // producing obvious noise.
    {
        VolumeControl volume;
        volume.prepare(stereo(AudioFormat::SampleType::Int24));
        volume.set(0.5, false);

        std::vector<unsigned char> block(static_cast<size_t>(kPastTheRamp) * 6);
        for (uint32_t i = 0; i < kPastTheRamp * 2; ++i)
        {
            // -4000000 packed little endian: negative, so the sign extension
            // on the way in is part of what is under test.
            const int32_t value = -4000000;
            block[i * 3 + 0] = static_cast<unsigned char>(value & 0xFF);
            block[i * 3 + 1] = static_cast<unsigned char>((value >> 8) & 0xFF);
            block[i * 3 + 2] = static_cast<unsigned char>((value >> 16) & 0xFF);
        }

        volume.process(block.data(), kPastTheRamp, 0);

        const size_t last = static_cast<size_t>(kPastTheRamp * 2 - 1) * 3;
        int32_t decoded = static_cast<int32_t>(static_cast<uint32_t>(block[last]) |
                                               (static_cast<uint32_t>(block[last + 1]) << 8) |
                                               (static_cast<uint32_t>(block[last + 2]) << 16));
        if ((decoded & 0x800000) != 0)
            decoded |= static_cast<int32_t>(0xFF000000u);
        CHECK_EQ(decoded, -2000000);
    }

    {
        VolumeControl volume;
        volume.prepare(stereo(AudioFormat::SampleType::Int32));
        volume.set(0.5, false);

        std::vector<unsigned char> block(static_cast<size_t>(kPastTheRamp) * 8);
        const int32_t value = 2000000000;
        for (uint32_t i = 0; i < kPastTheRamp * 2; ++i)
            std::memcpy(block.data() + static_cast<size_t>(i) * 4, &value, 4);

        volume.process(block.data(), kPastTheRamp, 0);
        CHECK_EQ(int32At(block, kPastTheRamp * 2 - 1), 1000000000);
    }

    {
        VolumeControl volume;
        volume.prepare(stereo(AudioFormat::SampleType::Float32));
        volume.set(0.5, false);

        std::vector<unsigned char> block(static_cast<size_t>(kPastTheRamp) * 8);
        const float value = 0.8f;
        for (uint32_t i = 0; i < kPastTheRamp * 2; ++i)
            std::memcpy(block.data() + static_cast<size_t>(i) * 4, &value, 4);

        volume.process(block.data(), kPastTheRamp, 0);

        float last = 0.0f;
        std::memcpy(&last, block.data() + static_cast<size_t>(kPastTheRamp * 2 - 1) * 4, 4);
        CHECK_NEAR(static_cast<double>(last), 0.4, 1e-6);
    }
}

// Int24In32 is one enum value over two layouts -- ALSA's S24_LE keeps the
// value in the low three bytes, WASAPI left-justifies it -- so scaling the
// container as if it were an Int32 is wrong in a different way on each
// platform. This is the case the layout test above did not cover.
MULTIROOM_TEST(volume_control_scales_the_valid_bits_of_a_24_in_32_container)
{
    VolumeControl volume;
    volume.prepare(stereo(AudioFormat::SampleType::Int24In32));
    volume.set(0.5, false);

    // The 24-bit value under test, and a container whose *padding* is wrong on
    // purpose: the padding is not signal, and a gain that reads it produces a
    // sample nobody sent.
    const int32_t value = -4000000;
    std::vector<unsigned char> block(static_cast<size_t>(kPastTheRamp) * 8);
    for (uint32_t i = 0; i < kPastTheRamp * 2; ++i)
    {
#if defined(_WIN32)
        // Valid bits 31..8, low byte padding. Dirty it.
        const int32_t raw = static_cast<int32_t>((static_cast<uint32_t>(value) << 8) | 0xABu);
#else
        // Valid bits 23..0, top byte padding. Dirty it with the sign extension
        // inverted, which is what the old code read as a huge negative sample.
        const int32_t raw = static_cast<int32_t>((static_cast<uint32_t>(value) & 0x00FFFFFFu) |
                                                 0x7F000000u);
#endif
        std::memcpy(block.data() + static_cast<size_t>(i) * 4, &raw, 4);
    }

    volume.process(block.data(), kPastTheRamp, 0);

    const int32_t result = int32At(block, kPastTheRamp * 2 - 1);
#if defined(_WIN32)
    // Halved in the valid bits, and the padding handed back to WASAPI as the
    // zero it is contracted to be.
    CHECK_EQ(result >> 8, value / 2);
    CHECK_EQ(result & 0xFF, 0);
#else
    // Halved in the valid bits, and the padding handed back to ALSA as zero:
    // S24_LE is three bytes of signal in a four-byte word, and the fourth is
    // not the driver's to interpret.
    CHECK_EQ(static_cast<uint32_t>(result) >> 24, 0u);
    CHECK_EQ(static_cast<uint32_t>(result) & 0x00FFFFFFu,
             static_cast<uint32_t>(value / 2) & 0x00FFFFFFu);
#endif
}

// The seqlock is single-producer, and a second thread is refused rather than
// warned about: a warning does not stop a race. This is observable without a
// log sink, because a refused call leaves the gain exactly as it was. It logs
// one ERROR line, which is expected output for this test.
MULTIROOM_TEST(volume_control_refuses_a_second_producer_thread)
{
    VolumeControl volume;
    volume.prepare(stereo(AudioFormat::SampleType::Int16));
    volume.set(0.25, false);
    CHECK_NEAR(volume.gain(), 0.25, 1e-12);

    // Joined, so the two never actually overlap: what is under test is the
    // identity of the caller, not a race the test would have to provoke.
    std::thread other([&volume]() {
        volume.set(1.0, true);
        volume.setAt(0.75, false, 4096);
        volume.prepare(stereo(AudioFormat::SampleType::Float32));
    });
    other.join();

    // Nothing the foreign thread asked for landed: not the gain, not the mute,
    // not the schedule, and not the format.
    CHECK_NEAR(volume.gain(), 0.25, 1e-12);
    CHECK(!volume.muted());
    CHECK(!volume.isScheduled());

    // Still the Int16 it was prepared with, and still working for the thread
    // that owns it.
    std::vector<unsigned char> block = constantInt16(kPastTheRamp, 8000);
    volume.process(block.data(), kPastTheRamp, 0);
    CHECK_EQ(int16At(block, 2 * kPastTheRamp - 1), static_cast<int16_t>(2000));

    volume.set(0.5, false);
    CHECK_NEAR(volume.gain(), 0.5, 1e-12);
}

MULTIROOM_TEST(volume_control_clamps_what_it_cannot_play)
{
    VolumeControl volume;
    volume.prepare(stereo(AudioFormat::SampleType::Int16));

    // There is no headroom in a fixed-point stream, so a gain above unity is
    // clamped rather than allowed to clip -- and clamping it here is what
    // keeps the sample loop free of a per-sample test.
    volume.set(4.0, false);
    CHECK_NEAR(volume.gain(), 1.0, 1e-12);

    volume.set(-1.0, false);
    CHECK_NEAR(volume.gain(), 0.0, 1e-12);
}

MULTIROOM_TEST(volume_control_keeps_the_volume_across_a_reopened_device)
{
    VolumeControl volume;
    volume.prepare(stereo(AudioFormat::SampleType::Int16));
    volume.set(0.5, true);

    // A device that was reopened is not a reason for a room to come back at
    // full volume; the ramp is what belongs to the old format, not the gain.
    volume.prepare(stereo(AudioFormat::SampleType::Int16, 44100));
    CHECK_NEAR(volume.gain(), 0.5, 1e-12);
    CHECK(volume.muted());
    CHECK(!volume.isScheduled());

    std::vector<unsigned char> block = constantInt16(64, 10000);
    volume.process(block.data(), 64, 0);
    CHECK_EQ(int16At(block, 0), static_cast<int16_t>(0)); // muted from the first frame
}

MULTIROOM_TEST(volume_control_is_already_at_the_new_gain_after_silence)
{
    VolumeControl volume;
    volume.prepare(stereo(AudioFormat::SampleType::Int16));

    // The device runs from PREPARE_STREAM on and plays silence until the
    // start, so a volume set in between must be in place on the first audible
    // frame rather than fade into it over the first 15 ms of music.
    volume.set(0.5, false);
    volume.silence(1024, 0);
    CHECK_NEAR(volume.appliedGain(), 0.5, 1e-9);

    std::vector<unsigned char> block = constantInt16(64, 10000);
    volume.process(block.data(), 64, 1024);
    CHECK_EQ(int16At(block, 0), static_cast<int16_t>(5000));
    CHECK_EQ(int16At(block, 127), static_cast<int16_t>(5000));
}

// Silence consumes a ramp at the rate audible frames do rather than cancelling
// it. A room that goes quiet halfway through a fade and comes back has half a
// fade left, not none: snapping to the target instead is a step in the
// waveform on the frame the audio returns, which is the click the ramp exists
// to avoid.
MULTIROOM_TEST(volume_control_carries_a_ramp_through_silence_rather_than_ending_it)
{
    VolumeControl volume;
    volume.prepare(stereo(AudioFormat::SampleType::Int16));

    // 15 ms at 48 kHz is 720 frames, so 360 is exactly half of the ramp.
    volume.set(0.0, false);
    volume.silence(360, 0);
    CHECK_NEAR(volume.appliedGain(), 0.5, 2e-3);

    // And the second half still has to be walked, not jumped.
    volume.silence(180, 360);
    CHECK_NEAR(volume.appliedGain(), 0.25, 2e-3);

    volume.silence(180, 540);
    CHECK_NEAR(volume.appliedGain(), 0.0, 1e-9);
}

// A change armed for a frame inside a silent period takes effect on that
// frame, not at the start of the period and not at the end of it: the silent
// frames are on the same device axis as the audible ones, and section 39's
// whole point is that every room moves at the same moment.
MULTIROOM_TEST(volume_control_applies_a_scheduled_change_inside_a_silent_period)
{
    VolumeControl volume;
    volume.prepare(stereo(AudioFormat::SampleType::Int16));
    volume.setAt(0.5, false, 1000);

    // Up to the scheduled frame the gain is untouched, even though the change
    // has already been published and adopted.
    volume.silence(1000, 0);
    CHECK_NEAR(volume.appliedGain(), 1.0, 1e-9);

    // From it, the ramp runs; 1024 frames is past the 720 it needs.
    volume.silence(1024, 1000);
    CHECK_NEAR(volume.appliedGain(), 0.5, 1e-9);

    std::vector<unsigned char> block = constantInt16(64, 10000);
    volume.process(block.data(), 64, 2024);
    CHECK_EQ(int16At(block, 0), static_cast<int16_t>(5000));
}
