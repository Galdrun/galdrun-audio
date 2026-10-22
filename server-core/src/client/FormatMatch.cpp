#include "client/FormatMatch.h"

#include <cstdint>
#include <tuple>

namespace multiroom::format_match {
namespace {

// Lower is better, everywhere in this file, so every rule is one tuple
// comparison and none of them can disagree about direction.
using RepackCost = std::tuple<uint32_t, uint32_t, uint32_t>;
using RateKey = std::tuple<int, uint64_t>;
using TypeKey = std::tuple<int, uint32_t, int, uint32_t>;

bool repackCost(AudioFormat::SampleType from, AudioFormat::SampleType to, RepackCost& out)
{
    if (!isValuePreserving(from, to))
        return false;

    out = RepackCost{AudioFormat::validBits(to) - AudioFormat::validBits(from),
                     from == to ? 0u : 1u, AudioFormat::bytesPerSample(to)};
    return true;
}

RateKey rateKey(uint32_t rate, uint32_t sourceRate, uint32_t preferredRate)
{
    if (rate % sourceRate == 0)
        return RateKey{0, uint64_t{rate} / sourceRate};
    if (sourceRate % rate == 0)
        return RateKey{1, uint64_t{sourceRate} / rate};
    if (preferredRate != 0 && rate == preferredRate)
        return RateKey{2, 0};

    // Highest first, and the subtraction keeps "lower is better" the one rule
    // every comparison here obeys.
    return RateKey{3, UINT64_MAX - rate};
}

TypeKey typeKey(AudioFormat::SampleType type, uint32_t sourceBits)
{
    const uint32_t bits = AudioFormat::validBits(type);
    const int tier = bits >= sourceBits ? 0 : 1;
    const int isFloat = type == AudioFormat::SampleType::Float32 ? 1 : 0;
    return TypeKey{tier, AudioFormat::validBits(kWidestSampleType) - bits, isFloat,
                   AudioFormat::bytesPerSample(type)};
}

} // namespace

const AudioFormat* bestValuePreserving(const AudioFormat& source,
                                       const std::vector<AudioFormat>& offered)
{
    const AudioFormat* best = nullptr;
    RepackCost bestCost{};

    for (const AudioFormat& candidate : offered)
    {
        if (candidate.sampleRate != source.sampleRate || candidate.channels != source.channels)
            continue;

        RepackCost cost{};
        if (!repackCost(source.sampleType, candidate.sampleType, cost))
            continue;

        if (best == nullptr || cost < bestCost)
        {
            best = &candidate;
            bestCost = cost;
        }
    }

    return best;
}

const AudioFormat* bestFallback(const AudioFormat& source,
                                const ClientCapabilities& capabilities)
{
    const AudioFormat* byRate = nullptr;
    RateKey bestRate{};

    for (const AudioFormat& candidate : capabilities.formats)
    {
        if (candidate.channels != source.channels)
            continue;

        const RateKey key =
            rateKey(candidate.sampleRate, source.sampleRate, capabilities.preferredSampleRate);
        if (byRate == nullptr || key < bestRate)
        {
            byRate = &candidate;
            bestRate = key;
        }
    }

    if (byRate == nullptr)
        return nullptr;

    // The rate is fixed before the layout is looked at, because the rules are
    // ordered: a room does not take a worse rate to reach a wider container.
    const uint32_t outputRate = byRate->sampleRate;
    const uint32_t sourceBits = AudioFormat::validBits(source.sampleType);

    const AudioFormat* chosen = nullptr;
    TypeKey bestType{};

    for (const AudioFormat& candidate : capabilities.formats)
    {
        if (candidate.channels != source.channels || candidate.sampleRate != outputRate)
            continue;

        const TypeKey key = typeKey(candidate.sampleType, sourceBits);
        if (chosen == nullptr || key < bestType)
        {
            chosen = &candidate;
            bestType = key;
        }
    }

    // The rate came from these same candidates, so one of them always matches.
    return chosen;
}

} // namespace multiroom::format_match
