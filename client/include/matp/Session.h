#pragma once

// The MATP handshake: HELLO, HELLO_ACK, AUDIO_CAPABILITIES, SESSION_CONFIG
// (MATP sections 10 to 12), and the clock synchronization that follows it
// (sections 8 and 9).
//
// Threading, which is the whole reason this class is shaped the way it is:
// onData() is called on the connection's thread and does nothing but buffer
// bytes. Everything else -- decoding, answering, and every accessor -- runs on
// the thread that owns the client's state, the one that also drains
// CommandQueue. Nothing here is touched from the audio thread.
//
// poll() takes the time rather than reading the clock, like PlaybackSupervisor
// does, so a test can walk the handshake timeout without sleeping through it.
// That applies to *scheduling* only. The four timestamps of a clock sync are a
// measurement, and a measurement has to come from the clock at the instant it
// happens: C1 is read immediately before the request goes out, and C4 is the
// moment the connection thread took the bytes off the socket. Timing an
// exchange with the poll clock instead would measure this loop's period rather
// than the network.
//
// The session owns no device and opens nothing itself. What PREPARE_STREAM
// asks for -- a device open in the negotiated format and an audio connection
// bound to the session -- is done by the hooks below, so this class stays a
// protocol state machine that can be tested without a sound card.

#include "matp/ClockModel.h"
#include "matp/Input.h"
#include "matp/Protocol.h"
#include "net/ServerConnection.h"

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace multiroom {
namespace matp {

enum class SessionState
{
    Idle,             // no connection, or the handshake has not started
    HelloSent,        // waiting for HELLO_ACK
    CapabilitiesSent, // waiting for SESSION_CONFIG
    Established,      // the server has chosen this client's stream format
    Failed            // protocol error; this connection was dropped
};

const char* sessionStateName(SessionState state);

class Session
{
public:
    // What the client can currently play. Asked freshly every time a report is
    // sent, never captured once: a rescan may have landed on a different
    // device, and that is exactly what has to be reported. Returns false and
    // fills the message when the client can describe no device at all.
    using CapabilityProvider = std::function<bool(CapabilityReport& out, std::string& error)>;

    // What PREPARE_STREAM, BUFFER_STATUS and PLAYBACK_REPORT need from the
    // layer that owns the device and the ring buffer (MATP sections 12, 18, 19
    // and 25). A session without them answers PREPARE_STREAM with "not ready",
    // which is the truth about a client that has no playback path.
    struct StreamHooks
    {
        // Opens the device in the negotiated format and brings the audio
        // connection up, then fills `out` with what the driver granted -- or
        // with the reason it could not. Reporting ready before the device is
        // open is what turns a recoverable gap into a room that misses the
        // start, so this call does not return until it knows.
        std::function<void(const PrepareStream& request, const AudioFormat& negotiated,
                           StreamReady& out)>
            prepare;

        // Closes it again. The session the stream belonged to is gone, so the
        // stream is too.
        std::function<void()> release;

        // Makes the timeline position the server named audible at the instant
        // it named (sections 20 and 21). The instant arrives here already
        // converted to this machine's monotonic clock: mapping the server's
        // time is this class's job, placing the timeline position on the sound
        // card's frame axis is the playback layer's. Returns false with a
        // reason when the client cannot honour the schedule -- the protocol
        // gives START_AT no answer, so the reason is logged rather than sent.
        std::function<bool(const StartAt& request, uint64_t localStartNs, std::string& error)>
            start;

        // What the ring buffer holds right now. False when there is nothing to
        // report on, which suppresses the message rather than sending zeros.
        std::function<bool(BufferStatus& out)> bufferStatus;

        // What the sound card actually played (section 25). False when there
        // is no running device to report about, which suppresses the message
        // rather than sending a position that is nobody's.
        std::function<bool(PlaybackReport& out)> playbackReport;

        // PAUSE_AT and STOP_AT (sections 37 and 38). Both name a timeline
        // position, and that position is what places them -- unlike START_AT,
        // no clock conversion happens here first, because the room is already
        // playing and already knows where that position falls on its own axis.
        // Returns false with a reason when the client cannot honour it; the
        // protocol gives neither message an answer, so the reason is logged.
        std::function<bool(const TransportAt& request, std::string& error)> pause;
        std::function<bool(const TransportAt& request, std::string& error)> stop;

        // FLUSH (section 54), placed the same way: the room cuts at the
        // timeline position, drops what it holds from there on and keeps the
        // line it is playing on for the audio after it.
        std::function<bool(const TransportAt& request, std::string& error)> flush;

        // SET_VOLUME (section 39), scheduled or not. The gain belongs to the
        // render path, which is as far from this class as the device is.
        std::function<bool(const SetVolume& request, std::string& error)> volume;

        // TRACK_BOUNDARY (section 23). Metadata, and the one message here that
        // cannot fail: there is nothing for a client to refuse about being
        // told where the next track begins, and section 22 is explicit that
        // nothing about playback may change because of it.
        std::function<void(const TrackBoundary& boundary)> trackBoundary;

        // TRACK_INFO (section 17). The same metadata without a position: what
        // the room is playing right now rather than where the next track
        // begins, which is what a late joiner (section 44) has no boundary
        // for. It cannot fail either, for the same reason.
        std::function<void(const TrackInfo& info)> trackInfo;
    };

    Session(ServerConnection& connection, Identity identity, CapabilityProvider capabilities);

    // Owner thread, before the first poll().
    void setStreamHooks(StreamHooks hooks);

    // The latest system sample for SYSTEM_REPORT (section 55), asked every
    // 2 s once the session is established. The owner samples; this class only
    // sends. False when there is none yet, which suppresses the message.
    using SystemProvider = std::function<bool(SystemMetrics& out)>;
    void setSystemProvider(SystemProvider provider);

    // The device list for AUDIO_DEVICES, and what SET_AUDIO_DEVICE applies
    // (section 56). The owner persists the choice and re-selects; this class
    // only reports and forwards. A client without them reports no list and
    // answers a SET_AUDIO_DEVICE with an ERROR.
    using AudioDeviceProvider = std::function<void(AudioDeviceList& out)>;
    using AudioDeviceSetter = std::function<bool(const std::string& deviceId, std::string& error)>;
    void setAudioDeviceHooks(AudioDeviceProvider provider, AudioDeviceSetter setter);

    // What RESCAN_AUDIO runs: the owner's rescan, which reports the new list
    // and capabilities itself. Without it the request is answered with an
    // ERROR.
    using AudioRescanner = std::function<bool(std::string& error)>;
    void setAudioRescanHook(AudioRescanner rescan);

    // Owner thread: the device list, the configured device or the selected
    // one changed. A list identical to the last one sent is not re-sent.
    void audioDevicesChanged();

    // The room's buttons and encoders for INPUT_CONTROLS, and what
    // SET_INPUT_BINDINGS applies (section 57). The owner persists the change
    // and calls inputControlsChanged(); this class only reports and forwards.
    // A client without them sends no list and answers SET_INPUT_BINDINGS with
    // an ERROR.
    using InputControlProvider = std::function<void(std::vector<InputControl>& out)>;
    using InputBindingSetter =
        std::function<bool(const SetInputBindings& request, std::string& error)>;
    // `deepSleep`: this client can run the deepSleep action (reported with the list).
    void setInputHooks(InputControlProvider provider, InputBindingSetter setter,
                       bool deepSleep = false);

    // Owner thread: a binding changed. A list identical to the last one sent
    // is not re-sent, and an empty one -- a room with no inputs -- never is.
    void inputControlsChanged();

    // The buffer's size for BUFFER_CONFIG, and what SET_BUFFER applies
    // (section 58). The owner validates, persists and applies, and refuses
    // rather than clamps; this class only reports and forwards. A client
    // without them sends no BUFFER_CONFIG and answers SET_BUFFER with an ERROR.
    using BufferProvider = std::function<uint32_t()>;
    using BufferSetter = std::function<bool(uint32_t bufferMs, std::string& error)>;
    void setBufferHooks(BufferProvider provider, BufferSetter setter);

    // Owner thread: the target changed. A value identical to the last one sent
    // is not re-sent.
    void bufferConfigChanged();

    // The room's name for ROOM_NAME, and what SET_ROOM_NAME applies (section
    // 60). The owner validates and persists; this class only reports and
    // forwards. A client without them sends no ROOM_NAME and answers
    // SET_ROOM_NAME with an ERROR.
    using RoomNameProvider = std::function<std::string()>;
    using RoomNameSetter = std::function<bool(const std::string& name, std::string& error)>;
    void setRoomNameHooks(RoomNameProvider provider, RoomNameSetter setter);

    // Owner thread: the name changed. A name identical to the last one sent is
    // not re-sent.
    void roomNameChanged();

    // Section 59. The Audio Control each SESSION_CONFIG names (BitPerfect and
    // `known` false where it names none) goes to the owner, which resolves the
    // volume path from it. VOLUME_CONFIG reports what the provider returns;
    // SET_VOLUME_CONTROL is applied by the setter, which validates, persists
    // and refuses rather than bends. A client without them sends no
    // VOLUME_CONFIG and answers SET_VOLUME_CONTROL with an ERROR.
    using AudioControlHook = std::function<void(AudioControl control, bool known)>;
    using VolumeConfigProvider = std::function<VolumeConfig()>;
    using VolumeControlSetter = std::function<bool(VolumeControlMode mode, std::string& error)>;
    void setVolumeControlHooks(AudioControlHook audioControl, VolumeConfigProvider provider,
                               VolumeControlSetter setter);

    // Owner thread: the setting or the resolved path changed. A report
    // identical to the last one sent is not re-sent.
    void volumeConfigChanged();

    // The drift-correction actuator.
    // `negotiated` is told every SESSION_CONFIG's selection -- resampling where
    // it names none -- before the stream it applies to is prepared; `feedback`
    // is handed each DRIFT_FEEDBACK of this session whose sequence rose;
    // `setter` persists SET_DRIFT_CORRECTION, and an accepted change is
    // answered with AUDIO_CAPABILITIES, which renegotiates the session.
    using DriftCorrectionHook = std::function<void(DriftCorrectionMode selected)>;
    using DriftFeedbackHook = std::function<void(const DriftFeedback& feedback)>;
    using DriftCorrectionSetter =
        std::function<bool(DriftCorrectionMode mode, std::string& error)>;
    void setDriftCorrectionHooks(DriftCorrectionHook negotiated, DriftFeedbackHook feedback,
                                 DriftCorrectionSetter setter);

    // Owner thread: a gesture happened (section 57). False, with nothing sent,
    // while no session is established: the action is dropped, not queued --
    // a volume step pressed during a reconnect is not wanted a minute later.
    bool sendInputAction(const InputActionEvent& event);

    // Parks only this room's audio. Control, clock sync and inputs stay live.
    // Automatic standby wakes on PREPARE_STREAM; manual off needs StandbyOff.
    bool setPower(bool on, std::string& error, bool automatic = false);
    const char* powerState() const;
    // Deep sleep is about to stop this client: the server keeps the state
    // through the disconnect, and with `wakeMac` can wake it with a magic
    // packet. resumeAfterFailedSleep reports the real state again when the
    // helper failed and this client is still running.
    bool announceSleep(const std::string& wakeMac = std::string());
    void resumeAfterFailedSleep();

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // Connection thread: buffers bytes and nothing else.
    void onData(const void* data, size_t size);

    // Owner thread. Starts the handshake on a new connection, decodes what has
    // arrived, and answers it.
    void poll(uint64_t nowNs);

    // Owner thread: the playback device or its format list changed, so the
    // server has to re-negotiate this client's stream (section 12). A report
    // identical to the last one sent is not a change and is not re-sent.
    void capabilitiesChanged();

    // Owner thread: the id changed through the control API. The server knows
    // a client by the id its HELLO carried, so a new one only takes effect on
    // a new connection -- a live one is dropped and the reconnect introduces
    // the client under the new name.
    void setClientId(const std::string& clientId);
    const Identity& identity() const { return identity_; }

    SessionState state() const { return state_; }
    const std::string& lastError() const { return lastError_; }

    // Empty until SESSION_CONFIG arrives.
    const std::string& sessionId() const { return sessionId_; }

    // The stream format the server chose, and whether the selected device
    // reported it. An unplayable format is recorded rather than worked around:
    // the client does not improvise a format the server did not choose.
    bool hasStreamFormat() const { return hasStreamFormat_; }
    const AudioFormat& streamFormat() const { return streamFormat_; }
    bool streamFormatPlayable() const { return streamFormatPlayable_; }

    // The mapping between the server's clock and this machine's, rebuilt from
    // scratch on every connection (section 43). Empty until the session is
    // established: the exchange only starts once there is a session for it to
    // belong to (section 10).
    const ClockModel& clock() const { return clock_; }

    // Exchanges sent and answered, for diagnostics and tests. A request that
    // was never answered counts as sent and not as completed.
    uint32_t clockSyncRequests() const { return clockSyncRequests_; }
    uint32_t clockSyncReplies() const { return clockSyncReplies_; }

    // CLOCK_SYNC_RESULT messages sent back (section 8). One per reply the
    // client accepted as belonging to an outstanding request, whether or not
    // its own model kept the sample.
    uint32_t clockSyncResults() const { return clockSyncResults_; }

    // Completed handshakes, for diagnostics and tests.
    uint32_t handshakes() const { return handshakes_; }

    // Whether the device is open in the negotiated format and the server has
    // been told so. False until PREPARE_STREAM has been answered with
    // "ready": true.
    bool streamPrepared() const { return streamPrepared_; }

    // BUFFER_STATUS messages sent, for diagnostics and tests.
    uint32_t bufferStatusReports() const { return bufferStatusReports_; }

    // PLAYBACK_REPORT messages sent, for diagnostics and tests.
    uint32_t playbackReports() const { return playbackReports_; }

    // SYSTEM_REPORT messages sent, for diagnostics and tests.
    uint32_t systemReports() const { return systemReports_; }

    // AUDIO_DEVICES sent and SET_AUDIO_DEVICE requests applied (section 56).
    uint32_t audioDeviceReports() const { return audioDeviceReports_; }
    uint32_t audioDeviceChanges() const { return audioDeviceChanges_; }
    uint32_t audioRescans() const { return audioRescans_; }

    // INPUT_CONTROLS and INPUT_ACTION sent, SET_INPUT_BINDINGS applied
    // (section 57).
    uint32_t inputControlReports() const { return inputControlReports_; }
    uint32_t inputActions() const { return inputActions_; }
    uint32_t inputBindingChanges() const { return inputBindingChanges_; }

    // BUFFER_CONFIG sent and SET_BUFFER requests applied (section 58).
    uint32_t bufferConfigReports() const { return bufferConfigReports_; }
    uint32_t roomNameReports() const { return roomNameReports_; }
    uint32_t bufferChanges() const { return bufferChanges_; }

    // VOLUME_CONFIG sent and SET_VOLUME_CONTROL requests applied (section 59).
    uint32_t volumeConfigReports() const { return volumeConfigReports_; }
    uint32_t volumeControlChanges() const { return volumeControlChanges_; }
    uint32_t driftFeedbacks() const { return driftFeedbacks_; }
    uint32_t driftCorrectionChanges() const { return driftCorrectionChanges_; }

    // START_AT messages the client actually scheduled, for diagnostics and
    // tests. One it could not honour is not counted.
    uint32_t scheduledStarts() const { return scheduledStarts_; }

    // The rest of the transport, counted the same way: a PAUSE_AT the client
    // could not honour is not a pause, and a SET_VOLUME it refused did not
    // change the volume.
    uint32_t scheduledPauses() const { return scheduledPauses_; }
    uint32_t scheduledStops() const { return scheduledStops_; }
    uint32_t scheduledFlushes() const { return scheduledFlushes_; }
    uint32_t volumeChanges() const { return volumeChanges_; }
    uint32_t trackBoundaries() const { return trackBoundaries_; }
    uint32_t trackInfos() const { return trackInfos_; }

    // PINGs answered with a PONG. A server that pings and is not answered
    // concludes the room is gone, so this is the one counter here that says
    // something about whether the session is believed to be alive.
    uint32_t pongs() const { return pongs_; }

private:
    void startHandshake(uint64_t nowNs);
    void resetSession();
    bool buildCapabilities(json::Value& payload, std::string& serialized);
    bool sendCapabilities();
    bool send(MessageType type, const json::Value& payload);
    void sendIfChanged(MessageType type, const json::Value& payload, std::string& last,
                       uint32_t& reports);
    void handleMessage(const Message& message, uint64_t arrivalNs);
    void handleHelloAck(const Message& message);
    void handleSessionConfig(const Message& message);
    void handleClockSyncReply(const Message& message, uint64_t arrivalNs);
    void handlePrepareStream(const Message& message);
    bool prepareStream();
    void handleStartAt(const Message& message);
    void handleTransportAt(const Message& message,
                           const std::function<bool(const TransportAt&, std::string&)>& hook,
                           uint32_t& counter);
    void handleSetVolume(const Message& message);
    void handleTrackBoundary(const Message& message);
    void handleTrackInfo(const Message& message);
    void handleSetAudioDevice(const Message& message);
    void handleRescanAudio(const Message& message);
    void handleSetInputBindings(const Message& message);
    void handleSetBuffer(const Message& message);
    void handleSetRoomName(const Message& message);
    void handleSetVolumeControl(const Message& message);
    void handleSetDriftCorrection(const Message& message);
    void handleDriftFeedback(const Message& message);
    void handlePing(const Message& message);
    // True when the message belongs to this session; a message out of turn or
    // for another session fails it.
    bool sessionMessageApplies(const Message& message, const std::string& sessionId);
    // As above, and there is a stream for it to act on; it logs the reason and
    // returns false when not.
    bool streamMessageApplies(const Message& message, const std::string& sessionId);
    // A settings message needs the session but no stream; one for another
    // session is logged and ignored.
    bool settingsMessageApplies(const Message& message, const std::string& sessionId);
    void pollClockSync(uint64_t nowNs);
    void pollBufferStatus(uint64_t nowNs);
    void pollPlaybackReport(uint64_t nowNs);
    void pollSystemReport(uint64_t nowNs);
    void releaseStream();
    bool formatIsPlayable(const AudioFormat& format) const;
    void fail(const std::string& reason);

    ServerConnection& connection_;
    Identity identity_;
    CapabilityProvider capabilities_;
    StreamHooks stream_;
    SystemProvider system_;
    AudioDeviceProvider audioDevices_;
    AudioDeviceSetter setAudioDevice_;
    AudioRescanner rescanAudio_;
    InputControlProvider inputControls_;
    bool deepSleep_ = false;
    InputBindingSetter setInputBindings_;
    BufferProvider buffer_;
    BufferSetter setBuffer_;
    RoomNameProvider roomName_;
    RoomNameSetter setRoomName_;
    AudioControlHook audioControl_;
    VolumeConfigProvider volumeConfig_;
    VolumeControlSetter setVolumeControl_;
    DriftCorrectionHook driftCorrection_;
    DriftFeedbackHook driftFeedback_;
    DriftCorrectionSetter setDriftCorrection_;

    // Guards the inbox, and only the inbox: it is the one thing two threads
    // touch.
    mutable std::mutex mutex_;
    std::string inbox_;
    uint64_t inboxGeneration_ = 0;
    // Set when inbox_ would have outgrown its limit; the bytes are dropped and
    // the next poll() fails the session, since the stream has lost its framing.
    bool inboxOverflow_ = false;

    // Where each read the connection delivered ends in inbox_, and when it
    // arrived: the closest thing to a receive timestamp a byte stream has. A
    // message is stamped with the read that completed it. Not with the first
    // read of the batch: the batch can begin before a clock sync request goes
    // out -- another message arriving between the last poll and the request --
    // and its reply would then be stamped before it was asked for, a C4 before
    // C1 the server rightly refuses.
    struct InboxChunk
    {
        size_t end = 0;
        uint64_t arrivalNs = 0;
    };
    std::vector<InboxChunk> inboxChunks_;

    // Which of the connection's connections this state machine belongs to.
    // A handshake never survives the socket it was made on.
    uint64_t generation_ = 0;

    FrameReader reader_;
    SessionState state_ = SessionState::Idle;
    uint64_t handshakeStartedNs_ = 0;

    // When the last message of any kind arrived, and what an established
    // session does about that not happening. A half-open TCP connection --
    // a cut cable, a NAT that dropped the mapping, a server that was killed
    // without its socket being closed -- is invisible to a reader: it never
    // ends and never errors, and a client that only waits would stay silently
    // attached to a room it is no longer part of. The handshake has its own
    // deadline above; this one covers everything after it.
    uint64_t lastHeardFromNs_ = 0;

    std::string sessionId_;
    bool hasStreamFormat_ = false;
    AudioFormat streamFormat_;
    bool streamFormatPlayable_ = false;

    // Serialized, so a rescan that changed nothing can be told from one that
    // did without comparing format lists by hand.
    std::string lastCapabilities_;

    // Clock synchronization. One request is outstanding at a time: twenty in
    // flight together would measure the time they spend queued behind each
    // other rather than the network (section 8 asks for repetition, not for a
    // burst on the wire).
    ClockModel clock_;
    bool clockRequestPending_ = false;
    uint64_t clockRequestSendNs_ = 0; // C1 of the outstanding request
    uint64_t clockRequestDeadlineNs_ = 0;
    uint64_t clockNextRequestNs_ = 0;
    uint32_t clockBurstRemaining_ = 0;
    uint32_t clockBurstAttemptsLeft_ = 0;
    uint32_t clockSyncRequests_ = 0;
    uint32_t clockSyncReplies_ = 0;
    uint32_t clockSyncResults_ = 0;

    // The stream this session carries. Prepared on PREPARE_STREAM and
    // released with the session, because the audio connection is bound to the
    // session and cannot outlive it (section 13).
    bool streamPrepared_ = false;
    bool standby_ = false;
    bool manualStandby_ = false;
    bool hasPrepareRequest_ = false;
    PrepareStream prepareRequest_;
    uint32_t scheduledStarts_ = 0;
    uint32_t scheduledPauses_ = 0;
    uint32_t scheduledStops_ = 0;
    uint32_t scheduledFlushes_ = 0;
    uint32_t volumeChanges_ = 0;
    uint32_t trackBoundaries_ = 0;
    uint32_t trackInfos_ = 0;
    uint32_t pongs_ = 0;
    uint64_t bufferStatusNextNs_ = 0;
    uint32_t bufferStatusReports_ = 0;
    uint64_t playbackReportNextNs_ = 0;
    uint32_t playbackReports_ = 0;
    uint64_t systemReportNextNs_ = 0;
    uint32_t systemReports_ = 0;

    // Serialized, like lastCapabilities_, so an unchanged list is not re-sent.
    std::string lastAudioDevices_;
    uint32_t audioDeviceReports_ = 0;
    uint32_t audioDeviceChanges_ = 0;
    uint32_t audioRescans_ = 0;

    std::string lastInputControls_;
    uint32_t inputControlReports_ = 0;
    uint32_t inputActions_ = 0;
    uint32_t inputBindingChanges_ = 0;

    // Serialized, like lastAudioDevices_. Cleared by a SET_BUFFER, which is
    // answered even when the value did not change.
    std::string lastBufferConfig_;
    uint32_t bufferConfigReports_ = 0;
    uint32_t bufferChanges_ = 0;

    // The same for ROOM_NAME, cleared by a SET_ROOM_NAME.
    std::string lastRoomName_;
    uint32_t roomNameReports_ = 0;

    // The same for VOLUME_CONFIG, cleared by a SET_VOLUME_CONTROL.
    std::string lastVolumeConfig_;
    uint32_t volumeConfigReports_ = 0;
    uint32_t volumeControlChanges_ = 0;

    // The newest DRIFT_FEEDBACK applied. The server numbers them per session,
    // so this starts again with a connection.
    uint64_t lastDriftSequence_ = 0;
    uint32_t driftFeedbacks_ = 0;
    uint32_t driftCorrectionChanges_ = 0;

    std::string lastError_;
    uint32_t handshakes_ = 0;
};

} // namespace matp
} // namespace multiroom
