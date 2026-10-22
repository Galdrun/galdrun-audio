#include "audio/linux/AlsaBackend.h"

#include <alsa/asoundlib.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

namespace {
std::atomic<unsigned> writes{0};
std::atomic<bool> lost{false};
bool stallDrain = false;
bool partialWrites = false;
unsigned writeAttempts = 0;
unsigned drains = 0;
bool drained = false;
bool droppedAfterDrain = false;
std::vector<int16_t> samples; // Only the render thread writes; read after stop joins.

uint32_t render(void*, void* target, uint32_t frames)
{
    auto* output = static_cast<int16_t*>(target);
    for (uint32_t i = 0; i < frames * 2; ++i)
        output[i] = 9000;
    return frames;
}

bool run(bool stalled, bool fail, bool partial = false)
{
    writes = 0;
    lost = false;
    stallDrain = stalled;
    partialWrites = partial;
    writeAttempts = 0;
    drains = 0;
    drained = false;
    droppedAfterDrain = false;
    samples.clear();
    multiroom::AlsaBackend backend;
    multiroom::AudioBackendConfig config;
    config.deviceId = "null";
    config.format = {48000, 2, multiroom::AudioFormat::SampleType::Int16};
    config.requestedBufferFrames = 480;
    config.renderCallback = render;
    if (!backend.open(config) || !backend.start())
        return false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (writes == 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (writes == 0)
        return false;
    if (fail)
    {
        lost = true;
        while (backend.state() == multiroom::BackendState::Running &&
               std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (backend.state() != multiroom::BackendState::Failed)
            return false;
    }
    const auto began = std::chrono::steady_clock::now();
    backend.stop();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - began).count();
    if (fail)
        return drains == 0 && ms < 200 && backend.state() == multiroom::BackendState::Failed;
    if (stalled)
        return !drained && drains > 1 && ms >= 400 && ms < 1500;
    bool faded = false;
    for (auto sample : samples)
        faded |= sample > 0 && sample < 9000;
    if (!faded || !drained || !droppedAfterDrain || samples.size() < 960)
        return false;
    for (size_t i = samples.size() - 960; i < samples.size(); ++i)
        if (samples[i] != 0)
            return false;
    // Stop's drain leaves SETUP; the backend must prepare before a restart.
    return backend.state() == multiroom::BackendState::Open && backend.start();
}
} // namespace

extern "C" int __wrap_snd_pcm_wait(snd_pcm_t*, int)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return lost ? -ENODEV : 1;
}
extern "C" snd_pcm_sframes_t __wrap_snd_pcm_writei(snd_pcm_t*, const void* data,
                                                  snd_pcm_uframes_t frames)
{
    if (partialWrites)
    {
        ++writeAttempts;
        if (writeAttempts % 4 == 1)
            return 0;
        if (writeAttempts % 4 == 2)
            return -EAGAIN;
        frames = (frames + 1) / 2;
    }
    const auto* first = static_cast<const int16_t*>(data);
    samples.insert(samples.end(), first, first + frames * 2);
    ++writes;
    return static_cast<snd_pcm_sframes_t>(frames);
}
extern "C" int __wrap_snd_pcm_drain(snd_pcm_t*)
{
    ++drains;
    if (stallDrain || drains < 3)
        return -EAGAIN;
    drained = true;
    return 0;
}
extern "C" int __real_snd_pcm_drop(snd_pcm_t*);
extern "C" int __wrap_snd_pcm_drop(snd_pcm_t* pcm)
{
    droppedAfterDrain = drained;
    return __real_snd_pcm_drop(pcm);
}

int main()
{
    const bool tail = run(false, false);
    const bool partial = run(false, false, true);
    const bool timeout = run(true, false);
    const bool failure = run(false, true);
    std::printf("ALSA fade/silence/drain/restart: %d; partial writes: %d; "
                "bounded drain: %d; failed-device stop: %d\n", tail, partial, timeout, failure);
    return tail && partial && timeout && failure ? 0 : 1;
}
