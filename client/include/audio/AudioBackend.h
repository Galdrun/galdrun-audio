#pragma once

#include "audio/AudioFormat.h"
#include "audio/IAudioBackend.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace multiroom {

// Who drives the I2S bit and word clocks. Master: the ESP32 generates them
// from its audio PLL, and the rate is whatever the server negotiates. Slave: an
// external clock (a DAC or a clock board with its own oscillator) drives them,
// so the rate is fixed by that hardware and cannot be negotiated.
enum class I2sClockMode
{
    Master,
    Slave
};

const char* i2sClockModeName(I2sClockMode mode);
bool parseI2sClockMode(const std::string& text, I2sClockMode& out);

// The rates the I2S backend clocks from its audio PLL: what an external DAC's
// format list may name. Ascending.
const std::vector<uint32_t>& i2sSampleRates();

// Int16 and Int32 are the two slot widths the I2S backend puts on the bus,
// and so the two an external DAC's format list names. Int24In32 rides in a
// 32-bit slot and is offered wherever Int32 is; packed Int24 is not played.
bool isI2sSampleType(AudioFormat::SampleType type);

// 44.1 to 192 kHz in both layouts: what a typical I2S DAC takes, and what the
// external DAC offered before its list could be configured.
std::vector<AudioFormat> defaultI2sExternalFormats();

// Read only by the I2S backend; WASAPI and ALSA ignore it.
struct I2sSettings
{
    I2sClockMode clockMode = I2sClockMode::Master;
    // The rate the external clock runs at in slave mode, and the only one the
    // device then offers.
    uint32_t slaveSampleRate = 48000;

    // What the external DAC (device "i2s") can play, as rate and sample type
    // pairs; two channels always. Such a DAC has no control interface to ask,
    // so the board's owner declares it. The backend offers these, in master
    // mode, and only slaveSampleRate's in slave mode.
    std::vector<AudioFormat> externalFormats = defaultI2sExternalFormats();
};

// The only public entry point to the platform audio layer. Everything above it
// works through IAudioBackend and never sees WASAPI, COM, snd_pcm_t or the
// ESP-IDF I2S driver.
//
// Returns nullptr when no backend is compiled in for this platform.
std::unique_ptr<IAudioBackend> createPlatformAudioBackend(const I2sSettings& i2s);

// "wasapi" | "alsa" | "i2s" | "none"
const char* platformAudioBackendName();

// How many bytes the stream's ring buffer may take on this platform, 0 for no
// limit beyond AudioRingBuffer's own. The ESP32 bounds it by its PSRAM: a
// larger buffer there leaves fewer formats the room can offer (MATP section
// 58).
uint64_t platformRingBufferBudgetBytes();

} // namespace multiroom
