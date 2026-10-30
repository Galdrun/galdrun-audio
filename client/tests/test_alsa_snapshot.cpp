#include "audio/linux/AlsaBackend.h"

#include <alsa/asoundlib.h>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <pthread.h>
#include <sched.h>

namespace {
std::mutex mutex;
std::condition_variable changed;
unsigned permits = 0;
unsigned waits = 0;
bool finished = false;
bool writeDuringStatus = false;
bool holdWrite = false;
bool enteredWrite = false;
bool timedOut = false;
bool requestedAudioPriority = false;
bool elapsedDriverTimestamp = false;

bool await(std::unique_lock<std::mutex>& lock, unsigned count)
{
    const bool ready = changed.wait_for(lock, std::chrono::seconds(2),
                                       [count] { return waits >= count; });
    timedOut |= !ready;
    return ready;
}

uint32_t render(void*, void* buffer, uint32_t frames)
{
    std::memset(buffer, 0, frames * 8);
    return frames;
}
} // namespace

// Exercise the actual backend with ALSA's null PCM and controlled writes.
// No sound card, timing luck or production test hooks are needed.
extern "C" int __real_snd_pcm_status(snd_pcm_t*, snd_pcm_status_t*);
extern "C" int __wrap_pthread_setschedparam(pthread_t thread, int policy,
                                           const sched_param* scheduling)
{
    requestedAudioPriority = policy == SCHED_RR && scheduling->sched_priority == 20 &&
                             !pthread_equal(thread, pthread_self());
    return EPERM; // Playback must still work without real-time permissions.
}

extern "C" int __wrap_snd_pcm_wait(snd_pcm_t*, int)
{
    std::unique_lock<std::mutex> lock(mutex);
    ++waits;
    changed.notify_all();
    changed.wait(lock, [] { return permits != 0 || finished; });
    if (finished)
        return 0;
    --permits;
    return 1;
}

extern "C" snd_pcm_sframes_t __wrap_snd_pcm_writei(snd_pcm_t*, const void*,
                                                  snd_pcm_uframes_t frames)
{
    std::unique_lock<std::mutex> lock(mutex);
    enteredWrite = true;
    changed.notify_all();
    changed.wait(lock, [] { return !holdWrite || finished; });
    return static_cast<snd_pcm_sframes_t>(frames);
}

extern "C" int __wrap_snd_pcm_status(snd_pcm_t* pcm, snd_pcm_status_t* status)
{
    const int result = __real_snd_pcm_status(pcm, status);
    std::unique_lock<std::mutex> lock(mutex);
    if (writeDuringStatus)
    {
        writeDuringStatus = false;
        const unsigned nextWait = waits + 1;
        ++permits;
        changed.notify_all();
        await(lock, nextWait); // the write and backend counter update completed
    }
    return result;
}

extern "C" snd_pcm_sframes_t __wrap_snd_pcm_status_get_delay(const snd_pcm_status_t*)
{
    return 441;
}

extern "C" void __wrap_snd_pcm_status_get_driver_htstamp(const snd_pcm_status_t*,
                                                         snd_htimestamp_t* stamp)
{
    stamp->tv_sec = elapsedDriverTimestamp ? 2 : 100;
    stamp->tv_nsec = 1;
}

extern "C" void __wrap_snd_pcm_status_get_htstamp(const snd_pcm_status_t*, snd_htimestamp_t* stamp)
{
    stamp->tv_sec = 100;
    stamp->tv_nsec = 1;
}

int main()
{
    multiroom::AlsaBackend backend;
    multiroom::AudioBackendConfig config;
    config.deviceId = "null";
    config.format = {44100, 2, multiroom::AudioFormat::SampleType::Int24In32};
    config.requestedBufferFrames = 441;
    config.renderCallback = render;
    if (!backend.open(config) || !backend.start())
    {
        std::fprintf(stderr, "backend open/start failed: %s\n", backend.lastError().c_str());
        return 1;
    }
    {
        std::unique_lock<std::mutex> lock(mutex);
        permits = 1;
        changed.notify_all();
        await(lock, 2);
        writeDuringStatus = true;
    }
    const bool rejectsCompletedWrite = !backend.clockSnapshot().isValid();
    const auto stable = backend.clockSnapshot();
    const bool acceptsStablePair = stable.isValid() && stable.deviceFramePosition == 441;
    elapsedDriverTimestamp = true;
    const auto elapsed = backend.clockSnapshot();
    const bool rejectsElapsedTimestamp = elapsed.isValid() && elapsed.monotonicTimeNs == 100000000001ULL &&
                                        elapsed.quality == multiroom::AudioClockQuality::Estimated;
    elapsedDriverTimestamp = false;
    {
        std::unique_lock<std::mutex> lock(mutex);
        holdWrite = true;
        enteredWrite = false;
        ++permits;
        changed.notify_all();
        timedOut |= !changed.wait_for(lock, std::chrono::seconds(2), [] { return enteredWrite; });
    }
    const bool rejectsInFlightWrite = !backend.clockSnapshot().isValid();
    {
        std::lock_guard<std::mutex> lock(mutex);
        finished = true;
        holdWrite = false;
        changed.notify_all();
    }
    backend.stop();
    std::printf("audio thread requests RR/20; permission denial preserves playback: %d\n",
                requestedAudioPriority);
    std::printf("completed-write race rejected: %d; stable pair accepted: %d; "
                "in-flight write rejected: %d; timeout: %d\n",
                rejectsCompletedWrite, acceptsStablePair, rejectsInFlightWrite, timedOut);
    std::printf("elapsed audio timestamp replaced with monotonic status time: %d\n", rejectsElapsedTimestamp);
    return requestedAudioPriority && rejectsElapsedTimestamp && rejectsCompletedWrite && acceptsStablePair &&
           rejectsInFlightWrite && !timedOut ? 0 : 1;
}
