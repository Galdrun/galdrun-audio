#pragma once

#include "audio/AudioClockSnapshot.h"
#include "audio/AudioDeviceInfo.h"
#include "audio/AudioFormat.h"
#include "audio/HardwareClock.h"
#include "audio/HardwareVolume.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace multiroom {

// Contract for the platform audio layer (WASAPI, ALSA).
//
// The backend reports facts and moves samples. It never decides how drift is
// corrected, how the server timeline is followed, or how resampling works --
// those live in platform-independent components above it.
//
// Implementations arrive in phase 2 (enumeration) and phase 3 (playback).

enum class CapabilityScan
{
    // Identity only: id, name, default flag, mix format. Cheap enough to run
    // at every start.
    Fast,
    // Probe every rate/format/channel combination. Opens and closes the device
    // many times and can take seconds, so results are cached to disk.
    Full
};

enum class BackendState
{
    Closed,
    Open,    // device acquired, not yet rendering
    Running, // rendering
    Failed   // device lost or driver error; needs close() then open()
};

const char* backendStateName(BackendState state);

// Called from the real-time audio thread. Must fill up to frameCount frames of
// interleaved samples in the negotiated format and return how many it filled;
// the backend silences the remainder and counts the shortfall as an underrun.
//
// A plain function pointer, not std::function: no indirection through a heap
// allocation and no risk of a capturing lambda pulling allocations into the
// real-time path.
//
// The callback must not allocate, lock unbounded, log synchronously or do I/O.
using AudioRenderCallback = uint32_t (*)(void* userData, void* buffer, uint32_t frameCount);

struct AudioBackendConfig
{
    std::string deviceId; // empty selects the system default device
    AudioFormat format;

    // Exclusive means direct hardware access (WASAPI exclusive mode, ALSA
    // "hw:"); shared goes through the system mixer. The backend must not
    // silently substitute the other one -- the caller decides, because falling
    // back costs clock accuracy and the user may have forbidden it.
    AudioAccessMode accessMode = AudioAccessMode::Exclusive;

    // 0 lets the backend choose the driver's default period.
    uint32_t requestedBufferFrames = 0;

    AudioRenderCallback renderCallback = nullptr;
    void* userData = nullptr;
};

class IAudioBackend
{
public:
    virtual ~IAudioBackend() = default;

    // Enumeration is a control-path operation; never call it from the audio
    // thread or while the device is running.
    virtual std::vector<AudioDeviceInfo> enumeratePlaybackDevices(CapabilityScan scan) = 0;

    virtual bool open(const AudioBackendConfig& config) = 0;
    virtual bool start() = 0;
    // Healthy streams fade to silence and let it play before stopping, with
    // a bounded wait. Failed devices stop immediately. close() uses this too.
    virtual void stop() = 0;
    virtual void close() = 0;

    virtual BackendState state() const = 0;

    // Format and buffer size actually granted by the driver, which may differ
    // from what was requested. Valid once open() has succeeded.
    virtual AudioFormat currentFormat() const = 0;
    virtual uint32_t bufferFrames() const = 0;

    // Called from the control/sync thread while audio is running, so
    // implementations must not block the audio thread to serve it. Returns a
    // snapshot with quality Unavailable if no position can be read.
    virtual AudioClockSnapshot clockSnapshot() = 0;

    // Monotonic count of render callbacks that could not be filled in time.
    // Backends keep this in a 32-bit atomic, since 64-bit atomics are not
    // lock-free on ARMv6, and widen it here.
    virtual uint64_t underrunCount() const = 0;

    virtual std::string lastError() const = 0;

    // Control path. The device's own volume control, for a device whose scan
    // found one (AudioDeviceInfo::hardwareVolume); null when it has none, when
    // it cannot be opened, or when this backend implements none, which is the
    // default. See audio/HardwareVolume.h.
    virtual std::unique_ptr<IHardwareVolume> openHardwareVolume(
        const AudioDeviceInfo& device) const
    {
        (void)device;
        return std::unique_ptr<IHardwareVolume>();
    }

    // Control path. The device's clock trim, for a device whose scan found one
    // (AudioDeviceInfo::hardwareClock); null otherwise, and by default. See
    // audio/HardwareClock.h.
    virtual std::unique_ptr<IHardwareClock> openHardwareClock(const AudioDeviceInfo& device) const
    {
        (void)device;
        return std::unique_ptr<IHardwareClock>();
    }
};

} // namespace multiroom
