#pragma once

#include "audio/AudioSource.h"
#include "audio/IAudioBackend.h"

#include <memory>
#include <string>

namespace multiroom {

// Platform-independent playback control. Knows nothing about WASAPI, COM,
// snd_pcm_t or COM apartments -- only IAudioBackend.
//
// The backend is injected rather than created internally, so the engine can be
// tested against a stub with no sound card present.
class AudioEngine
{
public:
    struct Settings
    {
        std::string deviceId; // empty selects the system default device
        AudioFormat format;
        AudioAccessMode accessMode = AudioAccessMode::Exclusive;
        uint32_t bufferFrames = 0; // 0 lets the driver choose
    };

    explicit AudioEngine(std::unique_ptr<IAudioBackend> backend);
    ~AudioEngine();

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    // Must be called before open(). The engine does not take ownership, and
    // the source must outlive the engine.
    void setSource(IAudioSource* source);

    bool open(const Settings& settings, std::string& error);
    bool start(std::string& error);
    void stop();
    void close();

    bool isOpen() const;
    bool isRunning() const;

    // Valid once open() has succeeded; may differ from what was requested.
    AudioFormat format() const;
    uint32_t bufferFrames() const;

    AudioClockSnapshot clockSnapshot();
    uint64_t underruns() const;

    const IAudioBackend* backend() const { return backend_.get(); }

private:
    // Trampoline from the backend's plain function pointer into the source.
    static uint32_t renderCallback(void* userData, void* buffer, uint32_t frameCount);

    std::unique_ptr<IAudioBackend> backend_;

    // Written only from the control thread while the audio thread is stopped,
    // so the audio thread sees a stable value; start() is the barrier.
    IAudioSource* source_ = nullptr;
};

} // namespace multiroom
