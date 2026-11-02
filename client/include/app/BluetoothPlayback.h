#pragma once

// Plays a phone's Bluetooth audio on this room's output while it streams
// ("local takeover"). The session's "bluetooth" power state releases the
// server's stream and the sound card first and tells the server; when the
// phone stops, the previous power state comes back. Audio goes through the same
// engine, device selection and hardware volume as the server's stream, at the
// phone's rate.
//
// Enabled by the board file's "bluetooth": true. Owner thread only.

#include "app/ClientCommandHandler.h"
#include "audio/AudioEngine.h"
#include "audio/AudioRingBuffer.h"
#include "audio/PlaybackSupervisor.h"
#include "audio/RingBufferSource.h"
#include "bluetooth/BluetoothReceiver.h"

#include <cstdint>
#include <memory>

namespace multiroom {

class BluetoothPlayback
{
public:
    explicit BluetoothPlayback(ClientRuntime& runtime);
    ~BluetoothPlayback();
    BluetoothPlayback(const BluetoothPlayback&) = delete;
    BluetoothPlayback& operator=(const BluetoothPlayback&) = delete;

    // Starts the platform receiver when the board has Bluetooth; otherwise
    // this object does nothing.
    void start();
    void poll(uint64_t nowNs);
    bool active() const { return playing_; }

private:
    void takeOver(uint64_t nowNs);
    void release(bool restorePower);

    ClientRuntime& runtime_;
    std::unique_ptr<IBluetoothReceiver> receiver_;
    AudioRingBuffer ring_;
    RingBufferSource source_;
    AudioEngine engine_;
    PlaybackSupervisor supervisor_;
    AudioFormat format_;
    std::string name_; // the room name last given to the receiver

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
