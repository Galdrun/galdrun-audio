#include "audio/AudioFormat.h"

namespace multiroom {

uint32_t AudioFormat::bytesPerSample(SampleType type)
{
    switch (type)
    {
    case SampleType::Int16:
        return 2;
    case SampleType::Int24:
        return 3;
    case SampleType::Int24In32:
        return 4;
    case SampleType::Int32:
        return 4;
    case SampleType::Float32:
        return 4;
    }
    return 0;
}

uint32_t AudioFormat::validBits(SampleType type)
{
    switch (type)
    {
    case SampleType::Int16:
        return 16;
    case SampleType::Int24:
        return 24;
    case SampleType::Int24In32:
        return 24;
    case SampleType::Int32:
        return 32;
    case SampleType::Float32:
        return 32;
    }
    return 0;
}

bool operator==(const AudioFormat& a, const AudioFormat& b)
{
    return a.sampleRate == b.sampleRate && a.channels == b.channels &&
           a.sampleType == b.sampleType;
}

bool operator!=(const AudioFormat& a, const AudioFormat& b)
{
    return !(a == b);
}

const char* sampleTypeName(AudioFormat::SampleType type)
{
    switch (type)
    {
    case AudioFormat::SampleType::Int16:
        return "int16";
    case AudioFormat::SampleType::Int24:
        return "int24";
    case AudioFormat::SampleType::Int24In32:
        return "int24in32";
    case AudioFormat::SampleType::Int32:
        return "int32";
    case AudioFormat::SampleType::Float32:
        return "float32";
    }
    return "unknown";
}

bool parseSampleType(const std::string& text, AudioFormat::SampleType& out)
{
    if (text == "int16")
        out = AudioFormat::SampleType::Int16;
    else if (text == "int24")
        out = AudioFormat::SampleType::Int24;
    else if (text == "int24in32")
        out = AudioFormat::SampleType::Int24In32;
    else if (text == "int32")
        out = AudioFormat::SampleType::Int32;
    else if (text == "float32")
        out = AudioFormat::SampleType::Float32;
    else
        return false;
    return true;
}

std::string describeFormat(const AudioFormat& format)
{
    return std::to_string(format.sampleRate) + " Hz " + std::to_string(format.channels) +
           "ch " + sampleTypeName(format.sampleType);
}

} // namespace multiroom
