#include "resampler/soxr/SoxrResampler.h"

#include "resampler/RatioText.h"

#include <string>

// The only translation unit in the server that sees a soxr header. soxr.h is
// plain C and warns cleanly, so unlike r8brain it needs no diagnostic fence.
#include <soxr.h>

namespace multiroom {
namespace {

using resampler::ratioText;

// SoXR's io_ratio is input/output. This server's ratio is output/input, and
// MATP section 30 requires an implementation to pick one and document it, so
// the inversion happens here, at the library's own boundary, and nowhere else.
// A sign error here does not sound broken -- it sounds like a correct
// implementation running backwards, with the room drifting away from the
// timeline instead of toward it, slowly.
double ioRatioFrom(double ratio) noexcept
{
    return 1.0 / ratio;
}

} // namespace

const ResamplerCapabilities& SoxrResampler::staticCapabilities() noexcept
{
    static const ResamplerCapabilities caps = [] {
        ResamplerCapabilities c;
        c.id = "soxr";
        c.variableRatio = true;
        c.maxChannels = kMaxChannels;
        c.minRatio = kMinRatio;
        c.maxRatio = kMaxRatio;
        return c;
    }();
    return caps;
}

const ResamplerCapabilities& SoxrResampler::capabilities() const noexcept
{
    return staticCapabilities();
}

SoxrResampler::~SoxrResampler()
{
    destroy();
}

void SoxrResampler::destroy() noexcept
{
    if (handle_ != nullptr)
    {
        soxr_delete(handle_);
        handle_ = nullptr;
    }
}

Status<ResamplerError> SoxrResampler::configure(const ResamplerConfig& config)
{
    if (!config.isValid())
    {
        return fail(ResamplerError::InvalidConfig,
                    std::to_string(config.inputRate) + " -> " + std::to_string(config.outputRate) +
                        " Hz, " + std::to_string(config.channels) + "ch, ratio " +
                        ratioText(config.initialRatio));
    }
    if (config.channels > kMaxChannels)
    {
        return fail(ResamplerError::UnsupportedChannels,
                    std::to_string(config.channels) + " channels, soxr is built for " +
                        std::to_string(kMaxChannels));
    }

    const double ratio = config.effectiveRatio();
    if (!staticCapabilities().supportsRatio(ratio))
    {
        return fail(ResamplerError::UnsupportedRatio,
                    ratioText(ratio) + " outside " + ratioText(kMinRatio) + ".." +
                        ratioText(kMaxRatio));
    }

    // Reconfiguring discards the old converter first, so a failure below
    // leaves an unconfigured object rather than one still running the previous
    // format. Phase 15 re-points an existing output path this way.
    destroy();
    channels_ = 0;
    ratio_ = 0.0;
    variableRatio_ = false;
    configuredRatio_ = 0.0;

    const bool variable = config.requiresVariableRatio;

    // Interleaved double in and out: the working format the roadmap fixes, and
    // the one shape every adapter sees so that the ResamplingProcessor does
    // the conversion once.
    soxr_io_spec_t ioSpec = soxr_io_spec(SOXR_FLOAT64_I, SOXR_FLOAT64_I);

    // The engine, and the whole reason requiresVariableRatio exists. SOXR_VR
    // for a room that will be drift-corrected, nothing for one that will not.
    //
    // The recipe differs with it. SOXR_VHQ is 28-bit precision and worth
    // asking for on the constant-rate engine, which delivers it: 131.2 dB on a
    // 1 kHz tone through 44.1 -> 48 kHz. The variable-rate engine works in
    // single precision and ignores the request -- measured, SOXR_HQ and
    // SOXR_VHQ both give it 115.7 dB -- so asking there would buy a larger
    // filter and not one decibel.
    soxr_quality_spec_t qualitySpec =
        variable ? soxr_quality_spec(SOXR_HQ, SOXR_VR) : soxr_quality_spec(SOXR_VHQ, 0);

    // One thread, explicitly. This runs on a per-client output thread, and a
    // converter that spawns its own pool behind that would multiply the
    // server's thread count by the number of rooms for no benefit -- 20 ms of
    // two-channel audio is not a workload worth splitting.
    soxr_runtime_spec_t runtimeSpec = soxr_runtime_spec(1);

    // A variable-rate converter is created with the largest io_ratio it will
    // ever be given, and designs its filter for that, so the rate pair is the
    // configured ratio trimmed as far down as setRatio() will be allowed to
    // take it. A constant-rate one is created at the ratio it will keep.
    // Either is expressed as a ratio rather than as real rates, which soxr
    // accepts and which keeps the number exact.
    const double createIoRatio = ioRatioFrom(variable ? ratio * (1.0 - kTrimRange) : ratio);

    soxr_error_t error = nullptr;
    soxr_t handle = soxr_create(createIoRatio, 1.0, config.channels, &error, &ioSpec, &qualitySpec,
                                &runtimeSpec);
    if (error != nullptr || handle == nullptr)
    {
        if (handle != nullptr)
            soxr_delete(handle);
        return fail(ResamplerError::LibraryFailure,
                    std::string("soxr_create: ") + soxr_strerror(error));
    }

    // Only the variable-rate engine needs moving off the ratio it was created
    // with; the constant-rate one was created at the right one, and telling it
    // again would be a second way to spell the same thing.
    if (variable)
    {
        error = soxr_set_io_ratio(handle, ioRatioFrom(ratio), 0);
        if (error != nullptr)
        {
            soxr_delete(handle);
            return fail(ResamplerError::LibraryFailure,
                        std::string("soxr_set_io_ratio: ") + soxr_strerror(error));
        }
    }

    handle_ = handle;
    channels_ = config.channels;
    ratio_ = ratio;
    variableRatio_ = variable;
    configuredRatio_ = ratio;
    return {};
}

Status<ResamplerError> SoxrResampler::setRatio(double ratio)
{
    if (!configured())
        return fail(ResamplerError::NotConfigured, "setRatio before configure");

    // The instance, not the implementation. SoXR can vary a ratio and says so
    // in its capabilities, but this one was built with the constant-rate
    // engine because configure() was told it would never be asked to -- and
    // that engine has no mechanism to move. A caller here has broken the
    // promise it made, and phase 6 is where that is a planning defect.
    if (!variableRatio_)
    {
        return fail(ResamplerError::FixedRatioOnly,
                    "configured without requiresVariableRatio, so soxr built the constant-rate "
                    "engine and " +
                        ratioText(ratio) + " cannot replace " + ratioText(ratio_));
    }

    // Two checks, not one, and the second is the interesting one: the window
    // was committed when the filter was designed, so a ratio inside the
    // implementation's declared range can still be outside this instance's.
    // Failing here names both numbers, because "outside 0.031250..32.000000"
    // would be a confusing thing to read about a ratio of 1.000030.
    if (!staticCapabilities().supportsRatio(ratio))
    {
        return fail(ResamplerError::UnsupportedRatio,
                    ratioText(ratio) + " outside " + ratioText(kMinRatio) + ".." +
                        ratioText(kMaxRatio));
    }

    const double lowest = configuredRatio_ * (1.0 - kTrimRange);
    const double highest = configuredRatio_ * (1.0 + kTrimRange);
    if (ratio < lowest || ratio > highest)
    {
        return fail(ResamplerError::UnsupportedRatio,
                    ratioText(ratio) + " outside " + ratioText(lowest) + ".." +
                        ratioText(highest) + ", the trim window fixed by configure(" +
                        ratioText(configuredRatio_) + ")");
    }

    // Slew length zero: the change lands on the next output frame, which is
    // what FakeResampler does and therefore what the interface means. An
    // adapter inventing a transition length would make the same call behave
    // differently depending on which implementation a room happened to get,
    // and at the +/-100 ppm a correction actually moves there is nothing to
    // transition -- the discontinuity a slew exists to hide is a step in
    // playback *speed*, and a hundred parts per million is not one. If a
    // measurement ever asks for a transition, it belongs in the interface as a
    // parameter, not in one adapter's defaults.
    const soxr_error_t error = soxr_set_io_ratio(handle_, ioRatioFrom(ratio), 0);
    if (error != nullptr)
    {
        return fail(ResamplerError::LibraryFailure,
                    std::string("soxr_set_io_ratio: ") + soxr_strerror(error));
    }

    ratio_ = ratio;
    return {};
}

Result<ProcessResult, ResamplerError> SoxrResampler::process(std::span<const double> input,
                                                             std::span<double> output)
{
    if (!configured())
        return fail(ResamplerError::NotConfigured, "process before configure");

    const size_t channels = channels_;
    if (input.size() % channels != 0 || output.size() % channels != 0)
    {
        return fail(ResamplerError::MisalignedBuffer,
                    std::to_string(input.size()) + " in / " + std::to_string(output.size()) +
                        " out samples is not a whole number of " + std::to_string(channels) +
                        "-channel frames");
    }

    // soxr counts in samples per channel, which is this server's frame, so the
    // two agree without a conversion.
    const size_t inputFrames = input.size() / channels;
    const size_t outputFrames = output.size() / channels;

    // A *null* input pointer means end of stream to soxr, not "nothing this
    // call", and an empty span's data() is allowed to be null -- so an empty
    // call would otherwise flush the filter's tail and leave the converter
    // expecting no more audio. Feeding it again after that puts a step in the
    // output, at around -44 dB, which is quiet enough to survive a noise-floor
    // measurement and loud enough to hear. Draining is a real operation and
    // belongs to phase 14's gapless boundary, where it will be asked for
    // rather than inferred.
    const double placeholder = 0.0;
    const double* in = inputFrames > 0 ? input.data() : &placeholder;

    size_t consumed = 0;
    size_t produced = 0;
    const soxr_error_t error = soxr_process(handle_, in, inputFrames, &consumed, output.data(),
                                            outputFrames, &produced);
    if (error != nullptr)
    {
        return fail(ResamplerError::LibraryFailure,
                    std::string("soxr_process: ") + soxr_strerror(error));
    }

    return ProcessResult{consumed, produced};
}

void SoxrResampler::reset() noexcept
{
    if (handle_ == nullptr)
        return;

    soxr_clear(handle_);

    // soxr_clear returns the converter to its *created* state. For the
    // variable-rate engine that state carries the maximum io_ratio rather than
    // the one in use, so without this the stream would silently resume at the
    // edge of the trim window after a seek or a rejoin -- the whole trim
    // budget spent on nothing. Re-applying cannot fail: the same value was
    // accepted a moment ago by the same object. The constant-rate engine was
    // created at the ratio it still has, and needs nothing.
    if (variableRatio_)
        soxr_set_io_ratio(handle_, ioRatioFrom(ratio_), 0);
}

double SoxrResampler::latencyFrames() const noexcept
{
    if (handle_ == nullptr)
        return 0.0;

    // soxr_delay is already in output samples per channel, which is what the
    // interface asks for and what moves with the ratio.
    return soxr_delay(handle_);
}

const ResamplerCapabilities& SoxrResamplerFactory::capabilities() const noexcept
{
    return SoxrResampler::staticCapabilities();
}

std::unique_ptr<IAudioResampler> SoxrResamplerFactory::create() const
{
    return std::make_unique<SoxrResampler>();
}

} // namespace multiroom
