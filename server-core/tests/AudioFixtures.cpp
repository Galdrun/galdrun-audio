#include "AudioFixtures.h"

#include <algorithm>
#include <cstring>
#include <initializer_list>
#include <iterator>

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

namespace {

// A FLAC stream's parts: its STREAMINFO metadata block (header included) and
// its frames, each whole, so a container other than FLAC's own can carry them.
struct FlacParts
{
    std::vector<std::byte> streamInfo;
    std::vector<std::vector<std::byte>> frames;
    std::vector<uint32_t> frameCounts;
};

FlacParts flacParts(const PcmFixture& fixture, uint32_t blockFrames)
{
    // STREAMINFO's minimum has to account for a short final block, which is
    // the normal shape of a stream whose length is not a multiple of one.
    uint32_t minBlock = blockFrames;
    if (fixture.frames < blockFrames)
        minBlock = fixture.frames;
    else if (fixture.frames % blockFrames != 0)
        minBlock = fixture.frames % blockFrames;

    FlacParts parts;

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
    parts.streamInfo = info.bytes();

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

        std::vector<std::byte> bytes = frame.bytes();
        bytes.push_back(static_cast<std::byte>((check >> 8) & 0xFF));
        bytes.push_back(static_cast<std::byte>(check & 0xFF));
        parts.frames.push_back(std::move(bytes));
        parts.frameCounts.push_back(count);

        ++frameNumber;
    }

    return parts;
}

// MP4 is big-endian throughout.
void putBe(std::vector<std::byte>& out, uint64_t value, int bytes)
{
    for (int index = bytes - 1; index >= 0; --index)
        out.push_back(static_cast<std::byte>((value >> (8 * index)) & 0xFF));
}

std::vector<std::byte> box(const char (&tag)[5], const std::vector<std::byte>& content)
{
    std::vector<std::byte> out;
    putBe(out, 8 + content.size(), 4);
    putTag(out, tag);
    out.insert(out.end(), content.begin(), content.end());
    return out;
}

// A full box's version and flags, then its fields.
std::vector<std::byte> fullBox(const char (&tag)[5], uint8_t version, uint32_t flags,
                               const std::vector<std::byte>& fields)
{
    std::vector<std::byte> content;
    putBe(content, (static_cast<uint32_t>(version) << 24) | flags, 4);
    content.insert(content.end(), fields.begin(), fields.end());
    return box(tag, content);
}

std::vector<std::byte> join(std::initializer_list<std::vector<std::byte>> parts)
{
    std::vector<std::byte> out;
    for (const auto& part : parts)
        out.insert(out.end(), part.begin(), part.end());
    return out;
}

void putMatrix(std::vector<std::byte>& out)
{
    for (uint32_t value : {0x00010000u, 0u, 0u, 0u, 0x00010000u, 0u, 0u, 0u, 0x40000000u})
        putBe(out, value, 4);
}

} // namespace

std::vector<std::byte> encodeFlac(const PcmFixture& fixture, uint32_t blockFrames)
{
    const FlacParts parts = flacParts(fixture, blockFrames);
    std::vector<std::byte> out;
    putTag(out, "fLaC");
    out.insert(out.end(), parts.streamInfo.begin(), parts.streamInfo.end());
    for (const auto& frame : parts.frames)
        out.insert(out.end(), frame.begin(), frame.end());
    return out;
}

std::vector<std::byte> encodeFragmentedMp4Flac(const PcmFixture& fixture, uint32_t blockFrames,
                                               uint32_t blocksPerFragment)
{
    const FlacParts parts = flacParts(fixture, blockFrames);
    const uint32_t rate = fixture.sampleRate;

    std::vector<std::byte> mvhd;
    putBe(mvhd, 0, 8);          // creation, modification
    putBe(mvhd, rate, 4);       // timescale
    putBe(mvhd, 0, 4);          // duration: unknown, as a fragmented file has it
    putBe(mvhd, 0x00010000, 4); // rate 1.0
    putBe(mvhd, 0x0100, 2);     // volume 1.0
    putBe(mvhd, 0, 10);         // reserved
    putMatrix(mvhd);
    putBe(mvhd, 0, 24);         // pre-defined
    putBe(mvhd, 2, 4);          // next track id

    std::vector<std::byte> tkhd;
    putBe(tkhd, 0, 8);      // creation, modification
    putBe(tkhd, 1, 4);      // track id
    putBe(tkhd, 0, 4);      // reserved
    putBe(tkhd, 0, 4);      // duration
    putBe(tkhd, 0, 8);      // reserved
    putBe(tkhd, 0, 4);      // layer, alternate group
    putBe(tkhd, 0x0100, 2); // volume
    putBe(tkhd, 0, 2);      // reserved
    putMatrix(tkhd);
    putBe(tkhd, 0, 8);      // width, height

    std::vector<std::byte> mdhd;
    putBe(mdhd, 0, 8);      // creation, modification
    putBe(mdhd, rate, 4);   // timescale
    putBe(mdhd, 0, 4);      // duration: 0, as Tidal's are
    putBe(mdhd, 0x55C4, 2); // language "und"
    putBe(mdhd, 0, 2);

    std::vector<std::byte> hdlr;
    putBe(hdlr, 0, 4);
    putTag(hdlr, "soun");
    putBe(hdlr, 0, 12);
    putBe(hdlr, 0, 1); // empty name

    std::vector<std::byte> dref;
    putBe(dref, 1, 4);
    const auto dataRef = join({dref, fullBox("url ", 0, 1, {})});

    // ISO/IEC 14496-12 AudioSampleEntry "fLaC", carrying FLAC's own
    // STREAMINFO in a dfLa box (the FLAC-in-ISOBMFF mapping).
    std::vector<std::byte> entry;
    putBe(entry, 0, 6);                 // reserved
    putBe(entry, 1, 2);                 // data reference index
    putBe(entry, 0, 8);                 // reserved
    putBe(entry, fixture.channels, 2);
    putBe(entry, fixture.bits, 2);
    putBe(entry, 0, 4);                 // pre-defined, reserved
    putBe(entry, static_cast<uint64_t>(rate) << 16, 4);
    const auto sampleEntry = box("fLaC", join({entry, fullBox("dfLa", 0, 0, parts.streamInfo)}));
    std::vector<std::byte> stsd;
    putBe(stsd, 1, 4);

    std::vector<std::byte> none; // an empty table: every sample is in a fragment
    putBe(none, 0, 4);
    std::vector<std::byte> stsz;
    putBe(stsz, 0, 8);

    const auto stbl = box("stbl", join({fullBox("stsd", 0, 0, join({stsd, sampleEntry})),
                                        fullBox("stts", 0, 0, none), fullBox("stsc", 0, 0, none),
                                        fullBox("stsz", 0, 0, stsz), fullBox("stco", 0, 0, none)}));
    std::vector<std::byte> smhd;
    putBe(smhd, 0, 4);
    const auto minf = box("minf", join({fullBox("smhd", 0, 0, smhd),
                                        box("dinf", fullBox("dref", 0, 0, dataRef)), stbl}));
    const auto trak = box("trak", join({fullBox("tkhd", 0, 3, tkhd),
                                        box("mdia", join({fullBox("mdhd", 0, 0, mdhd),
                                                          fullBox("hdlr", 0, 0, hdlr), minf}))}));
    std::vector<std::byte> trex;
    putBe(trex, 1, 4); // track id
    putBe(trex, 1, 4); // sample description index
    putBe(trex, 0, 12);

    std::vector<std::byte> ftyp;
    putTag(ftyp, "iso6");
    putBe(ftyp, 0, 4);
    putTag(ftyp, "iso6");
    putTag(ftyp, "mp41");

    std::vector<std::byte> out = join({box("ftyp", ftyp),
                                       box("moov", join({fullBox("mvhd", 0, 0, mvhd), trak,
                                                         box("mvex", fullBox("trex", 0, 0, trex))}))});

    // One moof and mdat per fragment, no sidx and no mfra: the demuxer can
    // only learn where fragments are by reading them.
    uint64_t decodeTime = 0;
    uint32_t sequence = 1;
    for (size_t first = 0; first < parts.frames.size(); first += blocksPerFragment)
    {
        const size_t last = std::min(parts.frames.size(), first + blocksPerFragment);

        std::vector<std::byte> mdat;
        std::vector<std::byte> samples;
        for (size_t index = first; index < last; ++index)
        {
            putBe(samples, parts.frameCounts[index], 4);
            putBe(samples, parts.frames[index].size(), 4);
            mdat.insert(mdat.end(), parts.frames[index].begin(), parts.frames[index].end());
        }

        std::vector<std::byte> mfhd;
        putBe(mfhd, sequence++, 4);
        std::vector<std::byte> tfhd;
        putBe(tfhd, 1, 4);
        std::vector<std::byte> tfdt;
        putBe(tfdt, decodeTime, 8);

        // trun's data offset counts from the moof, whose size depends on the
        // trun: built once with 0 to measure, then again with the offset.
        const auto moofWith = [&](uint32_t dataOffset) {
            std::vector<std::byte> trun;
            putBe(trun, last - first, 4);
            putBe(trun, dataOffset, 4);
            trun.insert(trun.end(), samples.begin(), samples.end());
            return box("moof", join({fullBox("mfhd", 0, 0, mfhd),
                                     box("traf", join({fullBox("tfhd", 0, 0x020000, tfhd),
                                                       fullBox("tfdt", 1, 0, tfdt),
                                                       fullBox("trun", 0, 0x000301, trun)}))}));
        };
        const auto moof = moofWith(static_cast<uint32_t>(moofWith(0).size() + 8));
        out.insert(out.end(), moof.begin(), moof.end());
        const auto data = box("mdat", mdat);
        out.insert(out.end(), data.begin(), data.end());

        for (size_t index = first; index < last; ++index)
            decodeTime += parts.frameCounts[index];
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
