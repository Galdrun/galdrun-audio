#pragma once

// IBluetoothReceiver on bluez-alsa: reads the A2DP sink's capture PCM through
// its ALSA plugin. BlueZ, pairing and bluez-alsa itself are run by the image
// (Connector 3: board/connector3/ram/galdrun-bluetooth), not by the client.

#include "bluetooth/BluetoothReceiver.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

namespace multiroom {

class BluealsaReceiver final : public IBluetoothReceiver
{
public:
    BluealsaReceiver() = default;
    ~BluealsaReceiver() override;

    bool start(std::string& error) override;
    void stop() override;
    uint32_t receivedFrames() const override;
    AudioFormat format() const override;
    void setSink(Sink sink) override;
    void setName(const std::string& name) override;

private:
    void run();
    void applyName();

    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<uint32_t> received_{0};
    std::atomic<uint32_t> sampleRate_{0};
    std::mutex sinkMutex_;
    Sink sink_;
    std::mutex nameMutex_;
    std::string name_;        // wanted, under nameMutex_
    std::string appliedName_; // receiver thread only
    std::chrono::steady_clock::time_point nextNameAttempt_;
};

} // namespace multiroom
