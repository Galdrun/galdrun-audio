#pragma once

#include "audio/IAudioBackend.h"
#include "audio/VolumeControl.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace multiroom {

// Render-thread state shared by the three backends. Stop fades for 10 ms,
// then supplies at least 10 ms of zeros. The backend must let that tail play
// before disabling its device, within the same bounded shutdown deadline.
class AudioShutdown
{
public:
    void prepare(const AudioFormat& format)
    {
        volume_.prepare(format);
        frameBytes_ = format.bytesPerFrame();
        fadeFrames_ = std::max<uint32_t>(1, (format.sampleRate + 99u) / 100u);
        fadeRemaining_ = fadeFrames_;
        silenceRemaining_ = fadeFrames_;
        active_ = false;
    }

    void begin(bool requested)
    {
        if (requested && !active_)
        {
            active_ = true;
            deadline_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        }
    }

    bool active() const { return active_; }
    bool finished() const { return active_ && fadeRemaining_ == 0 && silenceRemaining_ == 0; }
    int remainingMs() const
    {
        if (!active_)
            return 500;
        return static_cast<int>(std::max<int64_t>(0, std::chrono::duration_cast<
            std::chrono::milliseconds>(deadline_ - std::chrono::steady_clock::now()).count()));
    }

    uint32_t render(AudioRenderCallback callback, void* context, void* buffer, uint32_t frames)
    {
        if (!active_)
            return callback(context, buffer, frames);

        const uint32_t fading = std::min(frames, fadeRemaining_);
        if (fading != 0)
        {
            const uint32_t filled = std::min(fading, callback(context, buffer, fading));
            std::memset(static_cast<unsigned char*>(buffer) + filled * frameBytes_, 0,
                        static_cast<size_t>(fading - filled) * frameBytes_);
            // A one-frame fade is already its final zero sample.
            const double step = fadeFrames_ > 1 ? -1.0 / (fadeFrames_ - 1) : 0.0;
            const double gain = fadeFrames_ > 1
                ? static_cast<double>(fadeRemaining_ - 1) / (fadeFrames_ - 1) : 0.0;
            volume_.fade(buffer, fading, gain, step);
            fadeRemaining_ -= fading;
        }
        const uint32_t silent = frames - fading;
        std::memset(static_cast<unsigned char*>(buffer) + fading * frameBytes_, 0,
                    static_cast<size_t>(silent) * frameBytes_);
        silenceRemaining_ -= std::min(silenceRemaining_, silent);
        return frames; // Requested silence is not an underrun.
    }

private:
    VolumeControl volume_;
    uint32_t frameBytes_ = 0;
    uint32_t fadeFrames_ = 0;
    uint32_t fadeRemaining_ = 0;
    uint32_t silenceRemaining_ = 0;
    bool active_ = false;
    std::chrono::steady_clock::time_point deadline_;
};

} // namespace multiroom
