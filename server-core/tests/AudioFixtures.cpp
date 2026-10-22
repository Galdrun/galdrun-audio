#include "AudioFixtures.h"

#include <algorithm>
#include <iterator>
#include <cstring>

namespace multiroomtest {
namespace {

void putU16(std::vector<std::byte>& out, uint16_t value)
{
    out.push_back(static_cast<std::byte>(value & 0xFF));
    out.push_back(static_cast<std::byte>((value >> 8) & 0xFF));
}

void putU32(std::vector<std::byte>& out, uint32_t value)
{
    for (int shift = 0; shift < 32; shift += 8)
        out.push_back(static_cast<std::byte>((value >> shift) & 0xFF));
}

void putTag(std::vector<std::byte>& out, const char (&tag)[5])
{
    for (int index = 0; index < 4; ++index)
        out.push_back(static_cast<std::byte>(tag[index]));
}

// FLAC's two check values. Both are the plain bitwise definitions rather than
// table-driven: a fixture generator is not on anybody's inner loop, and the
// shift-and-xor form is the one that can be read against the specification.
uint8_t crc8(std::span<const std::byte> bytes)
{
    uint8_t crc = 0;
    for (std::byte value : bytes)
    {
        crc ^= static_cast<uint8_t>(value);
        for (int bit = 0; bit < 8; ++bit)
            crc = static_cast<uint8_t>((crc & 0x80) ? ((crc << 1) ^ 0x07) : (crc << 1));
    }
    return crc;
}

uint16_t crc16(std::span<const std::byte> bytes)
{
    uint16_t crc = 0;
    for (std::byte value : bytes)
    {
        crc ^= static_cast<uint16_t>(static_cast<uint8_t>(value) << 8);
        for (int bit = 0; bit < 8; ++bit)
            crc = static_cast<uint16_t>((crc & 0x8000) ? ((crc << 1) ^ 0x8005) : (crc << 1));
    }
    return crc;
}

// Most-significant-bit-first, which is the order every field in a FLAC stream
// is written in.
class BitWriter
{
public:
    void write(uint64_t value, uint32_t bits)
    {
        for (uint32_t index = bits; index > 0; --index)
            writeBit((value >> (index - 1)) & 1u);
    }

    void writeSigned(int32_t value, uint32_t bits)
    {
        const uint64_t mask = (bits >= 64) ? ~0ULL : ((1ULL << bits) - 1);
        write(static_cast<uint64_t>(static_cast<int64_t>(value)) & mask, bits);
    }

    void alignToByte()
    {
        while (used_ != 0)
            writeBit(0);
    }

    const std::vector<std::byte>& bytes() const { return bytes_; }

private:
    void writeBit(uint64_t bit)
    {
        if (used_ == 0)
            bytes_.push_back(std::byte{0});
        const uint32_t shift = 7 - used_;
        bytes_.back() |= static_cast<std::byte>((bit & 1u) << shift);
        used_ = (used_ + 1) % 8;
    }

    std::vector<std::byte> bytes_;
    uint32_t used_ = 0;
};

// FLAC's "UTF-8 like" coding of a frame number. Everything this generator
// produces is well under 2^21, so two continuation bytes is as far as it goes;
// the general form is written out anyway because getting it wrong only shows
// up on the 128th frame.
void writeUtf8Number(BitWriter& writer, uint64_t value)
{
    if (value < 0x80)
    {
        writer.write(value, 8);
        return;
    }

    // A `bytes`-byte sequence carries (7 - bytes) data bits in its lead byte
    // and six in each of the rest, so 5 * bytes + 1 in total.
    uint32_t bytes = 2;
    while (bytes < 7 && value >= (1ULL << (5 * bytes + 1)))
        ++bytes;

    const uint32_t leadDataBits = 7 - bytes;
    writer.write((1ULL << bytes) - 1, bytes); // `bytes` ones
    writer.write(0, 1);                       // and the zero that ends them
    if (leadDataBits > 0)
        writer.write((value >> (6 * (bytes - 1))) & ((1ULL << leadDataBits) - 1), leadDataBits);

    for (uint32_t index = bytes - 1; index > 0; --index)
    {
        writer.write(0b10, 2);
        writer.write((value >> (6 * (index - 1))) & 0x3F, 6);
    }
}

} // namespace

PcmFixture makeRamp(uint32_t sampleRate, uint16_t channels, uint32_t bits, uint32_t frames)
{
    PcmFixture fixture;
    fixture.sampleRate = sampleRate;
    fixture.channels = channels;
    fixture.bits = bits;
    fixture.frames = frames;

    const int32_t maxValue = static_cast<int32_t>((1u << (bits - 1)) - 1);
    const int32_t minValue = -maxValue - 1;
    const uint32_t period = 1u << bits;

    const int32_t opening[] = {0,          1,           -1,        maxValue,
                               minValue,   maxValue - 1, minValue + 1, maxValue / 2,
                               minValue / 2};

    fixture.samples.resize(static_cast<size_t>(frames) * channels);
    for (size_t index = 0; index < fixture.samples.size(); ++index)
    {
        if (index < std::size(opening))
        {
            fixture.samples[index] = opening[index];
            continue;
        }

        // A large odd stride, so the sequence covers the range and no two
        // neighbours agree in their high bits.
        const uint32_t step = static_cast<uint32_t>((index * 2654435761ULL) % period);
        fixture.samples[index] = static_cast<int32_t>(step) + minValue;
    }

    return fixture;
}

std::vector<std::byte> encodeWav(const PcmFixture& fixture)
{
    const uint32_t bytesPerSample = fixture.bits / 8;
    const uint32_t blockAlign = bytesPerSample * fixture.channels;
    const uint32_t dataBytes = blockAlign * fixture.frames;

    std::vector<std::byte> out;
    out.reserve(44 + dataBytes);

    putTag(out, "RIFF");
    putU32(out, 36 + dataBytes);
    putTag(out, "WAVE");

    putTag(out, "fmt ");
    putU32(out, 16);
    putU16(out, 1); // WAVE_FORMAT_PCM
    putU16(out, fixture.channels);
    putU32(out, fixture.sampleRate);
    putU32(out, fixture.sampleRate * blockAlign);
    putU16(out, static_cast<uint16_t>(blockAlign));
    putU16(out, static_cast<uint16_t>(fixture.bits));

    putTag(out, "data");
    putU32(out, dataBytes);

    for (int32_t sample : fixture.samples)
    {
        const auto bits = static_cast<uint32_t>(sample);
        for (uint32_t index = 0; index < bytesPerSample; ++index)
            out.push_back(static_cast<std::byte>((bits >> (8 * index)) & 0xFF));
    }

    return out;
}

std::vector<std::byte> encodeFlac(const PcmFixture& fixture, uint32_t blockFrames)
{
    // STREAMINFO's minimum has to account for a short final block, which is
    // the normal shape of a stream whose length is not a multiple of one.
    uint32_t minBlock = blockFrames;
    if (fixture.frames < blockFrames)
        minBlock = fixture.frames;
    else if (fixture.frames % blockFrames != 0)
        minBlock = fixture.frames % blockFrames;

    std::vector<std::byte> out;
    putTag(out, "fLaC");

    // STREAMINFO, and the last metadata block.
    BitWriter info;
    info.write(1, 1);  // last-metadata-block
    info.write(0, 7);  // type 0: STREAMINFO
    info.write(34, 24);
    info.write(minBlock, 16);
    info.write(blockFrames, 16);
    info.write(0, 24); // minimum frame size: unknown
    info.write(0, 24); // maximum frame size: unknown
    info.write(fixture.sampleRate, 20);
    info.write(static_cast<uint64_t>(fixture.channels) - 1, 3);
    info.write(static_cast<uint64_t>(fixture.bits) - 1, 5);
    info.write(fixture.frames, 36);
    for (int index = 0; index < 16; ++index)
        info.write(0, 8); // MD5 of the unencoded audio: all zero means "not computed"
    out.insert(out.end(), info.bytes().begin(), info.bytes().end());

    uint64_t frameNumber = 0;
    for (uint32_t first = 0; first < fixture.frames; first += blockFrames)
    {
        const uint32_t count = std::min(blockFrames, fixture.frames - first);

        BitWriter header;
        header.write(0x3FFE, 14); // sync
        header.write(0, 1);       // reserved
        header.write(0, 1);       // fixed block size, so the number below is a frame number
        header.write(0b0111, 4);  // block size: 16 bits, at the end of the header
        header.write(0b0000, 4);  // sample rate: from STREAMINFO
        header.write(static_cast<uint64_t>(fixture.channels) - 1, 4); // independent channels
        header.write(0b000, 3); // sample size: from STREAMINFO
        header.write(0, 1);     // reserved
        writeUtf8Number(header, frameNumber);
        header.write(count - 1, 16);

        BitWriter frame;
        for (std::byte value : header.bytes())
            frame.write(static_cast<uint8_t>(value), 8);
        frame.write(crc8(header.bytes()), 8);

        for (uint16_t channel = 0; channel < fixture.channels; ++channel)
        {
            frame.write(0, 1);         // subframe padding
            frame.write(0b000001, 6);  // VERBATIM
            frame.write(0, 1);         // no wasted bits
            for (uint32_t index = 0; index < count; ++index)
            {
                const size_t sample =
                    (static_cast<size_t>(first) + index) * fixture.channels + channel;
                frame.writeSigned(fixture.samples[sample], fixture.bits);
            }
        }

        frame.alignToByte();
        const uint16_t check = crc16(frame.bytes());

        out.insert(out.end(), frame.bytes().begin(), frame.bytes().end());
        out.push_back(static_cast<std::byte>((check >> 8) & 0xFF));
        out.push_back(static_cast<std::byte>(check & 0xFF));

        ++frameNumber;
    }

    return out;
}

std::vector<int32_t> readSamples(std::span<const std::byte> bytes,
                                 multiroom::AudioFormat::SampleType type)
{
    std::vector<int32_t> values;

    switch (type)
    {
    case multiroom::AudioFormat::SampleType::Int16:
    {
        values.reserve(bytes.size() / 2);
        for (size_t index = 0; index + 1 < bytes.size(); index += 2)
        {
            int16_t value = 0;
            std::memcpy(&value, bytes.data() + index, sizeof(value));
            values.push_back(value);
        }
        break;
    }
    case multiroom::AudioFormat::SampleType::Int24:
    {
        values.reserve(bytes.size() / 3);
        for (size_t index = 0; index + 2 < bytes.size(); index += 3)
        {
            const uint32_t raw = static_cast<uint32_t>(bytes[index]) |
                                 (static_cast<uint32_t>(bytes[index + 1]) << 8) |
                                 (static_cast<uint32_t>(bytes[index + 2]) << 16);
            // Sign-extend from 24 bits.
            values.push_back(static_cast<int32_t>(raw << 8) >> 8);
        }
        break;
    }
    case multiroom::AudioFormat::SampleType::Int24In32:
    case multiroom::AudioFormat::SampleType::Int32:
    {
        values.reserve(bytes.size() / 4);
        for (size_t index = 0; index + 3 < bytes.size(); index += 4)
        {
            int32_t value = 0;
            std::memcpy(&value, bytes.data() + index, sizeof(value));
            values.push_back(value);
        }
        break;
    }
    case multiroom::AudioFormat::SampleType::Float32:
        break; // a float fixture would compare as doubles, not as integers
    }

    return values;
}

} // namespace multiroomtest
