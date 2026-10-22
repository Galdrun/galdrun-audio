#include "audio/AudioDeviceInfo.h"

namespace multiroom {
namespace {

// Preference order when several sample types are available at the same rate:
// more valid bits first, and a float container last among the 32-bit options
// because integer transport keeps the server protocol simple.
int sampleTypeRank(AudioFormat::SampleType type)
{
    switch (type)
    {
    case AudioFormat::SampleType::Int32:
        return 5;
    case AudioFormat::SampleType::Float32:
        return 4;
    case AudioFormat::SampleType::Int24In32:
        return 3;
    case AudioFormat::SampleType::Int24:
        return 2;
    case AudioFormat::SampleType::Int16:
        return 1;
    }
    return 0;
}

bool listContains(const std::vector<AudioFormat>& formats, const AudioFormat& format)
{
    for (size_t i = 0; i < formats.size(); ++i)
    {
        if (formats[i] == format)
            return true;
    }
    return false;
}

// Scores a candidate within one access mode. Channel match dominates, sample
// width breaks ties.
int scoreFormat(const AudioFormat& candidate, uint16_t preferredChannels)
{
    int score = sampleTypeRank(candidate.sampleType);
    if (candidate.channels == preferredChannels)
        score += 100;
    else if (candidate.channels > preferredChannels)
        score += 10;
    return score;
}

uint32_t maxRate(uint32_t a, uint32_t b)
{
    return a > b ? a : b;
}

} // namespace

const char* audioAccessModeName(AudioAccessMode mode)
{
    switch (mode)
    {
    case AudioAccessMode::Shared:
        return "shared";
    case AudioAccessMode::Exclusive:
        return "exclusive";
    }
    return "unknown";
}

const char* audioAccessPreferenceName(AudioAccessPreference preference)
{
    switch (preference)
    {
    case AudioAccessPreference::PreferExclusive:
        return "exclusive";
    case AudioAccessPreference::ExclusiveOnly:
        return "exclusive-only";
    case AudioAccessPreference::SharedOnly:
        return "shared";
    }
    return "unknown";
}

bool parseAudioAccessPreference(const std::string& text, AudioAccessPreference& out)
{
    if (text == "exclusive")
        out = AudioAccessPreference::PreferExclusive;
    else if (text == "exclusive-only")
        out = AudioAccessPreference::ExclusiveOnly;
    else if (text == "shared")
        out = AudioAccessPreference::SharedOnly;
    else
        return false;
    return true;
}

const std::vector<AudioFormat>& AudioDeviceInfo::formats(AudioAccessMode mode) const
{
    return mode == AudioAccessMode::Exclusive ? exclusiveFormats : sharedFormats;
}

std::vector<AudioFormat>& AudioDeviceInfo::formats(AudioAccessMode mode)
{
    return mode == AudioAccessMode::Exclusive ? exclusiveFormats : sharedFormats;
}

bool AudioDeviceInfo::supports(const AudioFormat& format, AudioAccessMode mode) const
{
    return listContains(formats(mode), format);
}

bool AudioDeviceInfo::supportsAnyMode(const AudioFormat& format) const
{
    return listContains(exclusiveFormats, format) || listContains(sharedFormats, format);
}

bool AudioDeviceInfo::bestFormatForRate(uint32_t sampleRate, uint16_t preferredChannels,
                                        AudioAccessPreference preference, AudioFormat& out,
                                        AudioAccessMode& outMode) const
{
    // Order matters: the first mode that offers the rate wins outright, so
    // PreferExclusive only reaches shared when exclusive has nothing.
    AudioAccessMode order[2] = {AudioAccessMode::Exclusive, AudioAccessMode::Shared};
    size_t modeCount = 2;

    if (preference == AudioAccessPreference::ExclusiveOnly)
    {
        modeCount = 1;
    }
    else if (preference == AudioAccessPreference::SharedOnly)
    {
        order[0] = AudioAccessMode::Shared;
        modeCount = 1;
    }

    for (size_t m = 0; m < modeCount; ++m)
    {
        const std::vector<AudioFormat>& candidates = formats(order[m]);
        const AudioFormat* best = nullptr;
        int bestScore = -1;

        for (size_t i = 0; i < candidates.size(); ++i)
        {
            if (candidates[i].sampleRate != sampleRate)
                continue;
            const int score = scoreFormat(candidates[i], preferredChannels);
            if (score > bestScore)
            {
                bestScore = score;
                best = &candidates[i];
            }
        }

        if (best != nullptr)
        {
            out = *best;
            outMode = order[m];
            return true;
        }
    }

    return false;
}

bool chooseOutputFormat(const AudioDeviceInfo& device, uint32_t preferredSampleRate,
                        uint16_t preferredChannels, AudioAccessPreference preference,
                        AudioFormat& out, AudioAccessMode& outMode)
{
    if (preferredSampleRate != 0 &&
        device.bestFormatForRate(preferredSampleRate, preferredChannels, preference, out,
                                 outMode))
    {
        return true;
    }

    // The two rates every server can produce without resampling gymnastics.
    const uint32_t fallbacks[] = {48000, 44100};
    for (size_t i = 0; i < sizeof(fallbacks) / sizeof(fallbacks[0]); ++i)
    {
        if (fallbacks[i] == preferredSampleRate)
            continue;
        if (device.bestFormatForRate(fallbacks[i], preferredChannels, preference, out, outMode))
            return true;
    }

    // Last resort: the highest rate the device reported in a permitted mode.
    // Lists the preference excludes must not be considered here either, or
    // ExclusiveOnly would silently pick a shared-only rate.
    uint32_t highest = 0;
    if (preference != AudioAccessPreference::SharedOnly)
    {
        for (size_t i = 0; i < device.exclusiveFormats.size(); ++i)
            highest = maxRate(highest, device.exclusiveFormats[i].sampleRate);
    }
    if (preference != AudioAccessPreference::ExclusiveOnly)
    {
        for (size_t i = 0; i < device.sharedFormats.size(); ++i)
            highest = maxRate(highest, device.sharedFormats[i].sampleRate);
    }
    if (highest == 0)
        return false;

    return device.bestFormatForRate(highest, preferredChannels, preference, out, outMode);
}

} // namespace multiroom
