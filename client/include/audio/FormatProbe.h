#pragma once

// Sample rates, sample types and channel counts a playback device is probed
// with during a capability scan. Kept in one place so both backends, the
// device selection and the tests agree on the same list.
//
// Client-only, which is why it is not in the shared audio/AudioFormat.h beside
// the type it is made of: the server probes no hardware, and a list of rates
// to ask a driver about has nothing to do with the wire format.

#include "audio/AudioFormat.h"

#include <cstddef>
#include <cstdint>

namespace multiroom {
namespace probe {

extern const uint32_t kSampleRates[];
extern const size_t kSampleRateCount;

extern const AudioFormat::SampleType kSampleTypes[];
extern const size_t kSampleTypeCount;

extern const uint16_t kChannelCounts[];
extern const size_t kChannelCountCount;

} // namespace probe
} // namespace multiroom
