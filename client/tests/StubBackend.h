#pragma once

// A sound card that is not there.
//
// Shared by the engine and the supervisor tests, which both need a backend
// whose failures are scripted rather than waited for: no test may require a
// device, and a device that has been unplugged cannot be simulated any other
// way.

#include "audio/IAudioBackend.h"

#include <memory>
#include <string>
#include <vector>

namespace multiroomtest {

// Mirror of the teardown counters in storage the caller owns. The engine takes
// ownership of the backend, so a test that asserts on what the engine's
// destructor did cannot read the stub itself afterwards -- that is a
// use-after-free, whatever the numbers happen to say.
struct StubCallCounts
{
    int stopCalls = 0;
    int closeCalls = 0;
};

// A device volume control, scripted the same way: what it was last set to,
// how often, and whether it refuses.
struct StubHardwareVolume
{
    bool offered = false; // openHardwareVolume() opens one
    bool failSet = false;
    int failNext = 0; // this many more writes refuse, then they work again
    int opens = 0;
    int sets = 0;
    int mutes = 0; // setMute() alone, level untouched
    double gain = 0.5; // where its owner left it before the room set anything
    bool muted = false;
};

class StubVolumeControl final : public multiroom::IHardwareVolume
{
public:
    explicit StubVolumeControl(StubHardwareVolume& state) : state_(state) {}

    bool set(double gain, bool muted, std::string& error) override
    {
        if (refuses())
        {
            error = "stub control refuses";
            return false;
        }
        ++state_.sets;
        state_.gain = multiroom::hardware_volume::clampGain(gain);
        state_.muted = muted;
        return true;
    }

    bool setMute(bool muted, std::string& error) override
    {
        if (refuses())
        {
            error = "stub control refuses";
            return false;
        }
        ++state_.mutes;
        state_.muted = muted;
        return true;
    }

    bool read(multiroom::HardwareVolumeState& out, std::string&) override
    {
        out = multiroom::HardwareVolumeState();
        out.gain = state_.gain;
        out.muted = state_.muted;
        return true;
    }

private:
    bool refuses()
    {
        if (state_.failNext > 0)
        {
            --state_.failNext;
            return true;
        }
        return state_.failSet;
    }

    StubHardwareVolume& state_;
};

// A device clock trim, scripted the same way. `ppm` starts where a previous
// run might have left it, so a test sees whether opening resets it.
struct StubHardwareClock
{
    bool offered = false; // openHardwareClock() opens one
    bool failSet = false;
    int opens = 0;
    int sets = 0;
    int32_t ppm = 37;
};

class StubClockTrim final : public multiroom::IHardwareClock
{
public:
    explicit StubClockTrim(StubHardwareClock& state) : state_(state) {}

    bool set(int32_t ppm, std::string& error) override
    {
        if (state_.failSet)
        {
            error = "stub clock refuses";
            return false;
        }
        ++state_.sets;
        state_.ppm = ppm;
        return true;
    }

private:
    StubHardwareClock& state_;
};

class StubBackend final : public multiroom::IAudioBackend
{
public:
    std::vector<multiroom::AudioDeviceInfo> enumeratePlaybackDevices(multiroom::CapabilityScan) override
    {
        return std::vector<multiroom::AudioDeviceInfo>();
    }

    bool open(const multiroom::AudioBackendConfig& config) override
    {
        ++openCalls;
        lastConfig = config;
        if (failOpen)
        {
            lastError_ = "stub refuses to open";
            return false;
        }
        // Deliberately grants something other than what was asked for: the
        // engine must prepare the source with what the driver returned.
        format_ = config.format;
        format_.sampleRate =
            grantedSampleRate != 0 ? grantedSampleRate : config.format.sampleRate;
        state_ = multiroom::BackendState::Open;
        return true;
    }

    bool start() override
    {
        ++startCalls;
        if (failStart)
        {
            lastError_ = "stub refuses to start";
            return false;
        }
        state_ = multiroom::BackendState::Running;
        return true;
    }

    void stop() override
    {
        ++stopCalls;
        if (counts != nullptr)
            counts->stopCalls = stopCalls;
        if (state_ == multiroom::BackendState::Running)
            state_ = multiroom::BackendState::Open;
    }

    void close() override
    {
        ++closeCalls;
        if (counts != nullptr)
            counts->closeCalls = closeCalls;
        state_ = multiroom::BackendState::Closed;
    }

    multiroom::BackendState state() const override { return state_; }
    multiroom::AudioFormat currentFormat() const override { return format_; }
    uint32_t bufferFrames() const override { return 480; }
    multiroom::AudioClockSnapshot clockSnapshot() override { return clock; }
    uint64_t underrunCount() const override { return underruns; }
    std::string lastError() const override { return lastError_; }

    std::unique_ptr<multiroom::IHardwareVolume> openHardwareVolume(
        const multiroom::AudioDeviceInfo&) const override
    {
        if (!hardwareVolume.offered)
            return std::unique_ptr<multiroom::IHardwareVolume>();
        ++hardwareVolume.opens;
        return std::unique_ptr<multiroom::IHardwareVolume>(new StubVolumeControl(hardwareVolume));
    }

    std::unique_ptr<multiroom::IHardwareClock> openHardwareClock(
        const multiroom::AudioDeviceInfo&) const override
    {
        if (!hardwareClock.offered)
            return std::unique_ptr<multiroom::IHardwareClock>();
        ++hardwareClock.opens;
        return std::unique_ptr<multiroom::IHardwareClock>(new StubClockTrim(hardwareClock));
    }

    // Drives the render callback the way a real audio thread would.
    uint32_t render(void* buffer, uint32_t frames)
    {
        if (lastConfig.renderCallback == nullptr)
            return 0;
        return lastConfig.renderCallback(lastConfig.userData, buffer, frames);
    }

    // What a backend does when the device is pulled out from under a running
    // stream: the render thread gives up and leaves Running behind.
    void simulateDeviceLoss(const std::string& reason)
    {
        lastError_ = reason;
        state_ = multiroom::BackendState::Failed;
    }

    bool failOpen = false;
    bool failStart = false;
    uint32_t grantedSampleRate = 0;
    uint64_t underruns = 0;

    // What the driver reports about its own position. Unset -- no usable
    // clock -- unless a test plays the sound card's clock as well.
    multiroom::AudioClockSnapshot clock;

    int openCalls = 0;
    int startCalls = 0;
    int stopCalls = 0;
    int closeCalls = 0;

    StubCallCounts* counts = nullptr; // optional, see StubCallCounts

    // Mutable because opening a control does not change the backend.
    mutable StubHardwareVolume hardwareVolume;
    mutable StubHardwareClock hardwareClock;

    multiroom::AudioBackendConfig lastConfig;

private:
    multiroom::BackendState state_ = multiroom::BackendState::Closed;
    multiroom::AudioFormat format_;
    std::string lastError_;
};

} // namespace multiroomtest
