#include "TestSupport.h"

#include "processing/Equalizer.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numbers>
#include <vector>

using namespace multiroom;

// The equalizer's claims are three: off changes nothing at all, on gives each
// filter type the response the cookbook says it has, and a change is a fade
// rather than a step. The responses are measured on a steady sine after the
// filters have settled, against the analytic gain at that frequency.

namespace {

constexpr uint32_t kRate = 48000;

EqualizerSettings with(std::vector<EqBand> bands, double preampDb = 0.0)
{
    EqualizerSettings settings;
    settings.enabled = true;
    settings.preampDb = preampDb;
    settings.bands = std::move(bands);
    return settings;
}

// Gain on a steady stereo sine: the RMS of the last tenth of a second, an
// integer number of periods for every whole frequency, over the input's.
// A peak would read a high sine wherever the samples happen to fall.
double amplitudeAt(Equalizer& eq, double frequencyHz, double level = 0.25)
{
    const size_t frames = kRate / 2;
    std::vector<double> samples(frames * 2);
    for (size_t i = 0; i < frames; ++i)
    {
        const double v =
            level * std::sin(2.0 * std::numbers::pi * frequencyHz * static_cast<double>(i) / kRate);
        samples[i * 2] = v;
        samples[i * 2 + 1] = v;
    }
    eq.process(samples);

    double sum = 0.0;
    const size_t window = kRate / 10;
    for (size_t i = frames - window; i < frames; ++i)
        sum += samples[i * 2] * samples[i * 2];
    return std::sqrt(sum / static_cast<double>(window)) / (level / std::sqrt(2.0));
}

double db(double gain)
{
    return 20.0 * std::log10(gain);
}

void prepare(Equalizer& eq, const EqualizerSettings& settings)
{
    eq.setFormat(kRate, 2);
    eq.configure(settings);
}

} // namespace

MULTIROOM_TEST(an_equalizer_that_is_off_or_flat_changes_no_sample)
{
    std::vector<EqBand> flat;
    for (const double f : {31.0, 62.0, 125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0, 16000.0})
        flat.push_back({EqFilterType::Peak, f, 0.0, 1.41});

    EqualizerSettings off = with({{EqFilterType::Peak, 1000.0, 6.0, 1.0}}, -3.0);
    off.enabled = false;

    for (const EqualizerSettings& settings : {EqualizerSettings{}, off, with(flat)})
    {
        Equalizer eq;
        prepare(eq, settings);
        CHECK(!eq.active());
        CHECK(!eq.shaping());

        std::vector<double> samples(256);
        for (size_t i = 0; i < samples.size(); ++i)
            samples[i] = std::sin(static_cast<double>(i) * 0.1) * 0.9;
        const std::vector<double> before = samples;
        eq.process(samples);
        CHECK(samples == before);
    }
}

MULTIROOM_TEST(each_filter_type_has_the_cookbook_response)
{
    {
        Equalizer eq;
        prepare(eq, with({{EqFilterType::Peak, 1000.0, 6.0, 1.41}}));
        CHECK_NEAR(db(amplitudeAt(eq, 1000.0)), 6.0, 0.1);
        eq.reset();
        CHECK_NEAR(db(amplitudeAt(eq, 50.0)), 0.0, 0.3);
    }
    {
        Equalizer eq;
        prepare(eq, with({{EqFilterType::Peak, 1000.0, -12.0, 2.0}}));
        CHECK_NEAR(db(amplitudeAt(eq, 1000.0)), -12.0, 0.1);
    }
    {
        Equalizer eq;
        prepare(eq, with({{EqFilterType::LowShelf, 200.0, 6.0, 0.7071}}));
        CHECK_NEAR(db(amplitudeAt(eq, 30.0)), 6.0, 0.3);
        eq.reset();
        CHECK_NEAR(db(amplitudeAt(eq, 8000.0)), 0.0, 0.2);
    }
    {
        Equalizer eq;
        prepare(eq, with({{EqFilterType::HighShelf, 2000.0, -6.0, 0.7071}}));
        CHECK_NEAR(db(amplitudeAt(eq, 15000.0)), -6.0, 0.3);
        eq.reset();
        CHECK_NEAR(db(amplitudeAt(eq, 100.0)), 0.0, 0.2);
    }
    {
        // Second order: -3 dB at the corner for Q 0.7071, -40 dB a decade out.
        Equalizer eq;
        prepare(eq, with({{EqFilterType::LowPass, 1000.0, 0.0, 0.7071}}));
        CHECK_NEAR(db(amplitudeAt(eq, 1000.0)), -3.01, 0.1);
        eq.reset();
        CHECK(db(amplitudeAt(eq, 10000.0)) < -38.0);
    }
    {
        Equalizer eq;
        prepare(eq, with({{EqFilterType::HighPass, 100.0, 0.0, 0.7071}}));
        CHECK_NEAR(db(amplitudeAt(eq, 100.0)), -3.01, 0.1);
        eq.reset();
        CHECK(db(amplitudeAt(eq, 10.0)) < -38.0);
        eq.reset();
        CHECK_NEAR(db(amplitudeAt(eq, 5000.0)), 0.0, 0.1);
    }
    {
        Equalizer eq;
        prepare(eq, with({}, -6.0));
        CHECK(eq.shaping());
        CHECK_NEAR(db(amplitudeAt(eq, 1000.0)), -6.0, 0.01);
    }
}

MULTIROOM_TEST(a_band_above_the_rooms_nyquist_stays_a_stable_filter)
{
    Equalizer eq;
    eq.setFormat(32000, 2);
    eq.configure(with({{EqFilterType::Peak, 20000.0, 12.0, 0.5}}));
    const double gain = amplitudeAt(eq, 1000.0);
    CHECK(std::isfinite(gain));
    CHECK(gain < 4.0);
}

MULTIROOM_TEST(a_change_while_running_fades_rather_than_steps)
{
    Equalizer eq;
    prepare(eq, with({}, 0.0));

    // Half scale DC, then twice the gain: the output walks from 0.5 to 1.0
    // over the crossfade, and never jumps between two samples.
    std::vector<double> samples(4800 * 2, 0.5);
    eq.process(std::span<double>(samples).first(480 * 2));
    eq.configure(with({}, 6.0206));
    eq.process(std::span<double>(samples).subspan(480 * 2));

    double largestStep = 0.0;
    for (size_t i = 1; i < samples.size() / 2; ++i)
        largestStep = std::max(largestStep, std::abs(samples[i * 2] - samples[(i - 1) * 2]));

    const size_t fadeFrames = static_cast<size_t>(kRate * Equalizer::kCrossfadeSeconds);
    CHECK_NEAR(samples[479 * 2], 0.5, 1e-12);
    CHECK_NEAR(samples[(480 + fadeFrames / 2) * 2], 0.75, 0.01);
    CHECK_NEAR(samples.back(), 1.0, 1e-3);
    CHECK(largestStep < 0.001);

    // And back off again, the same way.
    std::vector<double> more(4800 * 2, 0.5);
    eq.configure(EqualizerSettings{});
    CHECK(!eq.shaping());
    CHECK(eq.active());
    eq.process(more);
    CHECK_NEAR(more.front(), 1.0, 0.01);
    CHECK_NEAR(more.back(), 0.5, 1e-12);
    CHECK(!eq.active());
}

MULTIROOM_TEST(settings_before_the_first_sample_apply_without_a_fade)
{
    Equalizer eq;
    eq.setFormat(kRate, 1);
    eq.configure(with({}, 6.0206));
    std::vector<double> samples(16, 0.25);
    eq.process(samples);
    CHECK_NEAR(samples.front(), 0.5, 1e-3);
}

MULTIROOM_TEST(samples_past_full_scale_are_counted)
{
    Equalizer eq;
    prepare(eq, with({}, 12.0));
    std::vector<double> samples(100 * 2, 0.5);
    samples[0] = 0.1; // 0.4 after +12 dB, not clipped
    eq.process(samples);
    CHECK_EQ(eq.clippedSamples(), uint64_t{199});
}

MULTIROOM_TEST(equalizer_settings_are_validated)
{
    CHECK(validateEqualizer(with({{EqFilterType::Peak, 1000.0, 6.0, 1.0}}, -6.0)).empty());
    CHECK(!validateEqualizer(with({}, 13.0)).empty());
    CHECK(!validateEqualizer(with({}, -31.0)).empty());
    CHECK(!validateEqualizer(with({{EqFilterType::Peak, 5.0, 0.0, 1.0}})).empty());
    CHECK(!validateEqualizer(with({{EqFilterType::Peak, 30000.0, 0.0, 1.0}})).empty());
    CHECK(!validateEqualizer(with({{EqFilterType::Peak, 1000.0, 25.0, 1.0}})).empty());
    CHECK(!validateEqualizer(with({{EqFilterType::Peak, 1000.0, 0.0, 0.0}})).empty());
    CHECK(!validateEqualizer(with({{EqFilterType::Peak, 1000.0,
                                    std::numeric_limits<double>::quiet_NaN(), 1.0}}))
               .empty());
    CHECK(!validateEqualizer(
               with(std::vector<EqBand>(EqualizerSettings::kMaxBands + 1, EqBand{})))
               .empty());

    for (const EqFilterType type : {EqFilterType::Peak, EqFilterType::LowShelf,
                                    EqFilterType::HighShelf, EqFilterType::HighPass,
                                    EqFilterType::LowPass})
    {
        EqFilterType parsed = EqFilterType::Peak;
        CHECK(parseEqFilterType(eqFilterTypeName(type), parsed));
        CHECK(parsed == type);
    }
    EqFilterType unused = EqFilterType::Peak;
    CHECK(!parseEqFilterType("notch", unused));
}
