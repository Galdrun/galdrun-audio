// BluetoothPlayback against a scripted phone (FakeReceiver) and a sound card
// played by the test (StubBackend). What is real is the takeover, the ring,
// the room's volume on the way out and the device selection it follows.

#include "StubBackend.h"
#include "TestSupport.h"

#include "app/BluetoothPlayback.h"
#include "app/StreamPlayback.h"
#include "config/ClientConfig.h"

#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace multiroom;

namespace {

const uint32_t kRate = 48000;
const uint64_t kMsNs = 1000000ULL;

AudioFormat phoneFormat()
{
    AudioFormat format;
    format.sampleRate = kRate;
    format.channels = 2;
    format.sampleType = AudioFormat::SampleType::Int16;
    return format;
}

class FakeReceiver final : public IBluetoothReceiver
{
public:
    bool start(std::string&) override { return true; }
    void stop() override {}
    uint32_t receivedFrames() const override { return frames; }
    AudioFormat format() const override { return phoneFormat(); }
    void setSink(Sink next) override { sink = std::move(next); }
    void setName(const std::string&) override {}

    // `count` frames of `value` on both channels, as the receiver thread would.
    void deliver(int16_t value, uint32_t count)
    {
        frames += count;
        if (!sink)
            return;
        std::vector<int16_t> samples(static_cast<size_t>(count) * 2, value);
        sink(samples.data(), count);
    }

    uint32_t frames = 0;
    Sink sink;
};

class Phone
{
public:
    Phone()
    {
        runtime.config = &config;
        runtime.deviceCache = &cache;
        runtime.deviceSelected = true;
        runtime.deviceId = "a";
        runtime.accessMode = AudioAccessMode::Shared;
        addDevice("a");
        addDevice("b");

        std::unique_ptr<multiroomtest::StubBackend> backend(new multiroomtest::StubBackend());
        stub = backend.get();
        std::unique_ptr<FakeReceiver> fake(new FakeReceiver());
        receiver = fake.get();
        playback.reset(new BluetoothPlayback(runtime, std::move(backend), std::move(fake)));
    }

    void addDevice(const std::string& id)
    {
        AudioDeviceInfo device;
        device.id = id;
        device.name = id;
        device.sharedFormats.push_back(phoneFormat());
        cache.devices.push_back(device);
    }

    // The phone streams long enough to take the room.
    bool takeOver()
    {
        receiver->deliver(0, 48);
        playback->poll(nowNs_);
        nowNs_ += 400 * kMsNs;
        receiver->deliver(0, 48);
        playback->poll(nowNs_);
        return playback->active();
    }

    // Feeds `value` and plays it: the last sample the device was handed.
    int16_t play(int16_t value)
    {
        nowNs_ += 10 * kMsNs;
        receiver->deliver(value, 4800);
        playback->poll(nowNs_);
        std::vector<unsigned char> block(48 * 4);
        int16_t last = 0;
        for (int i = 0; i < 100; ++i)
        {
            if (stub->render(block.data(), 48) == 48)
                std::memcpy(&last, block.data() + 47 * 4, 2);
        }
        return last;
    }

    void poll()
    {
        nowNs_ += 10 * kMsNs;
        receiver->deliver(0, 48);
        playback->poll(nowNs_);
    }

    ClientConfig config;
    AudioDeviceCacheData cache;
    ClientRuntime runtime;
    multiroomtest::StubBackend* stub = nullptr; // owned by the playback's engine
    FakeReceiver* receiver = nullptr;           // owned by the playback
    std::unique_ptr<BluetoothPlayback> playback;

private:
    uint64_t nowNs_ = 1000 * kMsNs;
};

} // namespace

MULTIROOM_TEST(bluetooth_playback_plays_the_room_volume_on_the_samples)
{
    Phone phone;
    phone.runtime.volumePath.effective = matp::VolumeControlMode::Software;
    phone.runtime.volumeSet = true;
    phone.runtime.volumeGain = 0.25;
    phone.runtime.volumeMuted = true;
    CHECK(phone.takeOver());
    CHECK_EQ(phone.play(1000), 0);

    phone.runtime.volumeMuted = false;
    phone.runtime.volumeGain = 0.1;
    const int16_t played = phone.play(1000);
    CHECK(played >= 99 && played <= 101);
}

MULTIROOM_TEST(bluetooth_playback_plays_the_room_volume_on_the_hardware_control)
{
    Phone phone;
    phone.cache.devices[0].hardwareVolume.available = true;
    phone.cache.devices[0].hardwareVolume.hasMute = true;
    phone.stub->hardwareVolume.offered = true;
    phone.runtime.volumePath.effective = matp::VolumeControlMode::Hardware;
    phone.runtime.volumeSet = true;
    phone.runtime.volumeGain = 0.25;
    phone.runtime.volumeMuted = true;
    CHECK(phone.takeOver());
    CHECK_EQ(phone.stub->hardwareVolume.opens, 1);
    CHECK_EQ(phone.stub->hardwareVolume.gain, 0.25);
    CHECK(phone.stub->hardwareVolume.muted);
    CHECK_EQ(phone.play(1000), 0);

    phone.runtime.volumeMuted = false;
    phone.runtime.volumeGain = 0.5;
    CHECK_EQ(phone.play(1000), 1000); // the control attenuates, not the samples
    CHECK_EQ(phone.stub->hardwareVolume.gain, 0.5);
    CHECK(!phone.stub->hardwareVolume.muted);
}

MULTIROOM_TEST(bluetooth_playback_leaves_an_unset_hardware_control_alone)
{
    Phone phone;
    phone.cache.devices[0].hardwareVolume.available = true;
    phone.stub->hardwareVolume.offered = true;
    phone.runtime.volumePath.effective = matp::VolumeControlMode::Hardware;
    CHECK(phone.takeOver());
    CHECK_EQ(phone.stub->hardwareVolume.sets, 0);
    CHECK_EQ(phone.play(1000), 1000);
}

MULTIROOM_TEST(bluetooth_playback_resets_the_control_when_the_path_leaves_it)
{
    Phone phone;
    phone.cache.devices[0].hardwareVolume.available = true;
    phone.cache.devices[0].hardwareVolume.hasMute = true;
    phone.stub->hardwareVolume.offered = true;
    phone.runtime.audioControl = matp::AudioControl::Auto;
    phone.runtime.volumePath.effective = matp::VolumeControlMode::Hardware;
    phone.runtime.volumeSet = true;
    phone.runtime.volumeGain = 0.25;
    phone.runtime.volumeMuted = true;
    CHECK(phone.takeOver());
    CHECK_EQ(phone.play(1000), 0);

    // Hardware -> Software: the control back to its ceiling, unmuted.
    phone.runtime.volumePath.effective = matp::VolumeControlMode::Software;
    phone.runtime.volumeMuted = false;
    CHECK_EQ(phone.play(1000), 250);
    CHECK_EQ(phone.stub->hardwareVolume.gain, boardGainLimit());
    CHECK(!phone.stub->hardwareVolume.muted);

    // Software -> Hardware -> Disabled: the ceiling, with the room's mute.
    phone.runtime.volumePath.effective = matp::VolumeControlMode::Hardware;
    CHECK_EQ(phone.play(1000), 1000);
    CHECK_EQ(phone.stub->hardwareVolume.gain, 0.25);
    phone.runtime.volumePath.effective = matp::VolumeControlMode::Disabled;
    phone.runtime.volumeMuted = true;
    phone.poll();
    CHECK_EQ(phone.stub->hardwareVolume.gain, boardGainLimit());
    CHECK(phone.stub->hardwareVolume.muted);
}

MULTIROOM_TEST(bluetooth_playback_puts_a_first_default_room_volume_on_the_control)
{
    Phone phone;
    phone.cache.devices[0].hardwareVolume.available = true;
    phone.stub->hardwareVolume.offered = true;
    phone.stub->hardwareVolume.gain = 0.2;
    phone.runtime.volumePath.effective = matp::VolumeControlMode::Hardware;
    CHECK(phone.takeOver());
    CHECK_EQ(phone.stub->hardwareVolume.sets, 0);

    phone.runtime.volumeSet = true; // SET_VOLUME 1.0, unmuted: the defaults
    phone.poll();
    CHECK_EQ(phone.stub->hardwareVolume.sets, 1);
    CHECK_EQ(phone.stub->hardwareVolume.gain, 1.0);
}

MULTIROOM_TEST(bluetooth_playback_tries_a_refused_control_again)
{
    Phone phone;
    phone.cache.devices[0].hardwareVolume.available = true;
    phone.cache.devices[0].hardwareVolume.hasMute = true;
    phone.stub->hardwareVolume.offered = true;
    phone.runtime.volumePath.effective = matp::VolumeControlMode::Hardware;
    phone.runtime.volumeSet = true;
    phone.runtime.volumeGain = 0.25;
    phone.runtime.volumeMuted = true;
    CHECK(phone.takeOver());
    CHECK(phone.stub->hardwareVolume.muted);

    phone.stub->hardwareVolume.failNext = 1;
    phone.runtime.volumeMuted = false;
    phone.poll();
    CHECK(phone.stub->hardwareVolume.muted); // refused
    for (int i = 0; i < 110; ++i)            // past the retry interval
        phone.play(0);
    CHECK(!phone.stub->hardwareVolume.muted);
    CHECK_EQ(phone.play(1000), 1000); // the control attenuates, not the samples
}

MULTIROOM_TEST(bluetooth_playback_never_scales_the_samples_under_bit_perfect)
{
    Phone phone;
    phone.cache.devices[0].hardwareVolume.available = true;
    phone.stub->hardwareVolume.offered = true;
    phone.runtime.audioControl = matp::AudioControl::BitPerfect;
    phone.runtime.volumePath.effective = matp::VolumeControlMode::Hardware;
    phone.runtime.volumeSet = true;
    phone.runtime.volumeGain = 0.5;
    CHECK(phone.takeOver());
    CHECK_EQ(phone.play(1000), 1000);

    phone.stub->hardwareVolume.failNext = 1;
    phone.runtime.volumeGain = 0.25;
    CHECK_EQ(phone.play(1000), 1000);
}

MULTIROOM_TEST(bluetooth_playback_resets_a_control_whose_first_write_failed)
{
    Phone phone;
    phone.cache.devices[0].hardwareVolume.available = true;
    phone.cache.devices[0].hardwareVolume.hasMute = true;
    phone.stub->hardwareVolume.offered = true;
    phone.stub->hardwareVolume.muted = true; // what a write that failed halfway left
    phone.stub->hardwareVolume.failNext = 1;
    phone.runtime.audioControl = matp::AudioControl::Auto;
    phone.runtime.volumePath.effective = matp::VolumeControlMode::Hardware;
    phone.runtime.volumeSet = true;
    phone.runtime.volumeGain = 0.25;
    phone.runtime.volumeMuted = true;
    CHECK(phone.takeOver());

    phone.runtime.volumePath.effective = matp::VolumeControlMode::Software;
    phone.runtime.volumeMuted = false;
    CHECK_EQ(phone.play(1000), 250);
    CHECK(!phone.stub->hardwareVolume.muted);
    CHECK_EQ(phone.stub->hardwareVolume.gain, boardGainLimit());
}

MULTIROOM_TEST(bluetooth_playback_stops_scaling_at_once_when_the_room_turns_bit_perfect)
{
    Phone phone;
    phone.cache.devices[0].hardwareVolume.available = true;
    phone.stub->hardwareVolume.offered = true;
    phone.stub->hardwareVolume.failSet = true;
    phone.runtime.audioControl = matp::AudioControl::Auto;
    phone.runtime.volumePath.effective = matp::VolumeControlMode::Hardware;
    phone.runtime.volumeSet = true;
    phone.runtime.volumeGain = 0.25;
    CHECK(phone.takeOver());
    CHECK_EQ(phone.play(1000), 250); // the refused control's stand-in

    phone.runtime.audioControl = matp::AudioControl::BitPerfect;
    CHECK_EQ(phone.play(1000), 1000);
}

MULTIROOM_TEST(bluetooth_playback_moves_to_a_newly_selected_output)
{
    Phone phone;
    CHECK(phone.takeOver());
    CHECK_EQ(phone.stub->lastConfig.deviceId, std::string("a"));

    phone.runtime.deviceId = "b";
    phone.poll();
    CHECK(phone.playback->active());
    CHECK_EQ(phone.stub->openCalls, 2);
    CHECK_EQ(phone.stub->lastConfig.deviceId, std::string("b"));
    CHECK_EQ(phone.play(1000), 1000);
}

MULTIROOM_TEST(bluetooth_playback_gives_the_room_back_for_an_output_without_the_phone_rate)
{
    Phone phone;
    CHECK(phone.takeOver());
    phone.cache.devices[1].sharedFormats[0].sampleRate = 44100;

    phone.runtime.deviceId = "b";
    phone.poll();
    CHECK(!phone.playback->active());
    CHECK_EQ(phone.stub->openCalls, 1);
}
