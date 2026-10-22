#include "audio/FormatProbe.h"

namespace multiroom {
namespace probe {

const uint32_t kSampleRates[] = {44100, 48000, 88200, 96000, 176400, 192000};
const size_t kSampleRateCount = sizeof(kSampleRates) / sizeof(kSampleRates[0]);

const AudioFormat::SampleType kSampleTypes[] = {
    AudioFormat::SampleType::Int16,
    AudioFormat::SampleType::Int24,
    AudioFormat::SampleType::Int24In32,
    AudioFormat::SampleType::Int32,
    AudioFormat::SampleType::Float32,
};
const size_t kSampleTypeCount = sizeof(kSampleTypes) / sizeof(kSampleTypes[0]);

const uint16_t kChannelCounts[] = {1, 2, 6, 8};
const size_t kChannelCountCount = sizeof(kChannelCounts) / sizeof(kChannelCounts[0]);

} // namespace probe
} // namespace multiroom
