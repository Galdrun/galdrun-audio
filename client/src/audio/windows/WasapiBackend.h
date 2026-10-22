#pragma once

#include "audio/IAudioBackend.h"
#include "audio/AudioShutdown.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

namespace multiroom {

// WASAPI backend. Internal to the audio layer: reach it through
// createPlatformAudioBackend() in audio/AudioBackend.h.
//
// The hardware clock (phase 4) is still a stub; everything else is live.
class WasapiBackend final : public IAudioBackend
{
public:
    WasapiBackend();
    ~WasapiBackend() override;

    std::vector<AudioDeviceInfo> enumeratePlaybackDevices(CapabilityScan scan) override;

    bool open(const AudioBackendConfig& config) override;
    bool start() override;
    void stop() override;
    void close() override;

    BackendState state() const override;
    AudioFormat currentFormat() const override { return format_; }
    uint32_t bufferFrames() const override { return bufferFrames_; }

    AudioClockSnapshot clockSnapshot() override;
    uint64_t underrunCount() const override;
    std::string lastError() const override;

    // IAudioEndpointVolume, for an endpoint whose device applies the level
    // itself (ENDPOINT_HARDWARE_SUPPORT_VOLUME).
    std::unique_ptr<IHardwareVolume> openHardwareVolume(
        const AudioDeviceInfo& device) const override;

private:
    // COM types are kept out of this header so nothing above the audio layer
    // ever pulls in windows.h.
    struct Impl;

    void renderLoop();
    // Releases the COM interfaces. Assumes clockMutex_ is already held, so
    // open() can reuse it without re-locking.
    void closeLocked();
    // open() once clockMutex_ is held and the previous handle closed; open()
    // closes again whatever this built before it failed.
    bool openLocked(const AudioBackendConfig& config);

    std::unique_ptr<Impl> impl_;

    // Guards the COM interfaces against a concurrent close() while a snapshot
    // is being taken. Only control-path threads ever touch it; the audio
    // thread never does.
    mutable std::mutex clockMutex_;

    // Written by the control thread, read by the audio thread.
    std::atomic<int> state_;
    std::atomic<bool> stopRequested_;
    // 32-bit on purpose: 64-bit atomics are not lock-free on ARMv6, and the
    // same discipline is kept on every platform so the code does not diverge.
    std::atomic<uint32_t> underruns_;
    // Why the render thread gave up (a RenderFailure in the source), and the
    // HRESULT that goes with it. lastError() turns them into text, because
    // the audio thread must not allocate. Both are written before the thread
    // publishes Failed.
    std::atomic<int> renderFailure_;
    std::atomic<int32_t> renderFailureCode_;

    std::thread renderThread_;
    AudioShutdown shutdown_;

    AudioFormat format_;
    uint32_t bufferFrames_ = 0;
    AudioRenderCallback renderCallback_ = nullptr;
    void* userData_ = nullptr;

    std::string lastError_;
};

} // namespace multiroom
