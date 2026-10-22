#include "audio/PlaybackSupervisor.h"

#include "util/Logger.h"

namespace multiroom {
namespace {

// The same ladder the server connection climbs, for the same reason: a device
// that is gone for good must not be probed once a second forever, and a device
// that comes straight back must not wait half a minute for it.
const uint32_t kRetryMs[] = {1000, 2000, 5000, 10000, 30000};
const size_t kRetryCount = sizeof(kRetryMs) / sizeof(kRetryMs[0]);

// A replugged device usually keeps its endpoint id, so the first attempts are
// worth making against the device we already know. Only when those keep
// failing is the cache itself suspect -- and a full scan opens every device in
// turn, which is expensive enough on the target board that it must not be the
// first answer to a failure.
const uint32_t kRescanEveryAttempts = 3;

} // namespace

const char* playbackStateName(PlaybackState state)
{
    switch (state)
    {
    case PlaybackState::Idle:
        return "idle";
    case PlaybackState::Playing:
        return "playing";
    case PlaybackState::Recovering:
        return "recovering";
    }
    return "unknown";
}

PlaybackSupervisor::PlaybackSupervisor(AudioEngine& engine, TargetProvider target)
    : engine_(engine), target_(std::move(target))
{
}

void PlaybackSupervisor::setRescan(RescanCallback rescan)
{
    rescan_ = std::move(rescan);
}

void PlaybackSupervisor::setHooks(Hooks hooks)
{
    hooks_ = std::move(hooks);
}

bool PlaybackSupervisor::start(uint64_t nowNs, std::string& error)
{
    if (!target_)
    {
        error = "no playback target";
        return false;
    }
    if (state_ == PlaybackState::Playing)
        return true;

    failedAttempts_ = 0;
    retryStep_ = 0;
    nextAttemptNs_ = 0;
    lastError_.clear();
    loggedError_.clear();
    state_ = PlaybackState::Recovering;

    if (attempt(nowNs))
        return true;

    error = lastError_;
    return false;
}

void PlaybackSupervisor::stop()
{
    if (state_ == PlaybackState::Idle)
        return;

    // Only a device that was actually opened gets the closing hook; a stop
    // during recovery has nothing open and nothing to park.
    const bool wasOpen = engine_.isOpen();
    state_ = PlaybackState::Idle;
    if (wasOpen)
        closePlayback();

    failedAttempts_ = 0;
    retryStep_ = 0;
    nextAttemptNs_ = 0;
    lastError_.clear();
    loggedError_.clear();
}

bool PlaybackSupervisor::poll(uint64_t nowNs)
{
    switch (state_)
    {
    case PlaybackState::Idle:
        return false;

    case PlaybackState::Playing:
        if (engine_.isRunning())
            return true;
        // The render thread left Running on its own, which is the backends'
        // one way of saying the device is dead.
        noteFailure(engine_.backend() != nullptr ? engine_.backend()->lastError()
                                                 : std::string());
        closePlayback();
        scheduleRetry(nowNs);
        return false;

    case PlaybackState::Recovering:
        if (nowNs < nextAttemptNs_)
            return false;
        return attempt(nowNs);
    }
    return false;
}

uint32_t PlaybackSupervisor::retryInMs(uint64_t nowNs) const
{
    if (state_ != PlaybackState::Recovering || nextAttemptNs_ <= nowNs)
        return 0;
    return static_cast<uint32_t>((nextAttemptNs_ - nowNs) / 1000000ULL);
}

bool PlaybackSupervisor::attempt(uint64_t nowNs)
{
    if (rescan_ && failedAttempts_ != 0 && failedAttempts_ % kRescanEveryAttempts == 0)
    {
        std::string rescanError;
        if (rescan_(rescanError))
            MULTIROOM_INFO("audio devices rescanned while recovering playback");
        else
            MULTIROOM_WARN("cannot rescan audio devices: %s", rescanError.c_str());
    }

    AudioEngine::Settings settings;
    std::string error;
    if (!target_(settings, error))
    {
        noteFailure(error);
        scheduleRetry(nowNs);
        return false;
    }

    if (!engine_.open(settings, error))
    {
        noteFailure(error);
        scheduleRetry(nowNs);
        return false;
    }

    if (hooks_.onOpened)
        hooks_.onOpened(engine_.format());

    if (!engine_.start(error))
    {
        noteFailure(error);
        closePlayback();
        scheduleRetry(nowNs);
        return false;
    }

    if (failedAttempts_ != 0)
    {
        ++recoveries_;
        MULTIROOM_INFO("playback recovered after %lu failed attempt(s)",
                 static_cast<unsigned long>(failedAttempts_));
    }

    failedAttempts_ = 0;
    retryStep_ = 0;
    nextAttemptNs_ = 0;
    lastError_.clear();
    loggedError_.clear();
    state_ = PlaybackState::Playing;
    return true;
}

void PlaybackSupervisor::noteFailure(const std::string& error)
{
    // An empty message would leave the status saying playback is broken
    // without saying why, which is worse than a generic sentence.
    lastError_ = error.empty() ? "the audio device failed" : error;
    ++failedAttempts_;
    state_ = PlaybackState::Recovering;

    if (lastError_ != loggedError_)
    {
        MULTIROOM_WARN("playback unavailable: %s", lastError_.c_str());
        loggedError_ = lastError_;
    }
    else
    {
        // Same failure as last time. A client left pointing at a device that
        // is gone must not fill the log with it.
        MULTIROOM_DEBUG("playback still unavailable: %s", lastError_.c_str());
    }
}

void PlaybackSupervisor::scheduleRetry(uint64_t nowNs)
{
    const uint32_t delayMs = kRetryMs[retryStep_];
    nextAttemptNs_ = nowNs + static_cast<uint64_t>(delayMs) * 1000000ULL;
    if (retryStep_ + 1 < kRetryCount)
        ++retryStep_;
    MULTIROOM_DEBUG("retrying playback in %lu ms", static_cast<unsigned long>(delayMs));
}

void PlaybackSupervisor::closePlayback()
{
    engine_.stop();
    engine_.close();
    if (hooks_.onClosed)
        hooks_.onClosed();
}

} // namespace multiroom
