#pragma once

#include "audio/AudioSource.h"

#include <atomic>
#include <cstddef>

namespace multiroom {

// Test signal for bringing up the playback path before the network exists.
// Also the only place in the client that converts float samples to the device
// format -- server audio arrives ready to copy.
class ToneGenerator final : public IAudioSource
{
public:
    explicit ToneGenerator(double frequencyHz = 440.0, double amplitude = 0.25);

    void prepare(const AudioFormat& format) override;
    uint32_t render(void* buffer, uint32_t frameCount) override;

    // Frames produced so far. Read from the control thread while the audio
    // thread writes it, so it is atomic -- and 32 bits, because 64-bit atomics
    // are not lock-free on ARMv6.
    uint32_t renderedFrames() const;

private:
    double frequency_;
    double amplitude_;

    AudioFormat format_;
    double phase_ = 0.0;
    double phaseStep_ = 0.0;

    std::atomic<uint32_t> frames_;
};

// Writes one sample into an interleaved buffer in the given format.
// Exposed for unit testing; the conversion is easy to get subtly wrong.
void writeSample(void* buffer, size_t sampleIndex, float value,
                 AudioFormat::SampleType type);

} // namespace multiroom
