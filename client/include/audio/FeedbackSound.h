#pragma once

#include "audio/AudioFormat.h"

#include <cstdint>
#include <vector>

namespace multiroom {

// Short button-press clicks mixed over whatever the device is playing, the way
// the Connector 3 stock firmware answers a touch field. Synthesized (not the
// vendor's files) at the device's own rate, so no resampling is needed.
//
// play() may be called from any thread; every open AudioEngine mixes the
// request into its next period. A device that is not running (standby, no
// server) does not click.
class FeedbackSound
{
public:
    enum class Sound : uint32_t
    {
        Touch = 1,
        PlayPause = 2,
    };

    static void play(Sound sound);

    // Control thread, before the render thread starts: builds the clicks for
    // this format and forgets a request made before the device was open.
    void prepare(const AudioFormat& format);

    // Render thread: adds a pending or still sounding click to `frames` frames.
    void mix(void* buffer, uint32_t frames);

private:
    AudioFormat format_;
    std::vector<float> touch_;
    std::vector<float> playPause_;
    const std::vector<float>* current_ = nullptr;
    size_t position_ = 0;
    uint32_t seen_ = 0;
};

} // namespace multiroom
