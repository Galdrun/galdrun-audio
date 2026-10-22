#include "audio/esp32/I2sBackend.h"

#include "audio/esp32/Es8388.h"
#include "util/Logger.h"

#include "esp_attr.h"
#include "esp_pthread.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "soc/soc_caps.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace multiroom {
namespace {

const char* const kCodecDeviceId = "es8388";
const char* const kExternalDeviceId = "i2s";

// The codec is run single speed at MCLK = 256 fs (Es8388.cpp), which is its
// 8 to 48 kHz range. Which of these a stream can actually use is also a
// question of memory: the ring buffer is sized against the PSRAM budget in
// DeviceSelection, not here.
const uint32_t kCodecRates[] = {8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000};

#ifdef CONFIG_MULTIROOM_CODEC_DOUBLE_SPEED
// Double speed at MCLK = 128 fs: the MCLK of 44.1 and 48 kHz again. As I2S
// master only: as clock master the codec would get the driver's 8 BCLK, up to
// 49 MHz at 96 kHz with 32-bit slots.
const uint32_t kCodecDoubleSpeedRates[] = {88200, 96000};
#endif

bool isCodecDoubleSpeed(uint32_t rate)
{
#ifdef CONFIG_MULTIROOM_CODEC_DOUBLE_SPEED
    return rate > 48000;
#else
    (void)rate;
    return false;
#endif
}

// A DMA buffer holds at most 4092 bytes.
const uint32_t kMaxDmaBufferBytes = 4092;
const uint32_t kDmaDescriptors = 6;
const int kWriteTimeoutMs = 500;
const uint32_t kCodecSettleMs = 50;

// The codec's DAC volume as audio/HardwareVolume.h describes a control. A raw
// step counts up with the level, the register counts down: step = 192 - reg.
HardwareVolumeInfo codecVolumeInfo()
{
    HardwareVolumeInfo info;
    info.available = true;
    info.control = "ES8388 DAC";
    info.minStep = 0;
    info.maxStep = es8388::kMaxAttenuation;
    info.hasDecibels = true;
    info.minDb = -96.0;
    info.maxDb = 0.0;
    info.hasMute = true;
    return info;
}

class Es8388Volume final : public IHardwareVolume
{
public:
    bool set(double gain, bool muted, std::string& error) override
    {
        // Rounded towards quieter, like the ALSA path: never louder than asked.
        const double db = hardware_volume::decibelsForGain(info_, gain);
        const double steps = std::ceil(-db * 2.0);
        const uint8_t attenuation = static_cast<uint8_t>(
            steps < es8388::kMaxAttenuation ? steps : es8388::kMaxAttenuation);
        if (es8388::setVolume(attenuation, muted))
            return true;
        error = "es8388: the volume register write failed";
        return false;
    }

    bool setMute(bool muted, std::string& error) override
    {
        uint8_t attenuation = 0;
        bool wasMuted = false;
        es8388::volume(attenuation, wasMuted);
        if (es8388::setVolume(attenuation, muted))
            return true;
        error = "es8388: the mute register write failed";
        return false;
    }

    bool read(HardwareVolumeState& out, std::string& error) override
    {
        uint8_t attenuation = 0;
        bool muted = false;
        if (!es8388::volume(attenuation, muted))
        {
            error = "es8388: a failed write left the DAC's volume unknown";
            return false;
        }
        out = HardwareVolumeState();
        out.hasDecibels = true;
        out.db = -0.5 * attenuation;
        out.gain = hardware_volume::gainForDecibels(info_, out.db);
        out.muted = muted;
        return true;
    }

private:
    const HardwareVolumeInfo info_ = codecVolumeInfo();
};

// Above the network and control threads, below the Wi-Fi task (23), on the
// core Wi-Fi does not run on.
const int kRenderTaskPriority = 20;
const int kRenderTaskCore = 1;
const size_t kRenderTaskStack = 4096;

struct Pins
{
    int mclk;
    int bclk;
    int ws;
    int dout;
};

Pins pinsFor(bool codec)
{
    if (codec)
        return {CONFIG_MULTIROOM_CODEC_I2S_MCLK, CONFIG_MULTIROOM_CODEC_I2S_BCLK,
                CONFIG_MULTIROOM_CODEC_I2S_WS, CONFIG_MULTIROOM_CODEC_I2S_DOUT};
    return {CONFIG_MULTIROOM_EXT_I2S_MCLK, CONFIG_MULTIROOM_EXT_I2S_BCLK, CONFIG_MULTIROOM_EXT_I2S_WS,
            CONFIG_MULTIROOM_EXT_I2S_DOUT};
}

gpio_num_t pin(int number)
{
    return number < 0 ? I2S_GPIO_UNUSED : static_cast<gpio_num_t>(number);
}

bool toDataBits(AudioFormat::SampleType type, i2s_data_bit_width_t& out)
{
    switch (type)
    {
    case AudioFormat::SampleType::Int16:
        out = I2S_DATA_BIT_WIDTH_16BIT;
        return true;
    case AudioFormat::SampleType::Int32:
    case AudioFormat::SampleType::Int24In32:
        // TimelineSource left-justifies the wire's s24le, so on the bus 24-in-32
        // is a 32-bit word whose low byte is zero: what any DAC that takes
        // 32-bit slots plays exactly. Packed 24-bit is not offered; the server
        // repacks it to s24le and stays bitperfect (MATP section 2).
        out = I2S_DATA_BIT_WIDTH_32BIT;
        return true;
    default:
        return false;
    }
}

void addFormats(AudioDeviceInfo& device, const uint32_t* rates, size_t rateCount)
{
    const AudioFormat::SampleType types[] = {AudioFormat::SampleType::Int16,
                                             AudioFormat::SampleType::Int24In32,
                                             AudioFormat::SampleType::Int32};
    for (size_t r = 0; r < rateCount; ++r)
    {
        for (AudioFormat::SampleType type : types)
        {
            AudioFormat format;
            format.sampleRate = rates[r];
            format.channels = 2;
            format.sampleType = type;
            device.exclusiveFormats.push_back(format);
        }
    }
}

} // namespace

std::vector<AudioFormat> es8388Formats(const I2sSettings& settings)
{
    AudioDeviceInfo device;
    // As clock master the codec divides the ESP32's MCLK itself. The driver's
    // slave MCLK is 8 BCLK, 512 fs with 32-bit slots, which the codec halves
    // (Es8388.cpp).
    if (settings.clockMode == I2sClockMode::Slave)
    {
        const uint32_t slaveRate[] = {settings.slaveSampleRate};
        const bool supported = std::find(std::begin(kCodecRates), std::end(kCodecRates),
                                         settings.slaveSampleRate) != std::end(kCodecRates);
        if (supported)
            addFormats(device, slaveRate, 1);
    }
    else
    {
        addFormats(device, kCodecRates, sizeof(kCodecRates) / sizeof(kCodecRates[0]));
#ifdef CONFIG_MULTIROOM_CODEC_DOUBLE_SPEED
        addFormats(device, kCodecDoubleSpeedRates,
                   sizeof(kCodecDoubleSpeedRates) / sizeof(kCodecDoubleSpeedRates[0]));
#endif
    }
    return device.exclusiveFormats;
}

I2sBackend::I2sBackend(const I2sSettings& settings)
    : settings_(settings), starvedFrames_(0), state_(static_cast<int>(BackendState::Closed)),
      stopRequested_(false), underruns_(0), renderError_(ESP_OK)
{
}

I2sBackend::~I2sBackend()
{
    close();
}

std::unique_ptr<IHardwareVolume> I2sBackend::openHardwareVolume(
    const AudioDeviceInfo& device) const
{
    if (device.id != kCodecDeviceId || es8388::detect() != es8388::Detection::Present)
        return std::unique_ptr<IHardwareVolume>();
    return std::unique_ptr<IHardwareVolume>(new Es8388Volume());
}

std::vector<AudioDeviceInfo> I2sBackend::enumeratePlaybackDevices(CapabilityScan)
{
    // Nothing is probed on the peripheral: a rescan may run while another
    // backend is playing on it, and the formats follow from the clock mode
    // alone. open() still reports a rate the driver cannot make.
    std::vector<AudioDeviceInfo> devices;
    const bool slave = settings_.clockMode == I2sClockMode::Slave;

    const es8388::Detection codec = es8388::detect();
    if (codec == es8388::Detection::Ac101)
        lastError_ = "an AC101 codec answers on I2C; only the ES8388 is supported";

    if (codec == es8388::Detection::Present)
    {
        AudioDeviceInfo device;
        device.id = kCodecDeviceId;
        device.name = "ES8388 (on board)";
        device.description = std::string("ESP32-A1S codec, I2S ") +
                             i2sClockModeName(settings_.clockMode);
        device.isDefault = true;
        device.exclusiveFormats = es8388Formats(settings_);
        // The external DAC has no control interface, so it gets none.
        device.hardwareVolume = codecVolumeInfo();
        devices.push_back(device);
    }

    AudioDeviceInfo external;
    external.id = kExternalDeviceId;
    external.name = "External I2S DAC";
    external.description =
        std::string("I2S ") + i2sClockModeName(settings_.clockMode) + " on the configured pins";
    external.isDefault = devices.empty();
    // What the owner declared the DAC can play (client.json), and in slave
    // mode only the entries at the rate the external clock runs at: a DAC
    // that takes no 32-bit words does not take them from an external clock
    // either. Every int32 entry brings 24-in-32 at its rate: on the bus the
    // two are the same 32-bit word.
    for (const AudioFormat& format : settings_.externalFormats)
    {
        if (slave && format.sampleRate != settings_.slaveSampleRate)
            continue;
        external.exclusiveFormats.push_back(format);
        if (format.sampleType == AudioFormat::SampleType::Int32)
        {
            AudioFormat twentyFour = format;
            twentyFour.sampleType = AudioFormat::SampleType::Int24In32;
            external.exclusiveFormats.push_back(twentyFour);
        }
    }
    devices.push_back(external);

    return devices;
}

bool I2sBackend::open(const AudioBackendConfig& config)
{
    stop();

    std::lock_guard<std::mutex> lock(clockMutex_);
    closeChannel();
    lastError_.clear();
    renderError_.store(ESP_OK, std::memory_order_relaxed);

    if (config.renderCallback == nullptr)
    {
        lastError_ = "no render callback";
        return false;
    }
    if (!config.format.isValid() || config.format.channels != 2)
    {
        lastError_ = "I2S plays two channels only";
        return false;
    }
    i2s_data_bit_width_t dataBits;
    if (!toDataBits(config.format.sampleType, dataBits))
    {
        lastError_ = std::string("I2S does not play ") + sampleTypeName(config.format.sampleType);
        return false;
    }

    const bool codec = config.deviceId.empty()
                           ? es8388::detect() == es8388::Detection::Present
                           : config.deviceId == kCodecDeviceId;
    if (!config.deviceId.empty() && !codec && config.deviceId != kExternalDeviceId)
    {
        lastError_ = "unknown I2S device " + config.deviceId;
        return false;
    }

    const bool slave = settings_.clockMode == I2sClockMode::Slave;
    if (slave && config.format.sampleRate != settings_.slaveSampleRate)
    {
        // The external clock decides the rate; opening at another one would
        // play at the wrong speed without any error from the driver.
        lastError_ = "slave mode runs at " + std::to_string(settings_.slaveSampleRate) +
                     " Hz, not " + std::to_string(config.format.sampleRate);
        return false;
    }

    const uint32_t bytesPerFrame = config.format.bytesPerFrame();
    uint32_t period = config.requestedBufferFrames > 0 ? config.requestedBufferFrames
                                                       : config.format.sampleRate / 100;
    period = std::min(period, kMaxDmaBufferBytes / bytesPerFrame);

    i2s_chan_config_t channelConfig =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, slave ? I2S_ROLE_SLAVE : I2S_ROLE_MASTER);
    channelConfig.dma_desc_num = kDmaDescriptors;
    channelConfig.dma_frame_num = period;
    // A starved DMA sends zeros rather than repeating its last buffer.
    channelConfig.auto_clear = true;

    esp_err_t result = i2s_new_channel(&channelConfig, &channel_, nullptr);
    if (result != ESP_OK)
    {
        channel_ = nullptr;
        lastError_ = std::string("cannot create the I2S channel: ") + esp_err_to_name(result);
        return false;
    }

    i2s_std_config_t std = {};
    std.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(config.format.sampleRate);
    // The 160 MHz PLL through the fractional divider, in slave mode as well:
    // MCLK still comes from here. Not the ESP32's APLL, although its rate is
    // closer: the ES8388 has no PLL of its own and runs its DAC from MCLK,
    // and the APLL's jitter came out of an A1S as a rustle that rose with the
    // signal (phase 25). The server corrects the divider's few ppm as drift.
    std.clk_cfg.clk_src = I2S_CLK_SRC_PLL_160M;
    std.clk_cfg.mclk_multiple = codec && isCodecDoubleSpeed(config.format.sampleRate)
                                    ? I2S_MCLK_MULTIPLE_128
                                    : I2S_MCLK_MULTIPLE_256;
    std.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(dataBits, I2S_SLOT_MODE_STEREO);
    const Pins pins = pinsFor(codec);
    std.gpio_cfg.mclk = pin(pins.mclk);
    std.gpio_cfg.bclk = pin(pins.bclk);
    std.gpio_cfg.ws = pin(pins.ws);
    std.gpio_cfg.dout = pin(pins.dout);
    std.gpio_cfg.din = I2S_GPIO_UNUSED;

    result = i2s_channel_init_std_mode(channel_, &std);
    if (result != ESP_OK)
    {
        lastError_ = std::string("I2S rejected ") + describeFormat(config.format) + ": " +
                     esp_err_to_name(result);
        closeChannel();
        return false;
    }

    i2s_event_callbacks_t callbacks = {};
    callbacks.on_sent = &I2sBackend::onSent;
    callbacks.on_send_q_ovf = &I2sBackend::onStarved;
    result = i2s_channel_register_event_callback(channel_, &callbacks, this);
    if (result != ESP_OK)
    {
        lastError_ = std::string("cannot register the DMA callbacks: ") + esp_err_to_name(result);
        closeChannel();
        return false;
    }

    if (codec)
    {
        std::string codecError;
        if (!es8388::start(config.format, slave, codecError))
        {
            lastError_ = codecError;
            closeChannel();
            return false;
        }
        codec_ = true;
    }

    format_ = config.format;
    periodFrames_ = period;
    renderCallback_ = config.renderCallback;
    userData_ = config.userData;
    staging_.assign(static_cast<size_t>(period) * bytesPerFrame, 0);
    underruns_.store(0, std::memory_order_relaxed);
    state_.store(static_cast<int>(BackendState::Open), std::memory_order_release);

    MULTIROOM_DEBUG("i2s: opened %s as %s, %s, period %u frames x %u", codec ? kCodecDeviceId
                                                                        : kExternalDeviceId,
              i2sClockModeName(settings_.clockMode), describeFormat(format_).c_str(),
              static_cast<unsigned>(periodFrames_), static_cast<unsigned>(kDmaDescriptors));
    return true;
}

bool I2sBackend::start()
{
    if (state() == BackendState::Running)
        return true;
    if (state() != BackendState::Open)
    {
        lastError_ = "device is not open";
        return false;
    }

    if (codec_ && codecStopped_)
    {
        if (!es8388::start(format_, settings_.clockMode == I2sClockMode::Slave, lastError_))
            return false;
        codecStopped_ = false;
    }

    // A fresh frame axis: the DMA starts over at its first descriptor.
    {
        std::lock_guard<std::mutex> lock(clockMutex_);
        portENTER_CRITICAL(&clockLock_);
        sentFrames_ = 0;
        sentTimeUs_ = 0;
        portEXIT_CRITICAL(&clockLock_);
        frameExtender_.reset();
    }
    starvedFrames_.store(0, std::memory_order_relaxed);
    starvedHandled_ = 0;
    shutdown_.prepare(format_);

    // Every descriptor is filled with rendered audio before the DMA runs, so
    // the first frame the device plays is the first frame the source made.
    // Otherwise the DMA would begin with its empty buffers, and the source's
    // count would lead the device's by all of them.
    const size_t periodBytes = staging_.size();
    for (uint32_t i = 0; i < kDmaDescriptors; ++i)
    {
        const uint32_t filled = renderCallback_(userData_, staging_.data(), periodFrames_);
        if (filled < periodFrames_)
            std::memset(staging_.data() + static_cast<size_t>(filled) * format_.bytesPerFrame(), 0,
                        static_cast<size_t>(periodFrames_ - filled) * format_.bytesPerFrame());
        size_t loaded = 0;
        const esp_err_t result =
            i2s_channel_preload_data(channel_, staging_.data(), periodBytes, &loaded);
        if (result != ESP_OK || loaded != periodBytes)
        {
            lastError_ = "cannot preload the I2S DMA buffers";
            return false;
        }
    }

    const esp_err_t result = i2s_channel_enable(channel_);
    if (result != ESP_OK)
    {
        lastError_ = std::string("cannot start I2S: ") + esp_err_to_name(result);
        return false;
    }

    stopRequested_.store(false, std::memory_order_release);
    state_.store(static_cast<int>(BackendState::Running), std::memory_order_release);

    // Pinned and prioritised through the pthread configuration, which is what
    // std::thread is built on here. Restored at once, so no other thread
    // inherits it.
    esp_pthread_cfg_t threadConfig = esp_pthread_get_default_config();
    threadConfig.stack_size = kRenderTaskStack;
    threadConfig.prio = kRenderTaskPriority;
    threadConfig.pin_to_core = kRenderTaskCore;
    threadConfig.thread_name = "client_i2s";
    esp_pthread_set_cfg(&threadConfig);
    renderThread_ = std::thread(&I2sBackend::renderLoop, this);
    const esp_pthread_cfg_t defaults = esp_pthread_get_default_config();
    esp_pthread_set_cfg(&defaults);

    if (codec_)
    {
        // Keep rendering while the muted DAC settles. Count consumed frames,
        // so a slave bus without clocks never enables the analog output.
        const uint32_t settleFrames = std::max(kDmaDescriptors * periodFrames_,
            (format_.sampleRate * kCodecSettleMs + 999U) / 1000U);
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(kWriteTimeoutMs);
        uint32_t sent = 0;
        while (state() == BackendState::Running && std::chrono::steady_clock::now() < deadline)
        {
            portENTER_CRITICAL(&clockLock_);
            sent = sentFrames_;
            portEXIT_CRITICAL(&clockLock_);
            if (sent >= settleFrames)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (state() != BackendState::Running || sent < settleFrames)
        {
            lastError_ = "I2S did not establish a running clock for the codec";
            stop();
            return false;
        }
        if (!es8388::enableOutput(lastError_))
        {
            stop();
            return false;
        }
        MULTIROOM_DEBUG("i2s: codec output enabled after %u startup frames",
                       static_cast<unsigned>(sent));
    }
    return true;
}

void I2sBackend::renderPeriod()
{
    const uint32_t frameBytes = format_.bytesPerFrame();
    const uint32_t filled = shutdown_.render(renderCallback_, userData_, staging_.data(),
                                             periodFrames_);
    if (filled < periodFrames_)
    {
        // The source could not keep up. Silence the rest rather than replaying
        // the previous period's contents.
        std::memset(staging_.data() + static_cast<size_t>(filled) * frameBytes, 0,
                    static_cast<size_t>(periodFrames_ - filled) * frameBytes);
        underruns_.fetch_add(1, std::memory_order_relaxed);
    }
}

void I2sBackend::renderLoop()
{
    while (true)
    {
        shutdown_.begin(stopRequested_.load(std::memory_order_acquire));
        if (shutdown_.active() && shutdown_.remainingMs() == 0)
            break;
        // Whole periods only: the interrupt counts starvation in descriptors,
        // and a descriptor is one period.
        const uint32_t starved = starvedFrames_.load(std::memory_order_acquire);
        while (starved - starvedHandled_ >= periodFrames_)
        {
            if (stopRequested_.load(std::memory_order_acquire))
                break; // Shutdown's fade must be sent, not discarded as catch-up.
            renderPeriod(); // the moment for this audio went by as silence
            starvedHandled_ += periodFrames_;
        }

        shutdown_.begin(stopRequested_.load(std::memory_order_acquire));
        renderPeriod();

        size_t written = 0;
        const esp_err_t result = i2s_channel_write(channel_, staging_.data(), staging_.size(),
            &written, static_cast<uint32_t>(std::min(kWriteTimeoutMs, shutdown_.remainingMs())));
        if (result != ESP_OK || written != staging_.size())
        {
            if (shutdown_.active())
                break;
            // A timeout here means the bus has no clock: in slave mode, the
            // external master stopped. It is the device lost, not a hiccup.
            renderError_.store(result != ESP_OK ? result : ESP_ERR_INVALID_SIZE,
                               std::memory_order_relaxed);
            state_.store(static_cast<int>(BackendState::Failed), std::memory_order_release);
            return;
        }
        if (shutdown_.finished())
        {
            // write() copies into DMA; onSent is the playback confirmation.
            // One complete ring after this write guarantees the tail passed
            // through every descriptor, including across the 32-bit wrap.
            portENTER_CRITICAL(&clockLock_);
            const uint32_t at = sentFrames_;
            portEXIT_CRITICAL(&clockLock_);
            while (shutdown_.remainingMs() != 0)
            {
                portENTER_CRITICAL(&clockLock_);
                const uint32_t elapsed = sentFrames_ - at;
                portEXIT_CRITICAL(&clockLock_);
                if (elapsed >= kDmaDescriptors * periodFrames_)
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            break;
        }
    }
}

IRAM_ATTR bool I2sBackend::onSent(i2s_chan_handle_t, i2s_event_data_t*, void* context)
{
    I2sBackend* self = static_cast<I2sBackend*>(context);
    const int64_t now = esp_timer_get_time();
    portENTER_CRITICAL_ISR(&self->clockLock_);
    self->sentFrames_ += self->periodFrames_;
    self->sentTimeUs_ = now;
    portEXIT_CRITICAL_ISR(&self->clockLock_);
    return false;
}

IRAM_ATTR bool I2sBackend::onStarved(i2s_chan_handle_t, i2s_event_data_t*, void* context)
{
    I2sBackend* self = static_cast<I2sBackend*>(context);
    if (!self->stopRequested_.load(std::memory_order_relaxed))
    {
        self->starvedFrames_.fetch_add(self->periodFrames_, std::memory_order_release);
        self->underruns_.fetch_add(1, std::memory_order_relaxed);
    }
    return false;
}

void I2sBackend::stop()
{
    if (state() != BackendState::Running && !renderThread_.joinable())
        return;

    stopRequested_.store(true, std::memory_order_release);
    if (renderThread_.joinable())
        renderThread_.join();

    // Mute/power down the onboard DAC while its clocks are still present.
    // start() restores the codec and the user's volume/mute on a restart.
    if (codec_)
    {
        es8388::stop();
        codecStopped_ = true;
    }
    if (channel_ != nullptr)
        i2s_channel_disable(channel_);

    // A failed stream stays failed; only a healthy one returns to Open.
    if (state() == BackendState::Running)
        state_.store(static_cast<int>(BackendState::Open), std::memory_order_release);
}

void I2sBackend::close()
{
    // Outside the lock: stop() joins the render thread.
    stop();

    std::lock_guard<std::mutex> lock(clockMutex_);
    closeChannel();
}

void I2sBackend::closeChannel()
{
    if (codec_)
    {
        es8388::stop();
        codec_ = false;
        codecStopped_ = false;
    }
    if (channel_ != nullptr)
    {
        i2s_del_channel(channel_);
        channel_ = nullptr;
    }

    renderCallback_ = nullptr;
    userData_ = nullptr;
    periodFrames_ = 0;
    format_ = AudioFormat();
    staging_.clear();
    state_.store(static_cast<int>(BackendState::Closed), std::memory_order_release);
}

BackendState I2sBackend::state() const
{
    return static_cast<BackendState>(state_.load(std::memory_order_acquire));
}

AudioClockSnapshot I2sBackend::clockSnapshot()
{
    AudioClockSnapshot snapshot;

    std::lock_guard<std::mutex> lock(clockMutex_);
    if (channel_ == nullptr || !format_.isValid())
        return snapshot;

    portENTER_CRITICAL(&clockLock_);
    const uint32_t frames = sentFrames_;
    const int64_t timeUs = sentTimeUs_;
    portEXIT_CRITICAL(&clockLock_);

    // Nothing sent yet: no position to report.
    if (timeUs <= 0)
        return snapshot;

    // Taken in the interrupt that retired the descriptor, on the same
    // esp_timer base clock_gettime(CLOCK_MONOTONIC) reads here. Microsecond
    // resolution and interrupt latency make it a driver timestamp, not a
    // hardware one.
    snapshot.deviceFramePosition = frameExtender_.extend(frames);
    snapshot.monotonicTimeNs = static_cast<uint64_t>(timeUs) * 1000ULL;
    snapshot.nominalSampleRate = format_.sampleRate;
    snapshot.quality = AudioClockQuality::Driver;
    return snapshot;
}

uint64_t I2sBackend::underrunCount() const
{
    return underruns_.load(std::memory_order_relaxed);
}

std::string I2sBackend::lastError() const
{
    const int error = renderError_.load(std::memory_order_relaxed);
    if (error != ESP_OK)
        return std::string("I2S write failed: ") + esp_err_to_name(error);
    return lastError_;
}

} // namespace multiroom
