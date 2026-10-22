#pragma once

// The three axes this server moves between, and the exact arithmetic that
// connects them.
//
// The client is C++17 and keeps the axes apart by naming convention; its
// documentation records that as its single most expensive class of bug, because a
// timeline nanosecond and a stream frame are both uint64_t and the compiler
// cannot tell them apart. This server is C++23 and does not have to live with
// it. Opaque enums are trivially copyable, admit no accidental arithmetic and
// cost nothing.
//
// Values are read out with std::to_underlying, which is deliberately more
// visible than a cast. Arithmetic on the axes belongs to the two mapping
// components (timeline/SourceTimeline and client/OutputClock); a conversion
// anywhere else is a defect, not a shortcut.

#include "core/Error.h"

#include <cstdint>
#include <utility>

namespace multiroom {

enum class TimelineNs : uint64_t
{
}; // the zone's axis, shared by every room
enum class SourceFrame : uint64_t
{
}; // a frame of the decoded source
enum class OutputFrame : uint64_t
{
}; // a frame of one client's own stream

inline constexpr uint64_t kNanosecondsPerSecond = 1'000'000'000ULL;

enum class FrameMathError
{
    InvalidSampleRate, // zero; a format that was never validated
    Overflow           // beyond what 64 bits can express on the other axis
};

const char* errorName(FrameMathError error);

// Frames spanned by a duration, rounded down: the frame a position falls in.
//
// Absolute, never accumulated. The client derives the timeline from the two
// nanosecond fields of every packet and reads a sender that adds up rounded
// durations as discontinuous, which costs it an endless series of rejoins.
// Every caller therefore converts a position from the origin, not a delta from
// the last one.
[[nodiscard]] Result<uint64_t, FrameMathError> framesFromNanoseconds(uint64_t nanoseconds,
                                                                     uint32_t sampleRate);

// Duration spanned by a frame count, rounded *up*.
//
// Rounding up is not a taste: it is what makes the pair exact in one
// direction. At 44.1 kHz a frame is 22675.7... ns, so no rounding can be
// lossless both ways, and rounding to nearest breaks the round trip outright
// (frame 2 is 45351.47 ns, which rounds down to 45351 and converts back to
// frame 1). Rounding up keeps
//
//     framesFromNanoseconds(nanosecondsFromFrames(f, rate), rate) == f
//
// for every frame and every rate, because the overshoot is under one
// nanosecond and a nanosecond is less than a frame at any rate this server
// supports. The other direction collapses by definition: many nanoseconds
// share a frame.
[[nodiscard]] Result<uint64_t, FrameMathError> nanosecondsFromFrames(uint64_t frames,
                                                                     uint32_t sampleRate);

} // namespace multiroom
