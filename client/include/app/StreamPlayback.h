#pragma once

// The client's end of the server's audio stream: the second TCP connection of
// MATP section 3, the ring buffer it fills, and the device that ring buffer
// will be played out of.
//
// It exists to answer PREPARE_STREAM honestly. Section 12 forbids reporting
// "ready" before the device is open in the negotiated format, so preparing a
// stream opens the device here and reports back what the driver granted --
// which is not always what was asked for.
//
// The device is started as soon as it is open, and plays silence until the
// instant START_AT names (section 21: prime the device, establish its clock,
// fill silence, then align the first audible frame). Nothing is drained out
// of the ring buffer before then -- TimelineSource does not touch it while it
// is waiting -- so the prebuffer still fills, and audio is never made audible
// the moment it arrives, which is what section 48 forbids.
//
// The device is held by a PlaybackSupervisor rather than opened directly, so
// phase 8's recovery covers the server's stream and not only the test tone: a
// device that is pulled out, preempted or invalidated mid-stream is reopened
// on a backoff instead of ending the room. What a reopen costs is the
// timeline, not the stream -- the device's frame axis begins again, so the
// schedule that was placed on it is dropped and the server is left to
// reschedule the room the way section 43 already has it reschedule one that
// was disconnected.

#include "app/ClientCommandHandler.h"
#include "audio/AudioEngine.h"
#include "audio/AudioRingBuffer.h"
#include "audio/DriftEstimator.h"
#include "audio/HardwareVolume.h"
#include "audio/PlaybackSupervisor.h"
#include "audio/TimelineSource.h"
#include "matp/AudioStream.h"
#include "matp/Protocol.h"
#include "net/ServerConnection.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace multiroom {

// Whether a ring buffer of `bufferMs` of `format` -- power-of-two rounding
// included -- fits this platform's budget (platformRingBufferBudgetBytes).
// Says why not in `error`.
bool streamBufferFits(const AudioFormat& format, uint32_t bufferMs, std::string& error);

class StreamPlayback
{
public:
    StreamPlayback(ClientRuntime& runtime, matp::Identity identity, uint32_t bufferMs);
    // The same with the backend supplied, so a test can play the sound card.
    StreamPlayback(ClientRuntime& runtime, matp::Identity identity, uint32_t bufferMs,
                   std::unique_ptr<IAudioBackend> backend);
    ~StreamPlayback();

    StreamPlayback(const StreamPlayback&) = delete;
    StreamPlayback& operator=(const StreamPlayback&) = delete;

    ServerConnection& connection() { return connection_; }
    const matp::AudioStream& stream() const { return stream_; }

    bool start(std::string& error);

    // The PREPARE_STREAM hook (MATP section 12).
    void prepare(const matp::PrepareStream& request, const AudioFormat& negotiated,
                 matp::StreamReady& ready);

    // Idempotent, because it is also how a session that never got as far as a
    // stream is cleaned up.
    void release();

    // The START_AT hook (MATP sections 20 and 21). The instant has already
    // been mapped onto this machine's monotonic clock by the session; what is
    // left is to turn the timeline position into a frame of this room's own
    // buffered stream and place the instant on the sound card's frame axis,
    // which is the only axis section 48 allows a room to be synchronized on.
    bool start(const matp::StartAt& request, uint64_t localStartNs, std::string& error);

    // The PAUSE_AT and STOP_AT hooks (MATP sections 37 and 38). Neither closes
    // the device: it keeps running and keeps its clock, because a resume is a
    // new START_AT and section 21 wants the device primed before one.
    bool pause(const matp::TransportAt& request, std::string& error);
    bool stop(const matp::TransportAt& request, std::string& error);

    // The FLUSH hook (MATP section 54): the cut is armed here, and poll()
    // finishes the flush once the first packet after it has arrived. See
    // pollFlush().
    bool flush(const matp::TransportAt& request, std::string& error);

    // The SET_VOLUME hook (MATP section 39). A change the server scheduled
    // against a timeline position this room cannot place -- it has no start
    // yet, or the position is behind it -- is applied at once rather than
    // dropped: a room at the wrong volume is worse than one that changed a
    // moment early.
    //
    // How it is played is the room's volume path (MATP section 59,
    // resolveVolumePath): Hardware sets the device's own control and leaves
    // the samples unscaled; Software scales them in the render path; Disabled
    // plays the mute alone. A scheduled change on the device's control is
    // applied by poll() once the sound card's position has reached its frame
    // -- within one polling interval of it, where the render path is exact to
    // the frame.
    bool volume(const matp::SetVolume& request, std::string& error);

    // Resolves the volume path again from the room's Audio Control, the
    // configured Volume Control and the device, and plays the room's volume
    // on it when it changed. Called whenever one of the three may have:
    // a SESSION_CONFIG, a SET_VOLUME_CONTROL, a device opened, closed or
    // re-selected. Publishes runtime.volumePath, and tells the session when
    // the path changed so that it reports it.
    void updateVolumePath(bool deviceOpened = false);

    // Drift correction (audio/HardwareClock.h). The actuator SESSION_CONFIG
    // selected: anything but the hardware clock returns it to nominal at once.
    void setDriftCorrection(matp::DriftCorrectionMode selected);

    // DRIFT_FEEDBACK: the measurement for the status, and the target for the
    // clock while it is the selected actuator confirmed on the open device.
    void driftFeedback(const matp::DriftFeedback& feedback);

    // The TRACK_BOUNDARY hook (MATP section 23). Metadata only: it is queued
    // here and becomes "the track that is playing" when the sound card reaches
    // the timeline position it names, and nothing about the device or the
    // stream changes because of it (section 22). A boundary whose next track
    // has another format is reconfigured by the server's own PAUSE_AT and
    // PREPARE_STREAM (section 24), not from here.
    void trackBoundary(const matp::TrackBoundary& boundary);

    // The TRACK_INFO hook (MATP section 17). The same metadata with no
    // position on it, so unlike a boundary it is not queued against a frame:
    // it says what is playing at the moment it arrives, which is what a room
    // that joined in the middle of a track has instead of the boundary it was
    // never sent (section 44).
    void trackInfo(const matp::TrackInfo& info);

    bool bufferStatus(matp::BufferStatus& out) const;

    // The PLAYBACK_REPORT hook (MATP section 25): where the sound card
    // actually is, what is still buffered ahead of it, and how fast its clock
    // is really running. False while no device is running, which is a client
    // with nothing to report rather than one at position zero.
    bool playbackReport(matp::PlaybackReport& out) const;

    void poll();

    // How long the main loop may sleep before poll() has a hardware volume
    // change due, capped at `maxMs`: a mixer cannot be told a frame, so poll()
    // is what applies it, and a loop asleep for its idle wait would apply it
    // that much late.
    uint32_t waitMs(uint32_t maxMs) const;

    // SET_BUFFER's two halves (MATP section 58). The check refuses a size
    // that would not fit for the format the stream is playing, before
    // anything has been changed. The apply takes effect at once: a size that
    // rounds to the buffer already open changes nothing, any other reopens
    // the device with the new buffer -- which costs the room its schedule, as
    // a device recovery does.
    bool bufferFits(uint32_t bufferMs, std::string& error) const;
    void setBufferMs(uint32_t bufferMs);

private:
    // Mirrors the timeline into the runtime the control API reads, for the
    // same reason the playback fields are published there: a status request
    // must not turn into a driver call at an arbitrary moment.
    //
    // `sampleDrift` is set on the polling path alone. The drift estimate is a
    // regression over a series of clock readings, and a publish that happens
    // to coincide with a release or a schedule is not a new point on it.
    void publishTimeline(bool sampleDrift);

    // Promotes every queued boundary the sound card has reached. The last one
    // wins: several can fall inside one polling interval on short tracks, and
    // what is playing is the newest of them, not the oldest.
    void promoteTracks(uint64_t audibleTimelineNs);

    // The room's volume onto the path in force. A hardware control that
    // refuses a write is given up, and the path resolved again without it.
    void applyVolume(double gain, bool muted);
    // Puts the device's control back where leaving its path left it; false,
    // and kept for the next try, while the control refuses.
    bool resetHardwareVolume(std::string& error);

    // Whether the device's own control is there to be used: the open one, or
    // the scan's word for the selected device while none is open.
    bool hardwareAvailable() const;
    // Whether the path in force is played on that control.
    bool usesHardware() const;
    // What the render path multiplies by: the gain on Software, unity on
    // anything else.
    double softwareGain(double gain) const;

    // Applies a hardware change poll() was holding for its frame, and reads
    // the control back now and then for the status.
    void pollHardwareVolume(uint64_t nowNs);

    // The room's volume and the path playing it, into the runtime.
    void publishVolume();

    // One line for a pause or a stop, which says something different when the
    // position could not be placed and the schedule was dropped instead.
    void logTransport(const char* what, uint64_t timelineNs);
    bool hardwareClockActive() const;
    void openHardwareClock(const AudioDeviceInfo* device);
    void closeHardwareClock();
    bool writeHardwareClock(int32_t ppm);
    void publishDriftCorrection();

    // The zone's media timeline against the stream of PCM frames the server
    // actually sends this room (MATP section 33). Every message names a
    // timeline position in nanoseconds and everything below this class works
    // on the stream's frames, so these two are the only place the two axes
    // meet.
    //
    // Both can fail, and for one reason: the audio the position belongs to is
    // older than the mapping the audio connection still holds. A position
    // ahead of what has arrived is not a failure -- a pause is scheduled
    // seconds before the audio reaches it -- and the map extrapolates at the
    // ratio the server is currently using, which over those seconds is exact
    // to well under a frame.
    bool timelineToStream(uint64_t timelineNs, uint64_t& streamFrame) const;
    bool streamToTimeline(uint64_t streamFrame, uint64_t& timelineNs) const;

    // The client's half of a rejoin (MATP section 43) and of a hard resync
    // (section 32), which are the same thing seen from here: the server has
    // moved this room to a timeline position that has nothing to do with what
    // is buffered, and the audio connection is parked until it is gone.
    //
    // It takes two turns of the polling loop by design. The ring buffer can
    // only be emptied by the render callback, so the first turn arms the drop
    // and the second one sees that it happened -- waiting for it here would
    // mean the control thread blocking on the audio thread, which is the one
    // thing this client never does.
    void pollRejoin();

    // The same two turns for a jump that carries kAudioFlagAfterFlush, which
    // keeps the line instead of dropping it. Once the FLUSH has arrived and
    // the render callback has taken its cut, the first packet after the cut is
    // placed on the kept start mapping; once the callback has taken that too,
    // the audio connection is let go. The FLUSH may arrive after the packet,
    // because the two travel on different connections, so the stream waits up
    // to kFlushWaitNs for it.
    //
    // False while the flush owns the parked stream. True when the jump has to
    // be a rejoin after all -- no FLUSH came, or the room had no line to keep
    // -- and pollRejoin() then goes on as for any other jump.
    bool pollFlush(uint64_t nowNs);

    // Where the audio connection goes. Section 3 gives the example pair
    // 4711/4712 and no way to learn the second number, so a server that does
    // not name a port in PREPARE_STREAM is taken to mean the control port plus
    // one -- on the same host, because the stream belongs to the session that
    // was negotiated there.
    bool audioAddress(const matp::PrepareStream& request, std::string& address) const;

    // What the supervisor reopens, asked freshly for every attempt. The format
    // is the one the server negotiated and never changes here -- a client that
    // opened something else would be decoding what the server is sending as
    // something it is not (section 12) -- but the device and the access mode
    // are re-read, because a rescan during recovery may have landed on another
    // one.
    bool playbackTarget(AudioEngine::Settings& settings, std::string& error) const;

    // The two ends of an open device, as the supervisor drives them. Opening
    // one binds the audio connection to the ring buffer it just sized; closing
    // one unbinds it *and* drops the timeline, because the device's frame axis
    // begins again and the schedule was a point on it.
    void onDeviceOpened(const AudioFormat& granted);
    void onDeviceClosed();

    ClientRuntime& runtime_;
    matp::Identity identity_;
    uint32_t bufferMs_;

    // Declared in the order the destructor needs them: the connection thread
    // reaches the stream, the render thread reaches the source, so both must
    // outlive what calls into them.
    ServerConnection connection_;
    AudioRingBuffer buffer_;
    TimelineSource source_;
    AudioEngine engine_;
    matp::AudioStream stream_;

    // After both, so it is gone before either: it holds references and nothing
    // else, and its hooks reach the two above.
    PlaybackSupervisor supervisor_;

    // What PREPARE_STREAM negotiated, and whether there is a stream at all.
    // The supervisor asks for it on every attempt, including the ones that
    // happen long after the message that produced it.
    AudioEngine::Settings target_;
    bool hasTarget_ = false;

    // This sound card against this CPU, measured and never acted on: the
    // server owns the correction (section 26). Fed from poll() with the same
    // snapshot the status command reports, and dropped whenever the device is
    // reopened or loses frames, because either one puts a step into the series.
    DriftEstimator drift_;
    uint64_t lastUnderruns_ = 0;

    // The timeline positions the server named for the start and for the end
    // it scheduled. Kept rather than converted back from the stream frames
    // TimelineSource holds: the server said these numbers, and reporting them
    // through a round trip that a rounding or an evicted segment could move
    // would make `status` disagree with the message that caused it.
    bool hasStartTimeline_ = false;
    uint64_t startTimelineNs_ = 0;
    uint64_t endTimelineNs_ = 0;

    // A rejoin that has been armed and not yet seen through, and the drop
    // count it was armed against: the render callback moves that count when
    // the buffer is actually gone.
    bool rejoinArmed_ = false;
    uint32_t rejoinDrops_ = 0;

    // The flush in progress (section 54): the cut the server named, the drop
    // count the next step waits on, whether the packet after the cut has been
    // placed on the line, and since when a flagged packet has waited for its
    // FLUSH.
    bool hasFlush_ = false;
    uint64_t flushTimelineNs_ = 0;
    uint32_t flushDrops_ = 0;
    bool flushResumeArmed_ = false;
    uint64_t flushResumeStreamFrame_ = 0;
    bool flushWaiting_ = false;
    uint64_t flushWaitSinceNs_ = 0;

    // The room's volume as SET_VOLUME last left it, clamped, whichever path
    // plays it; `volumeSet_` stays false until the first one, so that a
    // hardware control is not moved from where its owner left it before the
    // server has asked for anything.
    bool volumeSet_ = false;
    double volumeGain_ = 1.0;
    bool volumeMuted_ = false;

    // The open device's own volume control, null where it has none or while
    // no device is open. A scheduled change waits here for its stream frame,
    // placed on the device axis by whichever start is in force when polled.
    std::unique_ptr<IHardwareVolume> hardwareVolume_;
    HardwareVolumeInfo hardwareInfo_; // what the scan said of the open control
    bool deviceOpen_ = false;
    VolumePath volumePath_;
    bool hardwarePending_ = false;
    uint64_t hardwarePendingStreamFrame_ = 0;
    double hardwarePendingGain_ = 1.0;
    bool hardwarePendingMuted_ = false;
    // The mute now in force: volumeMuted_ less a change still waiting for its
    // device frame. What a late level reset keeps on Disabled's switch.
    bool appliedMuted_ = false;
    uint64_t hardwareReadNs_ = 0;
    // A reset of the control that a path change owes it and that has not yet
    // succeeded: its level to the ceiling and its mute off, or the mute alone.
    enum class HardwareReset { None, Level, Mute };
    HardwareReset hardwareReset_ = HardwareReset::None;

    // The hardware clock: the actuator selected, the trim of the open device,
    // whether it was confirmed at nominal when the device opened, and what it
    // was last set to. A failed write ends its use for this stream; the error
    // stays until the next device is opened.
    matp::DriftCorrectionMode driftSelected_ = matp::DriftCorrectionMode::Resampling;
    std::unique_ptr<IHardwareClock> hardwareClock_;
    HardwareClockInfo clockInfo_;
    bool clockNominal_ = false;
    int32_t clockPpm_ = 0;
    uint64_t clockSequence_ = 0;
    bool clockSaturated_ = false;
    std::string clockError_;

    // Track boundaries the server has announced and the sound card has not
    // reached yet, oldest first, plus the one it has (section 23). Control
    // thread only, like everything else here that is not the ring buffer.
    std::vector<matp::TrackBoundary> pendingTracks_;
    bool hasTrack_ = false;
    // False for a track a TRACK_INFO named: that message carries no frame, and
    // `track_`'s is then the zero it was constructed with rather than a
    // position the room is at.
    bool trackTimelineKnown_ = false;
    matp::TrackBoundary track_;
};

} // namespace multiroom
