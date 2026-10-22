// Compile the real codec driver against a recording I2C/GPIO bus. This checks
// that opening and volume hooks cannot enable the output before DMA starts.
#include "TestSupport.h"
#include "audio/esp32/Es8388.h"
#include "driver/i2c_master.h"

namespace {

uint8_t registers[256] = {};
bool amplifier = false;
bool failWrite = false;
unsigned writes = 0;
unsigned resets = 0;

} // namespace

int gpio_reset_pin(gpio_num_t) { ++resets; return ESP_OK; }
int gpio_set_direction(gpio_num_t, int) { return ESP_OK; }
int gpio_set_level(gpio_num_t, int level)
{
    amplifier = level != 0;
    return ESP_OK;
}

esp_err_t i2c_new_master_bus(const i2c_master_bus_config_t*, i2c_master_bus_handle_t* bus)
{
    *bus = registers;
    return ESP_OK;
}
esp_err_t i2c_master_probe(i2c_master_bus_handle_t, uint16_t, int) { return ESP_OK; }
esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t,
                                  const i2c_device_config_t*, i2c_master_dev_handle_t* device)
{
    *device = registers;
    return ESP_OK;
}
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t, const uint8_t* data, size_t size, int)
{
    CHECK_EQ(size, 2u);
    ++writes;
    if (failWrite)
    {
        failWrite = false;
        return 1;
    }
    registers[data[0]] = data[1];
    return ESP_OK;
}
const char* esp_err_to_name(esp_err_t) { return "stub I2C error"; }

MULTIROOM_TEST(es8388_keeps_startup_muted_until_output_is_explicitly_enabled)
{
    using namespace multiroom;
    AudioFormat format;
    format.sampleRate = 48000;
    format.channels = 2;
    format.sampleType = AudioFormat::SampleType::Int16;
    std::string error;

    CHECK(es8388::setVolume(70, false));
    CHECK(es8388::start(format, false, error));
    CHECK(!amplifier);
    CHECK_EQ(registers[0x19], 0x04u);

    // The device-opened volume hook runs before I2S starts. Cache its newest
    // settings without touching the still-muted hardware.
    const unsigned preparedWrites = writes;
    CHECK(es8388::setVolume(20, true));
    CHECK(es8388::setVolume(30, false));
    CHECK_EQ(writes, preparedWrites);
    CHECK(!amplifier);
    CHECK_EQ(registers[0x19], 0x04u);
    CHECK(es8388::enableOutput(error));
    CHECK(amplifier);
    CHECK_EQ(registers[0x1A], 30u);
    CHECK_EQ(registers[0x1B], 30u);
    CHECK_EQ(registers[0x19], 0u);

    CHECK(es8388::setVolume(40, true));
    CHECK_EQ(registers[0x19], 0x04u);
    es8388::stop();
    CHECK(!amplifier);
    CHECK_EQ(registers[0x04], 0xC0u);
    CHECK(es8388::start(format, true, error));
    CHECK(!amplifier);
    CHECK(es8388::enableOutput(error));
    CHECK_EQ(registers[0x1A], 40u);
    CHECK_EQ(registers[0x19], 0x04u); // User mute survives reopening.
    es8388::stop();

    failWrite = true;
    CHECK(!es8388::start(format, false, error));
    CHECK(!amplifier);
    CHECK_EQ(registers[0x19], 0x04u);
    CHECK_EQ(registers[0x04], 0xC0u);
    CHECK(es8388::start(format, false, error));
    failWrite = true;
    CHECK(!es8388::enableOutput(error));
    CHECK(!amplifier);
    CHECK_EQ(registers[0x19], 0x04u);
    es8388::stop();

    // A failed unmute is not reported as applied; the next write retries it.
    CHECK(es8388::start(format, false, error));
    CHECK(es8388::enableOutput(error));
    CHECK_EQ(registers[0x19], 0x04u);
    uint8_t attenuation = 0;
    bool muted = false;
    failWrite = true;
    CHECK(!es8388::setVolume(20, false));
    CHECK(!es8388::volume(attenuation, muted));
    CHECK_EQ(registers[0x19], 0x04u);
    CHECK(es8388::setVolume(20, false));
    CHECK(es8388::volume(attenuation, muted));
    CHECK(!muted);
    CHECK_EQ(registers[0x19], 0u);
    es8388::stop();
    CHECK_EQ(resets, 1u); // No floating GPIO pulse on amplifier transitions.
}
