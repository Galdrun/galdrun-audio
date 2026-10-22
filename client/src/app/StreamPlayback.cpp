#include "app/StreamPlayback.h"

#include "audio/AudioBackend.h"
#include "config/ClientConfig.h"
#include "util/Logger.h"
#include "util/Time.h"
#if defined(__linux__) && !defined(ESP_PLATFORM)
#include "config/BoardConfig.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>

namespace multiroom {
namespace {

// Track boundaries the server has announced for frames the room has not
// reached yet. Not a protocol limit: it is what keeps a server that announces
// a whole playlist at once from growing the queue without end.
const size_t kMaxPendingBoundaries = 64;

// How long the first packet after a flush waits for the FLUSH it follows
// (MATP section 54). The two are sent back to back on two connections, so
// anything near this is not a race any more: it is a server that flagged a
// packet and never sent the message, and a room waiting for it would be
// silent for good.
const uint64_t kFlushWaitNs = 1000000000ULL;

// How often a hardware volume control is read back for the status. It is a
// mixer read on the control thread, and a person turning a knob does not need
// it more often than this.
const uint64_t kHardwareVolumeReadNs = 1000000000ULL;

// The ring buffer's capacity in frames, the way TimelineSource::prepare()
// asks for it: rounded up, so a capacity in milliseconds never becomes zero.
uint64_t capacityFrames(const AudioFormat& format, uint32_t bufferMs)
{
    return (static_cast<uint64_t>(format.sampleRate) * bufferMs + 999ULL) / 1000ULL;
}

// Whether opening the device puts its volume control back to 0 dB when the
// room's volume does not use it. Only on boards whose control is the room's
// own DAC: an ESP32 build, or Linux firmware whose board file opts in. On a
// desktop or a plain Pi it may be a level someone set, so it is left alone.
bool resetsHardwareVolumeOnOpen()
{
#if defined(ESP_PLATFORM)
    return true;
#elif defined(__linux__)
    return board_config::enabled(board_config::current(), "resetHardwareVolume");
#else
    return false;
#endif
}

// The board's ceiling as a linear gain, from its board file's maxVolumeDb: for
// an analog stage that clips before the DAC's full scale (Connector 2's clips
// about 9 dB under it). It caps the hardware control on every path, Disabled
// included, so no room setting can overdrive the board; the samples carry it
// only when there is no hardware control to hold it.
double boardGainLimit()
{
#if defined(__linux__) && !defined(ESP_PLATFORM)
    static const double limit = [] {
        double db = 0.0;
        if (json::getDouble(board_config::current(), "maxVolumeDb", db) && db < 0.0)
            return std::pow(10.0, db / 20.0);
        return 1.0;
    }();
    return limit;
#else
    return 1.0;
#endif
}

} // namespace

bool streamBufferFits(const AudioFormat& format, uint32_t bufferMs, std::string& error)
{
    const uint64_t frames = capacityFrames(format, bufferMs);
    const uint64_t bytes =
        frames > UINT32_MAX
            ? 0
            : AudioRingBuffer::storageBytes(format.bytesPerFrame(), static_cast<uint32_t>(frames));
    const uint64_t budget = platformRingBufferBudgetBytes();
    if (bytes != 0 && (budget == 0 || bytes <= budget))
        return true;

    error = "a " + std::to_string(bufferMs) + " ms buffer of " + describeFormat(format);
    if (bytes == 0)
        error += " is larger than the ring buffer can be";
    else
        error += " needs " + std::to_string(bytes) + " bytes of ring buffer, and this client has " +
                 std::to_string(budget);
    return false;
}

StreamPlayback::StreamPlayback(ClientRuntime& runtime, matp::Identity identity,
                               uint32_t bufferMs)
    : StreamPlayback(runtime, std::move(identity), bufferMs,
                     createPlatformAudioBackend(runtime.config->i2s))
{
}

StreamPlayback::StreamPlayback(ClientRuntime& runtime, matp::Identity identity,
                               uint32_t bufferMs, std::unique_ptr<IAudioBackend> backend)
    : runtime_(runtime), identity_(std::move(identity)), bufferMs_(bufferMs),
      source_(buffer_, bufferMs),
      engine_(std::move(backend)), stream_(connection_, buffer_),
      supervisor_(engine_, [this](AudioEngine::Settings& settings, std::string& error) {
          return playbackTarget(settings, error);
      })
{
    engine_.setSource(&source_);
    source_.setDeclick(runtime_.config->declick);

    PlaybackSupervisor::Hooks hooks;
    hooks.onOpened = [this](const AudioFormat& granted) { onDeviceOpened(granted); };
    hooks.onClosed = [this]() { onDeviceClosed(); };
    supervisor_.setHooks(hooks);

    // A device that keeps refusing is eventually the cache being wrong rather
    // than the device being busy, and the rescan re-reports capabilities so
    // the server can renegotiate a format the new device can actually play.
    if (runtime_.rescanAudio)
        supervisor_.setRescan(runtime_.rescanAudio);

    updateVolumePath(false);
}

StreamPlayback::~StreamPlayback()
{
    // Before anything else is destroyed: the connection thread is the one
    // that calls into the stream, and joining it is the only way to know
    // it is not in there now. Detached first: a thread waiting for room in a
    // full buffer that nothing drains (paused, prebuffering) only leaves that
    // wait when accepting_ drops, and the join would otherwise never return.
    stream_.detach();
    connection_.stop();

    // A trim must not outlive the process that was told to hold it.
    closeHardwareClock();

    // The runtime outlives this object, and the estimator does not.
    runtime_.drift = nullptr;
}

bool StreamPlayback::start(std::string& error)
{
    return connection_.start(
        [this](const void* data, size_t size) { stream_.onData(data, size); }, error);
}

void StreamPlayback::prepare(const matp::PrepareStream& request, const AudioFormat& negotiated,
                             matp::StreamReady& ready)
{
    release();

    if (!runtime_.deviceSelected)
    {
        ready.reason = "no usable audio playback device";
        return;
    }

    std::string address;
    if (!audioAddress(request, address))
    {
        ready.reason = "no address for the audio connection";
        return;
    }

    target_ = AudioEngine::Settings();
    target_.deviceId = runtime_.deviceId;
    target_.format = negotiated;
    target_.accessMode = runtime_.accessMode;
    hasTarget_ = true;

    // Section 21: the device runs from here on, feeding the sound card
    // silence, so that its clock is established and its buffer primed by
    // the time START_AT names an instant. A device that opens but will
    // not run is not ready, and saying so now is the whole point of
    // section 12's rule about answering late rather than optimistically.
    //
    // The first attempt is the answer to PREPARE_STREAM and the supervisor is
    // stopped when it fails: the server has just been told this room is not
    // ready, and asking again is its decision (section 12 sends the capability
    // list with the refusal for exactly that). Recovery is for a device that
    // dies *after* a stream the server believes in.
    std::string error;
    if (!supervisor_.start(monotonicNowNs(), error))
    {
        release();
        ready.reason = error;
        return;
    }

    // Sized by the source's prepare() from the format the driver granted,
    // which is the only moment the frame size is known for certain. Checked
    // here rather than in the hook that attaches the stream, because a hook
    // cannot refuse an attempt and this has to refuse the whole stream.
    if (!buffer_.isConfigured())
    {
        release();
        ready.reason = "the audio ring buffer could not be sized for this format";
        return;
    }

    if (!connection_.setServer(address, error))
    {
        release();
        ready.reason = error;
        return;
    }

    // A trim that could not be returned to nominal leaves the room's rate
    // unknown, and neither actuator can plan around that: held, and said so.
    if (hardwareClock_ && !clockNominal_)
    {
        const std::string reason = clockError_;
        release();
        ready.reason = reason;
        return;
    }

    ready.ready = true;
    ready.format = engine_.format();
    ready.periodFrames = engine_.bufferFrames();
    ready.hasDriftCorrection = true;
    ready.driftCorrection = hardwareClockActive() ? matp::DriftCorrectionMode::HardwareClock
                                                  : matp::DriftCorrectionMode::Resampling;
}

bool StreamPlayback::bufferFits(uint32_t bufferMs, std::string& error) const
{
    if (!hasTarget_)
        return true;
    // What the driver granted while the device runs, what the server chose
    // while it is being recovered: the next attempt opens that.
    const AudioFormat format = engine_.isRunning() ? engine_.format() : target_.format;
    return streamBufferFits(format, bufferMs, error);
}

void StreamPlayback::setBufferMs(uint32_t bufferMs)
{
    bufferMs_ = bufferMs;
    source_.setCapacityMilliseconds(bufferMs);

    // No stream, or a device the supervisor is still bringing back: the next
    // open sizes the buffer from the values above.
    if (!hasTarget_ || !engine_.isRunning() || !buffer_.isConfigured())
        return;

    // The capacity is a power of two frames, so a nearby size is often the
    // buffer that is open already.
    const AudioFormat format = engine_.format();
    const uint64_t frames = capacityFrames(format, bufferMs);
    const uint64_t bytes =
        frames > UINT32_MAX
            ? 0
            : AudioRingBuffer::storageBytes(format.bytesPerFrame(), static_cast<uint32_t>(frames));
    if (bytes == static_cast<uint64_t>(buffer_.capacityFrames()) * format.bytesPerFrame())
    {
        MULTIROOM_INFO("buffer now %lu ms, the ring buffer it already has",
                 static_cast<unsigned long>(bufferMs));
        return;
    }

    // The buffer cannot be resized under the render thread and the connection
    // thread, and closing the device is what parks both. The supervisor
    // reopens it, or keeps trying on its backoff if the device refuses.
    MULTIROOM_INFO("buffer now %lu ms; reopening the device with a ring buffer of that size",
             static_cast<unsigned long>(bufferMs));
    supervisor_.stop();
    std::string error;
    if (!supervisor_.start(monotonicNowNs(), error))
        MULTIROOM_WARN("the device did not reopen; retrying: %s", error.c_str());
    publishTimeline(false);
    publishPlayback(runtime_, supervisor_, engine_, monotonicNowNs());
}

bool StreamPlayback::playbackTarget(AudioEngine::Settings& settings, std::string& error) const
{
    if (!hasTarget_)
    {
        error = "no stream is prepared";
        return false;
    }
    if (!runtime_.deviceSelected)
    {
        error = "no usable audio playback device";
        return false;
    }

    settings = target_;
    // Re-read rather than remembered: a rescan during recovery may have landed
    // on a different device, and the next attempt has to use that one. The
    // format is not re-read, because the server chose it and is sending it.
    settings.deviceId = runtime_.deviceId;
    settings.accessMode = runtime_.accessMode;
    return true;
}

void StreamPlayback::onDeviceOpened(const AudioFormat& granted)
{
    runtime_.deviceFormat = granted;

    // The device's own volume control where it has one, whichever path ends
    // up using it: Hardware plays the gain on it, Disabled only its mute. A
    // room whose device changed carries its volume across to the path the new
    // one resolves to; one the server never set leaves the control where it
    // was.
    hardwareVolume_.reset();
    hardwareInfo_ = HardwareVolumeInfo();
    hardwarePending_ = false;
    hardwareReadNs_ = 0;
    const AudioDeviceInfo* device = runtime_.deviceCache != nullptr
                                        ? runtime_.deviceCache->findById(runtime_.deviceId)
                                        : nullptr;
    if (device != nullptr && device->hardwareVolume.available && engine_.backend() != nullptr)
    {
        hardwareVolume_ = engine_.backend()->openHardwareVolume(*device);
        if (hardwareVolume_)
        {
            hardwareInfo_ = device->hardwareVolume;
            MULTIROOM_INFO("volume: hardware control \"%s\"", hardwareInfo_.control.c_str());
        }
    }
    deviceOpen_ = true;
    updateVolumePath(true);
    openHardwareClock(device);

    // A format the ring buffer could not be sized for leaves nothing to
    // produce into. prepare() turns that into a refused stream; here it is
    // simply not attached, and the buffer stays empty.
    if (!buffer_.isConfigured())
        return;

    const std::string sessionId =
        runtime_.session != nullptr ? runtime_.session->sessionId() : std::string();
    // The session's identity rather than the one this was built with: a
    // setClientId since then has changed it, and STREAM_ATTACH has to name
    // the client the HELLO did.
    const matp::Identity& identity =
        runtime_.session != nullptr ? runtime_.session->identity() : identity_;
    stream_.attach(sessionId, identity, granted);

    // A reopened device starts its position counter over, so nothing measured
    // on the last one belongs to the series this one begins.
    drift_.reset();
    lastUnderruns_ = engine_.underruns();
    runtime_.drift = &drift_;
}

void StreamPlayback::onDeviceClosed()
{
    runtime_.deviceFormat = AudioFormat();

    // The stream is the ring buffer's only producer and the buffer is about to
    // be resized by the next attempt's prepare(); detach() returns once the
    // connection thread is out of it.
    stream_.detach();

    // The device's frame axis began again, so the start that was placed on it
    // names a moment that will never come round, and the audio buffered for
    // that timeline belongs to a room that is no longer playing it. Both go.
    // What replaces them is a new START_AT -- the server sees the reports stop
    // and the buffer report itself anew, which is section 43's rejoin arriving
    // through the same door a reconnected client comes through.
    source_.cancel();
    hasStartTimeline_ = false;
    startTimelineNs_ = 0;
    endTimelineNs_ = 0;
    rejoinArmed_ = false;
    hasFlush_ = false;
    flushResumeArmed_ = false;
    flushWaiting_ = false;

    // A hardware change still waiting for its frame is not lost: volumeGain_
    // already holds it, and the next device opened is set to that.
    hardwareVolume_.reset();
    hardwareInfo_ = HardwareVolumeInfo();
    hardwarePending_ = false;
    deviceOpen_ = false;
    updateVolumePath(false);
    closeHardwareClock();

    runtime_.drift = nullptr;
    drift_.reset();
}

void StreamPlayback::release()
{
    // Stops the device if one is open, which fires the closing hook: that is
    // what detaches the stream and drops the timeline. Nothing below it is
    // conditional, because a stream that never got as far as a device is
    // released through here too.
    hasTarget_ = false;
    supervisor_.stop();

    stream_.detach(); // returns once the connection thread is out of the buffer
    std::string error;
    connection_.setServer("", error);
    source_.cancel();
    runtime_.drift = nullptr;
    drift_.reset();
    rejoinArmed_ = false;
    hasFlush_ = false;
    flushResumeArmed_ = false;
    flushWaiting_ = false;
    hasStartTimeline_ = false;
    startTimelineNs_ = 0;
    endTimelineNs_ = 0;

    // The track metadata belongs to the timeline the stream was carrying, and
    // that timeline is gone with it.
    pendingTracks_.clear();
    hasTrack_ = false;
    trackTimelineKnown_ = false;
    track_ = matp::TrackBoundary();
    publishTimeline(false);
    publishPlayback(runtime_, supervisor_, engine_, monotonicNowNs());
}

bool StreamPlayback::start(const matp::StartAt& request, uint64_t localStartNs,
                           std::string& error)
{
    if (!engine_.isRunning())
    {
        error = "the audio device is not running";
        return false;
    }
    if (!stream_.isAttached())
    {
        error = "the audio connection is not attached";
        return false;
    }

    const matp::BufferStatus buffered = stream_.status();
    if (!buffered.hasContent)
    {
        // Nothing has arrived, so there is no timeline position to align
        // the buffer against. Section 20 has the server wait for a buffer
        // status before it schedules anything, so this is a start that
        // came too early rather than a client that is behind.
        error = "no audio has arrived yet";
        return false;
    }

    // The server names a position on the media timeline; the ring buffer
    // holds this room's own frames, at this room's own rate. Which of those
    // the start falls on is the only thing that ties the two axes together,
    // and only the packets this room received can say -- which is why it is a
    // lookup and not a division by a rate.
    uint64_t startStreamFrame = 0;
    if (!timelineToStream(request.timelineNs, startStreamFrame))
    {
        error = "timeline " + std::to_string(request.timelineNs) +
                " ns is older than the audio this room still holds";
        return false;
    }

    const AudioClockSnapshot snapshot = engine_.clockSnapshot();
    if (!source_.scheduleStart(snapshot, localStartNs, startStreamFrame,
                               stream_.bufferHeadStreamFrame(), error))
        return false;

    hasStartTimeline_ = true;
    startTimelineNs_ = request.timelineNs;

    publishTimeline(false);
    MULTIROOM_INFO("playback armed: timeline %llu ns (stream frame %llu) at device frame %llu, "
             "%llu ms buffered",
             static_cast<unsigned long long>(request.timelineNs),
             static_cast<unsigned long long>(source_.startStreamFrame()),
             static_cast<unsigned long long>(source_.startDeviceFrame()),
             static_cast<unsigned long long>(buffered.bufferedDurationNs / 1000000ULL));
    return true;
}

bool StreamPlayback::timelineToStream(uint64_t timelineNs, uint64_t& streamFrame) const
{
    return stream_.timelineToStream(timelineNs, streamFrame);
}

bool StreamPlayback::streamToTimeline(uint64_t streamFrame, uint64_t& timelineNs) const
{
    return stream_.streamToTimeline(streamFrame, timelineNs);
}


bool StreamPlayback::pause(const matp::TransportAt& request, std::string& error)
{
    if (!engine_.isRunning())
    {
        error = "the audio device is not running";
        return false;
    }

    // A timeline position this room can no longer place is one it has already
    // played past, so nothing of that timeline is left to cut: dropping the
    // schedule is what section 37 wants, and it is what schedulePause does
    // with a frame that precedes the start.
    uint64_t streamFrame = 0;
    if (!timelineToStream(request.timelineNs, streamFrame))
        streamFrame = 0;

    source_.schedulePause(streamFrame);
    endTimelineNs_ = request.timelineNs;
    publishTimeline(false);
    logTransport("pause", request.timelineNs);
    return true;
}

bool StreamPlayback::stop(const matp::TransportAt& request, std::string& error)
{
    if (!engine_.isRunning())
    {
        error = "the audio device is not running";
        return false;
    }

    uint64_t streamFrame = 0;
    if (!timelineToStream(request.timelineNs, streamFrame))
        streamFrame = 0;

    source_.scheduleStop(streamFrame);
    endTimelineNs_ = request.timelineNs;
    publishTimeline(false);
    logTransport("stop", request.timelineNs);
    return true;
}

bool StreamPlayback::flush(const matp::TransportAt& request, std::string& error)
{
    if (!engine_.isRunning())
    {
        error = "the audio device is not running";
        return false;
    }

    // A cut this room can no longer place is behind it, and TimelineSource
    // makes it at once, as section 54 asks.
    uint64_t streamFrame = 0;
    if (!timelineToStream(request.timelineNs, streamFrame))
        streamFrame = 0;

    // Read before the cut is published, so the count moving is the cut.
    flushDrops_ = source_.bufferDrops();
    source_.scheduleFlush(streamFrame);
    hasFlush_ = true;
    flushTimelineNs_ = request.timelineNs;
    flushResumeArmed_ = false;
    if (!source_.hasScheduledEnd())
        endTimelineNs_ = 0;

    // Boundaries at or after the cut belong to the audio it drops; the server
    // announces the replacement's own after the FLUSH. The queue is ordered.
    pendingTracks_.erase(std::lower_bound(pendingTracks_.begin(), pendingTracks_.end(),
                                          request.timelineNs,
                                          [](const matp::TrackBoundary& boundary, uint64_t cut)
                                          { return boundary.timelineNs < cut; }),
                         pendingTracks_.end());

    publishTimeline(false);
    MULTIROOM_INFO("flush armed: timeline %llu ns (stream frame %llu)",
             static_cast<unsigned long long>(request.timelineNs),
             static_cast<unsigned long long>(streamFrame));
    return true;
}

void StreamPlayback::logTransport(const char* what, uint64_t timelineNs)
{
    if (source_.isScheduled())
    {
        MULTIROOM_INFO("%s armed: timeline %llu ns (stream frame %llu) at device frame %llu", what,
                 static_cast<unsigned long long>(timelineNs),
                 static_cast<unsigned long long>(source_.endStreamFrame()),
                 static_cast<unsigned long long>(source_.endDeviceFrame()));
        return;
    }

    // No start survived it, so there was nothing to cut at a frame: either the
    // room was not playing or it was armed for a timeline the server has just
    // taken back.
    MULTIROOM_INFO("%s at timeline %llu ns: nothing was scheduled to play from there, "
             "so the schedule was dropped",
             what, static_cast<unsigned long long>(timelineNs));
}

bool StreamPlayback::volume(const matp::SetVolume& request, std::string&)
{
    if (request.hasValue && request.value > 1.0)
        MULTIROOM_WARN("volume %.3f is above unity and was clamped to 1.0: a fixed-point "
                 "stream has no headroom to amplify into",
                 request.value);

    // A message that carries only one of the two keeps the other: that is what
    // makes a mute reversible and a volume change during a mute meaningful.
    const double gain =
        hardware_volume::clampGain(request.hasValue ? request.value : volumeGain_);
    const bool muted = request.hasMuted ? request.muted : volumeMuted_;
    volumeSet_ = true;
    volumeGain_ = gain;
    volumeMuted_ = muted;
    const char* const path = matp::volumeControlModeName(volumePath_.effective);

    if (!engine_.isRunning())
    {
        // No device to play it on -- none yet, or one the supervisor is
        // bringing back. Remembered, and the next device opened is set to it
        // (updateVolumePath); the position it was meant for is gone with the
        // device's frame axis.
        hardwarePending_ = false;
        MULTIROOM_INFO("volume %.3f%s kept for when the audio device opens", gain,
                 muted ? " (muted)" : "");
        publishVolume();
        return true;
    }

    uint64_t streamFrame = 0;
    const bool placed = request.hasApplyAtTimelineNs &&
                        timelineToStream(request.applyAtTimelineNs, streamFrame);

    if (placed && usesHardware())
    {
        // Held for poll(): a mixer cannot be told a frame. Like the render
        // path's mailbox, a newer change replaces one that is still waiting.
        uint64_t deviceFrame = 0;
        if (source_.deviceFrameForStream(streamFrame, deviceFrame))
        {
            hardwarePending_ = true;
            hardwarePendingStreamFrame_ = streamFrame;
            hardwarePendingGain_ = gain;
            hardwarePendingMuted_ = muted;
            MULTIROOM_INFO("volume %.3f%s armed for timeline %llu ns (%s)", gain,
                     muted ? " (muted)" : "",
                     static_cast<unsigned long long>(request.applyAtTimelineNs), path);
            publishTimeline(false);
            return true;
        }
    }
    else if (placed && source_.setVolumeAt(softwareGain(gain), muted, streamFrame))
    {
        MULTIROOM_INFO("volume %.3f%s armed for timeline %llu ns (%s)", gain, muted ? " (muted)" : "",
                 static_cast<unsigned long long>(request.applyAtTimelineNs), path);
        publishTimeline(false);
        return true;
    }

    if (request.hasApplyAtTimelineNs)
    {
        // Nothing maps that position onto this device: the room has no start
        // yet, the position is already behind it, or the audio it belongs to
        // has been played and forgotten. Applying now beats leaving the room
        // at a volume the server did not ask for.
        MULTIROOM_WARN("volume change for timeline %llu ns applies at once: this room has no "
                 "playing timeline to place it on",
                 static_cast<unsigned long long>(request.applyAtTimelineNs));
    }

    hardwarePending_ = false;
    applyVolume(gain, muted);
    MULTIROOM_INFO("volume %.3f%s (%s)", gain, muted ? " (muted)" : "",
             matp::volumeControlModeName(volumePath_.effective));
    publishTimeline(false);
    return true;
}

void StreamPlayback::updateVolumePath(bool deviceOpened)
{
    const matp::VolumeControlMode configured =
        runtime_.config != nullptr ? runtime_.config->volumeControl
                                   : matp::VolumeControlMode::Auto;
    const VolumePath path =
        resolveVolumePath(runtime_.audioControl, configured, hardwareAvailable());
    const bool changed = path != volumePath_;
    if (changed)
        MULTIROOM_INFO("volume control %s: %s%s%s", matp::volumeControlModeName(configured),
                 matp::volumeControlModeName(path.effective), path.reason.empty() ? "" : " -- ",
                 path.reason.c_str());

    if (changed || deviceOpened)
    {
        // The device's control goes back to 0 dB (or the board's ceiling,
        // boardGainLimit()) when it stops playing the
        // room's gain: the path that follows plays over it, and would
        // otherwise attenuate twice. Boards that opt in also reset it on
        // opening, for a level restored from saved DAC settings. A mute on
        // the device's switch is taken off when the switch stops being the
        // room's; Disabled keeps using it.
        // One that fails is owed until it succeeds: the next volume change and
        // the status poll try it again, and a level still owed stays owed
        // through a further change to a path that is not Hardware.
        const bool levelOwed = !deviceOpened && hardwareReset_ == HardwareReset::Level;
        hardwareReset_ = HardwareReset::None;
        if (hardwareVolume_ && path.effective != matp::VolumeControlMode::Hardware &&
            (deviceOpened ? resetsHardwareVolumeOnOpen()
                          : levelOwed || volumePath_.effective == matp::VolumeControlMode::Hardware))
            hardwareReset_ = HardwareReset::Level;
        else if (!deviceOpened && usesHardware() && hardwareInfo_.hasMute &&
                 path.effective != matp::VolumeControlMode::Disabled &&
                 path.effective != matp::VolumeControlMode::Hardware)
            hardwareReset_ = HardwareReset::Mute;

        // The new path first: a level reset keeps the mute that path needs,
        // the room's own, which the path is set to at once below.
        volumePath_ = path;
        appliedMuted_ = volumeMuted_;
        std::string error;
        if (!resetHardwareVolume(error))
            MULTIROOM_WARN("volume: %s; the reset is tried again", error.c_str());

        hardwarePending_ = false;
        // Nothing but the path now in force scales the samples.
        source_.setVolume(1.0, false);
        if (volumeSet_)
            applyVolume(volumeGain_, volumeMuted_);
    }

    runtime_.volumePath = volumePath_;
    publishVolume();
    if (changed && runtime_.session != nullptr)
        runtime_.session->volumeConfigChanged();
}

bool StreamPlayback::hardwareAvailable() const
{
    // An open device answers for itself: its control opened or it did not.
    // Otherwise the scan's word for the selected device, so the path the room
    // will take can be reported before anything plays.
    if (deviceOpen_)
        return hardwareVolume_ != nullptr;
    const AudioDeviceInfo* device = runtime_.deviceCache != nullptr && runtime_.deviceSelected
                                        ? runtime_.deviceCache->findById(runtime_.deviceId)
                                        : nullptr;
    return device != nullptr && device->hardwareVolume.available;
}

bool StreamPlayback::usesHardware() const
{
    return hardwareVolume_ &&
           (volumePath_.effective == matp::VolumeControlMode::Hardware ||
            (volumePath_.effective == matp::VolumeControlMode::Disabled && hardwareInfo_.hasMute));
}

double StreamPlayback::softwareGain(double gain) const
{
    if (volumePath_.effective != matp::VolumeControlMode::Software)
        return 1.0;
    // The board's ceiling is on the hardware control while there is one.
    return hardwareVolume_ ? gain : gain * boardGainLimit();
}

bool StreamPlayback::resetHardwareVolume(std::string& error)
{
    if (!hardwareVolume_)
        hardwareReset_ = HardwareReset::None;
    if (hardwareReset_ == HardwareReset::None)
        return true;
    // Disabled plays the room's mute on the device's switch: a level reset
    // made late, from the poll, must not take that mute off, nor bring in a
    // scheduled one before its frame.
    const bool keepMuted = volumePath_.effective == matp::VolumeControlMode::Disabled &&
                           hardwareInfo_.hasMute && appliedMuted_;
    const bool reset = hardwareReset_ == HardwareReset::Level
                           ? hardwareVolume_->set(boardGainLimit(), keepMuted, error)
                           : hardwareVolume_->setMute(false, error);
    if (reset)
    {
        hardwareReset_ = HardwareReset::None;
        hardwareReadNs_ = 0;
    }
    return reset;
}

void StreamPlayback::applyVolume(double gain, bool muted)
{
    appliedMuted_ = muted;
    std::string error;
    if (!resetHardwareVolume(error))
        MULTIROOM_DEBUG("volume: %s; the reset is still owed", error.c_str());
    error.clear();
    switch (volumePath_.effective)
    {
    case matp::VolumeControlMode::Hardware:
        if (!hardwareVolume_)
            return; // no device open: the next one is set when it opens
        if (hardwareVolume_->set(gain * boardGainLimit(), muted, error))
        {
            // A control's lowest step need not be silence, and a gain of 0
            // leaves its switch on: the render path makes either one silent.
            source_.setVolume(1.0, muted || gain <= 0.0);
            hardwareReadNs_ = 0; // show what the control made of it
            return;
        }
        break;
    case matp::VolumeControlMode::Disabled:
        // The gain is not played; the mute is -- on the device's own switch
        // where it has one, as silence where it has not.
        if (hardwareVolume_ && hardwareInfo_.hasMute)
        {
            if (hardwareVolume_->setMute(muted, error))
            {
                hardwareReadNs_ = 0;
                return;
            }
            break;
        }
        source_.setVolume(1.0, muted);
        return;
    case matp::VolumeControlMode::Software:
    case matp::VolumeControlMode::Auto: // never resolved to
        source_.setVolume(softwareGain(gain), muted);
        return;
    }

    // The control refused. Before the room goes on without it, the device is
    // put back where a path change leaves it (updateVolumePath): unmuted, and
    // under Hardware at its ceiling, so a mute or attenuation the room put
    // there does not outlive the path that could see it.
    std::string resetError;
    const bool reset = volumePath_.effective == matp::VolumeControlMode::Hardware
                           ? hardwareVolume_->set(boardGainLimit(), false, resetError)
                           : hardwareVolume_->setMute(false, resetError);
    if (!reset)
    {
        HardwareVolumeState state;
        std::string readError;
        if (!hardwareVolume_->read(state, readError) || state.muted)
        {
            // Muted, or not known not to be: no other path can make it heard,
            // and giving the control up would report a room that plays. Kept,
            // so its state is what the status shows and the next change tries
            // it again.
            MULTIROOM_WARN("volume: %s; the hardware control is kept, as the device may "
                     "still be muted",
                     error.c_str());
            hardwareReadNs_ = 0;
            return;
        }
        MULTIROOM_WARN("volume: the device keeps its own level %.3f under the path that "
                 "follows: %s",
                 state.gain, resetError.c_str());
    }

    // The room goes on without it, on whatever path is left -- which under
    // bitPerfect is never software.
    MULTIROOM_WARN("volume: %s; the hardware control is given up", error.c_str());
    hardwareVolume_.reset();
    hardwareInfo_ = HardwareVolumeInfo();
    hardwarePending_ = false;
    const VolumePath before = volumePath_;
    updateVolumePath(false);

    // An explicit Disabled is Disabled with or without the control, so the
    // path did not change and updateVolumePath() applied nothing: the mute the
    // switch refused would be lost. Applied again, it lands on the samples.
    if (volumePath_ == before)
        applyVolume(gain, muted);
}

uint32_t StreamPlayback::waitMs(uint32_t maxMs) const
{
    // Checked every few milliseconds while one waits, rather than predicted
    // from a clock snapshot that may itself be a poll old.
    const uint32_t kHardwarePendingWaitMs = 5;
    return usesHardware() && hardwarePending_ &&
                   !source_.waitsForResume(hardwarePendingStreamFrame_)
               ? std::min(maxMs, kHardwarePendingWaitMs)
               : maxMs;
}

void StreamPlayback::pollHardwareVolume(uint64_t nowNs)
{
    // Not while a pause or a stop holds the timeline short of it: the device
    // runs on, its timeline does not, and the resume's start places it again.
    if (usesHardware() && hardwarePending_ &&
        !source_.waitsForResume(hardwarePendingStreamFrame_))
    {
        // The sound card's own position when there is one, which is when the
        // frame is heard; the render cursor otherwise, which is when it is
        // written. A start that resumed past the frame has played it already.
        const AudioClockSnapshot& clock = runtime_.lastClock;
        const uint64_t position =
            clock.isValid() ? clock.deviceFramePosition : source_.submittedFrames();
        uint64_t deviceFrame = 0;
        if (!source_.deviceFrameForStream(hardwarePendingStreamFrame_, deviceFrame) ||
            position >= deviceFrame)
        {
            hardwarePending_ = false;
            applyVolume(hardwarePendingGain_, hardwarePendingMuted_);
            MULTIROOM_INFO("volume %.3f%s applied at device frame %llu (%s)", hardwarePendingGain_,
                     hardwarePendingMuted_ ? " (muted)" : "",
                     static_cast<unsigned long long>(position),
                     matp::volumeControlModeName(volumePath_.effective));
        }
    }

    if (hardwareVolume_ && nowNs >= hardwareReadNs_)
    {
        hardwareReadNs_ = nowNs + kHardwareVolumeReadNs;
        HardwareVolumeState state;
        std::string error;
        if (!resetHardwareVolume(error))
            MULTIROOM_DEBUG("volume: %s; the reset is still owed", error.c_str());
        error.clear();
        runtime_.hasHardwareVolumeState = hardwareVolume_->read(state, error);
        if (runtime_.hasHardwareVolumeState)
            runtime_.hardwareVolumeState = state;
        else
            MULTIROOM_DEBUG("volume: %s", error.c_str());
    }
    publishVolume();
}

void StreamPlayback::publishVolume()
{
    runtime_.volumeGain = volumeGain_;
    runtime_.volumeMuted = volumeMuted_;
    runtime_.hardwareVolumeActive =
        hardwareVolume_ && volumePath_.effective == matp::VolumeControlMode::Hardware;
    runtime_.volumeScheduled = usesHardware() ? hardwarePending_ : source_.volumeScheduled();
    runtime_.appliedGain = source_.appliedGain();
    if (!hardwareVolume_)
        runtime_.hasHardwareVolumeState = false;
}

void StreamPlayback::trackBoundary(const matp::TrackBoundary& boundary)
{
    // A queue rather than a single value, because the server announces a
    // boundary before the room reaches it and section 23 asks for the change
    // exactly when the frame becomes audible.
    if (pendingTracks_.size() >= kMaxPendingBoundaries)
    {
        MULTIROOM_WARN("ignoring the track boundary at timeline %llu ns: %lu are already queued",
                 static_cast<unsigned long long>(boundary.timelineNs),
                 static_cast<unsigned long>(pendingTracks_.size()));
        return;
    }

    // Kept ordered by position. The server sends them in order, and one that
    // arrives out of order must not hide the boundaries behind it.
    size_t position = pendingTracks_.size();
    while (position > 0 && pendingTracks_[position - 1].timelineNs > boundary.timelineNs)
        --position;
    pendingTracks_.insert(pendingTracks_.begin() + static_cast<std::ptrdiff_t>(position),
                          boundary);

    MULTIROOM_INFO("track boundary announced: \"%s\" at timeline %llu ns",
             boundary.title.empty() ? boundary.trackId.c_str() : boundary.title.c_str(),
             static_cast<unsigned long long>(boundary.timelineNs));

    // A boundary the room has already passed -- a late join, or one announced
    // for a position that is behind the start -- belongs to what is playing
    // now.
    if (runtime_.hasTimelineNsAudible)
        promoteTracks(runtime_.timelineNsAudible);
}

void StreamPlayback::promoteTracks(uint64_t audibleTimelineNs)
{
    size_t reached = 0;
    while (reached < pendingTracks_.size() &&
           pendingTracks_[reached].timelineNs <= audibleTimelineNs)
        ++reached;

    if (reached == 0)
        return;

    track_ = pendingTracks_[reached - 1];
    hasTrack_ = true;
    trackTimelineKnown_ = true;
    pendingTracks_.erase(pendingTracks_.begin(),
                         pendingTracks_.begin() + static_cast<std::ptrdiff_t>(reached));

    // Section 22: within one format the stream itself has no gap here, and
    // neither does the device. This is the only thing that happens at a track
    // boundary.
    MULTIROOM_INFO("now playing \"%s\" from timeline %llu ns",
             track_.title.empty() ? track_.trackId.c_str() : track_.title.c_str(),
             static_cast<unsigned long long>(track_.timelineNs));
}

void StreamPlayback::trackInfo(const matp::TrackInfo& info)
{
    // No frame to wait for, so nothing is queued: section 17's message says
    // what the room is playing at the moment it arrives. The boundaries that
    // are still ahead stay queued and will replace this when the sound card
    // reaches them.
    track_ = matp::TrackBoundary();
    track_.trackId = info.trackId;
    track_.title = info.title;
    hasTrack_ = true;
    trackTimelineKnown_ = false;

    MULTIROOM_INFO("now playing \"%s\"",
             info.title.empty() ? info.trackId.c_str() : info.title.c_str());
    publishTimeline(false);
}

bool StreamPlayback::bufferStatus(matp::BufferStatus& out) const
{
    if (!stream_.isAttached())
        return false;
    out = stream_.status();
    return true;
}

bool StreamPlayback::playbackReport(matp::PlaybackReport& out) const
{
    if (!engine_.isRunning())
        return false; // no device, so nothing about a sound card to say

    // The snapshot the last poll published rather than a fresh driver call.
    // The position and the instant have to be one reading taken together --
    // that pairing is the whole value of the report -- and it is the same
    // reading the status command shows, so the two can never disagree.
    const AudioClockSnapshot& snapshot = runtime_.lastClock;
    out.clockQuality = snapshot.quality;
    out.deviceFramePosition = snapshot.deviceFramePosition;
    out.localMonotonicTimeNs =
        snapshot.isValid() ? snapshot.monotonicTimeNs : monotonicNowNs();

    out.hasTimelineNsAudible = runtime_.hasTimelineNsAudible;
    out.timelineNsAudible = runtime_.timelineNsAudible;

    // Media time, like every other position the server is told: how much music
    // this room still holds, not how many samples that became here.
    out.bufferedDurationNs = stream_.status().bufferedDurationNs;
    out.underruns = engine_.underruns();

    // Section 25: a client with no usable clock reports that it has none and
    // leaves the measurement out. An estimate from before the clock went is
    // not a measurement of what the card is doing now.
    if (out.clockQuality != AudioClockQuality::Unavailable && drift_.hasEstimate())
    {
        out.hasDrift = true;
        out.measuredSampleRate = drift_.estimatedSampleRate();
        out.measuredDriftPpm = drift_.driftPpm();
        out.driftConfidence = drift_.confidence();
    }

    // What was programmed, not what the oscillator does: the drift above is
    // the measured residual of the trimmed clock.
    if (driftSelected_ == matp::DriftCorrectionMode::HardwareClock && hardwareClock_)
    {
        out.hasHardwareClock = true;
        out.hardwareClockPpm = clockPpm_;
        out.hardwareClockSequence = clockSequence_;
        out.hardwareClockSaturated = clockSaturated_;
        out.hardwareClockError = clockError_;
    }
    return true;
}

bool StreamPlayback::hardwareClockActive() const
{
    return driftSelected_ == matp::DriftCorrectionMode::HardwareClock && hardwareClock_ &&
           clockNominal_ && clockError_.empty();
}

void StreamPlayback::openHardwareClock(const AudioDeviceInfo* device)
{
    closeHardwareClock();
    clockError_.clear();
    if (device != nullptr && device->hardwareClock.available && engine_.backend() != nullptr)
        hardwareClock_ = engine_.backend()->openHardwareClock(*device);
    if (hardwareClock_)
    {
        // Nominal before anything plays, whichever actuator is selected: a
        // trim a stopped or crashed client left behind must not run a
        // resampled room off its rate.
        clockInfo_ = device->hardwareClock;
        std::string error;
        clockNominal_ = hardwareClock_->set(0, error);
        if (!clockNominal_)
        {
            clockError_ = "could not reset the hardware clock to nominal: " + error;
            MULTIROOM_ERROR("%s", clockError_.c_str());
        }
    }
    publishDriftCorrection();
}

void StreamPlayback::closeHardwareClock()
{
    // The controlled stop: the trim belonged to the stream that just ended.
    if (hardwareClock_ && clockPpm_ != 0)
    {
        std::string error;
        if (!hardwareClock_->set(0, error))
            MULTIROOM_ERROR("could not reset the hardware clock to nominal: %s", error.c_str());
    }
    hardwareClock_.reset();
    clockInfo_ = HardwareClockInfo();
    clockNominal_ = false;
    clockPpm_ = 0;
    clockSequence_ = 0;
    clockSaturated_ = false;
    publishDriftCorrection();
}

bool StreamPlayback::writeHardwareClock(int32_t ppm)
{
    std::string error;
    if (hardwareClock_->set(ppm, error))
    {
        clockPpm_ = ppm;
        return true;
    }

    // What the clock runs at is unknown now, and nominal is the one rate the
    // server can plan a fallback around. Reported in PLAYBACK_REPORT.
    clockError_ = "hardware clock write failed: " + error;
    std::string resetError;
    if (hardwareClock_->set(0, resetError))
        clockPpm_ = 0;
    else
    {
        clockNominal_ = false;
        clockError_ += "; reset to nominal failed: " + resetError;
    }
    MULTIROOM_ERROR("%s", clockError_.c_str());
    return false;
}

void StreamPlayback::setDriftCorrection(matp::DriftCorrectionMode selected)
{
    if (selected == driftSelected_)
        return;
    driftSelected_ = selected;
    MULTIROOM_INFO("drift correction: %s", matp::driftCorrectionModeName(selected));

    // Never both actuators: the server resamples from the next stream on, so
    // the clock goes back to nominal now rather than when that stream opens.
    if (selected != matp::DriftCorrectionMode::HardwareClock && hardwareClock_ && clockPpm_ != 0)
        writeHardwareClock(0);
    clockSequence_ = 0;
    clockSaturated_ = false;
    publishDriftCorrection();
}

void StreamPlayback::driftFeedback(const matp::DriftFeedback& feedback)
{
    runtime_.driftCorrection.hasServerDrift = feedback.hasDrift;
    runtime_.driftCorrection.serverDriftPpm = feedback.measuredDriftPpm;
    runtime_.driftCorrection.serverDriftConfidence = feedback.driftConfidence;

    // A room the server resamples never touches its clock, whatever arrives.
    if (feedback.hasHardwareTarget && hardwareClockActive())
    {
        const hardware_clock::Setting setting =
            hardware_clock::settingFor(clockInfo_, feedback.hardwareTargetPpm, clockPpm_);
        if (setting.ppm == clockPpm_ || writeHardwareClock(setting.ppm))
        {
            clockSaturated_ = setting.saturated;
            clockSequence_ = feedback.sequence;
        }
    }
    publishDriftCorrection();
}

void StreamPlayback::publishDriftCorrection()
{
    DriftCorrectionState& state = runtime_.driftCorrection;
    state.selected = driftSelected_;
    state.hardwareClockActive = hardwareClockActive();
    state.hardwareClockPpm = clockPpm_;
    state.hardwareClockSaturated = clockSaturated_;
    state.hardwareClockError = clockError_;
}

void StreamPlayback::poll()
{
    const uint64_t nowNs = monotonicNowNs();

    // Phase 8's recovery, for the server's stream: a device that died is
    // noticed here, closed and reopened on a backoff. The room is silent from
    // the moment it died until the server schedules it again, which is what
    // the reports stopping and then starting over tells the server to do.
    const uint32_t recoveriesBefore = supervisor_.recoveries();
    supervisor_.poll(nowNs);
    if (supervisor_.recoveries() != recoveriesBefore)
        MULTIROOM_WARN("the stream's device came back on a new frame axis: the timeline that was "
                 "playing was dropped, and this room is silent until the server schedules "
                 "it again");

    stream_.poll();
    pollRejoin();
    publishTimeline(true);
    pollHardwareVolume(nowNs);
    publishPlayback(runtime_, supervisor_, engine_, nowNs);
}

void StreamPlayback::pollRejoin()
{
    if (!stream_.resyncPending())
    {
        rejoinArmed_ = false;
        flushWaiting_ = false;
        return;
    }

    if (!rejoinArmed_ && stream_.resyncAfterFlush() && !pollFlush(monotonicNowNs()))
        return;

    if (!rejoinArmed_)
    {
        // Section 32 in one call: the timeline that was playing stops being
        // played and everything buffered for it is dropped, rather than the
        // room drifting back to where the server wants it. The device is left
        // running -- section 21 wants it primed for the start that rejoins,
        // and its clock and this stream's drift estimate both stay good
        // because nothing was reopened.
        rejoinDrops_ = source_.bufferDrops();
        source_.discardTimeline();
        rejoinArmed_ = true;

        MULTIROOM_INFO("rejoining at timeline %llu ns: the timeline that was playing and the "
                 "%lu frame(s) buffered for it were dropped",
                 static_cast<unsigned long long>(stream_.resyncTimelineNs()),
                 static_cast<unsigned long>(stream_.status().streamFrames));
        publishTimeline(false);
        return;
    }

    // A device that is not running has no reader, so there is nothing to wait
    // for and nothing that could be in the buffer at the same time as this.
    const bool running = engine_.isRunning();
    if (running && source_.bufferDrops() == rejoinDrops_)
        return; // the render callback has not reached the drop yet

    if (!running)
        buffer_.discard(buffer_.readableFrames());

    rejoinArmed_ = false;
    stream_.resume();
}

bool StreamPlayback::pollFlush(uint64_t nowNs)
{
    // No render callback to take a cut or a resume, so no line to keep either.
    if (!engine_.isRunning())
    {
        hasFlush_ = false;
        flushResumeArmed_ = false;
        flushWaiting_ = false;
        return true;
    }

    if (flushResumeArmed_)
    {
        if (source_.bufferDrops() == flushDrops_)
            return false; // the render callback has not taken the resume yet

        stream_.resumeAfterFlush(flushResumeStreamFrame_);
        hasFlush_ = false;
        flushResumeArmed_ = false;
        flushWaiting_ = false;
        MULTIROOM_INFO("flush done: timeline %llu ns plays on the kept line from stream frame %llu",
                 static_cast<unsigned long long>(stream_.resyncTimelineNs()),
                 static_cast<unsigned long long>(flushResumeStreamFrame_));
        publishTimeline(false);
        return false;
    }

    if (!hasFlush_)
    {
        if (!flushWaiting_)
        {
            flushWaiting_ = true;
            flushWaitSinceNs_ = nowNs;
        }
        if (nowNs - flushWaitSinceNs_ < kFlushWaitNs)
            return false;

        MULTIROOM_ERROR("server defect: the audio at timeline %llu ns follows a flush, and no FLUSH "
                  "arrived within %llu ms; rejoining instead",
                  static_cast<unsigned long long>(stream_.resyncTimelineNs()),
                  static_cast<unsigned long long>(kFlushWaitNs / 1000000ULL));
        flushWaiting_ = false;
        return true;
    }

    // The resume replaces the cut in the schedule, so the cut has to have
    // come round first.
    if (source_.bufferDrops() == flushDrops_)
        return false;

    // Where the audio after the cut belongs on the line this room is playing:
    // the mapping of the audio before the cut, extrapolated at its ratio if
    // the position is past what arrived. It is still there, because the stream
    // parks before it records the packet that jumped.
    const uint32_t drops = source_.bufferDrops();
    uint64_t streamFrame = 0;
    if (!timelineToStream(stream_.resyncTimelineNs(), streamFrame) ||
        !source_.resumeAfterFlush(streamFrame))
    {
        MULTIROOM_INFO("flush at timeline %llu ns: this room has no playing line to keep, so it "
                 "rejoins at %llu ns",
                 static_cast<unsigned long long>(flushTimelineNs_),
                 static_cast<unsigned long long>(stream_.resyncTimelineNs()));
        hasFlush_ = false;
        flushWaiting_ = false;
        return true;
    }

    flushDrops_ = drops;
    flushResumeArmed_ = true;
    flushResumeStreamFrame_ = streamFrame;
    publishTimeline(false);
    return false;
}

void StreamPlayback::publishTimeline(bool sampleDrift)
{
    runtime_.timelineState = source_.state();
    runtime_.startScheduled = source_.isScheduled();
    // The timeline position the server named, not one converted back out of
    // the stream axis: `status` and the START_AT that caused it must agree.
    runtime_.startTimelineNs = hasStartTimeline_ ? startTimelineNs_ : 0;
    runtime_.startDeviceFrame = source_.startDeviceFrame();
    runtime_.startInMs = 0;
    runtime_.hasTimelineNsAudible = false;
    runtime_.timelineUnderrunFrames = source_.underrunFrames();
    runtime_.timelineSkippedFrames = source_.skippedFrames();
    runtime_.timelineFlushedFrames = source_.flushedFrames();
    runtime_.endScheduled = source_.hasScheduledEnd();
    runtime_.endTimelineNs = endTimelineNs_;
    publishVolume();
    runtime_.trackKnown = hasTrack_;
    runtime_.trackId = track_.trackId;
    runtime_.trackTitle = track_.title;
    runtime_.trackTimelineNsKnown = trackTimelineKnown_;
    runtime_.trackTimelineNs = track_.timelineNs;
    runtime_.pendingTrackBoundaries = static_cast<uint32_t>(pendingTracks_.size());

    // Read on every pass, not only at a START_AT: the render cursor reaches
    // this thread as 32 bits and is widened here, which only works if a wrap
    // (a day at 48 kHz, three hours at 384 kHz) is never missed.
    source_.submittedFrames();

    if (!engine_.isRunning())
    {
        runtime_.lastClock = AudioClockSnapshot();
        return;
    }

    const AudioClockSnapshot snapshot = engine_.clockSnapshot();
    runtime_.lastClock = snapshot;

    if (sampleDrift && snapshot.isValid())
    {
        // An underrun drops frames the position counter has already counted,
        // which puts a step into the series. Drift is a slope, so the step
        // itself is harmless -- a window that spans it is not, because it
        // measures the gap rather than the clock.
        const uint64_t underruns = engine_.underruns();
        if (underruns != lastUnderruns_)
        {
            lastUnderruns_ = underruns;
            drift_.reset();
        }
        drift_.addSample(snapshot);
    }

    if (!source_.isScheduled() || !snapshot.isValid())
        return;

    // The sound card's position is on the stream axis, and every position the
    // server is told is a timeline nanosecond (section 33). An audible
    // position the mapping can no longer convert is reported as absent rather
    // than as a number on the wrong axis: this is the one value the server's
    // correction is computed from (section 30).
    uint64_t audibleStreamFrame = 0;
    if (source_.inFlushGap(snapshot))
    {
        // Section 54: between the cut and the audio after it no position is
        // playing, so the room reports the cut, as it would report a pause.
        runtime_.hasTimelineNsAudible = true;
        runtime_.timelineNsAudible = flushTimelineNs_;
    }
    else
    {
        runtime_.hasTimelineNsAudible =
            source_.streamFrameAudible(snapshot, audibleStreamFrame) &&
            streamToTimeline(audibleStreamFrame, runtime_.timelineNsAudible);
    }

    // Section 23 puts the track change exactly where the position becomes
    // audible, which is here and not where the message arrived.
    if (runtime_.hasTimelineNsAudible)
    {
        promoteTracks(runtime_.timelineNsAudible);
        runtime_.trackKnown = hasTrack_;
        runtime_.trackId = track_.trackId;
        runtime_.trackTitle = track_.title;
        runtime_.trackTimelineNsKnown = trackTimelineKnown_;
        runtime_.trackTimelineNs = track_.timelineNs;
        runtime_.pendingTrackBoundaries = static_cast<uint32_t>(pendingTracks_.size());
    }

    // Counted down in device frames rather than in wall time: the same
    // axis the start itself sits on, so the two cannot disagree.
    if (snapshot.deviceFramePosition < source_.startDeviceFrame() &&
        snapshot.nominalSampleRate != 0)
    {
        const uint64_t remaining =
            source_.startDeviceFrame() - snapshot.deviceFramePosition;
        runtime_.startInMs =
            static_cast<uint32_t>(remaining * 1000 / snapshot.nominalSampleRate);
    }
}

bool StreamPlayback::audioAddress(const matp::PrepareStream& request, std::string& address) const
{
    if (runtime_.config == nullptr)
        return false;

    std::string host;
    uint16_t port = 0;
    if (!parseServerAddress(runtime_.config->server, host, port))
        return false;

    uint16_t audioPort = request.audioPort;
    if (!request.hasAudioPort)
    {
        if (port == 65535)
            return false;
        audioPort = static_cast<uint16_t>(port + 1);
    }

    // An IPv6 literal goes back in brackets, or parseServerAddress() rejects it.
    if (host.find(':') != std::string::npos)
        address = "[" + host + "]:" + std::to_string(audioPort);
    else
        address = host + ":" + std::to_string(audioPort);
    return true;
}

} // namespace multiroom
