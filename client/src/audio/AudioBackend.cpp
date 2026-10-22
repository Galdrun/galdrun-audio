#include "audio/AudioBackend.h"

#if defined(_WIN32)
#  include "audio/windows/WasapiBackend.h"
#elif defined(ESP_PLATFORM)
#  include "audio/esp32/I2sBackend.h"
#  include "esp_heap_caps.h"
#elif defined(__linux__)
#  include "audio/linux/AlsaBackend.h"
#endif

namespace multiroom {

const char* i2sClockModeName(I2sClockMode mode)
{
    switch (mode)
    {
    case I2sClockMode::Master:
        return "master";
    case I2sClockMode::Slave:
        return "slave";
    }
    return "unknown";
}

bool parseI2sClockMode(const std::string& text, I2sClockMode& out)
{
    if (text == "master")
        out = I2sClockMode::Master;
    else if (text == "slave")
        out = I2sClockMode::Slave;
    else
        return false;
    return true;
}

const std::vector<uint32_t>& i2sSampleRates()
{
    static const std::vector<uint32_t> rates = {8000,  11025, 12000, 16000, 22050,
                                                24000, 32000, 44100, 48000, 64000,
                                                88200, 96000, 176400, 192000};
    return rates;
}

bool isI2sSampleType(AudioFormat::SampleType type)
{
    return type == AudioFormat::SampleType::Int16 || type == AudioFormat::SampleType::Int32;
}

std::vector<AudioFormat> defaultI2sExternalFormats()
{
    std::vector<AudioFormat> formats;
    for (uint32_t rate : {44100u, 48000u, 88200u, 96000u, 176400u, 192000u})
    {
        for (AudioFormat::SampleType type :
             {AudioFormat::SampleType::Int16, AudioFormat::SampleType::Int32})
        {
            AudioFormat format;
            format.sampleRate = rate;
            format.channels = 2;
            format.sampleType = type;
            formats.push_back(format);
        }
    }
    return formats;
}

std::unique_ptr<IAudioBackend> createPlatformAudioBackend(const I2sSettings& i2s)
{
#if defined(_WIN32)
    (void)i2s;
    return std::unique_ptr<IAudioBackend>(new WasapiBackend());
#elif defined(ESP_PLATFORM)
    return std::unique_ptr<IAudioBackend>(new I2sBackend(i2s));
#elif defined(__linux__)
    (void)i2s;
    return std::unique_ptr<IAudioBackend>(new AlsaBackend());
#else
    (void)i2s;
    return std::unique_ptr<IAudioBackend>();
#endif
}

const char* platformAudioBackendName()
{
#if defined(_WIN32)
    return "wasapi";
#elif defined(ESP_PLATFORM)
    return "i2s";
#elif defined(__linux__)
    return "alsa";
#else
    return "none";
#endif
}

uint64_t platformRingBufferBudgetBytes()
{
#if defined(ESP_PLATFORM)
    // The ring buffer is the one large block in PSRAM; the rest of the heap
    // above the malloc threshold (sdkconfig.defaults) -- packet buffers, JSON
    // -- needs this much beside it. Fixed rather than read from the free heap,
    // so the formats offered do not change with the moment they are counted.
    const uint64_t kReserveBytes = 1024 * 1024;
    const uint64_t total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    // No PSRAM leaves 1 byte: no stream buffer fits, and the room says so
    // instead of offering formats it cannot hold.
    return total > kReserveBytes ? total - kReserveBytes : 1;
#else
    return 0;
#endif
}

} // namespace multiroom
