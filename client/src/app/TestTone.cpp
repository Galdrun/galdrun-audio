#include "app/TestTone.h"

#include "app/DeviceSelection.h"
#include "audio/AudioBackend.h"
#include "audio/AudioEngine.h"
#include "audio/AudioRingBuffer.h"
#include "audio/DriftEstimator.h"
#include "audio/PlaybackSupervisor.h"
#include "audio/RingBufferSource.h"
#include "audio/ToneGenerator.h"
#include "matp/Session.h"
#include "util/Logger.h"
#include "util/Time.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

namespace multiroom {
namespace {

// Half a second of audio, and blocks of five milliseconds at 48 kHz. Both only
// describe the buffered test tone; the server's own stream is sized from
// bufferMs instead (MATP section 58).
const uint32_t kToneRingBufferMs = 500;
const uint32_t kProducerBlockFrames = 240;

} // namespace

bool playTestTone(ClientRuntime& runtime, CommandQueue& queue, IControlHandler& handler,
                  uint32_t seconds, bool throughRingBuffer,
                  const volatile std::sig_atomic_t& stopRequested)
{
    // Declared before the engine, so they outlive it: the engine's destructor
    // stops a render thread that is still reading from them.
    ToneGenerator tone(440.0, 0.25);
    AudioRingBuffer ring;
    RingBufferSource ringSource(ring, kToneRingBufferMs);

    AudioEngine engine(createPlatformAudioBackend(runtime.config->i2s));
    if (throughRingBuffer)
        engine.setSource(&ringSource);
    else
        engine.setSource(&tone);

    // The producer side of the ring buffer, which is matp::AudioStream for a
    // real stream. It is spawned and joined around each open attempt
    // rather than parked across one: the source's prepare() reconfigures the
    // buffer, and a join is a cheaper barrier than making configure() safe
    // against a concurrent push().
    std::atomic<bool> producerStop(false);
    std::thread producer;
    bool ringUnusable = false;

    const auto startProducer = [&](const AudioFormat& format) {
        runtime.deviceFormat = format;
        if (!throughRingBuffer)
            return;
        if (!ring.isConfigured())
        {
            // prepare() already logged why.
            MULTIROOM_ERROR("no ring buffer for %s", runtime.deviceName.c_str());
            ringUnusable = true;
            return;
        }

        tone.prepare(format);
        const size_t blockBytes =
            static_cast<size_t>(kProducerBlockFrames) * ring.bytesPerFrame();

        // Fills the buffer before the device starts: a render callback that
        // arrives before the first frames do is an underrun by construction
        // and would say nothing about the code under test.
        std::vector<unsigned char> prefill(blockBytes);
        while (ring.writableFrames() >= kProducerBlockFrames)
        {
            tone.render(prefill.data(), kProducerBlockFrames);
            ring.push(prefill.data(), kProducerBlockFrames);
        }

        producerStop.store(false);
        producer = std::thread([&ring, &tone, &producerStop, blockBytes] {
            std::vector<unsigned char> block(blockBytes);
            while (!producerStop.load())
            {
                if (ring.writableFrames() < kProducerBlockFrames)
                {
                    // The consumer drains a period at a time, so a short sleep
                    // costs nothing here and leaves the core to the audio
                    // thread.
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    continue;
                }
                tone.render(block.data(), kProducerBlockFrames);
                ring.push(block.data(), kProducerBlockFrames);
            }
        });
    };

    const auto stopProducer = [&]() {
        runtime.deviceFormat = AudioFormat();
        producerStop.store(true);
        if (producer.joinable())
            producer.join();
    };

    PlaybackSupervisor playback(
        engine, [&runtime](AudioEngine::Settings& settings, std::string& error) {
            return currentPlaybackTarget(runtime, settings, error);
        });
    if (runtime.rescanAudio)
        playback.setRescan(runtime.rescanAudio);

    PlaybackSupervisor::Hooks hooks;
    hooks.onOpened = startProducer;
    hooks.onClosed = stopProducer;
    playback.setHooks(hooks);

    // A 20 second window sampled every 50 ms: 400 points, which resolves well
    // under a ppm on a sane clock.
    DriftEstimator drift;
    runtime.drift = &drift;

    MULTIROOM_INFO("playing a 440 Hz test tone for %lu second(s)%s",
             static_cast<unsigned long>(seconds),
             throughRingBuffer ? " through the ring buffer" : "");

    std::string error;
    if (!playback.start(monotonicNowNs(), error))
    {
        // Not fatal on its own: a device that is busy or still settling is
        // exactly what phase 8 is about, and the supervisor keeps trying for
        // as long as the tone was asked to run. Only a tone that never got a
        // device fails the run, and that is decided at the deadline.
        MULTIROOM_WARN("playback is not up yet: %s", error.c_str());
    }
    publishPlayback(runtime, playback, engine, monotonicNowNs());

    const uint64_t deadline = monotonicNowNs() + static_cast<uint64_t>(seconds) * 1000000000ULL;

    uint64_t lastUnderruns = 0;
    uint32_t lastRecoveries = 0;
    bool qualityLogged = false;

    while (stopRequested == 0 && !ringUnusable)
    {
        const uint64_t now = monotonicNowNs();
        if (now >= deadline)
            break;

        // Polled rather than slept through, so a device that dies mid-tone is
        // reopened instead of being waited out.
        const bool running = playback.poll(now);

        if (playback.recoveries() != lastRecoveries)
        {
            // A reopened device starts its position counter over, so every
            // sample from before the gap belongs to a different series.
            lastRecoveries = playback.recoveries();
            lastUnderruns = 0;
            qualityLogged = false;
            drift.reset();
        }

        if (running)
        {
            // An xrun drops frames the position counter already counted, which
            // puts a step into the series. Drift is a slope, so the resulting
            // offset is harmless, but a window spanning the step is not.
            const uint64_t underruns = engine.underruns();
            if (underruns != lastUnderruns)
            {
                drift.reset();
                lastUnderruns = underruns;
            }

            const AudioClockSnapshot snapshot = engine.clockSnapshot();
            if (snapshot.isValid())
            {
                if (!qualityLogged)
                {
                    MULTIROOM_INFO("audio clock quality: %s", audioClockQualityName(snapshot.quality));
                    qualityLogged = true;
                }
                drift.addSample(snapshot);
                runtime.lastClock = snapshot;
            }
        }

        publishPlayback(runtime, playback, engine, now);
        sampleSystem(runtime, now);

        // The handshake runs from this loop as well, so a tone and a server
        // connection can be brought up together.
        if (runtime.session != nullptr)
            runtime.session->poll(now);

        // Control commands are served from this loop too, so status reports
        // the live clock, drift and recovery figures while a tone is playing --
        // which is exactly when they are worth looking at.
        queue.drain(handler, 50);
    }

    // Read before the stop, which closes the device and takes its counters
    // with it.
    const uint64_t underruns = engine.underruns();
    const uint32_t renderedFrames = ringSource.renderedFrames();
    const uint32_t shortFrames = ringSource.underrunFrames();
    const uint32_t recoveries = playback.recoveries();
    const bool deviceDown = playback.state() != PlaybackState::Playing;
    const std::string failure = playback.lastError();

    playback.stop();
    stopProducer(); // a no-op unless the producer outlived the device

    runtime.playbackState = PlaybackState::Idle;
    runtime.playbackError.clear();
    runtime.playbackAttempts = 0;
    runtime.playbackRetryMs = 0;
    runtime.bufferFrames = 0;
    runtime.lastClock = AudioClockSnapshot();
    runtime.drift = nullptr;

    if (recoveries != 0)
    {
        // The per-stream counters below were reset by the last reopen, so say
        // so rather than letting them read as the whole run.
        MULTIROOM_INFO("playback recovered %lu time(s); the figures below cover the last stream only",
                 static_cast<unsigned long>(recoveries));
    }

    if (ringUnusable)
        return false;

    if (deviceDown)
    {
        MULTIROOM_ERROR("no playback device for the whole tone: %s", failure.c_str());
        return false;
    }

    if (throughRingBuffer)
    {
        // Frames the device could not be given are reported separately from
        // the backend's underrun count: one counts frames, the other counts
        // periods, and a mismatch between them is worth seeing.
        MULTIROOM_INFO("test tone finished: %lu frames played from the ring buffer, "
                 "%lu frame(s) short, %llu underrun(s)",
                 static_cast<unsigned long>(renderedFrames),
                 static_cast<unsigned long>(shortFrames),
                 static_cast<unsigned long long>(underruns));
    }
    else
    {
        MULTIROOM_INFO("test tone finished: %lu frames rendered, %llu underrun(s)",
                 static_cast<unsigned long>(tone.renderedFrames()),
                 static_cast<unsigned long long>(underruns));
    }

    if (!qualityLogged)
    {
        MULTIROOM_WARN("the device reported no usable clock; drift cannot be measured here");
        return true;
    }

    if (!drift.hasEstimate())
    {
        MULTIROOM_WARN("not enough usable clock samples for a drift estimate");
        return true;
    }

    MULTIROOM_INFO("device clock: %.3f Hz (%+.2f +/- %.2f ppm, %s, confidence %.2f)",
             drift.estimatedSampleRate(), drift.driftPpm(), drift.standardErrorPpm(),
             audioClockQualityName(drift.quality()), drift.confidence());
    MULTIROOM_INFO("  regression over %lu sample(s) spanning %.1f s, %llu rejected",
             static_cast<unsigned long>(drift.sampleCount()), drift.windowSpanSeconds(),
             static_cast<unsigned long long>(drift.rejectedCount()));

    return true;
}

} // namespace multiroom
