#pragma once

// Plays a phone's Bluetooth audio on this room's output while it streams
// ("local takeover"). The session's "bluetooth" power state releases the
// server's stream and the sound card first and tells the server; when the
// phone stops, the previous power state comes back. Audio goes through the same
// engine, device selection and hardware volume as the server's stream, at the
// phone's rate. The room's volume and mute (runtime, published by
// StreamPlayback) play on the room's volume path here too, at once: the phone's
// stream has no timeline to schedule them on. A change of the selected device
// or access mode reopens the output on the new one.
//
// Enabled by the board file's "bluetooth": true. Owner thread only.

#include "app/ClientCommandHandler.h"
#include "audio/AudioEngine.h"
#include "audio/AudioRingBuffer.h"
#include "audio/HardwareVolume.h"
#include "audio/PlaybackSupervisor.h"
#include "audio/RingBufferSource.h"
#include "audio/VolumeControl.h"
#include "bluetooth/BluetoothReceiver.h"

#include <cstdint>
#include <memory>

namespace multiroom {

class BluetoothPlayback
{
public:
    explicit BluetoothPlayback(ClientRuntime& runtime);
    // Tests: plays through `backend` and takes audio from `receiver`.
    BluetoothPlayback(ClientRuntime& runtime, std::unique_ptr<IAudioBackend> backend,
                      std::unique_ptr<IBluetoothReceiver> receiver);
    ~BluetoothPlayback();
    BluetoothPlayback(const BluetoothPlayback&) = delete;
    BluetoothPlayback& operator=(const BluetoothPlayback&) = delete;

    // Starts the platform receiver when the board has Bluetooth; otherwise
    // this object does nothing.
    void start();
    void poll(uint64_t nowNs);
    bool active() const { return playing_; }

private:
    // The ring, scaled by the room's volume on its way to the device.
    class VolumeSource final : public IAudioSource
    {
    public:
        VolumeSource(RingBufferSource& ring, VolumeControl& volume) : ring_(ring), volume_(volume) {}
        void prepare(const AudioFormat& format) override;
        uint32_t render(void* buffer, uint32_t frameCount) override;

    private:
        RingBufferSource& ring_;
        VolumeControl& volume_;
        uint64_t cursor_ = 0; // audio thread
    };

    void takeOver(uint64_t nowNs);
    void release(bool restorePower);
    // The room's volume onto its path: the device's control, else the samples.
    // A control that refuses leaves it unapplied, for the poll to try again.
    void applyVolume();

    ClientRuntime& runtime_;
    std::unique_ptr<IBluetoothReceiver> receiver_;
    AudioRingBuffer ring_;
    RingBufferSource ringSource_;
    VolumeControl volume_;
    VolumeSource source_;
    AudioEngine engine_;
    PlaybackSupervisor supervisor_;
    AudioFormat format_;
    // The output takeOver() chose the format for.
    std::string deviceId_;
    AudioAccessMode accessMode_ = AudioAccessMode::Shared;
    std::string name_; // the room name last given to the receiver

    // Between the supervisor's opened and closed hooks.
    bool deviceOpen_ = false;
    // The open device's own volume control, null without one.
    std::unique_ptr<IHardwareVolume> hardwareVolume_;
    bool hardwareMute_ = false; // it has a mute switch
    // What this playback last put on a device control: the room's level (and
    // mute) under Hardware, or only its mute under Disabled. A path that stops
    // using the control puts it back to the board's ceiling, unmuted.
    enum class Control { Untouched, Level, Mute };
    Control control_ = Control::Untouched;
    // The room volume last applied, to apply only changes. Not applied while
    // the control refuses: tried again at volumeRetryNs_.
    bool volumeApplied_ = false;
    uint64_t volumeRetryNs_ = 0;
    double appliedGain_ = 1.0;
    bool appliedMuted_ = false;
    bool appliedSet_ = false;
    matp::VolumeControlMode appliedPath_ = matp::VolumeControlMode::Disabled;
    matp::AudioControl appliedAudioControl_ = matp::AudioControl::BitPerfect;

    uint32_t lastFrames_ = 0;
    uint64_t lastAudioNs_ = 0;
    uint64_t streamingSinceNs_ = 0;
    bool streaming_ = false;
    bool playing_ = false;
    // The user switched the room on during a phone stream: leave that stream
    // alone until it pauses.
    bool suppressed_ = false;
};

} // namespace multiroom
