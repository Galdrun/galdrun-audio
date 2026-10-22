#include "audio/ToneGenerator.h"

#include <cmath>
#include <cstring>

namespace multiroom {
namespace {

const double kTwoPi = 6.283185307179586476925286766559;

int32_t clampToRange(double value, int32_t low, int32_t high)
{
    if (value <= static_cast<double>(low))
        return low;
    if (value >= static_cast<double>(high))
        return high;
    return static_cast<int32_t>(value);
}

} // namespace

// Container alignment for 24-in-32 differs between the platforms, and
// getting it wrong costs 48 dB rather than producing obvious noise, so it is
// spelled out here rather than left implicit:
//
//   ALSA S24_LE          valid bits in the LOW three bytes, MSB zero
//   WASAPI 24-in-32      valid bits in the HIGH three bytes (left-justified),
//                        which is what wValidBitsPerSample < wBitsPerSample
//                        means in a WAVEFORMATEXTENSIBLE
//   ESP32 I2S            left-justified like WASAPI: sent MSB first in a
//                        32-bit slot
//
// Packed 24-bit (ALSA S24_3LE) is three bytes with no container at all.
void writeSample(void* buffer, size_t sampleIndex, float value, AudioFormat::SampleType type)
{
    const double scaled = static_cast<double>(value);
    unsigned char* bytes = static_cast<unsigned char*>(buffer);

    switch (type)
    {
    case AudioFormat::SampleType::Int16:
    {
        const int32_t sample = clampToRange(scaled * 32767.0, -32768, 32767);
        int16_t narrow = static_cast<int16_t>(sample);
        std::memcpy(bytes + sampleIndex * 2, &narrow, 2);
        return;
    }

    case AudioFormat::SampleType::Int24:
    {
        const int32_t sample = clampToRange(scaled * 8388607.0, -8388608, 8388607);
        unsigned char* target = bytes + sampleIndex * 3;
        target[0] = static_cast<unsigned char>(sample & 0xFF);
        target[1] = static_cast<unsigned char>((sample >> 8) & 0xFF);
        target[2] = static_cast<unsigned char>((sample >> 16) & 0xFF);
        return;
    }

    case AudioFormat::SampleType::Int24In32:
    {
        const int32_t sample = clampToRange(scaled * 8388607.0, -8388608, 8388607);
#if defined(_WIN32) || defined(ESP_PLATFORM)
        const int32_t stored = sample * 256; // left-justified, low byte zero
#else
        const int32_t stored = sample; // ALSA keeps it in the low three bytes
#endif
        std::memcpy(bytes + sampleIndex * 4, &stored, 4);
        return;
    }

    case AudioFormat::SampleType::Int32:
    {
        // 2147483647.0 is not representable as a double multiplier without
        // rounding past the maximum, so scale from the 24-bit range and shift.
        const int32_t sample = clampToRange(scaled * 8388607.0, -8388608, 8388607);
        const int32_t stored = sample * 256;
        std::memcpy(bytes + sampleIndex * 4, &stored, 4);
        return;
    }

    case AudioFormat::SampleType::Float32:
    {
        float narrow = value;
        if (narrow > 1.0f)
            narrow = 1.0f;
        if (narrow < -1.0f)
            narrow = -1.0f;
        std::memcpy(bytes + sampleIndex * 4, &narrow, 4);
        return;
    }
    }
}

ToneGenerator::ToneGenerator(double frequencyHz, double amplitude)
    : frequency_(frequencyHz), amplitude_(amplitude), frames_(0)
{
}

void ToneGenerator::prepare(const AudioFormat& format)
{
    format_ = format;
    phase_ = 0.0;
    phaseStep_ = format.sampleRate > 0
                     ? kTwoPi * frequency_ / static_cast<double>(format.sampleRate)
                     : 0.0;
    frames_.store(0, std::memory_order_relaxed);
}

uint32_t ToneGenerator::render(void* buffer, uint32_t frameCount)
{
    if (buffer == nullptr || !format_.isValid())
        return 0;

    const uint16_t channels = format_.channels;

    for (uint32_t frame = 0; frame < frameCount; ++frame)
    {
        const float value = static_cast<float>(amplitude_ * std::sin(phase_));
        phase_ += phaseStep_;
        if (phase_ >= kTwoPi)
            phase_ -= kTwoPi;

        // Same signal on every channel: this is a bring-up tone, not content.
        const size_t base = static_cast<size_t>(frame) * channels;
        for (uint16_t channel = 0; channel < channels; ++channel)
            writeSample(buffer, base + channel, value, format_.sampleType);
    }

    frames_.fetch_add(frameCount, std::memory_order_relaxed);
    return frameCount;
}

uint32_t ToneGenerator::renderedFrames() const
{
    return frames_.load(std::memory_order_relaxed);
}

} // namespace multiroom
