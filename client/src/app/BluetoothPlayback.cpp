#include "app/BluetoothPlayback.h"

#include "audio/AudioBackend.h"
#include "audio/AudioDeviceCache.h"
#if defined(__linux__) && !defined(ESP_PLATFORM)
#include "config/BoardConfig.h"
#endif
#include "matp/Session.h"
#include "util/Logger.h"

#include <cstring>
#include <string>

namespace multiroom {
namespace {

// Room for the phone's jitter. The phone's clock and the sound card's are not
// locked: when the ring runs full, frames are dropped; when it runs empty, the
// device plays a short silence. Either is a rare, short glitch.
const uint32_t kRingMs = 500;
// A source must stream this long before it takes the room...
const uint64_t kTakeOverNs = 300000000ULL;
// ...and is let go after this long without audio (a pause, or a disconnect).
const uint64_t kReleaseNs = 3000000000ULL;

// The phone sends 16-bit; widen into the device's layout. A sample type with
// no 16-bit form on the card is chosen from the device's list below.
void convert(const int16_t* in, uint32_t samples, AudioFormat::SampleType type, unsigned char* out)
{
    for (uint32_t i = 0; i < samples; ++i)
    {
        const int32_t sample = in[i];
        switch (type)
        {
        case AudioFormat::SampleType::Int16:
            std::memcpy(out, &in[i], 2);
            out += 2;
            break;
        case AudioFormat::SampleType::Int24:
        {
            const int32_t value = sample * 256;
            out[0] = static_cast<unsigned char>(value);
            out[1] = static_cast<unsigned char>(value >> 8);
            out[2] = static_cast<unsigned char>(value >> 16);
            out += 3;
            break;
        }
        case AudioFormat::SampleType::Int24In32:
        case AudioFormat::SampleType::Int32:
        {
            const int32_t value = sample * (type == AudioFormat::SampleType::Int32 ? 65536 : 256);
            std::memcpy(out, &value, 4);
            out += 4;
            break;
        }
        case AudioFormat::SampleType::Float32:
        {
            const float value = static_cast<float>(sample) / 32768.0f;
            std::memcpy(out, &value, 4);
            out += 4;
            break;
        }
        }
    }
}

// The selected device's format at the phone's rate, 16-bit if it has one.
bool deviceFormat(const ClientRuntime& runtime, uint32_t sampleRate, AudioFormat& out)
{
    const AudioDeviceInfo* device =
        runtime.deviceSelected ? runtime.deviceCache->findById(runtime.deviceId) : nullptr;
    if (device == nullptr)
        return false;
    bool found = false;
    for (const AudioFormat& format : device->formats(runtime.accessMode))
    {
        if (format.sampleRate != sampleRate || format.channels != 2)
            continue;
        if (!found || format.sampleType == AudioFormat::SampleType::Int16)
            out = format;
        found = true;
    }
    return found;
}

} // namespace

BluetoothPlayback::BluetoothPlayback(ClientRuntime& runtime)
    : runtime_(runtime),
      source_(ring_, kRingMs),
      engine_(createPlatformAudioBackend(runtime.config->i2s)),
      supervisor_(engine_, [this](AudioEngine::Settings& settings, std::string& error) {
          if (!runtime_.deviceSelected)
          {
              error = "no usable audio playback device";
              return false;
          }
          settings.deviceId = runtime_.deviceId;
          settings.format = format_;
          settings.accessMode = runtime_.accessMode;
          return true;
      })
{
    engine_.setSource(&source_);
    PlaybackSupervisor::Hooks hooks;
    // The receiver thread is the ring's producer, attached only while the
    // device is open: the ring is reconfigured by every open.
    hooks.onOpened = [this](const AudioFormat& granted) {
        const AudioFormat::SampleType type = granted.sampleType;
        const uint32_t bytesPerFrame = granted.bytesPerFrame();
        receiver_->setSink([this, type, bytesPerFrame](const int16_t* frames, uint32_t count) {
            unsigned char block[4096];
            const uint32_t perBlock = sizeof(block) / bytesPerFrame;
            while (count > 0)
            {
                const uint32_t chunk = count < perBlock ? count : perBlock;
                convert(frames, chunk * 2, type, block);
                ring_.push(block, chunk); // full: dropped (see kRingMs)
                frames += chunk * 2;
                count -= chunk;
            }
        });
    };
    hooks.onClosed = [this]() { receiver_->setSink(IBluetoothReceiver::Sink()); };
    supervisor_.setHooks(hooks);
}

BluetoothPlayback::~BluetoothPlayback()
{
    if (playing_)
        release(false);
    if (receiver_)
        receiver_->stop();
}

void BluetoothPlayback::start()
{
#if defined(__linux__) && !defined(ESP_PLATFORM)
    if (!board_config::enabled(board_config::current(), "bluetooth"))
        return;
#endif
    receiver_ = createPlatformBluetoothReceiver();
    std::string error;
    if (receiver_ && !receiver_->start(error))
    {
        MULTIROOM_WARN("bluetooth receiver: %s", error.c_str());
        receiver_.reset();
    }
}

void BluetoothPlayback::poll(uint64_t nowNs)
{
    if (!receiver_)
        return;
    if (runtime_.config->name != name_)
    {
        name_ = runtime_.config->name;
        receiver_->setName(name_);
    }

    const uint32_t frames = receiver_->receivedFrames();
    if (frames != lastFrames_)
    {
        lastFrames_ = frames;
        if (!streaming_)
            streamingSinceNs_ = nowNs;
        streaming_ = true;
        lastAudioNs_ = nowNs;
    }
    else if (streaming_ && nowNs - lastAudioNs_ >= kReleaseNs)
    {
        streaming_ = false;
        suppressed_ = false;
    }

    if (playing_)
    {
        matp::Session* session = runtime_.session;
        if (!streaming_)
        {
            MULTIROOM_INFO("bluetooth: the phone stopped; returning the room");
            release(true);
        }
        else if (session != nullptr && *session->localSource() == '\0')
        {
            // Switched on by the user or the server while the phone plays.
            MULTIROOM_INFO("bluetooth: room switched on; phone audio stopped");
            suppressed_ = true;
            release(false);
            session->resumeStream();
        }
        else if (receiver_->format().isValid() &&
                 receiver_->format().sampleRate != format_.sampleRate)
        {
            // The phone renegotiated (another codec or rate): reopen at it.
            supervisor_.stop();
            playing_ = false;
            takeOver(nowNs);
            if (!playing_)
                release(true); // the new rate is not playable here
        }
        else
        {
            supervisor_.poll(nowNs);
        }
        return;
    }

    if (streaming_ && !suppressed_ && nowNs - streamingSinceNs_ >= kTakeOverNs)
        takeOver(nowNs);
}

void BluetoothPlayback::takeOver(uint64_t nowNs)
{
    const AudioFormat received = receiver_->format();
    if (!received.isValid() || !deviceFormat(runtime_, received.sampleRate, format_))
    {
        MULTIROOM_WARN("bluetooth: %s cannot be played on the selected device",
                       describeFormat(received).c_str());
        suppressed_ = true;
        return;
    }
    matp::Session* session = runtime_.session;
    std::string error;
    // Releases the server's stream and the sound card synchronously, and
    // tells the server the room plays Bluetooth.
    if (session != nullptr && !session->setBluetooth(true, error))
        MULTIROOM_WARN("bluetooth: %s", error.c_str());

    MULTIROOM_INFO("bluetooth: playing the phone's audio as %s", describeFormat(format_).c_str());
    playing_ = true;
    if (!supervisor_.start(nowNs, error))
        MULTIROOM_WARN("bluetooth playback is not up yet: %s", error.c_str());
}

void BluetoothPlayback::release(bool restorePower)
{
    supervisor_.stop();
    playing_ = false;
    std::string error;
    if (restorePower && runtime_.session != nullptr &&
        !runtime_.session->setBluetooth(false, error))
        MULTIROOM_WARN("bluetooth: %s", error.c_str());
}

} // namespace multiroom
