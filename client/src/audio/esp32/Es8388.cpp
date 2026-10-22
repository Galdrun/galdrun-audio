#include "audio/esp32/Es8388.h"

#include "util/Logger.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "sdkconfig.h"

#include <mutex>

namespace multiroom {
namespace es8388 {
namespace {

const uint16_t kEs8388Address = 0x10;
const uint16_t kAc101Address = 0x1A;
const uint32_t kI2cSpeedHz = 100000;
const int kI2cTimeoutMs = 50;

// Register names from the ES8388 datasheet.
const uint8_t kControl1 = 0x00;
const uint8_t kControl2 = 0x01;
const uint8_t kChipPower = 0x02;
const uint8_t kAdcPower = 0x03;
const uint8_t kDacPower = 0x04;
const uint8_t kMasterMode = 0x08;
const uint8_t kDacControl1 = 0x17;
const uint8_t kDacControl2 = 0x18;
const uint8_t kDacControl3 = 0x19;
const uint8_t kDacControl4 = 0x1A;
const uint8_t kDacControl5 = 0x1B;
const uint8_t kDacControl16 = 0x26;
const uint8_t kDacControl17 = 0x27;
const uint8_t kDacControl20 = 0x2A;
const uint8_t kDacControl21 = 0x2B;
const uint8_t kDacControl23 = 0x2D;
const uint8_t kDacControl24 = 0x2E; // LOUT1 volume, then ROUT1, LOUT2, ROUT2

const uint8_t kDacMute = 0x04;
const uint8_t kOutputZeroDb = 0x1E;

std::mutex g_mutex;
bool g_probed = false;
Detection g_detection = Detection::Absent;
i2c_master_bus_handle_t g_bus = nullptr;
i2c_master_dev_handle_t g_device = nullptr;
bool g_running = false;
uint8_t g_attenuation = 0; // DACCONTROL4 and 5, both channels alike
bool g_muted = false;
// False once a write while running failed: the DAC may not hold the settings
// above until a later write of all of them succeeds.
bool g_applied = true;

bool openBusLocked()
{
    if (g_bus != nullptr)
        return true;

    i2c_master_bus_config_t config = {};
    config.i2c_port = I2C_NUM_0;
    config.sda_io_num = static_cast<gpio_num_t>(CONFIG_MULTIROOM_CODEC_I2C_SDA);
    config.scl_io_num = static_cast<gpio_num_t>(CONFIG_MULTIROOM_CODEC_I2C_SCL);
    config.clk_source = I2C_CLK_SRC_DEFAULT;
    config.glitch_ignore_cnt = 7;
    config.flags.enable_internal_pullup = true;
    const esp_err_t result = i2c_new_master_bus(&config, &g_bus);
    if (result != ESP_OK)
    {
        MULTIROOM_WARN("es8388: cannot open the I2C bus: %s", esp_err_to_name(result));
        g_bus = nullptr;
        return false;
    }
    return true;
}

bool write(uint8_t reg, uint8_t value)
{
    const uint8_t bytes[2] = {reg, value};
    return i2c_master_transmit(g_device, bytes, sizeof(bytes), kI2cTimeoutMs) == ESP_OK;
}

void setAmplifier(bool on)
{
#if CONFIG_MULTIROOM_CODEC_PA_GPIO >= 0
    const gpio_num_t pin = static_cast<gpio_num_t>(CONFIG_MULTIROOM_CODEC_PA_GPIO);
    static bool configured = false;
    if (!configured)
    {
        gpio_reset_pin(pin);
        gpio_set_level(pin, 0);
        gpio_set_direction(pin, GPIO_MODE_OUTPUT);
        configured = true;
    }
    gpio_set_level(pin, on ? 1 : 0);
#else
    (void)on;
#endif
}

} // namespace

Detection detect()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_probed)
        return g_detection;
    g_probed = true;

    if (!openBusLocked())
        return g_detection;

    if (i2c_master_probe(g_bus, kEs8388Address, kI2cTimeoutMs) == ESP_OK)
    {
        i2c_device_config_t device = {};
        device.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        device.device_address = kEs8388Address;
        device.scl_speed_hz = kI2cSpeedHz;
        if (i2c_master_bus_add_device(g_bus, &device, &g_device) == ESP_OK)
            g_detection = Detection::Present;
        else
            MULTIROOM_WARN("es8388: found, but cannot be added to the I2C bus");
    }
    else if (i2c_master_probe(g_bus, kAc101Address, kI2cTimeoutMs) == ESP_OK)
    {
        g_detection = Detection::Ac101;
    }
    return g_detection;
}

bool start(const AudioFormat& format, bool codecIsClockMaster, std::string& error)
{
    if (detect() != Detection::Present)
    {
        error = "no ES8388 codec on the I2C bus";
        return false;
    }

    // Word length in bits 5:3 of DACCONTROL1, I2S framing in bits 2:1.
    uint8_t wordLength = 0;
    switch (format.sampleType)
    {
    case AudioFormat::SampleType::Int16:
        wordLength = 0x18;
        break;
    case AudioFormat::SampleType::Int32:
    case AudioFormat::SampleType::Int24In32:
        // 24-in-32 is left-justified by then (TimelineSource), so the bus
        // carries the same 32-bit words either way.
        wordLength = 0x20;
        break;
    default:
        error = std::string("the ES8388 path does not play ") + sampleTypeName(format.sampleType);
        return false;
    }

    // Single speed, MCLK = 256 fs, up to 48 kHz. Above it double speed
    // (DACFsMode, bit 5) at MCLK = 128 fs: what I2sBackend configures the
    // driver for at those rates, and only as its slave.
    uint8_t speed = 0x02;
#ifdef CONFIG_MULTIROOM_CODEC_DOUBLE_SPEED
    if (format.sampleRate > 48000)
    {
        if (codecIsClockMaster)
        {
            error = "the ES8388 plays above 48 kHz only with the ESP32 as I2S master";
            return false;
        }
        speed = 0x20;
    }
#endif

    std::lock_guard<std::mutex> lock(g_mutex);
    // Volume updates during preparation must not unmute the DAC either.
    g_running = false;
    setAmplifier(false);
    bool ok = write(kDacControl3, kDacMute);
    ok = ok && write(kControl2, 0x50);
    ok = ok && write(kChipPower, 0x00);
    ok = ok && write(0x35, 0xA0);
    ok = ok && write(0x37, 0xD0);
    ok = ok && write(0x39, 0xD0);
    // Bit 7 makes the codec the clock master; bits 4:0 at zero leave the bit
    // clock divider to the codec. As its slave the ESP32 drives MCLK at 8 BCLK
    // (the driver's minimum divider), which with 32-bit slots is 512 fs: bit 6
    // halves it to the 256 fs DACCONTROL2 below expects.
    uint8_t masterMode = 0x00;
    if (codecIsClockMaster)
        masterMode = wordLength == 0x20 ? 0xC0 : 0x80;
    ok = ok && write(kMasterMode, masterMode);
    ok = ok && write(kDacPower, 0xC0);
    ok = ok && write(kControl1, 0x12);
    ok = ok && write(kDacControl1, wordLength);
    ok = ok && write(kDacControl2, speed);
    ok = ok && write(kDacControl16, 0x00);
    ok = ok && write(kDacControl17, 0x90);
    ok = ok && write(kDacControl20, 0x90);
    ok = ok && write(kDacControl21, 0x80);
    ok = ok && write(kDacControl23, 0x00);
    ok = ok && write(kDacControl4, g_attenuation);
    ok = ok && write(kDacControl5, g_attenuation);
    for (uint8_t reg = kDacControl24; ok && reg < kDacControl24 + 4; ++reg)
        ok = write(reg, kOutputZeroDb);
    ok = ok && write(kAdcPower, 0xFF);
    ok = ok && write(kDacPower, 0x3C);
    ok = ok && write(kChipPower, 0x00);
    if (!ok)
    {
        write(kDacControl3, kDacMute);
        write(kDacPower, 0xC0);
        error = "es8388: an I2C register write failed";
        return false;
    }

    return true;
}

bool enableOutput(std::string& error)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    // The opening hook may have changed the room's volume since start().
    if (g_device == nullptr || !write(kDacControl4, g_attenuation) ||
        !write(kDacControl5, g_attenuation) ||
        !write(kDacControl3, g_muted ? kDacMute : 0x00))
    {
        error = "es8388: cannot enable the DAC output";
        return false;
    }
    g_applied = true;
    g_running = true;
    setAmplifier(true);
    return true;
}

void stop()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_device == nullptr)
        return;
    g_running = false;
    setAmplifier(false);
    write(kDacControl3, kDacMute);
    write(kDacPower, 0xC0);
}

bool setVolume(uint8_t attenuation, bool muted)
{
    if (detect() != Detection::Present)
        return false;

    std::lock_guard<std::mutex> lock(g_mutex);
    g_attenuation = attenuation < kMaxAttenuation ? attenuation : kMaxAttenuation;
    g_muted = muted;
    // Stopped or preparing, the DAC stays muted; enableOutput() writes both.
    if (!g_running)
        return true;
    g_applied = write(kDacControl4, g_attenuation) && write(kDacControl5, g_attenuation) &&
                write(kDacControl3, g_muted ? kDacMute : 0x00);
    return g_applied;
}

bool volume(uint8_t& attenuation, bool& muted)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    attenuation = g_attenuation;
    muted = g_muted;
    return !g_running || g_applied;
}

} // namespace es8388
} // namespace multiroom
