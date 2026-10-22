#pragma once

// The audio thread's end of the ring buffer.
//
// Whatever fills the buffer -- the audio connection carrying the server's
// stream, or the buffered test tone -- is the single producer; this is the
// single consumer. An empty buffer is an underrun: the source returns short, the
// backend silences the rest of the period and counts it, and nothing waits.

#include "audio/AudioRingBuffer.h"
#include "audio/AudioSource.h"

#include <atomic>

namespace multiroom {

class RingBufferSource final : public IAudioSource
{
public:
    // The buffer is sized in milliseconds rather than frames because the frame
    // size and rate are only known once the driver has granted a format, which
    // happens in prepare().
    RingBufferSource(AudioRingBuffer& buffer, uint32_t capacityMilliseconds);

    // Configures the buffer for the granted format. Allocates, which is
    // allowed here: this runs on the control thread before the audio thread
    // starts.
    void prepare(const AudioFormat& format) override;

    uint32_t render(void* buffer, uint32_t frameCount) override;

    // Frames delivered to the device and frames the device had to silence.
    // Read from the control thread while the audio thread writes them, so both
    // are 32-bit atomics -- 64-bit atomics are not lock-free on ARMv6.
    uint32_t renderedFrames() const;
    uint32_t underrunFrames() const;

    uint32_t bufferedFrames() const { return buffer_.readableFrames(); }

private:
    AudioRingBuffer& buffer_;
    uint32_t capacityMilliseconds_;

    std::atomic<uint32_t> rendered_;
    std::atomic<uint32_t> underrun_;
};

} // namespace multiroom
