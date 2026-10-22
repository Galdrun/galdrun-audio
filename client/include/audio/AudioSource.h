#pragma once

#include "audio/AudioFormat.h"

#include <cstdint>

namespace multiroom {

// Whatever fills the device buffer: a test tone now, the network ring buffer
// from phase 7 on.
//
// render() runs on the real-time audio thread. It must not allocate, lock
// unbounded, log, or do I/O, and it must return promptly -- the whole period
// budget is a few milliseconds.
//
// Samples are written directly in the device's negotiated format rather than
// converted by the engine: once audio comes from the server it already arrives
// in exactly that format, and an engine-side conversion layer would put a
// pointless copy in the hot path.
class IAudioSource
{
public:
    virtual ~IAudioSource() = default;

    // Called once from the control thread before the audio thread starts, so a
    // source may allocate or precompute here.
    virtual void prepare(const AudioFormat& format) = 0;

    // Fills up to frameCount interleaved frames and returns how many it
    // produced. A short return is an underrun: the backend silences the rest
    // and counts it.
    virtual uint32_t render(void* buffer, uint32_t frameCount) = 0;
};

} // namespace multiroom
