#pragma once

#include "audio/IAudioBackend.h"
#include "audio/AudioShutdown.h"

#include "util/Wrap32.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace multiroom {

// ALSA backend. Internal to the audio layer: reach it through
// createPlatformAudioBackend() in audio/AudioBackend.h.
//
// alsa-lib is the only audio dependency: no PulseAudio, no PipeWire, no
// desktop session required, so this works on a headless ARMv6 board.
//
// The hardware clock (phase 4) is still a stub; everything else is live.
class AlsaBackend final : public IAudioBackend
{
public:
    AlsaBackend();
    ~AlsaBackend() override;

    std::vector<AudioDeviceInfo> enumeratePlaybackDevices(CapabilityScan scan) override;

    bool open(const AudioBackendConfig& config) override;
    bool start() override;
    void stop() override;
    void close() override;

    BackendState state() const override;
    AudioFormat currentFormat() const override { return format_; }
    uint32_t bufferFrames() const override { return periodFrames_; }

    AudioClockSnapshot clockSnapshot() override;
    uint64_t underrunCount() const override;
    std::string lastError() const override;

    std::unique_ptr<IHardwareVolume> openHardwareVolume(
        const AudioDeviceInfo& device) const override;
    std::unique_ptr<IHardwareClock> openHardwareClock(
        const AudioDeviceInfo& device) const override;

private:
    // Keeps snd_pcm_t out of this header.
    struct Impl;

    void renderLoop();
    // Closes the PCM handle. Assumes clockMutex_ is already held, so open()
    // can reuse it without re-locking.
    void closeLocked();

    std::unique_ptr<Impl> impl_;

    // Guards the PCM handle against a concurrent close() while a snapshot is
    // being taken. Control-path only; the audio thread never takes it.
    mutable std::mutex clockMutex_;
    // Widens framesWritten_ on the reading side. Owned by whoever calls
    // clockSnapshot(), and therefore protected by clockMutex_ too.
    Wrap32Extender frameExtender_;

    std::atomic<int> state_;
    std::atomic<bool> stopRequested_;
    // 32-bit on purpose: 64-bit atomics are not lock-free on ARMv6.
    std::atomic<uint32_t> underruns_;
    // Frames handed to the driver. Wraps after ~24.8 hours at 48 kHz, which
    // frameExtender_ turns back into a monotonic 64-bit count.
    std::atomic<uint32_t> framesWritten_;
    // Odd while a write/recovery changes ALSA and our frame axis. Readers
    // discard overlapping snapshots; the audio thread never waits for them.
    std::atomic<uint32_t> clockSequence_;
    // framesWritten_ where the current run began, i.e. at the last prepare:
    // the frame the link clock counts from once the run is triggered. Same
    // 32-bit wrap as framesWritten_.
    std::atomic<uint32_t> runStartFrames_;
    // The ALSA error that ended the render thread, 0 while it has not.
    // lastError() turns it into text, because the audio thread must not
    // allocate. Written before the thread publishes Failed.
    std::atomic<int> renderError_;

    std::thread renderThread_;
    AudioShutdown shutdown_;

    AudioFormat format_;
    uint32_t periodFrames_ = 0;
    uint32_t bufferFrames_ = 0;
    AudioRenderCallback renderCallback_ = nullptr;
    void* userData_ = nullptr;

    // Preallocated staging buffer: one period, filled by the source and handed
    // to snd_pcm_writei. Allocated in open(), never in the render loop.
    std::vector<unsigned char> staging_;

    std::string lastError_;
};

} // namespace multiroom
