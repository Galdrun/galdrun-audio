#pragma once

// Keeps playback alive across everything the hardware does to it.
//
// The backends report a dead device by going to BackendState::Failed -- a
// removed endpoint, AUDCLNT_E_DEVICE_INVALIDATED, an ALSA device that
// disappeared, a driver that stopped signalling. This is what acts on that: a
// controlled stop, then reopen attempts on a backoff, with an optional device
// rescan when the same device keeps refusing, so a client whose device has
// been replaced lands on the one that is actually there.
//
// Nothing here touches the audio thread, and poll() never blocks or sleeps: it
// is called from the same control loop that serves the control API, so a
// device that is gone cannot stall command handling and cannot become a busy
// loop either -- the next attempt has a deadline, and until it is due poll()
// does nothing.

#include "audio/AudioEngine.h"

#include <cstdint>
#include <functional>
#include <string>

namespace multiroom {

enum class PlaybackState
{
    Idle,      // nobody has asked for playback
    Playing,   // the device is running
    Recovering // asked for, not running: waiting out the backoff before the next attempt
};

const char* playbackStateName(PlaybackState state);

class PlaybackSupervisor
{
public:
    // Says what to open, freshly for every attempt: a rescan may have picked a
    // different device or format since the last one. Returning false means
    // there is nothing usable right now, which counts as a failed attempt
    // rather than an error -- a replugged device makes a later one succeed.
    using TargetProvider =
        std::function<bool(AudioEngine::Settings& settings, std::string& error)>;

    // Runs the capability scan and re-selects the device. Optional: without it
    // the supervisor only ever retries what the target provider names.
    using RescanCallback = std::function<bool(std::string& error)>;

    struct Hooks
    {
        // Called between open() and start() with the format the driver
        // granted, so whatever feeds the source can be set up and the buffer
        // prefilled before the device runs dry on its first period.
        std::function<void(const AudioFormat& format)> onOpened;

        // Called once the engine is closed again, so that producer can be
        // parked: the source's prepare() reallocates on the next attempt, and
        // AudioRingBuffer::configure() must not race with a push(). Fires if
        // and only if onOpened fired.
        std::function<void()> onClosed;
    };

    PlaybackSupervisor(AudioEngine& engine, TargetProvider target);

    PlaybackSupervisor(const PlaybackSupervisor&) = delete;
    PlaybackSupervisor& operator=(const PlaybackSupervisor&) = delete;

    void setRescan(RescanCallback rescan);
    void setHooks(Hooks hooks);

    // Asks for playback and makes the first attempt inside this call, so a
    // caller bringing hardware up learns straight away whether it worked.
    // Returns false when the device did not come up -- the state is then
    // Recovering and poll() keeps trying, so a caller that would rather wait
    // for the hardware can ignore the result and watch state() instead.
    bool start(uint64_t nowNs, std::string& error);

    // Controlled stop. Idempotent, and the only way back to Idle.
    void stop();

    // Call from the control loop. Notices a device that died, closes it, and
    // makes the next attempt once its backoff has expired. Returns true while
    // the device is running.
    bool poll(uint64_t nowNs);

    PlaybackState state() const { return state_; }

    // Why playback is not running. Empty while it is, so it is omitted from
    // the status response rather than reported as a stale message.
    const std::string& lastError() const { return lastError_; }

    // Consecutive failed attempts, back to zero on a successful one.
    uint32_t failedAttempts() const { return failedAttempts_; }

    // How often playback came back after a failure, over the client's life.
    uint32_t recoveries() const { return recoveries_; }

    // Milliseconds until the next attempt; 0 when one is due or when nothing
    // is pending.
    uint32_t retryInMs(uint64_t nowNs) const;

private:
    bool attempt(uint64_t nowNs);
    void noteFailure(const std::string& error);
    void scheduleRetry(uint64_t nowNs);
    void closePlayback();

    AudioEngine& engine_;
    TargetProvider target_;
    RescanCallback rescan_;
    Hooks hooks_;

    PlaybackState state_ = PlaybackState::Idle;
    std::string lastError_;
    // The message the last warning carried, so an unchanged failure drops to
    // debug instead of filling the log for as long as the device stays away.
    std::string loggedError_;

    uint32_t failedAttempts_ = 0;
    uint32_t recoveries_ = 0;
    size_t retryStep_ = 0;
    uint64_t nextAttemptNs_ = 0;
};

} // namespace multiroom
