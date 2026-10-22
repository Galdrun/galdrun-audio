#pragma once

// One room's parametric equalizer.
//
// RBJ-cookbook biquads in 64-bit transposed direct form II, one filter state
// per channel and band. It runs on the resampled path only, on the doubles a
// converter has already *produced*: the frames a converter did not consume
// are re-presented by the caller, and filtering on the input side would run
// them through the filters twice. It is therefore designed at the room's
// output rate. It adds no latency, so a room with it stays in sync with a
// room without it.
//
// **Bitperfect wins.** Nothing here decides that: a bitperfect room's
// DirectPcmProcessor never calls this, and the pass-through copy never reaches
// a processor at all. Switching the equalizer on is not a reason to leave
// bitperfect.
//
// **A change crossfades.** The previous filter set keeps running beside the
// new one for kCrossfadeSeconds and is faded out linearly. Coefficients are
// never interpolated: a low, narrow filter can go unstable on the way between
// two stable ones.
//
// **Off costs nothing.** A disabled equalizer, or one with no band and a
// preamp of 0 dB, is not run at all, so the samples are exactly what they
// were before this file existed.
//
// Threading: one room's thread (or its caller while that thread is stopped),
// except clippedSamples(), which status reads from anywhere.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace multiroom {

enum class EqFilterType
{
    Peak,
    LowShelf,
    HighShelf,
    HighPass,
    LowPass
};

// The control API's spelling: "peak", "lowShelf", "highShelf", "highPass",
// "lowPass".
const char* eqFilterTypeName(EqFilterType type);
[[nodiscard]] bool parseEqFilterType(std::string_view text, EqFilterType& out);

struct EqBand
{
    EqFilterType type = EqFilterType::Peak;
    double frequencyHz = 1000.0;
    double gainDb = 0.0; // not used by HighPass and LowPass
    double q = 0.7071;

    bool operator==(const EqBand&) const = default;
};

struct EqualizerSettings
{
    static constexpr size_t kMaxBands = 20;
    static constexpr double kMinFrequencyHz = 10.0;
    static constexpr double kMaxFrequencyHz = 24000.0;
    static constexpr double kMaxGainDb = 24.0;
    static constexpr double kMinQ = 0.1;
    static constexpr double kMaxQ = 10.0;
    static constexpr double kMinPreampDb = -30.0;
    static constexpr double kMaxPreampDb = 12.0;

    bool enabled = false;
    double preampDb = 0.0;
    std::vector<EqBand> bands;

    bool operator==(const EqualizerSettings&) const = default;
};

// Empty when every value is within the ranges above, otherwise what is not.
[[nodiscard]] std::string validateEqualizer(const EqualizerSettings& settings);

class Equalizer
{
public:
    static constexpr double kCrossfadeSeconds = 0.020;

    // The room's output format. Rebuilds the filters for the new rate with an
    // empty history and no crossfade: a format change is a new stream.
    void setFormat(uint32_t sampleRate, uint16_t channels);

    // New settings, crossfaded from the ones in force once audio has run
    // through them.
    void configure(const EqualizerSettings& settings);

    // Interleaved, in place.
    void process(std::span<double> samples);

    // Forgets the filter history and any crossfade, for a seek or a rejoin.
    void reset();

    // Whether process() changes anything at all.
    [[nodiscard]] bool active() const noexcept;

    // Whether the settings in force change anything, a fade aside.
    [[nodiscard]] bool shaping() const noexcept { return !current_.identity(); }

    // Samples past full scale after the equalizer, since this room began.
    [[nodiscard]] uint64_t clippedSamples() const noexcept { return clipped_.load(); }

private:
    struct Coefficients
    {
        double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
    };

    struct Chain
    {
        double gain = 1.0;
        std::vector<Coefficients> bands;
        std::vector<double> state; // two per band and channel, band-major per channel

        [[nodiscard]] bool identity() const noexcept { return bands.empty() && gain == 1.0; }
        [[nodiscard]] double run(size_t channel, double x) noexcept;
        void clear() noexcept;
    };

    [[nodiscard]] Chain build(const EqualizerSettings& settings) const;

    EqualizerSettings settings_;
    uint32_t sampleRate_ = 0;
    uint16_t channels_ = 0;

    Chain current_;
    Chain previous_;
    size_t fadeFrames_ = 0;
    size_t fadeRemaining_ = 0;

    // Whether anything has been filtered since the last reset: settings that
    // arrive before the first sample have nothing to fade from.
    bool running_ = false;

    std::atomic<uint64_t> clipped_{0};
};

} // namespace multiroom
