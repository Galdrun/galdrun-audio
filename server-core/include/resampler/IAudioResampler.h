#pragma once

// The only resampler the rest of the server ever sees.
//
// Adding a third implementation must touch nothing outside its own directory
// (the roadmap's criterion 10), so everything that differs between libsoxr,
// r8brain and anything after them is expressed here as a capability rather
// than as a branch somewhere up the call chain.
//
// The working format is interleaved double, chosen in the roadmap's phase 3:
// CPU is not the constraint and a 24-bit source has more resolution than a
// float mantissa carries once a filter has summed a few hundred taps.
// Conversion into and out of double is the ResamplingProcessor's job (phase
// 7), so every adapter sees the same input type and none of them repeats it.
//
// A capability an implementation does not have is refused at configure(),
// never mid-stream. The protocol is explicit that there is no silent fallback
// (MATP section 28), and a drift-corrected room discovering at its first
// correction that its converter cannot vary its ratio is a fault that shows up
// hours after the configuration that caused it.

#include "core/Error.h"

#include <cstdint>
#include <span>
#include <string>

namespace multiroom {

enum class ResamplerError
{
    UnknownId,           // no factory is registered under that id
    DuplicateId,         // two factories claiming the same id
    InvalidConfig,       // zero rate, zero channels, or a ratio that is not positive
    UnsupportedChannels, // more channels than the implementation handles
    UnsupportedRatio,    // outside the implementation's declared range
    FixedRatioOnly,      // setRatio() on an implementation that takes its ratio once
    NotConfigured,       // process() before configure()
    MisalignedBuffer,    // a span whose length is not a whole number of frames

    // A resampling library refused something this adapter had already checked.
    // It carries the library's own message rather than a code of ours,
    // because there is nothing useful to map it onto: every condition libsoxr
    // and r8brain document as refusable is ruled out before they are called,
    // so reaching this means the library disagrees with its own
    // documentation, and the only honest thing to report is what it said.
    LibraryFailure
};

const char* errorName(ResamplerError error);

// What an implementation can actually do. Reported honestly: a value that is
// not known or not offered is absent, never optimistic.
struct ResamplerCapabilities
{
    // Stable, and the only place in the server where a resampler is named:
    // "fake", "soxr", "r8brain". Config files name them and status reports
    // them, so renaming one is a breaking change to a deployment.
    std::string id;

    // Every implementation takes a ratio at configure(). Only some can change
    // it afterwards, and drift correction (phase 13) is exactly the case that
    // needs to: r8brain takes its ratio at construction and reports false
    // here, so a configuration naming it as the drift resampler is refused at
    // startup rather than at the moment a room first needs correcting.
    bool variableRatio = false;

    uint16_t maxChannels = 0;

    // Inclusive bounds on output/input. A converter asked for more than it can
    // express fails at configure() with the number in the message.
    double minRatio = 0.0;
    double maxRatio = 0.0;

    [[nodiscard]] bool supportsRatio(double ratio) const noexcept;
};

// "fake (variable ratio, <=8ch, 0.03125..32)"
std::string describeCapabilities(const ResamplerCapabilities& capabilities);

struct ResamplerConfig
{
    uint32_t inputRate = 0;
    uint32_t outputRate = 0;
    uint16_t channels = 0;

    // Total output/input ratio to start with, drift trim included. Zero means
    // the nominal rate ratio, which is every client that is not being
    // drift-corrected -- most of them, most of the time.
    //
    // It is a total and not a trim because that is the number MATP section 28
    // defines and the number a converter is given: a 44.1 kHz source into a
    // 48 kHz room whose card runs +30 ppm fast is one ratio of 1.088468, not a
    // rate pair and a correction to be recombined by whoever consumes it.
    double initialRatio = 0.0;

    // Whether this client's ratio will move -- phase 6's OutputPlan drift
    // flag, handed straight through. It is the *caller's* requirement, where
    // ResamplerCapabilities::variableRatio is the implementation's ability,
    // and the two are separate questions: a converter may be able to vary its
    // ratio and still be better at a fixed one.
    //
    // SoXR is exactly that case. Its variable-rate engine works in single
    // precision and its constant-rate engine does not, which is 15.5 dB of
    // noise floor measured on one 44.1 -> 48 kHz tone, so a room that is not
    // being drift-corrected has no reason to pay for a correction it will
    // never receive. Leaving the choice to the implementation to guess -- from
    // whether initialRatio carried a trim, say -- would mean a room silently
    // changing engine the first time a planner passed one, so it is asked for
    // here instead.
    //
    // False is the promise that setRatio() will not be called, and an
    // implementation is free to configure itself so that it cannot honour one.
    // True is refused at configure() by an implementation whose capabilities
    // say it cannot vary a ratio at all, which is what stops a configuration
    // naming r8brain as the drift resampler from failing hours later, at the
    // moment a room first needs correcting.
    bool requiresVariableRatio = false;

    [[nodiscard]] bool isValid() const noexcept;

    // outputRate / inputRate, or zero if either is zero.
    [[nodiscard]] double nominalRatio() const noexcept;

    // What configure() will actually use.
    [[nodiscard]] double effectiveRatio() const noexcept;
};

// What one process() call did. Both counts are in frames, never samples.
//
// process() is push-pull and not one-in-one-out: a resampler holds state and
// returns however many frames it has. Nothing in the pipeline may assume
// out == in * ratio for a single call -- that assumption is how a stream
// develops a hole every few seconds.
struct ProcessResult
{
    size_t inputFramesConsumed = 0;
    size_t outputFramesProduced = 0;
};

class IAudioResampler
{
public:
    virtual ~IAudioResampler() = default;

    [[nodiscard]] virtual const ResamplerCapabilities& capabilities() const noexcept = 0;

    // Fixes the rates, the channel count and the starting ratio, and discards
    // whatever state a previous configuration left. Calling it again is how a
    // format transition (phase 15) re-points an existing output path.
    virtual Status<ResamplerError> configure(const ResamplerConfig& config) = 0;

    [[nodiscard]] virtual bool configured() const noexcept = 0;

    // The current total output/input ratio, zero before configure().
    [[nodiscard]] virtual double ratio() const noexcept = 0;

    // The drift path. Refused with FixedRatioOnly by an implementation whose
    // capabilities say it cannot vary a ratio, and by any instance configured
    // with requiresVariableRatio false -- which asked to be built for a fixed
    // ratio and may have been. Either way the failure lands on a configuration
    // rather than on the music.
    virtual Status<ResamplerError> setRatio(double ratio) = 0;

    // Consumes what it can of `input` and fills what it can of `output`, both
    // interleaved and both a whole number of frames. Neither being fully used
    // is normal: the caller re-presents the unconsumed input frames on the
    // next call, exactly as it would to libsamplerate or soxr.
    virtual Result<ProcessResult, ResamplerError> process(std::span<const double> input,
                                                          std::span<double> output) = 0;

    // Back to the state configure() left, for a seek or a rejoin. The
    // configuration survives; the filter history does not.
    virtual void reset() noexcept = 0;

    // In *output* frames, and a double on purpose: it moves with the ratio,
    // and phase 11 needs it to place a scheduled start correctly for a
    // resampled room. Rounding it to an integer in the interface would throw
    // away the part that matters.
    [[nodiscard]] virtual double latencyFrames() const noexcept = 0;

protected:
    IAudioResampler() = default;
    IAudioResampler(const IAudioResampler&) = delete;
    IAudioResampler& operator=(const IAudioResampler&) = delete;
};

} // namespace multiroom
