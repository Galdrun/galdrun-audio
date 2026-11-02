#pragma once

// A phone (or any other A2DP source) sending audio to this device over
// Bluetooth. One implementation per platform: bluez-alsa's capture PCM on
// Linux; an ESP32 with Classic Bluetooth (ESP32-S31) would feed its A2DP sink
// callback through the same interface.
//
// The receiver runs on its own thread and only delivers decoded PCM. Whether
// that audio takes over the room's output is decided on the owner thread by
// app/BluetoothPlayback, which is platform independent.

#include "audio/AudioFormat.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace multiroom {

class IBluetoothReceiver
{
public:
    // Called on the receiver's thread with interleaved signed 16-bit frames in
    // format(). Not the real-time audio thread, so it may convert and copy.
    using Sink = std::function<void(const int16_t* frames, uint32_t frameCount)>;

    virtual ~IBluetoothReceiver();

    virtual bool start(std::string& error) = 0;
    virtual void stop() = 0;

    // Any thread. Frames received since start(); it only advances while a
    // source is actually streaming, which is how the owner sees one. Wraps.
    virtual uint32_t receivedFrames() const = 0;

    // Any thread. The current stream's format; invalid until audio arrived.
    virtual AudioFormat format() const = 0;

    // Owner thread. An empty sink stops delivery; returns only once no call to
    // the previous sink is in progress, so its target may be torn down after.
    virtual void setSink(Sink sink) = 0;

    // Owner thread. The name phones see when pairing: the room's name.
    // Applied in the background, again whenever it changes.
    virtual void setName(const std::string& name) = 0;
};

// Null on a platform without an implementation.
std::unique_ptr<IBluetoothReceiver> createPlatformBluetoothReceiver();

} // namespace multiroom
