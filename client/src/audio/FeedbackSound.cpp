#include "audio/FeedbackSound.h"

#include "audio/ToneGenerator.h"

#include <atomic>
#include <cmath>

namespace multiroom {
namespace {

// Low two bits: the sound; the rest counts requests, so the render thread
// sees each press once. 32 bits: 64-bit atomics are not lock-free on ARMv6.
std::atomic<uint32_t> g_request{0};

// Peak level per millisecond and pitch contour measured from the Connector 3
// stock firmware's touch.flac and touch-play-pause.flac (44.1 kHz, 40 ms):
// a drop from ~2.9 kHz to ~600 Hz, and a ~1.2 kHz click held near full scale
// for 4 ms. Rebuilt from these numbers rather than shipping Teufel's files.
const float kTouchLevel[] = {0.723f, 0.725f, 0.564f, 0.388f, 0.276f, 0.196f, 0.168f, 0.140f,
                             0.111f, 0.091f, 0.074f, 0.057f, 0.039f, 0.030f, 0.026f, 0.020f,
                             0.018f, 0.013f, 0.010f, 0.009f, 0.007f, 0.007f, 0.005f, 0.004f,
                             0.003f, 0.003f, 0.002f, 0.002f, 0.001f, 0.0f};
const float kPlayPauseLevel[] = {0.965f, 0.967f, 0.967f, 0.964f, 0.499f, 0.193f, 0.068f,
                                 0.048f, 0.019f, 0.010f, 0.010f, 0.004f, 0.007f, 0.005f,
                                 0.005f, 0.004f, 0.002f, 0.003f, 0.002f, 0.0f};

double touchHz(double ms)
{
    return 590.0 + 110.0 * std::exp(-ms / 8.0) + 2300.0 * std::exp(-ms / 0.6);
}

double playPauseHz(double ms) { return 1180.0 + 2500.0 * std::exp(-ms / 0.45); }

template <size_t N>
std::vector<float> synthesize(uint32_t rate, const float (&level)[N], double (*hz)(double))
{
    const double kTwoPi = 6.283185307179586476925286766559;
    std::vector<float> out(static_cast<size_t>(rate) * N / 1000);
    double phase = 0.0;
    for (size_t n = 0; n < out.size(); ++n)
    {
        const double ms = 1000.0 * static_cast<double>(n) / rate;
        // Each level is the peak of its millisecond, placed at its middle.
        const double at = ms - 0.5;
        size_t i = at <= 0.0 ? 0 : static_cast<size_t>(at);
        if (i + 1 >= N)
            i = N - 2;
        const double frac = at <= 0.0 ? 0.0 : std::fmin(1.0, at - static_cast<double>(i));
        const double amplitude = level[i] + (level[i + 1] - level[i]) * frac;
        out[n] = static_cast<float>(amplitude * std::sin(phase));
        phase += kTwoPi * hz(ms) / rate;
    }
    return out;
}

} // namespace

void FeedbackSound::play(Sound sound)
{
    uint32_t expected = g_request.load(std::memory_order_relaxed);
    while (!g_request.compare_exchange_weak(
        expected, ((expected & ~3u) + 4u) | static_cast<uint32_t>(sound), std::memory_order_release,
        std::memory_order_relaxed))
    {
    }
}

void FeedbackSound::prepare(const AudioFormat& format)
{
    format_ = format;
    touch_ = synthesize(format.sampleRate, kTouchLevel, touchHz);
    playPause_ = synthesize(format.sampleRate, kPlayPauseLevel, playPauseHz);
    current_ = nullptr;
    position_ = 0;
    seen_ = g_request.load(std::memory_order_acquire);
}

void FeedbackSound::mix(void* buffer, uint32_t frames)
{
    const uint32_t request = g_request.load(std::memory_order_acquire);
    if (request != seen_)
    {
        seen_ = request;
        current_ = (request & 3u) == static_cast<uint32_t>(Sound::PlayPause) ? &playPause_ : &touch_;
        position_ = 0;
    }
    if (current_ == nullptr)
        return;

    const uint16_t channels = format_.channels;
    for (uint32_t frame = 0; frame < frames && position_ < current_->size(); ++frame, ++position_)
    {
        const float click = (*current_)[position_];
        for (uint16_t channel = 0; channel < channels; ++channel)
        {
            const size_t index = static_cast<size_t>(frame) * channels + channel;
            writeSample(buffer, index, readSample(buffer, index, format_.sampleType) + click,
                        format_.sampleType);
        }
    }
    if (position_ >= current_->size())
        current_ = nullptr;
}

} // namespace multiroom
