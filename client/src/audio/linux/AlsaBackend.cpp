#include "audio/linux/AlsaBackend.h"

#include "audio/FormatProbe.h"
#include "audio/linux/AlsaMixer.h"
#include "util/Logger.h"

#include <alsa/asoundlib.h>
#include <pthread.h>
#include <sched.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace multiroom {
namespace {

// On 32-bit targets the same binary meets two libasound ABIs: libasound2t64
// (Debian trixie and later) copies a 16-byte timespec with a 64-bit tv_sec,
// older releases an 8-byte one. Read into room for both, pre-filled so the
// larger layout shows itself: its tv_nsec lands in the third word, and a
// valid tv_nsec is never all ones. Reading the 64-bit layout as ours leaves
// tv_nsec at 0 on every stamp -- whole seconds, useless for drift.
template <typename Getter>
uint64_t readHtstampNs(Getter getter, const snd_pcm_status_t* status)
{
    if (sizeof(time_t) >= 8)
    {
        snd_htimestamp_t stamp{};
        getter(status, &stamp);
        return static_cast<uint64_t>(stamp.tv_sec) * 1000000000ULL +
               static_cast<uint64_t>(stamp.tv_nsec);
    }

    // Little-endian words, as on every 32-bit target this is built for.
    alignas(8) uint32_t words[4];
    std::memset(words, 0xFF, sizeof(words));
    getter(status, reinterpret_cast<snd_htimestamp_t*>(words));
    if (words[2] != 0xFFFFFFFFu)
        return ((static_cast<uint64_t>(words[1]) << 32) | words[0]) * 1000000000ULL + words[2];
    return static_cast<uint64_t>(words[0]) * 1000000000ULL + words[1];
}

// Owns a snd_pcm_hw_params_t across the many early exits below.
class HwParams
{
public:
    HwParams() { snd_pcm_hw_params_malloc(&params_); }
    ~HwParams()
    {
        if (params_ != nullptr)
            snd_pcm_hw_params_free(params_);
    }

    HwParams(const HwParams&) = delete;
    HwParams& operator=(const HwParams&) = delete;

    snd_pcm_hw_params_t* get() const { return params_; }
    bool valid() const { return params_ != nullptr; }

private:
    snd_pcm_hw_params_t* params_ = nullptr;
};

class SwParams
{
public:
    SwParams() { snd_pcm_sw_params_malloc(&params_); }
    ~SwParams()
    {
        if (params_ != nullptr)
            snd_pcm_sw_params_free(params_);
    }

    SwParams(const SwParams&) = delete;
    SwParams& operator=(const SwParams&) = delete;

    snd_pcm_sw_params_t* get() const { return params_; }
    bool valid() const { return params_ != nullptr; }

private:
    snd_pcm_sw_params_t* params_ = nullptr;
};

class PcmHandle
{
public:
    PcmHandle() = default;
    ~PcmHandle() { close(); }

    PcmHandle(const PcmHandle&) = delete;
    PcmHandle& operator=(const PcmHandle&) = delete;

    // SND_PCM_NONBLOCK so a device that is already in use by another process
    // fails immediately instead of blocking the whole scan.
    int open(const std::string& name)
    {
        close();
        return snd_pcm_open(&pcm_, name.c_str(), SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
    }

    void close()
    {
        if (pcm_ != nullptr)
        {
            snd_pcm_close(pcm_);
            pcm_ = nullptr;
        }
    }

    snd_pcm_t* get() const { return pcm_; }

private:
    snd_pcm_t* pcm_ = nullptr;
};

bool toAlsaFormat(AudioFormat::SampleType type, snd_pcm_format_t& out)
{
    switch (type)
    {
    case AudioFormat::SampleType::Int16:
        out = SND_PCM_FORMAT_S16_LE;
        return true;
    case AudioFormat::SampleType::Int24:
        out = SND_PCM_FORMAT_S24_3LE; // three packed bytes
        return true;
    case AudioFormat::SampleType::Int24In32:
        out = SND_PCM_FORMAT_S24_LE; // 24 valid bits in a 32-bit container
        return true;
    case AudioFormat::SampleType::Int32:
        out = SND_PCM_FORMAT_S32_LE;
        return true;
    case AudioFormat::SampleType::Float32:
        out = SND_PCM_FORMAT_FLOAT_LE;
        return true;
    }
    return false;
}

void readBufferLimits(snd_pcm_t* pcm, AudioBufferLimits& out)
{
    HwParams params;
    if (!params.valid() || snd_pcm_hw_params_any(pcm, params.get()) < 0)
        return;

    snd_pcm_uframes_t frames = 0;
    int direction = 0;

    if (snd_pcm_hw_params_get_buffer_size_min(params.get(), &frames) == 0)
        out.minBufferFrames = static_cast<uint32_t>(frames);
    if (snd_pcm_hw_params_get_buffer_size_max(params.get(), &frames) == 0)
        out.maxBufferFrames = static_cast<uint32_t>(frames);
    if (snd_pcm_hw_params_get_period_size_min(params.get(), &frames, &direction) == 0)
        out.minPeriodFrames = static_cast<uint32_t>(frames);
    if (snd_pcm_hw_params_get_period_size_max(params.get(), &frames, &direction) == 0)
        out.maxPeriodFrames = static_cast<uint32_t>(frames);

    unsigned int microseconds = 0;
    if (snd_pcm_hw_params_get_period_time_min(params.get(), &microseconds, &direction) == 0)
        out.minPeriodNs = static_cast<uint64_t>(microseconds) * 1000;

    // defaultPeriodNs stays zero: unlike WASAPI, ALSA has no notion of a
    // driver-preferred period, so the engine picks one from the limits.
}

// Rate is tested only after format and channel count have been applied,
// because the three are not independent: a device may take 192 kHz in stereo
// but not in 8 channels. Testing them separately would record combinations the
// driver would reject at open time, and the server picks this client's stream
// format from exactly this list.
void probeFormats(snd_pcm_t* pcm, std::vector<AudioFormat>& out)
{
    HwParams params;
    if (!params.valid())
        return;
    // Connector 2's AK4104 also supports these rates, omitted by the original
    // desktop probe set. Test them against the hardware like every other rate.
    std::vector<uint32_t> rates(probe::kSampleRates, probe::kSampleRates + probe::kSampleRateCount);
    rates.insert(rates.begin(), {22050, 32000});

    for (size_t t = 0; t < probe::kSampleTypeCount; ++t)
    {
        snd_pcm_format_t alsaFormat;
        if (!toAlsaFormat(probe::kSampleTypes[t], alsaFormat))
            continue;

        for (size_t c = 0; c < probe::kChannelCountCount; ++c)
        {
            for (uint32_t rate : rates)
            {
                // A fresh parameter space per combination; set_* narrows it.
                if (snd_pcm_hw_params_any(pcm, params.get()) < 0)
                    continue;
                if (snd_pcm_hw_params_set_access(pcm, params.get(),
                                                 SND_PCM_ACCESS_RW_INTERLEAVED) < 0)
                    continue;
                if (snd_pcm_hw_params_set_format(pcm, params.get(), alsaFormat) < 0)
                    continue;
                if (snd_pcm_hw_params_set_channels(pcm, params.get(),
                                                   probe::kChannelCounts[c]) < 0)
                    continue;
                if (snd_pcm_hw_params_test_rate(pcm, params.get(), rate, 0) <
                    0)
                    continue;

                AudioFormat format;
                format.sampleRate = rate;
                format.channels = probe::kChannelCounts[c];
                format.sampleType = probe::kSampleTypes[t];
                out.push_back(format);
            }
        }
    }
}

// Opens one PCM name and fills the matching format list. Returns false when
// the device cannot be opened at all (busy, or capture-only).
bool probePcm(const std::string& pcmName, AudioAccessMode mode, AudioDeviceInfo& device,
              bool full)
{
    PcmHandle pcm;
    const int result = pcm.open(pcmName);
    if (result < 0)
    {
        if (result == -EBUSY)
        {
            if (mode == AudioAccessMode::Exclusive)
                device.exclusiveProbeBusy = true;
            else
                device.sharedProbeBusy = true;
        }
        MULTIROOM_DEBUG("alsa: cannot open %s for probing: %s", pcmName.c_str(),
                  snd_strerror(result));
        return false;
    }

    if (mode == AudioAccessMode::Exclusive)
        readBufferLimits(pcm.get(), device.buffer);

    if (full)
        probeFormats(pcm.get(), device.formats(mode));

    return true;
}

std::string makeDeviceId(const char* cardId, int deviceIndex)
{
    char buffer[128];
    std::snprintf(buffer, sizeof(buffer), "hw:CARD=%s,DEV=%d", cardId, deviceIndex);
    return std::string(buffer);
}

std::string makePlugName(const char* cardId, int deviceIndex)
{
    char buffer[128];
    std::snprintf(buffer, sizeof(buffer), "plughw:CARD=%s,DEV=%d", cardId, deviceIndex);
    return std::string(buffer);
}

// Turns a cached device id into the PCM name for the requested access mode.
// "hw:CARD=x,DEV=0" is direct hardware; "plughw:..." is the converting plugin,
// which is the closest ALSA equivalent of WASAPI's shared mode. "default" is
// already a plugin and has no hardware form.
std::string pcmNameForMode(const std::string& deviceId, AudioAccessMode mode)
{
    if (deviceId.empty())
        return "default";
    if (mode == AudioAccessMode::Exclusive)
        return deviceId;
    if (deviceId.compare(0, 3, "hw:") == 0)
        return "plug" + deviceId;
    return deviceId;
}

// Underruns and suspends are recoverable; anything else ends the stream.
// Returns false when the stream is dead, having stored the error code: the
// audio thread must not build the message itself, because that allocates.
bool recoverFromError(snd_pcm_t* pcm, int error, std::atomic<uint32_t>& underruns,
                      std::atomic<int>& renderError, const std::atomic<bool>& stopRequested)
{
    if (error == -EPIPE)
        underruns.fetch_add(1, std::memory_order_relaxed);

    // snd_pcm_recover waits out a suspend with no limit, a second at a time,
    // and nothing in there sees a stop: done here instead, bounded, so a
    // device that stays suspended goes to the supervisor as failed and a stop
    // is never held up by it.
    if (error == -ESTRPIPE)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        int result = 0;
        while ((result = snd_pcm_resume(pcm)) == -EAGAIN)
        {
            if (stopRequested.load(std::memory_order_acquire) ||
                std::chrono::steady_clock::now() >= deadline)
            {
                renderError.store(error, std::memory_order_relaxed);
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        // No resume of its own (-ENOSYS), or it failed: start over prepared.
        if (result < 0)
            result = snd_pcm_prepare(pcm);
        if (result < 0)
        {
            renderError.store(error, std::memory_order_relaxed);
            return false;
        }
        return true;
    }

    // silent = 1: snd_pcm_recover must not print to stderr from the audio
    // thread.
    const int recovered = snd_pcm_recover(pcm, error, 1);
    if (recovered < 0)
    {
        renderError.store(error, std::memory_order_relaxed);
        return false;
    }
    return true;
}

// The "default" PCM follows whatever the system is configured to use (dmix, a
// sound server, a plain card). It is the only entry that reliably works on a
// machine where another process already holds the hardware, so it is listed as
// the default device and probed through the converting path only.
void appendDefaultDevice(std::vector<AudioDeviceInfo>& devices, bool full)
{
    AudioDeviceInfo device;
    device.id = "default";
    device.name = "ALSA default device";
    device.description = "system-configured default PCM";
    device.isDefault = true;

    if (!probePcm("default", AudioAccessMode::Shared, device, full))
    {
        MULTIROOM_WARN("alsa: the default PCM could not be opened");
        if (!device.sharedProbeBusy)
            return;
    }
    devices.push_back(device);
}

void appendCardDevices(int card, std::vector<AudioDeviceInfo>& devices, bool full)
{
    char controlName[32];
    std::snprintf(controlName, sizeof(controlName), "hw:%d", card);

    snd_ctl_t* control = nullptr;
    int result = snd_ctl_open(&control, controlName, 0);
    if (result < 0)
    {
        MULTIROOM_WARN("alsa: cannot open control %s: %s", controlName, snd_strerror(result));
        return;
    }

    snd_ctl_card_info_t* cardInfo = nullptr;
    snd_pcm_info_t* pcmInfo = nullptr;
    snd_ctl_card_info_malloc(&cardInfo);
    snd_pcm_info_malloc(&pcmInfo);

    if (cardInfo != nullptr && pcmInfo != nullptr && snd_ctl_card_info(control, cardInfo) == 0)
    {
        const char* cardId = snd_ctl_card_info_get_id(cardInfo);
        const char* cardName = snd_ctl_card_info_get_name(cardInfo);
        const char* cardLongName = snd_ctl_card_info_get_longname(cardInfo);

        // A card has one mixer and possibly several PCM devices, and nothing in
        // ALSA says which of them its volume element drives. The first playback
        // device is the card's main output -- the analog out of an HDA codec,
        // the only one of a USB DAC or an I2S board -- so it alone gets the
        // mixer; the rest, HDMI among them, are scaled in software.
        bool firstPlayback = true;
        int deviceIndex = -1;
        while (snd_ctl_pcm_next_device(control, &deviceIndex) == 0 && deviceIndex >= 0)
        {
            snd_pcm_info_set_device(pcmInfo, static_cast<unsigned int>(deviceIndex));
            snd_pcm_info_set_subdevice(pcmInfo, 0);
            snd_pcm_info_set_stream(pcmInfo, SND_PCM_STREAM_PLAYBACK);
            // Fails for capture-only devices, which is how they are filtered.
            if (snd_ctl_pcm_info(control, pcmInfo) < 0)
                continue;

            AudioDeviceInfo device;
            device.id = makeDeviceId(cardId, deviceIndex);

            const char* pcmName = snd_pcm_info_get_name(pcmInfo);
            device.name = std::string(cardName != nullptr ? cardName : cardId) + ": " +
                          (pcmName != nullptr ? pcmName : "PCM");
            device.description = cardLongName != nullptr ? cardLongName : "";

            // "hw:" is direct hardware access, "plughw:" converts in software.
            // A device that only opens through plughw is still usable, so a
            // failed exclusive probe is not fatal.
            probePcm(device.id, AudioAccessMode::Exclusive, device, full);
            probePcm(makePlugName(cardId, deviceIndex), AudioAccessMode::Shared, device, full);

            if (firstPlayback)
            {
                firstPlayback = false;
                if (detectAlsaHardwareVolume(cardId, device.hardwareVolume))
                    MULTIROOM_INFO("alsa: %s has hardware volume \"%s\"", device.id.c_str(),
                             device.hardwareVolume.control.c_str());
                if (detectAlsaHardwareClock(cardId, device.hardwareClock))
                    MULTIROOM_INFO("alsa: %s has a hardware clock trim \"%s\" (%d..%d ppm)",
                             device.id.c_str(), device.hardwareClock.control.c_str(),
                             static_cast<int>(device.hardwareClock.minPpm),
                             static_cast<int>(device.hardwareClock.maxPpm));
            }

            if (device.exclusiveFormats.empty() && device.sharedFormats.empty() && full)
            {
                MULTIROOM_DEBUG("alsa: %s reported no usable format", device.id.c_str());
            }
            devices.push_back(device);
        }
    }

    if (pcmInfo != nullptr)
        snd_pcm_info_free(pcmInfo);
    if (cardInfo != nullptr)
        snd_ctl_card_info_free(cardInfo);
    snd_ctl_close(control);
}

} // namespace

// SND_PCM_AUDIO_TSTAMP_TYPE_LINK, which alsa-lib names only after 1.2.4, the
// ARM baseline. The value is kernel ABI (SNDRV_PCM_AUDIO_TSTAMP_TYPE_LINK).
constexpr int kAudioTimestampLink = 2;

// Keeps snd_pcm_t out of the header.
struct AlsaBackend::Impl
{
    snd_pcm_t* pcm = nullptr;
    size_t bytesPerFrame = 0;

    // Allocated once in open(): snd_pcm_status() must not malloc on every
    // telemetry tick.
    snd_pcm_status_t* status = nullptr;

    // The device reports link timestamps: audio time read from the audio
    // hardware's own counter together with the system time of that read.
    bool linkTimestamps = false;

    ~Impl()
    {
        if (status != nullptr)
            snd_pcm_status_free(status);
    }
};

AlsaBackend::AlsaBackend()
    : impl_(new Impl()),
      state_(static_cast<int>(BackendState::Closed)),
      stopRequested_(false),
      underruns_(0),
      framesWritten_(0),
      clockSequence_(0),
      runStartFrames_(0),
      renderError_(0)
{
}

AlsaBackend::~AlsaBackend()
{
    close();
}

BackendState AlsaBackend::state() const
{
    return static_cast<BackendState>(state_.load(std::memory_order_acquire));
}

uint64_t AlsaBackend::underrunCount() const
{
    return underruns_.load(std::memory_order_relaxed);
}

std::string AlsaBackend::lastError() const
{
    // The render thread stores the code before it publishes Failed; the text
    // is built here, on the caller's thread.
    if (state() != BackendState::Failed)
        return lastError_;
    const int renderError = renderError_.load(std::memory_order_relaxed);
    if (renderError == -ETIMEDOUT)
        return "the ALSA device stopped taking audio";
    if (renderError != 0)
        return std::string("unrecoverable ALSA error: ") + snd_strerror(renderError);
    return lastError_;
}

std::unique_ptr<IHardwareVolume> AlsaBackend::openHardwareVolume(
    const AudioDeviceInfo& device) const
{
    return openAlsaHardwareVolume(device);
}

std::unique_ptr<IHardwareClock> AlsaBackend::openHardwareClock(const AudioDeviceInfo& device) const
{
    return openAlsaHardwareClock(device);
}

std::vector<AudioDeviceInfo> AlsaBackend::enumeratePlaybackDevices(CapabilityScan scan)
{
    std::vector<AudioDeviceInfo> devices;
    const bool full = scan == CapabilityScan::Full;

    appendDefaultDevice(devices, full);

    int card = -1;
    while (snd_card_next(&card) == 0 && card >= 0)
        appendCardDevices(card, devices, full);

    if (devices.empty())
        lastError_ = "no ALSA playback devices found";

    return devices;
}

bool AlsaBackend::open(const AudioBackendConfig& config)
{
    stop();

    // Held for the whole function: clockSnapshot() may run on a telemetry
    // thread, and it must never see a half-configured PCM handle. Every early
    // return below releases it.
    std::lock_guard<std::mutex> lock(clockMutex_);
    closeLocked();
    lastError_.clear();
    renderError_.store(0, std::memory_order_relaxed);

    if (config.renderCallback == nullptr)
    {
        lastError_ = "no render callback";
        return false;
    }
    if (!config.format.isValid())
    {
        lastError_ = "invalid audio format";
        return false;
    }

    snd_pcm_format_t alsaFormat;
    if (!toAlsaFormat(config.format.sampleType, alsaFormat))
    {
        lastError_ = "unsupported sample type";
        return false;
    }

    // Exclusive goes straight at the hardware, shared through the converting
    // plugin -- the same two names phase 2 probed. The backend never
    // substitutes the other one: the caller decided, and falling back costs
    // clock accuracy.
    const std::string pcmName = pcmNameForMode(config.deviceId, config.accessMode);

    int result = snd_pcm_open(&impl_->pcm, pcmName.c_str(), SND_PCM_STREAM_PLAYBACK,
                              SND_PCM_NONBLOCK);
    if (result < 0)
    {
        impl_->pcm = nullptr;
        lastError_ = "cannot open " + pcmName + ": " + snd_strerror(result);
        return false;
    }

    HwParams params;
    if (!params.valid())
    {
        lastError_ = "out of memory";
        closeLocked();
        return false;
    }

    result = snd_pcm_hw_params_any(impl_->pcm, params.get());
    if (result < 0)
    {
        lastError_ = std::string("no configurations available: ") + snd_strerror(result);
        closeLocked();
        return false;
    }

    result =
        snd_pcm_hw_params_set_access(impl_->pcm, params.get(), SND_PCM_ACCESS_RW_INTERLEAVED);
    if (result < 0)
    {
        lastError_ = std::string("interleaved access rejected: ") + snd_strerror(result);
        closeLocked();
        return false;
    }

    result = snd_pcm_hw_params_set_format(impl_->pcm, params.get(), alsaFormat);
    if (result < 0)
    {
        lastError_ = std::string("device rejected ") + sampleTypeName(config.format.sampleType) +
                     ": " + snd_strerror(result);
        closeLocked();
        return false;
    }

    result = snd_pcm_hw_params_set_channels(impl_->pcm, params.get(), config.format.channels);
    if (result < 0)
    {
        lastError_ = std::string("device rejected the channel count: ") + snd_strerror(result);
        closeLocked();
        return false;
    }

    // Exact rate, not _near: a device that silently ran at a different rate
    // would wreck both the timeline and the drift measurement.
    result = snd_pcm_hw_params_set_rate(impl_->pcm, params.get(), config.format.sampleRate, 0);
    if (result < 0)
    {
        lastError_ = std::string("device rejected the sample rate: ") + snd_strerror(result);
        closeLocked();
        return false;
    }

    snd_pcm_uframes_t periodFrames = config.requestedBufferFrames > 0
                                         ? config.requestedBufferFrames
                                         : config.format.sampleRate / 100; // 10 ms
    result = snd_pcm_hw_params_set_period_size_near(impl_->pcm, params.get(), &periodFrames,
                                                    nullptr);
    if (result < 0)
    {
        lastError_ = std::string("cannot set the period size: ") + snd_strerror(result);
        closeLocked();
        return false;
    }

    // Four periods per buffer is the usual compromise between latency and
    // resilience against scheduling jitter on a loaded small board.
    snd_pcm_uframes_t bufferFrames = periodFrames * 4;
    result = snd_pcm_hw_params_set_buffer_size_near(impl_->pcm, params.get(), &bufferFrames);
    if (result < 0)
    {
        lastError_ = std::string("cannot set the buffer size: ") + snd_strerror(result);
        closeLocked();
        return false;
    }

    result = snd_pcm_hw_params(impl_->pcm, params.get());
    if (result < 0)
    {
        lastError_ =
            std::string("cannot apply the hardware parameters: ") + snd_strerror(result);
        closeLocked();
        return false;
    }

    // Only once the parameters are applied do they carry the driver's
    // capability flags. alsa-lib 1.1.0 added the query; older builds keep the
    // position-based clock.
    impl_->linkTimestamps = false;
#if SND_LIB_VERSION >= 0x10100
    impl_->linkTimestamps = snd_pcm_hw_params_supports_audio_ts_type(
                                params.get(), kAudioTimestampLink) != 0;
#endif

    // Start once the buffer is full, and wake us when one period of space is
    // available.
    SwParams swParams;
    if (!swParams.valid() || snd_pcm_sw_params_current(impl_->pcm, swParams.get()) < 0)
    {
        lastError_ = "cannot read the software parameters";
        closeLocked();
        return false;
    }
    snd_pcm_sw_params_set_start_threshold(impl_->pcm, swParams.get(), bufferFrames);
    snd_pcm_sw_params_set_avail_min(impl_->pcm, swParams.get(), periodFrames);

    // Timestamps for the hardware clock. MONOTONIC matters: the default
    // gettimeofday base can step, which would put false jumps into the drift
    // regression.
    snd_pcm_sw_params_set_tstamp_mode(impl_->pcm, swParams.get(), SND_PCM_TSTAMP_ENABLE);
    // snd_pcm_sw_params_set_tstamp_type arrived in alsa-lib 1.0.26. Every
    // distribution that still targets ARMv6 is well past that, but a build
    // against an older header should degrade rather than fail.
#if SND_LIB_VERSION >= 0x1001A
    snd_pcm_sw_params_set_tstamp_type(impl_->pcm, swParams.get(),
                                      SND_PCM_TSTAMP_TYPE_MONOTONIC);
#endif

    result = snd_pcm_sw_params(impl_->pcm, swParams.get());
    if (result < 0)
    {
        lastError_ =
            std::string("cannot apply the software parameters: ") + snd_strerror(result);
        closeLocked();
        return false;
    }

    result = snd_pcm_prepare(impl_->pcm);
    if (result < 0)
    {
        lastError_ = std::string("cannot prepare the device: ") + snd_strerror(result);
        closeLocked();
        return false;
    }

    if (impl_->status == nullptr && snd_pcm_status_malloc(&impl_->status) < 0)
    {
        lastError_ = "out of memory";
        closeLocked();
        return false;
    }

    format_ = config.format;
    periodFrames_ = static_cast<uint32_t>(periodFrames);
    bufferFrames_ = static_cast<uint32_t>(bufferFrames);
    renderCallback_ = config.renderCallback;
    userData_ = config.userData;
    impl_->bytesPerFrame = config.format.bytesPerFrame();
    framesWritten_.store(0, std::memory_order_relaxed);
    clockSequence_.store(0, std::memory_order_relaxed);
    runStartFrames_.store(0, std::memory_order_release);
    frameExtender_.reset();

    // One period, allocated once. The render loop never allocates.
    staging_.assign(static_cast<size_t>(periodFrames_) * impl_->bytesPerFrame, 0);

    underruns_.store(0, std::memory_order_relaxed);
    state_.store(static_cast<int>(BackendState::Open), std::memory_order_release);

    MULTIROOM_DEBUG("alsa: opened %s, period %u frames, buffer %u frames", pcmName.c_str(),
              periodFrames_, bufferFrames_);
    return true;
}

bool AlsaBackend::start()
{
    if (state() == BackendState::Running)
        return true;
    if (state() != BackendState::Open)
    {
        lastError_ = "device is not open";
        return false;
    }

    shutdown_.prepare(format_);
    stopRequested_.store(false, std::memory_order_release);
    state_.store(static_cast<int>(BackendState::Running), std::memory_order_release);
    renderThread_ = std::thread(&AlsaBackend::renderLoop, this);
    // Elevate only the deadline-bound render thread. This runs on the control
    // thread so permission diagnostics never log from the audio callback.
    sched_param scheduling{};
    scheduling.sched_priority = 20;
    const int priorityError = pthread_setschedparam(renderThread_.native_handle(), SCHED_RR,
                                                   &scheduling);
    if (priorityError != 0)
        MULTIROOM_WARN("alsa: audio thread SCHED_RR priority 20 unavailable: %s; using normal priority",
                 std::strerror(priorityError));
    else
        MULTIROOM_INFO("alsa: audio thread uses SCHED_RR priority 20");
    return true;
}

void AlsaBackend::renderLoop()
{
    snd_pcm_t* const pcm = impl_->pcm;
    const size_t frameBytes = impl_->bytesPerFrame;

    // A device that stops taking audio without reporting an error -- a lost
    // bit clock, a wedged USB endpoint -- would otherwise leave this loop
    // waiting forever with the backend Running, and the supervisor never
    // recovers it. Several buffers' worth, and never under two seconds.
    const std::chrono::milliseconds stallLimit(std::max<uint64_t>(
        2000, 4000ULL * bufferFrames_ / std::max<uint32_t>(1, format_.sampleRate)));
    auto lastProgress = std::chrono::steady_clock::now();
    const auto stalled = [&] {
        if (shutdown_.active() ||
            std::chrono::steady_clock::now() - lastProgress < stallLimit)
            return false;
        renderError_.store(-ETIMEDOUT, std::memory_order_relaxed);
        state_.store(static_cast<int>(BackendState::Failed), std::memory_order_release);
        return true;
    };

    while (true)
    {
        shutdown_.begin(stopRequested_.load(std::memory_order_acquire));
        if (shutdown_.active() && shutdown_.remainingMs() == 0)
            break;
        // A bounded wait rather than a blocking write: the loop has to notice a
        // stop request even when the device never asks for more data again.
        const int ready = snd_pcm_wait(pcm, std::min(200, shutdown_.remainingMs()));
        shutdown_.begin(stopRequested_.load(std::memory_order_acquire));

        if (ready == 0)
        {
            if (stalled())
                return;
            continue; // timeout; check the stop flag and wait again
        }

        if (ready < 0)
        {
            if (shutdown_.active())
                break; // Do not restart a device while trying to shut it down.
            clockSequence_.fetch_add(1, std::memory_order_acq_rel);
            const bool recovered =
                recoverFromError(pcm, ready, underruns_, renderError_, stopRequested_);
            runStartFrames_.store(framesWritten_.load(std::memory_order_relaxed),
                                  std::memory_order_release);
            clockSequence_.fetch_add(1, std::memory_order_release);
            if (!recovered)
            {
                state_.store(static_cast<int>(BackendState::Failed), std::memory_order_release);
                return;
            }
            lastProgress = std::chrono::steady_clock::now(); // a fresh start
            continue;
        }

        const uint32_t filled = shutdown_.render(renderCallback_, userData_, staging_.data(),
                                                 periodFrames_);
        if (filled < periodFrames_)
        {
            // The source could not keep up. Silence the rest rather than
            // replaying the previous period's contents.
            std::memset(staging_.data() + static_cast<size_t>(filled) * frameBytes, 0,
                        static_cast<size_t>(periodFrames_ - filled) * frameBytes);
            underruns_.fetch_add(1, std::memory_order_relaxed);
        }

        snd_pcm_uframes_t offset = 0;
        while (offset < periodFrames_)
        {
            shutdown_.begin(stopRequested_.load(std::memory_order_acquire));
            if (shutdown_.active() && shutdown_.remainingMs() == 0)
                return;
            clockSequence_.fetch_add(1, std::memory_order_acq_rel);
            const snd_pcm_sframes_t written = snd_pcm_writei(
                pcm, staging_.data() + offset * frameBytes, periodFrames_ - offset);
            if (written > 0)
            {
                lastProgress = std::chrono::steady_clock::now();
                offset += static_cast<snd_pcm_uframes_t>(written);
                // Published for clockSnapshot(); a plain relaxed add, since the
                // reader only needs a recent value, not a synchronized one.
                framesWritten_.fetch_add(static_cast<uint32_t>(written),
                                         std::memory_order_relaxed);
                clockSequence_.fetch_add(1, std::memory_order_release);
                continue;
            }
            if (written == -EAGAIN || written == 0)
            {
                clockSequence_.fetch_add(1, std::memory_order_release);
                if (stalled())
                    return;
                snd_pcm_wait(pcm, std::min(20, shutdown_.remainingMs()));
                continue;
            }
            if (shutdown_.active())
            {
                clockSequence_.fetch_add(1, std::memory_order_release);
                return;
            }
            if (!recoverFromError(pcm, static_cast<int>(written), underruns_, renderError_,
                                  stopRequested_))
            {
                clockSequence_.fetch_add(1, std::memory_order_release);
                state_.store(static_cast<int>(BackendState::Failed), std::memory_order_release);
                return;
            }
            lastProgress = std::chrono::steady_clock::now();
            // Recovered: the remainder of this period is dropped on purpose
            // rather than written late into a freshly prepared stream. It is
            // still counted: the render callback has handed it out, and the
            // position has to stay on the axis TimelineSource schedules
            // against, or every start and end after an xrun lands that many
            // frames early.
            framesWritten_.fetch_add(static_cast<uint32_t>(periodFrames_ - offset),
                                     std::memory_order_relaxed);
            runStartFrames_.store(framesWritten_.load(std::memory_order_relaxed),
                                  std::memory_order_release);
            clockSequence_.fetch_add(1, std::memory_order_release);
            break;
        }
        if (shutdown_.finished())
        {
            // The PCM is nonblocking: draining a disconnected/stalled device
            // must not hang shutdown. stop() drops it after this deadline.
            while (shutdown_.remainingMs() != 0 && snd_pcm_drain(pcm) == -EAGAIN)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            break;
        }
    }
}

void AlsaBackend::stop()
{
    if (state() != BackendState::Running && !renderThread_.joinable())
        return;

    stopRequested_.store(true, std::memory_order_release);
    if (renderThread_.joinable())
        renderThread_.join();

    if (impl_->pcm != nullptr)
        snd_pcm_drop(impl_->pcm);

    // A failed stream stays failed; only a healthy one returns to Open.
    if (state() != BackendState::Running)
        return;

    // The drop leaves the PCM in SETUP and start() does not prepare it, so
    // the next start's first write would fail with -EBADFD, which
    // snd_pcm_recover cannot handle. Open has to mean ready to start.
    const int result = impl_->pcm != nullptr ? snd_pcm_prepare(impl_->pcm) : 0;
    if (result < 0)
    {
        lastError_ = std::string("cannot prepare the device after stopping: ") +
                     snd_strerror(result);
        state_.store(static_cast<int>(BackendState::Failed), std::memory_order_release);
        return;
    }
    runStartFrames_.store(framesWritten_.load(std::memory_order_relaxed),
                          std::memory_order_release);
    state_.store(static_cast<int>(BackendState::Open), std::memory_order_release);
}

void AlsaBackend::close()
{
    // Outside the lock: stop() joins the render thread, and holding a lock a
    // telemetry thread may be waiting on across a join invites trouble.
    stop();

    std::lock_guard<std::mutex> lock(clockMutex_);
    closeLocked();
}

void AlsaBackend::closeLocked()
{
    if (impl_->pcm != nullptr)
    {
        snd_pcm_close(impl_->pcm);
        impl_->pcm = nullptr;
    }

    renderCallback_ = nullptr;
    userData_ = nullptr;
    periodFrames_ = 0;
    bufferFrames_ = 0;
    format_ = AudioFormat();
    staging_.clear();
    state_.store(static_cast<int>(BackendState::Closed), std::memory_order_release);
}

AudioClockSnapshot AlsaBackend::clockSnapshot()
{
    AudioClockSnapshot snapshot;

    std::lock_guard<std::mutex> lock(clockMutex_);
    if (impl_->pcm == nullptr || impl_->status == nullptr || !format_.isValid())
        return snapshot;

    const uint32_t sequence = clockSequence_.load(std::memory_order_acquire);
    if ((sequence & 1u) != 0)
        return snapshot;

#if SND_LIB_VERSION >= 0x10100
    // Asked for on every call: the kernel writes its report into the same
    // field of the status. report_delay makes the link time subtract the
    // codec delay, as the status delay below adds it.
    if (impl_->linkTimestamps)
    {
        snd_pcm_audio_tstamp_config_t config;
        std::memset(&config, 0, sizeof(config));
        config.type_requested = kAudioTimestampLink;
        config.report_delay = 1;
        snd_pcm_status_set_audio_htstamp_config(impl_->status, &config);
    }
#endif

    if (snd_pcm_status(impl_->pcm, impl_->status) < 0)
        return snapshot;

    // ALSA gives no "frames the hardware has consumed" directly. What it does
    // give is how much of what we wrote is still queued, so the played
    // position is written minus delay.
    //
    // An xrun breaks this: frames are dropped that we still counted as
    // written, so afterwards the position carries a permanent offset. Drift is
    // a slope, so a constant offset does not hurt it -- but the step at the
    // moment of the xrun does corrupt a regression window spanning it. The
    // drift estimator therefore has to reset when underrunCount() rises, which
    // is why that counter is part of the backend interface.
    const uint32_t written32 = framesWritten_.load(std::memory_order_relaxed);
    const uint32_t runStart32 = runStartFrames_.load(std::memory_order_relaxed);
    // ALSA's delay and our writes must describe the same instant. A write
    // between the status ioctl and this load otherwise invents a whole
    // period of clock progress and contaminates the server's drift fit.
    std::atomic_thread_fence(std::memory_order_acquire);
    if (sequence != clockSequence_.load(std::memory_order_relaxed))
        return snapshot;
    const uint64_t written = frameExtender_.extend(written32);
    const snd_pcm_sframes_t delay = snd_pcm_status_get_delay(impl_->status);
    const uint64_t queued = delay > 0 ? static_cast<uint64_t>(delay) : 0;
    snapshot.deviceFramePosition = written > queued ? written - queued : 0;
    snapshot.nominalSampleRate = format_.sampleRate;

    uint64_t stampNs = 0;

#if SND_LIB_VERSION >= 0x10100
    // Best: the link time. audio_htstamp is then the time the audio hardware
    // has played since this run was triggered, read by the driver together
    // with htstamp, so htstamp is its system time. The run began at
    // runStartFrames_ on our axis. Only while running: before the trigger the
    // audio time still belongs to the previous run.
    snd_pcm_audio_tstamp_report_t report;
    std::memset(&report, 0, sizeof(report));
    snd_pcm_status_get_audio_htstamp_report(impl_->status, &report);
    if (impl_->linkTimestamps && report.actual_type == kAudioTimestampLink &&
        snd_pcm_status_get_state(impl_->status) == SND_PCM_STATE_RUNNING)
    {
        const uint64_t audioNs = readHtstampNs(snd_pcm_status_get_audio_htstamp, impl_->status);
        stampNs = readHtstampNs(snd_pcm_status_get_htstamp, impl_->status);

        // Seconds and nanoseconds apart: nanoseconds times the rate would
        // overflow 64 bits within a day.
        const uint64_t rate = format_.sampleRate;
        const uint64_t played = audioNs / 1000000000ULL * rate +
                                audioNs % 1000000000ULL * rate / 1000000000ULL;
        // Distance back from the newest write, which keeps the 32-bit wrap
        // out of the subtraction.
        const uint32_t sinceRunStart = written32 - runStart32;
        const uint64_t runStart = written - std::min<uint64_t>(sinceRunStart, written);
        const uint64_t position = runStart + played;

        // The link position only refines the ALSA position; it never
        // disagrees with it by more than the buffer. If it does, the driver
        // counts from somewhere else, and its time is not ours to use.
        const uint64_t difference = position > snapshot.deviceFramePosition
                                        ? position - snapshot.deviceFramePosition
                                        : snapshot.deviceFramePosition - position;
        if (stampNs != 0 && difference <= bufferFrames_)
        {
            snapshot.deviceFramePosition = position;
            snapshot.quality = AudioClockQuality::Hardware;
            snapshot.monotonicTimeNs = stampNs;
            return snapshot;
        }
    }
#endif

    // Prefer the driver timestamp when it has subsecond precision; a driver
    // that does not fill it leaves it at zero, so pair the position with the
    // status timestamp instead.
    stampNs = readHtstampNs(snd_pcm_status_get_driver_htstamp, impl_->status);
    if (stampNs % 1000000000ULL != 0)
    {
        snapshot.quality = AudioClockQuality::Driver;
    }
    else
    {
        stampNs = readHtstampNs(snd_pcm_status_get_htstamp, impl_->status);
        snapshot.quality = AudioClockQuality::Estimated;
    }

    if (stampNs == 0)
    {
        // No usable timestamp at all; reporting a position without a time
        // would be worse than reporting nothing.
        return AudioClockSnapshot();
    }

    snapshot.monotonicTimeNs = stampNs;
    return snapshot;
}

} // namespace multiroom
