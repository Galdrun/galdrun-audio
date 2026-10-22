#include "TestSupport.h"

#include "resampler/BuiltinResamplers.h"
#include "resampler/IAudioResampler.h"
#include "resampler/ResamplerRegistry.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <span>
#include <string>
#include <vector>

using namespace multiroom;

// One test body, run through IAudioResampler against every real converter this
// build contains, with the implementations' declared capabilities deciding
// which cases apply. The point is not that soxr and r8brain behave alike --
// they do not, and the cases below say where -- but that everything above this
// layer can rely on the same properties whichever one a room was given.
//
// FakeResampler is not among the subjects. Its output is linear interpolation,
// whose stopband is tens of decibels rather than a hundred, so it would fail
// the case that measures audio quality rather than accounting;
// test_resampler.cpp tests the *contract* against it, and that is the division
// of labour. With both build options off there are no subjects, this file
// registers no tests at all, and the roadmap's rule that such a build still
// passes everything holds trivially -- which is why all four configurations
// are built before the phase is called done, and not only the default one.

#if MULTIROOM_WITH_SOXR || MULTIROOM_WITH_R8BRAIN

namespace {

constexpr double kPi = 3.14159265358979323846;

// 44.1 -> 48 kHz: what every 44.1 kHz album played into a 48 kHz-only room
// needs, and the conversion the roadmap names for this phase.
constexpr uint32_t kInputRate = 44100;
constexpr uint32_t kOutputRate = 48000;
constexpr double kToneHz = 1000.0;
constexpr double kToneAmplitude = 0.5;

// 4800 output frames at 48 kHz is 0.1 s, which is exactly 100 periods of a
// 1 kHz tone and puts it on bin 100. Bin-centred and a whole number of periods
// means a rectangular window leaks nothing, so no window function is needed
// and what is left outside the bin is the converter's noise rather than the
// analysis's.
constexpr size_t kWindowFrames = 4800;
constexpr size_t kToneBin = 100;

// Half a second of output discarded before the analysis window, which is two
// orders of magnitude past any of these filters' settling.
constexpr size_t kSettleFrames = 24000;

// Where a sample-to-sample scan starts: past the opening transient, and early
// enough that a seam half a second into the stream is inside what is scanned.
constexpr size_t kScanFromFrames = 2000;

// How far above its reported latency a converter may be holding output. It is
// one internal block: soxr's default log2_min_dft_size is 10, so 1024 frames,
// and this leaves headroom over the 926 measured. It is a bound on a quantity
// that must not grow, not an estimate of one.
constexpr double kMaxHeldFrames = 2048.0;

// The ids this build registered, in the order the registry reports them.
std::vector<std::string> registeredIds()
{
    ResamplerRegistry registry;
    CHECK_OK(registerBuiltinResamplers(registry));
    return registry.ids();
}

std::unique_ptr<IAudioResampler> make(const std::string& id)
{
    ResamplerRegistry registry;
    CHECK_OK(registerBuiltinResamplers(registry));
    auto created = registry.create(id);
    CHECK_OK(created);
    return std::move(*created);
}

ResamplerCapabilities capabilitiesOf(const std::string& id)
{
    ResamplerRegistry registry;
    CHECK_OK(registerBuiltinResamplers(registry));
    auto caps = registry.capabilities(id);
    CHECK_OK(caps);
    return *caps;
}

// One implementation configured one way. An implementation that can vary its
// ratio appears twice, because that is two converters as far as every case
// below is concerned: soxr builds a different engine for each, and a room that
// is not being drift-corrected gets the quieter one. Everything that measures
// audio or counts frames runs against all of them.
struct Subject
{
    std::string id;
    bool variable = false;
};

std::vector<Subject> subjects()
{
    std::vector<Subject> out;
    for (const std::string& id : registeredIds())
    {
        out.push_back(Subject{id, false});
        if (capabilitiesOf(id).variableRatio)
            out.push_back(Subject{id, true});
    }
    return out;
}

ResamplerConfig config(uint32_t inputRate, uint32_t outputRate, uint16_t channels,
                       double initialRatio = 0.0, bool requiresVariableRatio = false)
{
    ResamplerConfig cfg;
    cfg.inputRate = inputRate;
    cfg.outputRate = outputRate;
    cfg.channels = channels;
    cfg.initialRatio = initialRatio;
    cfg.requiresVariableRatio = requiresVariableRatio;
    return cfg;
}

// The configuration a subject is defined by, for the cases that only differ in
// which engine they are pointed at.
ResamplerConfig configFor(const Subject& subject, uint16_t channels, double initialRatio = 0.0)
{
    return config(kInputRate, kOutputRate, channels, initialRatio, subject.variable);
}

// Every channel gets its own frequency, so a converter that mixes them shows
// up as a tone in the wrong channel rather than as a subtle level error.
std::vector<double> tones(size_t frames, uint16_t channels, double baseFrequency,
                          double sampleRate, double amplitude)
{
    std::vector<double> out(frames * channels);
    for (uint16_t channel = 0; channel < channels; ++channel)
    {
        const double frequency = baseFrequency * static_cast<double>(channel + 1);
        for (size_t frame = 0; frame < frames; ++frame)
        {
            out[frame * channels + channel] =
                amplitude *
                std::sin(2.0 * kPi * frequency * static_cast<double>(frame) / sampleRate);
        }
    }
    return out;
}

// What one call did, accumulated over a whole stream.
struct StreamRun
{
    std::vector<double> output;
    size_t inputFramesConsumed = 0;
    size_t outputFramesProduced = 0;
};

// Pushes an entire input buffer through in calls of at most inChunk input and
// outChunk output frames, collecting everything produced and checking the
// per-call contract as it goes. Stops on the first call that consumes nothing
// and produces nothing, which is how a push-pull converter says it wants input
// it has not been given.
StreamRun runStream(IAudioResampler& resampler, const std::vector<double>& input,
                    uint16_t channels, size_t inChunk, size_t outChunk)
{
    const size_t inputFrames = input.size() / channels;
    std::vector<double> block(outChunk * channels);
    StreamRun run;
    size_t offset = 0;

    for (;;)
    {
        const size_t available = std::min(inChunk, inputFrames - offset);
        const std::span<const double> in(input.data() + offset * channels, available * channels);
        auto result = resampler.process(in, std::span<double>(block));
        CHECK_OK(result);

        // Neither count may exceed what the call was offered. A converter that
        // overruns the output span corrupts whatever is next to it, and one
        // that claims more input than it was given loses audio silently.
        CHECK(result->inputFramesConsumed <= available);
        CHECK(result->outputFramesProduced <= outChunk);

        offset += result->inputFramesConsumed;
        run.inputFramesConsumed += result->inputFramesConsumed;
        run.outputFramesProduced += result->outputFramesProduced;
        run.output.insert(run.output.end(), block.begin(),
                          block.begin() + static_cast<std::ptrdiff_t>(
                                              result->outputFramesProduced * channels));

        if (result->inputFramesConsumed == 0 && result->outputFramesProduced == 0)
            break;
    }

    CHECK_EQ(run.output.size(), run.outputFramesProduced * channels);
    return run;
}

// Goertzel: one DFT bin's magnitude, without transforming the whole block. A
// bin test catches a converter that is broken, not one that is merely
// mediocre, and the roadmap is explicit that the quality comparison belongs to
// phase 11 and a real room.
double binMagnitude(const std::vector<double>& signal, size_t offsetFrames, uint16_t channels,
                    uint16_t channel, size_t frames, size_t bin)
{
    const double omega = 2.0 * kPi * static_cast<double>(bin) / static_cast<double>(frames);
    const double cosine = std::cos(omega);
    const double sine = std::sin(omega);
    const double coefficient = 2.0 * cosine;

    double previous = 0.0;
    double older = 0.0;
    for (size_t frame = 0; frame < frames; ++frame)
    {
        const double current =
            signal[(offsetFrames + frame) * channels + channel] + coefficient * previous - older;
        older = previous;
        previous = current;
    }

    const double real = previous - older * cosine;
    const double imaginary = older * sine;
    return std::sqrt(real * real + imaginary * imaginary);
}

// The amplitude of a real sine sitting exactly on `bin`.
double binAmplitude(const std::vector<double>& signal, size_t offsetFrames, uint16_t channels,
                    uint16_t channel, size_t frames, size_t bin)
{
    return 2.0 * binMagnitude(signal, offsetFrames, channels, channel, frames, bin) /
           static_cast<double>(frames);
}

// Everything in the window that is not the tone, in decibels below it.
double signalToNoiseDb(const std::vector<double>& signal, size_t offsetFrames, uint16_t channels,
                       uint16_t channel, size_t frames, size_t bin)
{
    double total = 0.0;
    for (size_t frame = 0; frame < frames; ++frame)
    {
        const double value = signal[(offsetFrames + frame) * channels + channel];
        total += value * value;
    }
    total /= static_cast<double>(frames);

    const double amplitude = binAmplitude(signal, offsetFrames, channels, channel, frames, bin);
    const double tone = amplitude * amplitude / 2.0;
    const double noise = total - tone;
    if (noise <= 0.0)
        return 1000.0; // Below what double precision can express: not a failure.
    return 10.0 * std::log10(tone / noise);
}

double amplitudeToDb(double amplitude, double reference)
{
    return 20.0 * std::log10(amplitude / reference);
}

} // namespace

MULTIROOM_TEST(real_resampler_registry_holds_what_the_build_contains)
{
    const std::vector<std::string> ids = registeredIds();
    CHECK(!ids.empty());

    // The build options are the only thing that decides this, and nothing
    // above BuiltinResamplers.h knows they exist.
#if MULTIROOM_WITH_SOXR
    CHECK(std::find(ids.begin(), ids.end(), "soxr") != ids.end());
#endif
#if MULTIROOM_WITH_R8BRAIN
    CHECK(std::find(ids.begin(), ids.end(), "r8brain") != ids.end());
#endif

    // The fake is never registered by production code. A room served by it
    // would be an audible mistake, and the way to make that impossible is for
    // this function to never put it in a registry.
    CHECK(std::find(ids.begin(), ids.end(), "fake") == ids.end());

    for (const std::string& id : ids)
    {
        const ResamplerCapabilities caps = capabilitiesOf(id);
        CHECK_EQ(caps.id, id);
        CHECK(caps.maxChannels >= 2);
        CHECK(caps.minRatio > 0.0);
        CHECK(caps.maxRatio > caps.minRatio);
        CHECK(caps.supportsRatio(static_cast<double>(kOutputRate) / kInputRate));
        CHECK(describeCapabilities(caps).find(id) == 0);

        // capabilities() answers without constructing anything, and what the
        // planner was told has to be what it gets.
        CHECK_EQ(make(id)->capabilities().id, caps.id);
    }
}

MULTIROOM_TEST(real_resampler_refuses_what_it_cannot_do_at_configure)
{
    for (const Subject& subject : subjects())
    {
        const ResamplerCapabilities caps = capabilitiesOf(subject.id);
        std::unique_ptr<IAudioResampler> resampler = make(subject.id);
        const bool drift = subject.variable;

        CHECK(!resampler->configured());
        CHECK_EQ(resampler->ratio(), 0.0);
        CHECK_EQ(resampler->latencyFrames(), 0.0);

        CHECK_ERR(resampler->configure(config(0, kOutputRate, 2, 0.0, drift)),
                  ResamplerError::InvalidConfig);
        CHECK_ERR(resampler->configure(config(kInputRate, 0, 2, 0.0, drift)),
                  ResamplerError::InvalidConfig);
        CHECK_ERR(resampler->configure(config(kInputRate, kOutputRate, 0, 0.0, drift)),
                  ResamplerError::InvalidConfig);

        CHECK_ERR(resampler->configure(config(kInputRate, kOutputRate,
                                              static_cast<uint16_t>(caps.maxChannels + 1), 0.0,
                                              drift)),
                  ResamplerError::UnsupportedChannels);

        // A ratio outside the declared range is refused here and never
        // mid-stream: MATP section 28 is explicit that there is no silent
        // fallback.
        CHECK_ERR(resampler->configure(
                      config(kInputRate, kOutputRate, 2, caps.maxRatio * 2.0, drift)),
                  ResamplerError::UnsupportedRatio);
        CHECK_ERR(resampler->configure(
                      config(kInputRate, kOutputRate, 2, caps.minRatio / 2.0, drift)),
                  ResamplerError::UnsupportedRatio);

        // None of that may have left it half-configured.
        CHECK(!resampler->configured());
        CHECK_ERR(resampler->process(std::span<const double>(), std::span<double>()),
                  ResamplerError::NotConfigured);

        CHECK_OK(resampler->configure(configFor(subject, 2)));
        CHECK(resampler->configured());
        CHECK_NEAR(resampler->ratio(), static_cast<double>(kOutputRate) / kInputRate, 1e-12);

        // A span that is not a whole number of frames is an error rather than
        // a silently truncated call.
        std::vector<double> odd(3);
        CHECK_ERR(resampler->process(std::span<const double>(odd), std::span<double>(odd)),
                  ResamplerError::MisalignedBuffer);
    }
}

MULTIROOM_TEST(real_resampler_refuses_a_drift_corrected_room_it_cannot_correct)
{
    // The refusal that makes the capability model worth having. A planner is
    // expected to have read capabilities() and never to have built this
    // configuration, but a fixed-ratio converter asked to be the drift
    // resampler has to fail at configure() -- at startup -- rather than at the
    // moment a room first needs correcting, which for a card running +30 ppm
    // fast is hours in.
    for (const std::string& id : registeredIds())
    {
        std::unique_ptr<IAudioResampler> resampler = make(id);
        const auto configured =
            resampler->configure(config(kInputRate, kOutputRate, 2, 0.0, true));

        if (capabilitiesOf(id).variableRatio)
        {
            CHECK_OK(configured);
            CHECK(resampler->configured());
        }
        else
        {
            CHECK_ERR(configured, ResamplerError::FixedRatioOnly);
            CHECK(!resampler->configured());
        }
    }
}

MULTIROOM_TEST(real_resampler_converts_a_sine_at_the_right_level_frequency_and_noise_floor)
{
    const std::vector<double> input =
        tones(kInputRate * 2, 1, kToneHz, static_cast<double>(kInputRate), kToneAmplitude);

    for (const Subject& subject : subjects())
    {
        std::unique_ptr<IAudioResampler> resampler = make(subject.id);
        CHECK_OK(resampler->configure(configFor(subject, 1)));

        const StreamRun run = runStream(*resampler, input, 1, 1024, 1024);
        CHECK(run.output.size() >= kSettleFrames + kWindowFrames);

        // Level. Measured: r8brain 0.00000 dB, soxr -0.00000 dB in
        // constant-rate mode and -0.00110 dB in variable-rate.
        const double amplitude = binAmplitude(run.output, kSettleFrames, 1, 0, kWindowFrames,
                                              kToneBin);
        CHECK_NEAR(amplitudeToDb(amplitude, kToneAmplitude), 0.0, 0.01);

        // Frequency. The tone is bin-centred, so a converter running at even a
        // slightly wrong ratio smears energy into the bins either side --
        // which is also the second line of defence against an inverted ratio
        // convention.
        for (size_t bin : {kToneBin - 1, kToneBin + 1, kToneBin - 20, kToneBin + 20})
        {
            const double stray = binAmplitude(run.output, kSettleFrames, 1, 0, kWindowFrames, bin);
            CHECK(amplitudeToDb(stray, amplitude) < -100.0);
        }

        // Noise floor. Measured: r8brain 132.7 dB, soxr 131.2 dB in
        // constant-rate mode and 115.7 dB in variable-rate -- the last is the
        // price of being correctable, paid only by rooms that are being
        // corrected, and this case is where a regression in which engine
        // configure() picks would show up. The threshold is set to catch a
        // converter that is broken, not to rank three that work.
        CHECK(signalToNoiseDb(run.output, kSettleFrames, 1, 0, kWindowFrames, kToneBin) > 100.0);
    }
}

MULTIROOM_TEST(real_resampler_ratio_is_output_over_input)
{
    // The one convention, and the one mistake that does not sound broken: a
    // room whose ratio is inverted converges away from the timeline instead of
    // toward it, slowly. Asserted against the real libraries rather than
    // assumed from their documentation, because each one's own convention is
    // the adapter's problem and this is where the adapter is checked.
    const std::vector<double> input =
        tones(kInputRate, 1, kToneHz, static_cast<double>(kInputRate), kToneAmplitude);

    constexpr double kTrim = 0.002;

    for (const Subject& subject : subjects())
    {
        const double nominal = static_cast<double>(kOutputRate) / kInputRate;

        std::unique_ptr<IAudioResampler> slower = make(subject.id);
        CHECK_OK(slower->configure(configFor(subject, 1, nominal * (1.0 - kTrim))));
        const StreamRun low = runStream(*slower, input, 1, 1024, 4096);

        std::unique_ptr<IAudioResampler> nominalRun = make(subject.id);
        CHECK_OK(nominalRun->configure(configFor(subject, 1)));
        const StreamRun middle = runStream(*nominalRun, input, 1, 1024, 4096);

        std::unique_ptr<IAudioResampler> faster = make(subject.id);
        CHECK_OK(faster->configure(configFor(subject, 1, nominal * (1.0 + kTrim))));
        const StreamRun high = runStream(*faster, input, 1, 1024, 4096);

        // A ratio above one means more output frames for the same input, which
        // is what a client whose card runs fast needs.
        CHECK(high.outputFramesProduced > middle.outputFramesProduced);
        CHECK(middle.outputFramesProduced > low.outputFramesProduced);

        // And by roughly the right amount: 2000 ppm of 48,000 frames is 96,
        // either side. The tolerance is wide because each converter's own
        // latency is in there too; the direction is the assertion.
        const double expectedStep = static_cast<double>(middle.outputFramesProduced) * kTrim;
        CHECK_NEAR(static_cast<double>(high.outputFramesProduced - middle.outputFramesProduced),
                   expectedStep, 4.0);
        CHECK_NEAR(static_cast<double>(middle.outputFramesProduced - low.outputFramesProduced),
                   expectedStep, 4.0);
    }
}

MULTIROOM_TEST(real_resampler_accounting_holds_over_many_calls)
{
    const std::vector<double> input =
        tones(kInputRate * 4, 1, kToneHz, static_cast<double>(kInputRate), kToneAmplitude);
    const std::vector<double> shortInput(input.begin(),
                                         input.begin() + static_cast<std::ptrdiff_t>(kInputRate));

    for (const Subject& subject : subjects())
    {
        std::unique_ptr<IAudioResampler> resampler = make(subject.id);
        CHECK_OK(resampler->configure(configFor(subject, 1)));
        const double ratio = resampler->ratio();

        // Four seconds, in eighths, with the books balanced at every boundary.
        //
        // There is no exact per-segment count to assert against, and that is a
        // property of the converters rather than a gap here: soxr's
        // constant-rate engine emits in whole internal DFT blocks, so a
        // half-second of input can come back as 7322 frames or as 8142 where
        // 8000 were due, and the difference is inside the library, not lost.
        // What *is* exact is that the difference never grows. A converter that
        // dropped a frame per block, or ran at a slightly wrong ratio, would
        // show up here as a fill climbing away over four seconds -- and 0.1%
        // short, which is a room running out of audio in twenty minutes, is
        // 190 frames by the end of this stream and well inside the bound.
        const size_t segments = 8;
        const size_t segment = input.size() / segments;
        size_t consumedTotal = 0;
        size_t producedTotal = 0;

        for (size_t part = 0; part < segments; ++part)
        {
            const size_t from = part * segment;
            const size_t to = part + 1 == segments ? input.size() : from + segment;
            const StreamRun run = runStream(
                *resampler,
                std::vector<double>(input.begin() + static_cast<std::ptrdiff_t>(from),
                                    input.begin() + static_cast<std::ptrdiff_t>(to)),
                1, 8192, 8192);

            consumedTotal += run.inputFramesConsumed;
            producedTotal += run.outputFramesProduced;

            // What the converter is holding, in output frames. Bounded below
            // by the latency it reports -- which is what ties latencyFrames()
            // to something observable rather than leaving it a number nobody
            // checks -- and above by that plus one internal block. Measured
            // over this stream: soxr variable-rate a flat 261 against a
            // reported 100, soxr constant-rate between 253 and 926 against a
            // reported 0, r8brain a flat 1850 against 1851.
            const double fill =
                static_cast<double>(consumedTotal) * ratio - static_cast<double>(producedTotal);
            CHECK(fill >= resampler->latencyFrames() - 2.0);
            CHECK(fill <= resampler->latencyFrames() + kMaxHeldFrames);
        }

        CHECK_EQ(consumedTotal, input.size());

        resampler->reset();
        const StreamRun bulk = runStream(*resampler, shortInput, 1, 8192, 8192);
        resampler->reset();

        // The same stream again in seven-frame input calls draining into
        // five-frame output spans -- a call pattern with no relationship to
        // any converter's internal block size.
        const StreamRun dribble = runStream(*resampler, shortInput, 1, 7, 5);
        CHECK_EQ(bulk.inputFramesConsumed, shortInput.size());
        CHECK_EQ(dribble.inputFramesConsumed, shortInput.size());

        // Splitting the calls changes nothing: the same input produces the
        // same number of output frames and the same samples. This is the test
        // that a frame is not lost or duplicated at a call boundary, and it is
        // exact rather than statistical. Measured worst difference: soxr 0,
        // r8brain 4e-14, which is double-precision summation order and not a
        // sample.
        CHECK_EQ(dribble.outputFramesProduced, bulk.outputFramesProduced);
        double worst = 0.0;
        for (size_t i = 0; i < bulk.output.size(); ++i)
            worst = std::max(worst, std::abs(bulk.output[i] - dribble.output[i]));
        CHECK(worst < 1e-12);
    }
}

MULTIROOM_TEST(real_resampler_output_is_aligned_with_its_input)
{
    // Both libraries compensate their own filter delay, so an impulse at input
    // frame 0 comes out at output frame 0 and a scheduled start needs no
    // per-implementation offset. That is a property phase 11 depends on and
    // neither library documents, so it is pinned here.
    for (const Subject& subject : subjects())
    {
        std::unique_ptr<IAudioResampler> resampler = make(subject.id);
        CHECK_OK(resampler->configure(configFor(subject, 1)));

        std::vector<double> impulse(kInputRate / 2, 0.0);
        impulse[0] = 1.0;

        const StreamRun run = runStream(*resampler, impulse, 1, 1024, 1024);
        CHECK(!run.output.empty());

        size_t peak = 0;
        for (size_t i = 0; i < run.output.size(); ++i)
        {
            if (std::abs(run.output[i]) > std::abs(run.output[peak]))
                peak = i;
        }
        CHECK(peak <= 2);

        // And the latency it reports is the signal it is holding, not an
        // offset it has already removed: positive, finite, and small next to
        // the stream.
        CHECK(resampler->latencyFrames() >= 0.0);
        CHECK(resampler->latencyFrames() < static_cast<double>(kOutputRate));
    }
}

MULTIROOM_TEST(real_resampler_reset_reproduces_the_first_run)
{
    const std::vector<double> input =
        tones(kInputRate / 2, 1, kToneHz, static_cast<double>(kInputRate), kToneAmplitude);

    for (const Subject& subject : subjects())
    {
        std::unique_ptr<IAudioResampler> resampler = make(subject.id);
        CHECK_OK(resampler->configure(configFor(subject, 1)));

        const StreamRun first = runStream(*resampler, input, 1, 1024, 1024);
        resampler->reset();
        const StreamRun second = runStream(*resampler, input, 1, 1024, 1024);

        // The configuration survives a reset; the filter history does not. A
        // seek or a rejoin that left history behind would fade the first
        // milliseconds of the new position into the old one.
        CHECK(resampler->configured());
        CHECK_NEAR(resampler->ratio(), static_cast<double>(kOutputRate) / kInputRate, 1e-12);
        CHECK_EQ(second.outputFramesProduced, first.outputFramesProduced);
        for (size_t i = 0; i < first.output.size(); ++i)
            CHECK_EQ(second.output[i], first.output[i]);
    }
}

MULTIROOM_TEST(real_resampler_keeps_channels_apart)
{
    // 1 kHz in the left channel and 2 kHz in the right, so a converter that
    // lets one leak into the other shows up as a tone in the wrong place
    // rather than as a level error nobody would attribute to this.
    const uint16_t channels = 2;
    const std::vector<double> input =
        tones(kInputRate * 2, channels, kToneHz, static_cast<double>(kInputRate), kToneAmplitude);

    for (const Subject& subject : subjects())
    {
        std::unique_ptr<IAudioResampler> resampler = make(subject.id);
        CHECK_OK(resampler->configure(configFor(subject, channels)));

        const StreamRun run = runStream(*resampler, input, channels, 1024, 1024);
        CHECK(run.outputFramesProduced >= kSettleFrames + kWindowFrames);

        for (uint16_t channel = 0; channel < channels; ++channel)
        {
            const size_t own = kToneBin * (channel + 1u);
            const size_t other = kToneBin * (2u - channel);

            const double wanted =
                binAmplitude(run.output, kSettleFrames, channels, channel, kWindowFrames, own);
            const double leaked =
                binAmplitude(run.output, kSettleFrames, channels, channel, kWindowFrames, other);

            CHECK_NEAR(amplitudeToDb(wanted, kToneAmplitude), 0.0, 0.01);
            CHECK(amplitudeToDb(leaked, wanted) < -100.0);
        }
    }
}

MULTIROOM_TEST(real_resampler_varies_its_ratio_only_where_it_says_it_can)
{
    for (const Subject& subject : subjects())
    {
        const ResamplerCapabilities caps = capabilitiesOf(subject.id);
        std::unique_ptr<IAudioResampler> resampler = make(subject.id);
        CHECK_OK(resampler->configure(configFor(subject, 1)));
        const double nominal = resampler->ratio();

        if (!subject.variable)
        {
            // Refused explicitly, by the code named in the message, rather
            // than accepted and ignored -- whether because the implementation
            // cannot vary a ratio at all, or because this instance was told at
            // configure() that it would never be asked to and was built the
            // cheaper, quieter way. Either is a planning defect, and both read
            // the same to a caller.
            CHECK_ERR(resampler->setRatio(nominal), ResamplerError::FixedRatioOnly);
            CHECK_EQ(resampler->ratio(), nominal);
            continue;
        }

        // +100 ppm: the protocol's normal correction limit, and the size of
        // move a room actually gets.
        CHECK_OK(resampler->setRatio(nominal * 1.000100));
        CHECK_NEAR(resampler->ratio(), nominal * 1.000100, 1e-12);
        CHECK_OK(resampler->setRatio(nominal * 0.999900));

        // Far outside the range the implementation declares at all.
        CHECK_ERR(resampler->setRatio(caps.maxRatio * 2.0), ResamplerError::UnsupportedRatio);

        // And inside the declared range but outside this instance's window,
        // which was fixed when its filter was designed. The failure has to
        // arrive as a refusal rather than as a quietly clamped ratio -- a room
        // silently corrected by less than it asked for never converges.
        CHECK_ERR(resampler->setRatio(nominal * 1.5), ResamplerError::UnsupportedRatio);
        CHECK_NEAR(resampler->ratio(), nominal * 0.999900, 1e-12);
    }
}

MULTIROOM_TEST(real_resampler_ratio_change_lands_without_a_discontinuity)
{
    // A drift correction arrives in the middle of music. What must not happen
    // is a step in the output, which is what a converter that restarts its
    // filter or rebases its phase on a ratio change would produce.
    const size_t halfSecond = kInputRate / 2;
    const std::vector<double> input =
        tones(halfSecond * 2, 1, kToneHz, static_cast<double>(kInputRate), kToneAmplitude);

    // The steepest a 1 kHz sine of this amplitude can be between two frames at
    // the output rate. Anything above it is a discontinuity rather than
    // signal.
    const double steepest =
        kToneAmplitude * 2.0 * kPi * kToneHz / static_cast<double>(kOutputRate);

    for (const Subject& subject : subjects())
    {
        if (!subject.variable)
            continue;

        std::unique_ptr<IAudioResampler> resampler = make(subject.id);
        CHECK_OK(resampler->configure(configFor(subject, 1)));
        const double nominal = resampler->ratio();

        const std::vector<double> before(input.begin(),
                                         input.begin() + static_cast<std::ptrdiff_t>(halfSecond));
        const std::vector<double> after(input.begin() + static_cast<std::ptrdiff_t>(halfSecond),
                                        input.end());

        const StreamRun first = runStream(*resampler, before, 1, 1024, 4096);
        CHECK_OK(resampler->setRatio(nominal * 1.000100));
        const StreamRun second = runStream(*resampler, after, 1, 1024, 4096);

        // Joined, so the seam between the two ratios is inside the run being
        // examined rather than at its edge, and the scan starts well before
        // the seam -- a scan that began at it would be looking at the one
        // place a step cannot show up as a difference between two samples.
        std::vector<double> joined = first.output;
        joined.insert(joined.end(), second.output.begin(), second.output.end());
        CHECK(first.output.size() > kScanFromFrames);

        double worstStep = 0.0;
        for (size_t i = kScanFromFrames; i < joined.size(); ++i)
            worstStep = std::max(worstStep, std::abs(joined[i] - joined[i - 1]));

        // Measured: 0.0654 against a bound of 0.0687, which is the tone's own
        // slope and nothing else. This case is also the one that caught the
        // adapter telling soxr the stream had ended every time it was handed
        // an empty input span; the step that left behind was 0.0684, five per
        // cent over -- quiet enough to pass a noise-floor measurement taken
        // somewhere else in the stream, and loud enough to hear.
        CHECK(worstStep < steepest * 1.05);

        // The change also has to have taken effect, or the test above would
        // pass on a converter that ignored it.
        CHECK_NEAR(resampler->ratio(), nominal * 1.000100, 1e-12);
    }
}

#endif // MULTIROOM_WITH_SOXR || MULTIROOM_WITH_R8BRAIN

