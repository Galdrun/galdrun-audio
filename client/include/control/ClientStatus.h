#pragma once

// Everything the control API's "status" command reports.
//
// A plain snapshot with no behaviour: the thread that owns the client state
// fills it in, and toJson() turns it into the response payload. Nothing here
// is read or written from the audio thread.

#include "audio/AudioClockSnapshot.h"
#include "audio/AudioDeviceInfo.h"
#include "audio/AudioFormat.h"
#include "audio/HardwareClock.h"
#include "audio/HardwareVolume.h"
#include "audio/PlaybackSupervisor.h"
#include "audio/TimelineSource.h"
#include "matp/Session.h"
#include "net/ServerConnection.h"
#include "util/Json.h"

#include <cstdint>
#include <string>

namespace multiroom {

struct ClientStatus
{
    std::string clientId;
    std::string name; // the room's name (MATP section 60)
    std::string version;
    std::string platform;
    uint64_t uptimeSeconds = 0;
    std::string powerState;
    uint32_t standbyAfterSeconds = 0;
    uint32_t deepSleepAfterSeconds = 0;

    // The connection's own state, which is why ServerConnectionState lives with
    // it in net/ rather than here.
    std::string server; // configured "host:port", empty when unconfigured
    ServerConnectionState serverState = ServerConnectionState::NotConfigured;

    // The MATP handshake on top of that connection. A client with no session
    // layer at all -- a test, or a client that has never been given a server
    // address -- reports idle, which is also what it is.
    matp::SessionState sessionState = matp::SessionState::Idle;
    std::string sessionId; // empty until the server has chosen one

    // The stream format the server picked, and whether the selected device
    // reported it as playable. Both are left out while there is no session,
    // rather than reported as a format of zero.
    bool sessionFormatSelected = false;
    AudioFormat sessionFormat;
    bool sessionFormatPlayable = false;

    // Why the last handshake did not complete, empty while nothing went wrong.
    std::string sessionError;

    // The mapping to the server's clock. Left out entirely until the exchange
    // has produced one -- an offset of zero is what two perfectly aligned
    // clocks look like, not what no measurement looks like -- and the skew is
    // left out separately until the window is long enough to resolve it.
    bool hasClockSync = false;
    int64_t clockOffsetNs = 0;
    bool hasClockSkew = false;
    double clockSkewPpm = 0.0;
    uint64_t clockRoundTripNs = 0;     // the best in the window
    uint64_t clockLastRoundTripNs = 0; // the most recent one
    uint64_t clockUncertaintyNs = 0;
    uint32_t clockSampleCount = 0;
    double clockWindowSeconds = 0.0;
    uint32_t clockSyncRequests = 0;
    uint32_t clockSyncReplies = 0;
    // Results sent back to the server (MATP section 8): the four timestamps it
    // needs to build a clock model of its own.
    uint32_t clockSyncResults = 0;

    // The audio connection and what it has put in the ring buffer (MATP
    // sections 14, 18 and 19). Reported only once a stream has been prepared:
    // before that there is no connection, no format and no timeline, and
    // reporting zeros for all three would read like an idle stream rather than
    // like no stream.
    bool streamPrepared = false;
    ServerConnectionState audioServerState = ServerConnectionState::NotConfigured;
    std::string audioServer; // "host:port" of the audio connection
    uint64_t streamPackets = 0;
    uint64_t streamFrames = 0;
    uint32_t streamWaits = 0;

    // What the stream has survived (MATP sections 43 and 32): audio
    // connections after the first, and timeline positions the server moved
    // this room to. Always reported, because zero is the answer for a stream
    // that has never been interrupted and is worth being able to see.
    uint32_t streamReattachments = 0;
    uint32_t streamRejoins = 0;
    // Seeks the room carried on its line (MATP section 54): a flush is a
    // move the server made on purpose, not an interruption, so it is counted
    // apart from the rejoins.
    uint32_t streamFlushes = 0;

    // What the ring buffer holds, and where it sits on the zone's media
    // timeline. The two positions are absent until the first packet has
    // arrived.
    bool streamHasContent = false;
    uint64_t streamPlayheadTimelineNs = 0;
    uint64_t streamBufferedUntilTimelineNs = 0;
    // The span of media time this room holds, and the number of its own frames
    // that span turned into. The second is local truth and is never sent
    // (MATP section 33).
    uint64_t streamBufferedDurationNs = 0;
    uint32_t streamBufferedFrames = 0;
    uint32_t streamBufferedMs = 0;
    uint32_t streamCapacityFrames = 0;
    uint32_t streamCapacityMs = 0;

    // Where the stream is on the zone's media timeline (MATP sections 20, 21
    // and 48). The scheduled start is absent until START_AT has been accepted,
    // and the audible position until the sound card has actually reached it --
    // before that there is no such position, which is not the same as 0 ns.
    TimelineState timelineState = TimelineState::Idle;
    bool startScheduled = false;
    uint64_t startTimelineNs = 0;
    uint64_t startDeviceFrame = 0;
    uint32_t startInMs = 0;
    bool hasTimelineNsAudible = false;
    uint64_t timelineNsAudible = 0;
    uint32_t timelineUnderrunFrames = 0;
    uint32_t timelineSkippedFrames = 0;
    uint32_t timelineFlushedFrames = 0;

    // The scheduled pause or stop (sections 37 and 38). Absent until one has
    // been accepted, like the start above.
    bool endScheduled = false;
    uint64_t endTimelineNs = 0;

    // The room's volume (section 39). Always reported, because "1.0, not
    // muted" is a real answer and the absence of a volume is not.
    double volumeGain = 1.0;
    bool volumeMuted = false;
    bool volumeScheduled = false;
    double appliedGain = 1.0;

    // What is playing (sections 17 and 23), absent until the sound card has
    // reached a boundary the server announced or a TRACK_INFO named the track
    // that was already running. The position is absent for the second of
    // those: TRACK_INFO carries none.
    bool trackKnown = false;
    std::string trackId;
    std::string trackTitle;
    bool hasTrackTimelineNs = false;
    uint64_t trackTimelineNs = 0;
    uint32_t pendingTrackBoundaries = 0;

    // Why the stream stopped taking audio, empty while it is healthy.
    std::string streamError;

    // Device cache, independent of whether a device could be selected from it.
    uint32_t deviceCount = 0;
    std::string audioScannedAt; // ISO 8601 UTC, from the cache

    // A device is only selected once the cache contains a usable one.
    bool deviceSelected = false;
    std::string deviceId;
    std::string deviceName;

    // A format is only selected once the device offers one the preference
    // allows; a selected device with no usable format is a real state.
    bool formatSelected = false;
    AudioAccessMode accessMode = AudioAccessMode::Shared;

    // The format the open sound card runs at; invalid while it is closed.
    AudioFormat deviceFormat;

    // The selected device's hardware volume, as the scan found it, whether
    // the room's volume is on it right now, and what it reads back as.
    HardwareVolumeInfo hardwareVolume;

    // MATP section 59: the room's Audio Control (absent until a SESSION_CONFIG
    // named one), the Volume Control in client.json, and the path resolved.
    bool audioControlKnown = false;
    matp::AudioControl audioControl = matp::AudioControl::BitPerfect;
    matp::VolumeControlMode volumeControl = matp::VolumeControlMode::Auto;
    VolumePath volumePath;
    bool hardwareVolumeActive = false;
    bool hasHardwareVolumeState = false;
    HardwareVolumeState hardwareVolumeState;

    // The drift correction this client asks for in client.json, the selected
    // device's clock trim as the scan found it, and what is happening now.
    matp::DriftCorrectionMode driftCorrection = matp::DriftCorrectionMode::Resampling;
    HardwareClockInfo hardwareClock;
    DriftCorrectionState driftCorrectionState;

    // Playback and its supervision. Like ServerConnectionState above, the
    // state enum lives with the component that owns it.
    PlaybackState playbackState = PlaybackState::Idle;
    uint64_t underruns = 0;
    uint32_t bufferFrames = 0; // period the driver granted, 0 while closed

    // Why playback is not running, empty while it is -- and then left out of
    // the response rather than reported as a stale message.
    std::string playbackError;
    uint32_t playbackAttempts = 0;   // consecutive failed reopen attempts
    uint32_t playbackRecoveries = 0; // times playback came back after a failure
    uint32_t playbackRetryMs = 0;    // until the next attempt, 0 when none is pending

    // quality Unavailable means the device reports no usable position, and the
    // clock block is then left out of the response entirely.
    AudioClockSnapshot clock;

    // Left out of the response while the estimator has no fit, rather than
    // reported as a fabricated zero -- the same rule PLAYBACK_REPORT follows.
    bool hasDrift = false;
    double measuredSampleRate = 0.0;
    double driftPpm = 0.0;
    double driftStandardErrorPpm = 0.0;
    double driftConfidence = 0.0;
    uint32_t driftSampleCount = 0;
    double driftWindowSeconds = 0.0;

    // The same object SYSTEM_REPORT carries (section 55), so the figures can
    // be read without a server. Left out until the first sample.
    bool hasSystem = false;
    SystemMetrics system;
};

json::Value toJson(const ClientStatus& status);

} // namespace multiroom
