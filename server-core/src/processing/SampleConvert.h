#pragma once

// Turning one byte layout into another, and into and out of the working
// double -- the sample arithmetic both processors are built from.
//
// It lives beside its source rather than in include/ because nothing above the
// processing layer may touch a sample: the direct path is permitted a layout
// change, the resampled path converts at each end, and no other component in
// this server reads a PCM byte at all.
//
// **Every layout is read and written byte by byte, little endian.** MATP's
// names say so -- s16le, s24le3, s24le, s32le, f32le -- and a memcpy of an
// int32_t says it only on the machines this has been built on. The cost is a
// few shifts per sample and the benefit is that the one place where getting an
// alignment wrong costs 48 dB rather than producing obvious noise does not
// depend on the host's byte order.
//
// **Int24In32's top byte is written sign-extended and read as padding.** The
// FFmpeg decoder already produces it that way -- it shifts a left-aligned S32
// right by eight, arithmetically -- so Int24In32 to Int24In32 stays a memcpy,
// and the client masks the byte off before it sign-extends for itself, so it
// reads a 0xFF fill and a 0x00 pad identically. Reading it back the same way
// means a source of either convention arrives correctly.
//
// **The double domain is normalized to +-1.0**, each layout scaled by its own
// 2^(validBits-1). That is what makes a depth change through the resampled
// path preserve the *level* rather than the integer, exactly as the direct
// path's shift does: a full-scale 16-bit sample reaches a 32-bit room at full
// scale and not 48 dB down.

#include "audio/AudioFormat.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace multiroom::sample_convert {

// Triangular-PDF dither, one LSB peak either way, added before the rounding
// that fromDouble() does.
//
// It is off by default (see IClientAudioProcessor.h): the case that needs it
// -- a 24-bit source into a 16-bit room -- is rarer than the case where a
// switched-on dither is an unexplained noise floor. The generator is a fixed
// seed and a plain xorshift rather than <random>, because a room's dither must
// be reproducible for a test to assert anything about it at all, and because
// nothing here needs a distribution better than two uniforms added.
class TriangularDither
{
public:
    // In LSB units, triangular over (-1, +1).
    [[nodiscard]] double next() noexcept;

private:
    [[nodiscard]] uint32_t nextBits() noexcept;

    uint64_t state_ = 0x9E3779B97F4A7C15ull;
};

// The same sample values in another integer layout: the direct path's only
// permitted operation, and an exact one.
//
// The caller has already established that the pair is value preserving
// (isValuePreserving in client/OutputPlan.h, which is the one definition the
// planner uses too), so a narrowing pair or anything involving Float32 is a
// defect here rather than a case. Identical layouts are a memcpy.
void repack(std::span<const std::byte> input, AudioFormat::SampleType from,
            std::span<std::byte> output, AudioFormat::SampleType to, size_t samples);

// output.size() samples, read from input at the source layout's width.
void toDouble(std::span<const std::byte> input, AudioFormat::SampleType from,
              std::span<double> output);

// input.size() samples, written at the target layout's width. Integer targets
// round to nearest and clamp; a Float32 target takes the value as it stands,
// because there is nothing to round and an excursion a converter produced is
// signal rather than an error to be hidden.
//
// `dither` is null when dithering is off, which is the default.
void fromDouble(std::span<const double> input, std::span<std::byte> output,
                AudioFormat::SampleType to, TriangularDither* dither);

} // namespace multiroom::sample_convert
