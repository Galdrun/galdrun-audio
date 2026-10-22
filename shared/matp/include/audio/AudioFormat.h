#pragma once

// Interleaved PCM format: a sample rate, a channel count and a byte layout.
//
// SampleType is a *byte layout*, not a bit depth. These five values are
// exactly MATP section 12's table, because they are what SESSION_CONFIG and
// every audio packet header carry, and both halves of the project reject
// anything else.
//
// Int24 and Int24In32 are not interchangeable and must never be collapsed:
// ALSA exposes both S24_3LE (three packed bytes) and S24_LE (24 valid bits in
// a 32-bit container), WASAPI reports the latter, and picking the wrong one
// produces noise rather than a quiet defect. Repacking between them is the
// only sample-touching operation the bitperfect path is allowed to perform
// (section 2).
//
// This file is shared because the wire code is: the client's AudioFormat and
// the server's were the same type written twice, and a format that means one
// thing on one side of a socket and another on the other is the one bug a
// shared library exists to make impossible. It is C++17 like everything else
// under shared/ -- see shared/json/CMakeLists.txt for why that is not a
// preference.
//
// The sample rates, layouts and channel counts a *device* is probed with are
// the client's own and live in client/include/audio/FormatProbe.h; the server
// probes nothing.

#include <cstddef>
#include <cstdint>
#include <string>

namespace multiroom {

struct AudioFormat
{
    enum class SampleType
    {
        Int16,     // 2 bytes                            MATP s16le
        Int24,     // 3 bytes per sample, packed         MATP s24le3, ALSA S24_3LE
        Int24In32, // 4 bytes per sample, 24 valid bits  MATP s24le,  ALSA S24_LE
        Int32,     // 4 bytes                            MATP s32le
        Float32    // 4 bytes                            MATP f32le
    };

    uint32_t sampleRate = 0;
    uint16_t channels = 0;
    SampleType sampleType = SampleType::Int16;

    bool isValid() const { return sampleRate > 0 && channels > 0; }

    uint32_t bytesPerSample() const { return bytesPerSample(sampleType); }
    uint32_t bytesPerFrame() const { return bytesPerSample() * channels; }

    static uint32_t bytesPerSample(SampleType type);

    // Significant bits, which differ from the container size for Int24In32.
    static uint32_t validBits(SampleType type);
};

// The widest layout in section 12's table. Int32 and Float32 are both four
// bytes; this names one of them so that a buffer sized for "the largest format
// the config allows" has a single, greppable definition of largest.
inline constexpr AudioFormat::SampleType kWidestSampleType = AudioFormat::SampleType::Int32;

bool operator==(const AudioFormat& a, const AudioFormat& b);
bool operator!=(const AudioFormat& a, const AudioFormat& b);

// Cache, config and status spelling: "int16", "int24", "int24in32", "int32",
// "float32". The wire spellings of section 12 belong to the MATP layer
// (matp/Protocol.h) and are not this type's business.
const char* sampleTypeName(AudioFormat::SampleType type);
bool parseSampleType(const std::string& text, AudioFormat::SampleType& out);

// "48000 Hz 2ch int24in32"
std::string describeFormat(const AudioFormat& format);

} // namespace multiroom
