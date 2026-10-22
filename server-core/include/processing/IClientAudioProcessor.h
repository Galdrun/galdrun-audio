#pragma once

// What one room's audio path does to a block between the store and the socket.
//
// Phase 6 decided whether a room's audio may be touched at all and wrote the
// answer down as an OutputPlan. This is where that plan is executed, and it is
// the *only* place in the server that constructs an IAudioResampler: a
// BitPerfect plan builds a DirectPcmProcessor, which cannot resample because
// it holds no converter, rather than a converter set to a ratio of one.
// Bitperfect is not a ratio of one; it is no resampler at all.
//
// The shape is IAudioResampler's, one layer up and in bytes rather than
// doubles: process() consumes what it can of an input span in the zone's
// source format and fills what it can of an output span in this room's own
// format, and reports both counts in frames. Nothing may assume
// out == in * ratio for a single call. The unconsumed input frames stay the
// caller's to re-present, which is what keeps a buffer out of every
// implementation of this interface, exactly as it does one layer down.
//
// **The processor owns the ratio but never decides it.** setRatio() comes from
// the ClientSyncController of phase 12 and is passed to the converter with
// whatever transition the converter allows. A processor that worked out its
// own ratio would put two controllers on one loop, and phase 13's whole
// argument is that there is one.
//
// Threading: one processor belongs to one client output thread and is touched
// by nothing else, which is why nothing here is synchronized. setRatio()
// arriving from the sync controller reaches it through that thread.

#include "audio/AudioFormat.h"
#include "client/OutputPlan.h"
#include "core/Error.h"
#include "processing/Equalizer.h"
#include "resampler/IAudioResampler.h"
#include "resampler/ResamplerRegistry.h"

#include <cstddef>
#include <memory>
#include <span>
#include <string_view>

namespace multiroom {

enum class ProcessorError
{
    // The plan cannot be executed as written: an invalid format, a mode that
    // does not match the path asked for, a channel count the direct path
    // would have to change, or a resampler id this build does not have.
    // Everything here is a plan the OutputPlanner could not have produced.
    InvalidPlan,

    // The direct path was asked for a layout pair that does not preserve the
    // sample values -- a narrowing, or a crossing between integer and float.
    // isValuePreserving is the one definition, and the planner works from it
    // too, so this is a hand-built plan rather than a room.
    UnsupportedConversion,

    // A span whose length is not a whole number of frames of its own format.
    MisalignedBuffer,

    // setRatio() on a room that is not drift corrected. The plan's drift flag
    // is what built the converter -- SoXR's variable-rate engine for a room
    // that is corrected and its quieter constant-rate one for a room that is
    // not -- so a ratio arriving at a room that was never planned for one is a
    // controller talking to the wrong output, not a value to accept.
    RatioNotAdjustable,

    // The converter refused something, carrying its own failure in the detail.
    ResamplerFailed
};

const char* errorName(ProcessorError error);

// Whether the conversion to a narrower output layout is dithered.
//
// Off by default and off unless a deployment asks: the case that needs it -- a
// 24-bit source into a 16-bit room -- is rarer than the case where a
// switched-on dither is an unexplained noise floor. It applies to the
// resampled path only, because the direct path performs no quantization at
// all: a layout change has nothing to round.
//
// **And only to a room below 24 valid bits** (phase 31). A 24-bit, 32-bit or
// float room's rounding is below anything audible either way, so dithering it
// would add noise and change bytes for nothing; such a room is sent the same
// bytes whatever this says.
enum class DitherMode
{
    No,
    TriangularPdf
};

// server.json's spelling of each mode: "off" and "tpdf".
const char* ditherModeName(DitherMode mode);
[[nodiscard]] bool parseDitherMode(std::string_view text, DitherMode& out);

class IClientAudioProcessor
{
public:
    virtual ~IClientAudioProcessor() = default;

    // What this processor was built from. Its two formats are the frame sizes
    // process() reads and writes, and its describePlan() is the line the
    // status API reports for this room.
    [[nodiscard]] virtual const OutputPlan& plan() const noexcept = 0;

    // The current total output/input ratio -- one for a room that is not
    // resampled, and one for a resampled room before its first correction.
    [[nodiscard]] virtual double ratio() const noexcept = 0;

    // Phase 12's drift trim. Refused with RatioNotAdjustable by every
    // processor whose plan is not drift corrected.
    virtual Status<ProcessorError> setRatio(double ratio) = 0;

    // Consumes what it can of `input`, fills what it can of `output`, and
    // reports both in frames. The caller re-presents the input frames it was
    // not told were consumed.
    virtual Result<ProcessResult, ProcessorError> process(std::span<const std::byte> input,
                                                          std::span<std::byte> output) = 0;

    // Back to where the last configuration left it, for a seek or a rejoin.
    // The plan and the ratio survive; the filter history does not.
    virtual void reset() = 0;

    // In *output* frames and a double, for the same reason IAudioResampler
    // reports it that way: phase 11 places a scheduled start with it, and a
    // resampled room's latency moves with its ratio.
    [[nodiscard]] virtual double latencyFrames() const noexcept = 0;

    // The room's equalizer, owned by its ClientOutput and kept across
    // converters; null for none. Only the resampled path runs it. A bitperfect
    // path keeps this default and ignores it, which is the whole of "the
    // equalizer is never applied to a bitperfect stream".
    virtual void setEqualizer(Equalizer* /*equalizer*/) noexcept {}

protected:
    IClientAudioProcessor() = default;
    IClientAudioProcessor(const IClientAudioProcessor&) = delete;
    IClientAudioProcessor& operator=(const IClientAudioProcessor&) = delete;
};

// The one place a plan becomes a path, and the one place a converter is built.
//
// A BitPerfect plan never reaches the registry -- that is phase 6's criterion 5
// made real rather than asserted about a planner that could not have
// constructed anything anyway.
[[nodiscard]] Result<std::unique_ptr<IClientAudioProcessor>, ProcessorError>
makeClientAudioProcessor(const OutputPlan& plan, const ResamplerRegistry& registry,
                         DitherMode dither = DitherMode::No);

} // namespace multiroom
