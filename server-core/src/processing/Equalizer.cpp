#include "processing/Equalizer.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace multiroom {

namespace {

constexpr EqFilterType kFilterTypes[] = {EqFilterType::Peak, EqFilterType::LowShelf,
                                         EqFilterType::HighShelf, EqFilterType::HighPass,
                                         EqFilterType::LowPass};

bool usesGain(EqFilterType type)
{
    return type == EqFilterType::Peak || type == EqFilterType::LowShelf ||
           type == EqFilterType::HighShelf;
}

// Written so that a NaN is outside every range.
bool within(double value, double lowest, double highest)
{
    return value >= lowest && value <= highest;
}

std::string number(double value)
{
    std::string text = std::to_string(value);
    text.erase(text.find_last_not_of('0') + 1);
    if (!text.empty() && text.back() == '.')
        text.pop_back();
    return text;
}

} // namespace

const char* eqFilterTypeName(EqFilterType type)
{
    switch (type)
    {
    case EqFilterType::Peak:
        return "peak";
    case EqFilterType::LowShelf:
        return "lowShelf";
    case EqFilterType::HighShelf:
        return "highShelf";
    case EqFilterType::HighPass:
        return "highPass";
    case EqFilterType::LowPass:
        return "lowPass";
    }
    return "peak";
}

bool parseEqFilterType(std::string_view text, EqFilterType& out)
{
    for (const EqFilterType type : kFilterTypes)
    {
        if (text == eqFilterTypeName(type))
        {
            out = type;
            return true;
        }
    }
    return false;
}

std::string validateEqualizer(const EqualizerSettings& settings)
{
    using S = EqualizerSettings;

    if (!within(settings.preampDb, S::kMinPreampDb, S::kMaxPreampDb))
        return "preampDb is " + number(settings.preampDb) + ", expected " +
               number(S::kMinPreampDb) + " to " + number(S::kMaxPreampDb);

    if (settings.bands.size() > S::kMaxBands)
        return std::to_string(settings.bands.size()) + " bands, at most " +
               std::to_string(S::kMaxBands);

    for (size_t i = 0; i < settings.bands.size(); ++i)
    {
        const EqBand& band = settings.bands[i];
        const std::string at = "band " + std::to_string(i) + ": ";
        if (!within(band.frequencyHz, S::kMinFrequencyHz, S::kMaxFrequencyHz))
            return at + "frequencyHz is " + number(band.frequencyHz) + ", expected " +
                   number(S::kMinFrequencyHz) + " to " + number(S::kMaxFrequencyHz);
        if (!within(band.gainDb, -S::kMaxGainDb, S::kMaxGainDb))
            return at + "gainDb is " + number(band.gainDb) + ", expected " +
                   number(-S::kMaxGainDb) + " to " + number(S::kMaxGainDb);
        if (!within(band.q, S::kMinQ, S::kMaxQ))
            return at + "q is " + number(band.q) + ", expected " + number(S::kMinQ) + " to " +
                   number(S::kMaxQ);
    }
    return {};
}

// ---------------------------------------------------------------------------

double Equalizer::Chain::run(size_t channel, double x) noexcept
{
    double y = x * gain;
    double* s = state.data() + channel * bands.size() * 2;
    for (const Coefficients& c : bands)
    {
        const double in = y;
        y = c.b0 * in + s[0];
        s[0] = c.b1 * in - c.a1 * y + s[1];
        s[1] = c.b2 * in - c.a2 * y;
        s += 2;
    }
    return y;
}

void Equalizer::Chain::clear() noexcept
{
    std::fill(state.begin(), state.end(), 0.0);
}

Equalizer::Chain Equalizer::build(const EqualizerSettings& settings) const
{
    Chain chain;
    if (!settings.enabled || sampleRate_ == 0 || channels_ == 0)
        return chain;

    chain.gain = std::pow(10.0, settings.preampDb / 20.0);

    const double rate = static_cast<double>(sampleRate_);
    for (const EqBand& band : settings.bands)
    {
        // A boost or cut of nothing is no filter, so a flat Basic-mode
        // equalizer costs exactly what an empty one does.
        if (usesGain(band.type) && band.gainDb == 0.0)
            continue;

        // Below Nyquist whatever the room's rate: 20 kHz is a valid band in a
        // 44.1 kHz room and has to stay a valid filter in a 32 kHz one.
        const double frequency = std::min(band.frequencyHz, 0.49 * rate);
        const double w0 = 2.0 * std::numbers::pi * frequency / rate;
        const double cosw = std::cos(w0);
        const double alpha = std::sin(w0) / (2.0 * band.q);
        const double a = std::pow(10.0, band.gainDb / 40.0);
        const double shelf = 2.0 * std::sqrt(a) * alpha;

        double b0 = 1.0, b1 = 0.0, b2 = 0.0, a0 = 1.0, a1 = 0.0, a2 = 0.0;
        switch (band.type)
        {
        case EqFilterType::Peak:
            b0 = 1.0 + alpha * a;
            b1 = -2.0 * cosw;
            b2 = 1.0 - alpha * a;
            a0 = 1.0 + alpha / a;
            a1 = -2.0 * cosw;
            a2 = 1.0 - alpha / a;
            break;
        case EqFilterType::LowShelf:
            b0 = a * ((a + 1.0) - (a - 1.0) * cosw + shelf);
            b1 = 2.0 * a * ((a - 1.0) - (a + 1.0) * cosw);
            b2 = a * ((a + 1.0) - (a - 1.0) * cosw - shelf);
            a0 = (a + 1.0) + (a - 1.0) * cosw + shelf;
            a1 = -2.0 * ((a - 1.0) + (a + 1.0) * cosw);
            a2 = (a + 1.0) + (a - 1.0) * cosw - shelf;
            break;
        case EqFilterType::HighShelf:
            b0 = a * ((a + 1.0) + (a - 1.0) * cosw + shelf);
            b1 = -2.0 * a * ((a - 1.0) + (a + 1.0) * cosw);
            b2 = a * ((a + 1.0) + (a - 1.0) * cosw - shelf);
            a0 = (a + 1.0) - (a - 1.0) * cosw + shelf;
            a1 = 2.0 * ((a - 1.0) - (a + 1.0) * cosw);
            a2 = (a + 1.0) - (a - 1.0) * cosw - shelf;
            break;
        case EqFilterType::HighPass:
            b0 = (1.0 + cosw) / 2.0;
            b1 = -(1.0 + cosw);
            b2 = (1.0 + cosw) / 2.0;
            a0 = 1.0 + alpha;
            a1 = -2.0 * cosw;
            a2 = 1.0 - alpha;
            break;
        case EqFilterType::LowPass:
            b0 = (1.0 - cosw) / 2.0;
            b1 = 1.0 - cosw;
            b2 = (1.0 - cosw) / 2.0;
            a0 = 1.0 + alpha;
            a1 = -2.0 * cosw;
            a2 = 1.0 - alpha;
            break;
        }

        chain.bands.push_back({b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0});
    }

    chain.state.assign(chain.bands.size() * 2 * channels_, 0.0);
    return chain;
}

void Equalizer::setFormat(uint32_t sampleRate, uint16_t channels)
{
    sampleRate_ = sampleRate;
    channels_ = channels;
    fadeFrames_ = std::max<size_t>(1, static_cast<size_t>(sampleRate * kCrossfadeSeconds));
    current_ = build(settings_);
    previous_ = Chain{};
    fadeRemaining_ = 0;
    running_ = false;
}

void Equalizer::configure(const EqualizerSettings& settings)
{
    settings_ = settings;
    Chain next = build(settings_);

    // Nothing was heard yet, or nothing was heard and nothing will be: no
    // fade to run.
    if (!running_ || (current_.identity() && next.identity() && fadeRemaining_ == 0))
    {
        previous_ = Chain{};
        fadeRemaining_ = 0;
        current_ = std::move(next);
        return;
    }

    // A change during a fade starts a new one from what was fading in. The
    // fade still under way is cut short, which is a step of at most one
    // fade's worth of difference and only while someone is dragging a slider.
    previous_ = std::move(current_);
    current_ = std::move(next);
    fadeRemaining_ = fadeFrames_;
}

void Equalizer::process(std::span<double> samples)
{
    running_ = true;
    if (!active() || channels_ == 0)
        return;

    const size_t channels = channels_;
    const size_t frames = samples.size() / channels;
    uint64_t clipped = 0;

    for (size_t frame = 0; frame < frames; ++frame)
    {
        double* sample = samples.data() + frame * channels;
        if (fadeRemaining_ > 0)
        {
            const double weight = 1.0 - static_cast<double>(fadeRemaining_) /
                                            static_cast<double>(fadeFrames_);
            for (size_t ch = 0; ch < channels; ++ch)
            {
                const double incoming = current_.run(ch, sample[ch]);
                const double outgoing = previous_.run(ch, sample[ch]);
                sample[ch] = outgoing + (incoming - outgoing) * weight;
            }
            if (--fadeRemaining_ == 0)
                previous_ = Chain{};
        }
        else
        {
            for (size_t ch = 0; ch < channels; ++ch)
                sample[ch] = current_.run(ch, sample[ch]);
        }

        for (size_t ch = 0; ch < channels; ++ch)
            clipped += std::abs(sample[ch]) > 1.0 ? 1 : 0;
    }

    if (clipped != 0)
        clipped_.fetch_add(clipped);

    // A filter ringing down in silence ends in denormals, which cost a
    // hundred times a normal multiply on x86. Once per call is enough: a
    // state this small takes far longer than one call to get there.
    for (Chain* chain : {&current_, &previous_})
    {
        for (double& s : chain->state)
        {
            if (std::abs(s) < 1e-30)
                s = 0.0;
        }
    }
}

void Equalizer::reset()
{
    current_.clear();
    previous_ = Chain{};
    fadeRemaining_ = 0;
    running_ = false;
}

bool Equalizer::active() const noexcept
{
    return !current_.identity() || fadeRemaining_ > 0;
}

} // namespace multiroom
