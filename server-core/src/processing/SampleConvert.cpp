#include "processing/SampleConvert.h"

#include <cmath>
#include <cstring>

namespace multiroom::sample_convert {
namespace {

constexpr uint32_t byteAt(const std::byte* bytes, size_t index)
{
    return static_cast<uint32_t>(static_cast<uint8_t>(bytes[index]));
}

constexpr void storeLe(std::byte* bytes, uint32_t bits, size_t count)
{
    for (size_t i = 0; i < count; ++i)
        bytes[i] = static_cast<std::byte>((bits >> (8 * i)) & 0xFFu);
}

// A 24-bit value sits in the low three bytes of the word the readers build, so
// the sign bit has to be carried up by hand. Done on the unsigned value and
// converted once, which keeps every step defined rather than relying on the
// shift behaviour of a negative int.
constexpr int32_t signExtend24(uint32_t bits)
{
    if ((bits & 0x00800000u) != 0)
        bits |= 0xFF000000u;
    return static_cast<int32_t>(bits);
}

using ReadInt = int32_t (*)(const std::byte*);
using WriteInt = void (*)(std::byte*, int32_t);

int32_t readInt16(const std::byte* p)
{
    return static_cast<int16_t>(static_cast<uint16_t>(byteAt(p, 0) | (byteAt(p, 1) << 8)));
}

int32_t readInt24(const std::byte* p)
{
    return signExtend24(byteAt(p, 0) | (byteAt(p, 1) << 8) | (byteAt(p, 2) << 16));
}

// The fourth byte is padding whichever convention the far end used: it is
// masked off and rebuilt, so a 0xFF sign fill and a 0x00 pad read alike.
int32_t readInt24In32(const std::byte* p)
{
    return signExtend24(byteAt(p, 0) | (byteAt(p, 1) << 8) | (byteAt(p, 2) << 16));
}

int32_t readInt32(const std::byte* p)
{
    return static_cast<int32_t>(byteAt(p, 0) | (byteAt(p, 1) << 8) | (byteAt(p, 2) << 16) |
                                (byteAt(p, 3) << 24));
}

void writeInt16(std::byte* p, int32_t value)
{
    storeLe(p, static_cast<uint32_t>(value), 2);
}

void writeInt24(std::byte* p, int32_t value)
{
    storeLe(p, static_cast<uint32_t>(value), 3);
}

// Int24In32 and Int32 are the same four-byte store: a 24-bit value arrives
// here already sign-extended into the top byte, which is what the decoder
// produces and what keeps the pair a memcpy when it is unchanged.
void writeLe32(std::byte* p, int32_t value)
{
    storeLe(p, static_cast<uint32_t>(value), 4);
}

ReadInt integerReader(AudioFormat::SampleType type)
{
    switch (type)
    {
    case AudioFormat::SampleType::Int16:
        return &readInt16;
    case AudioFormat::SampleType::Int24:
        return &readInt24;
    case AudioFormat::SampleType::Int24In32:
        return &readInt24In32;
    case AudioFormat::SampleType::Int32:
        return &readInt32;
    case AudioFormat::SampleType::Float32:
        break;
    }
    return nullptr;
}

WriteInt integerWriter(AudioFormat::SampleType type)
{
    switch (type)
    {
    case AudioFormat::SampleType::Int16:
        return &writeInt16;
    case AudioFormat::SampleType::Int24:
        return &writeInt24;
    case AudioFormat::SampleType::Int24In32:
    case AudioFormat::SampleType::Int32:
        return &writeLe32;
    case AudioFormat::SampleType::Float32:
        break;
    }
    return nullptr;
}

float readFloat(const std::byte* p)
{
    const uint32_t bits =
        byteAt(p, 0) | (byteAt(p, 1) << 8) | (byteAt(p, 2) << 16) | (byteAt(p, 3) << 24);
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

void writeFloat(std::byte* p, float value)
{
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    storeLe(p, bits, 4);
}

// 2^(validBits-1): the magnitude of the most negative code, so that -1.0 lands
// exactly on it and +1.0 clamps to the largest positive one.
double fullScale(AudioFormat::SampleType type)
{
    switch (type)
    {
    case AudioFormat::SampleType::Int16:
        return 32768.0;
    case AudioFormat::SampleType::Int24:
    case AudioFormat::SampleType::Int24In32:
        return 8388608.0;
    case AudioFormat::SampleType::Int32:
        return 2147483648.0;
    case AudioFormat::SampleType::Float32:
        break;
    }
    return 1.0;
}

} // namespace

uint32_t TriangularDither::nextBits() noexcept
{
    // xorshift64*, whose only requirement here is that two draws are
    // independent enough to add into a triangle.
    state_ ^= state_ >> 12;
    state_ ^= state_ << 25;
    state_ ^= state_ >> 27;
    return static_cast<uint32_t>((state_ * 0x2545F4914F6CDD1Dull) >> 32);
}

double TriangularDither::next() noexcept
{
    constexpr double kScale = 1.0 / 4294967296.0; // 2^-32, into [0, 1)
    const double a = static_cast<double>(nextBits()) * kScale;
    const double b = static_cast<double>(nextBits()) * kScale;
    return a - b;
}

void repack(std::span<const std::byte> input, AudioFormat::SampleType from,
            std::span<std::byte> output, AudioFormat::SampleType to, size_t samples)
{
    const size_t fromBytes = AudioFormat::bytesPerSample(from);
    const size_t toBytes = AudioFormat::bytesPerSample(to);

    if (from == to)
    {
        std::memcpy(output.data(), input.data(), samples * toBytes);
        return;
    }

    const ReadInt read = integerReader(from);
    const WriteInt write = integerWriter(to);
    if (read == nullptr || write == nullptr ||
        AudioFormat::validBits(to) < AudioFormat::validBits(from))
        return; // not a layout pair; the caller has already refused one

    // Value preserving means the *level* is preserved, so a narrower source
    // moves up rather than sitting at the bottom of a wider container: 16-bit
    // full scale reaches a 32-bit room at full scale and not 48 dB down. The
    // shift is exactly what makes the pair reversible.
    const uint32_t shift = AudioFormat::validBits(to) - AudioFormat::validBits(from);

    const std::byte* in = input.data();
    std::byte* out = output.data();
    for (size_t i = 0; i < samples; ++i)
    {
        const int32_t value = read(in);
        write(out, static_cast<int32_t>(static_cast<uint32_t>(value) << shift));
        in += fromBytes;
        out += toBytes;
    }
}

void toDouble(std::span<const std::byte> input, AudioFormat::SampleType from,
              std::span<double> output)
{
    const size_t samples = output.size();
    const size_t fromBytes = AudioFormat::bytesPerSample(from);
    const std::byte* in = input.data();

    if (from == AudioFormat::SampleType::Float32)
    {
        for (size_t i = 0; i < samples; ++i)
        {
            output[i] = static_cast<double>(readFloat(in));
            in += fromBytes;
        }
        return;
    }

    const ReadInt read = integerReader(from);
    const double scale = 1.0 / fullScale(from);
    for (size_t i = 0; i < samples; ++i)
    {
        output[i] = static_cast<double>(read(in)) * scale;
        in += fromBytes;
    }
}

void fromDouble(std::span<const double> input, std::span<std::byte> output,
                AudioFormat::SampleType to, TriangularDither* dither)
{
    const size_t samples = input.size();
    const size_t toBytes = AudioFormat::bytesPerSample(to);
    std::byte* out = output.data();

    if (to == AudioFormat::SampleType::Float32)
    {
        // Neither rounded, clamped nor dithered: there is no quantization step
        // to dither, and an excursion past full scale is something the
        // converter produced rather than something to hide.
        for (size_t i = 0; i < samples; ++i)
        {
            writeFloat(out, static_cast<float>(input[i]));
            out += toBytes;
        }
        return;
    }

    const WriteInt write = integerWriter(to);
    const double scale = fullScale(to);
    const double lowest = -scale;
    const double highest = scale - 1.0;

    for (size_t i = 0; i < samples; ++i)
    {
        double value = input[i] * scale;
        if (dither != nullptr)
            value += dither->next();

        // Rounded first and clamped after, so that a sample a hair past full
        // scale becomes the largest code rather than wrapping to the most
        // negative one. std::round rather than std::nearbyint: nearbyint reads
        // the current floating-point rounding mode, and a room's output must
        // not depend on what some other library left that mode set to.
        value = std::round(value);
        if (value < lowest)
            value = lowest;
        else if (value > highest)
            value = highest;

        write(out, static_cast<int32_t>(value));
        out += toBytes;
    }
}

} // namespace multiroom::sample_convert
