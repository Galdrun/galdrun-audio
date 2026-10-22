#pragma once

#include "audio/AudioBackend.h"
#include "audio/IAudioBackend.h"
#include "audio/AudioShutdown.h"

#include "util/Wrap32.h"

#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace multiroom {

// What the on-board ES8388 plays in this clock mode: 8 to 48 kHz in both
// layouts as I2S master, only the external clock's rate in 16 bits as slave.
// The settings page shows the same list the backend offers.
std::vector<AudioFormat> es8388Formats(const I2sSettings& settings);

// ESP32 I2S backend. Internal to the audio layer: reach it through
// createPlatformAudioBackend() in audio/AudioBackend.h.
//
// Two devices: "es8388", the codec on the ESP32-A1S, offered when it answers
// on I2C; and "i2s", a plain external DAC on its own pins (menuconfig) with no
// control interface. Both are exclusive -- there is no mixer on this platform.
//
// Master mode clocks the bus from the audio PLL (APLL), whose fractional
// divider hits 44.1 and 48 kHz families within a few ppm instead of the tens
// of ppm the 160 MHz PLL rounds to; the ESP32-S3 has no APLL and uses that
// PLL. Slave mode takes the bit and word clocks
// from outside and offers only I2sSettings::slaveSampleRate, the one rate the
// external clock runs at. The client measures either clock and never corrects
// it; the server resamples.
//
// The clock is the DMA itself: every descriptor the peripheral finishes is
// counted in the driver's interrupt, and the time is taken there too. That
// position is what the hardware consumed, not what was written.
class I2sBackend final : public IAudioBackend
{
public:
    explicit I2sBackend(const I2sSettings& settings);
    ~I2sBackend() override;

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

    // The ES8388's DAC volume; the external DAC has none.
    std::unique_ptr<IHardwareVolume> openHardwareVolume(
        const AudioDeviceInfo& device) const override;

private:
    static bool onSent(i2s_chan_handle_t handle, i2s_event_data_t* event, void* context);
    static bool onStarved(i2s_chan_handle_t handle, i2s_event_data_t* event, void* context);

    void renderLoop();
    void renderPeriod();
    void closeChannel();

    I2sSettings settings_;
    i2s_chan_handle_t channel_ = nullptr;
    bool codec_ = false;
    bool codecStopped_ = false;

    // Guards the channel against a concurrent close() while a snapshot is
    // being taken, and owns frameExtender_. The audio thread never takes it.
    mutable std::mutex clockMutex_;
    Wrap32Extender frameExtender_;

    // Written by the DMA interrupt, read by clockSnapshot(). A spinlock rather
    // than atomics, because the time is 64 bits and ARMv6's rule about 64-bit
    // atomics holds here too. The critical section is two stores long.
    portMUX_TYPE clockLock_ = portMUX_INITIALIZER_UNLOCKED;
    uint32_t sentFrames_ = 0;
    int64_t sentTimeUs_ = 0;

    // Frames the DMA played that the render loop never handed it: each
    // starved descriptor goes out as silence. The loop renders and drops the
    // same amount, so the frames it hands the driver stay on the device's
    // frame axis instead of playing a period late for every underrun.
    std::atomic<uint32_t> starvedFrames_;
    uint32_t starvedHandled_ = 0;

    std::atomic<int> state_;
    std::atomic<bool> stopRequested_;
    std::atomic<uint32_t> underruns_;
    // The driver error that ended the render thread, ESP_OK while it has not.
    std::atomic<int> renderError_;

    std::thread renderThread_;
    AudioShutdown shutdown_;

    AudioFormat format_;
    uint32_t periodFrames_ = 0;
    AudioRenderCallback renderCallback_ = nullptr;
    void* userData_ = nullptr;

    // One period, allocated in open(), never in the render loop.
    std::vector<unsigned char> staging_;

    std::string lastError_;
};

} // namespace multiroom
