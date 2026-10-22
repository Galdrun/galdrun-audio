#pragma once

// One MATP control connection, as a state machine and nothing else.
//
// Bytes in, frames out. It opens no socket, owns no thread and reads no
// global: whoever holds the connection hands it what arrived and writes what
// it produced. That is what lets the whole of sections 10 to 12 -- the
// handshake, the version check, the capability exchange, the clock sync and
// the liveness timers -- be tested in process, without a port and without
// sleeping through a twenty-second timeout. The client's `matp::Session` is
// the same shape for the same reason, and it is the parser on the other end of
// every frame this class produces.
//
// **Time is injected** (`const IClock&`), and the distinction the client's session
// draws applies here too: the ping and the silence timeout are *scheduling*
// and read the clock when poll() runs, but S2 and S3 of a clock exchange are a
// *measurement* and are read at the instant they describe -- S2 when the bytes
// came off the socket, S3 when the reply is handed back to it. Timing an
// exchange off a poll loop would measure that loop's period instead of the
// network, which is the one number section 8 exists to avoid.
//
// What it is not: it holds no zone, no source, no decoder and no output. A
// client that has connected, reported what it can play and synchronized its
// clock but has been given no zone is in a completely normal state -- idle
// rather than broken -- and that is the state every session is in at the end
// of phase 8.

#include "audio/AudioFormat.h"
#include "client/ClientClockModel.h"
#include "client/OutputPlan.h"
#include "core/Error.h"
#include "core/Ids.h"
#include "matp/Input.h"
#include "matp/Protocol.h"
#include "util/Clock.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <span>
#include <string>
#include <string_view>

namespace multiroom {

class ClientSession;

// The handshake of section 10, from this end.
//
// `Configured` is deliberately **re-enterable**: section 12 makes a second
// SESSION_CONFIG a re-negotiation rather than an error, and the client is
// already built for it. That is what lets phase 15's format transition reuse
// these three messages instead of needing new client machinery, and it is why
// this is not a terminal state.
enum class SessionState
{
    Connected,  // the socket is up; nothing has been said yet
    Identified, // HELLO accepted and HELLO_ACK sent; waiting for capabilities
    Configured, // capabilities known and SESSION_CONFIG sent -- re-enterable
    Closed      // finished; lastError() says why
};

const char* sessionStateName(SessionState state);

enum class SessionError
{
    NotIdentified,     // something arrived before HELLO
    VersionMismatch,   // section 51: a peer speaking another version
    MalformedMessage,  // framing, JSON, or a body that does not parse
    UnexpectedMessage, // a message this side of the protocol never receives
    Unsolicited,       // an answer to a question this server never asked
    WrongSession,      // a session id that is not this one
    OutboundFull,      // the peer stopped reading and the queue hit its limit
    PeerSilent,        // no message for the silence timeout; a half-open socket
    Closed             // the session is over and does not reopen
};

const char* errorName(SessionError error);

// The two reasons a session is closed with by the listener rather than by a
// rule of this class, spelled once because the control API tells them apart
// (phase 24's `room.disconnected`): the peer hung up, and a newer session of
// the same client took its place (section 43). Every other reason is an
// error's own text, a PeerSilent one included.
inline constexpr std::string_view kClosedByPeer = "the client closed the connection";
inline constexpr std::string_view kReplacedByReconnect =
    "replaced by a new connection from the same client";

// What a session tells its owner about the room behind it.
//
// The same shape the client's own `matp::Session::Hooks` has, and for the same
// reason: the state machine knows *what* happened and nothing about what to do
// with it. The zone that owns a room is what creates its output, paces it and
// tears it down, and in phase 9 that owner is `main.cpp`.
//
// **Called on the owner's thread, synchronously, and the reference is valid
// for the call only.** A hook that kept the session would be keeping a state
// machine another thread is inside, which is exactly what ControlServer
// refuses to hand out.
struct ClientSessionHooks
{
    // What SESSION_CONFIG is to announce, asked once per capability exchange.
    //
    // A room with no zone has nothing to be right about and is told its own
    // ceiling (idleStreamFormat below); a room a zone owns is told what that
    // zone's OutputPlanner decided, because announcing the ceiling and then
    // sending the plan would be two different formats on one stream. Returning
    // false is "no zone has an opinion" and keeps the ceiling, which is also
    // what a zone with no source yet answers.
    //
    // The session asks rather than being told, because the answer depends on
    // the capability list that has only just arrived -- and asking keeps the
    // format decision in the planner instead of splitting it across a hook
    // and a state machine.
    std::function<bool(const ClientSession&, AudioFormat&)> onChooseFormat;

    // The device answered PREPARE_STREAM (section 12), either way:
    // `streamReady()` says which. `grantedFormat()` is what it granted, which
    // is not always what SESSION_CONFIG asked for; `streamNotReadyReason()` is
    // why it granted nothing.
    std::function<void(const ClientSession&)> onStreamReady;

    // PREPARE_STREAM is about to go out. The client lets go of its audio
    // connection on every one (it reopens the device and connects again), so
    // whatever is written to the old one from here on is lost: the owner lets
    // go of it first, and the stream resumes on the connection that attaches next.
    std::function<void(const ClientSession&)> onPrepareStream;

    // Section 19, four times a second once a stream is prepared. The only
    // input this server's pacing has.
    std::function<void(const ClientSession&, const matp::BufferStatus&)> onBufferStatus;

    // Section 25, twice a second once a device is open. Phase 11 reads one
    // field of it: a report that names an audible timeline position is this
    // room confirming that its START_AT took, and it is the only
    // acknowledgement the protocol has. Phase 12 reads the rest, in the zone:
    // the device frame position against server time is that card's drift, and
    // what was audible against what the zone's line says should have been is
    // that room's offset.
    std::function<void(const ClientSession&, const matp::PlaybackReport&)> onPlaybackReport;

    // Section 55, every 2 s once the session is established (phase 34): how
    // the room's machine is doing. Only well-formed reports for this session
    // arrive here.
    std::function<void(const ClientSession&, const SystemMetrics&)> onSystemReport;

    // Section 56: the room's sound cards, the one configured and the one it
    // plays on. Sent once the session is established and again on every
    // change. Only well-formed lists for this session arrive here.
    std::function<void(const ClientSession&, const matp::AudioDeviceList&)> onAudioDevices;

    // Section 58: the size of the room's own buffer, once the session is
    // established, as the answer to every SET_BUFFER it accepted, and on every
    // other change. Only well-formed reports for this session arrive here.
    std::function<void(const ClientSession&, uint32_t bufferMs)> onBufferConfig;

    // Section 60: the room's name, once the session is established, as the
    // answer to every SET_ROOM_NAME it accepted, and on every other change.
    // Only well-formed reports for this session arrive here.
    std::function<void(const ClientSession&, const std::string& name)> onRoomName;

    // Section 59: the room's Audio Control, which every SESSION_CONFIG names so
    // that the client knows what it may do to the samples. Without the hook
    // none is named, and the client reads the room as bitPerfect.
    std::function<AudioControl(const ClientSession&)> onAudioControl;

    // Section 12: an established session's client sent AUDIO_CAPABILITIES
    // again -- another device, a rescan, a buffer that changed which formats
    // fit. After the re-negotiation it starts; the first report of a session
    // is its arrival, and does not come here.
    std::function<void(const ClientSession&)> onCapabilitiesChanged;

    // Section 59: the room's Volume Control and the path its client resolved,
    // once the session is established, as the answer to every
    // SET_VOLUME_CONTROL it accepted, and on every other change. Only
    // well-formed reports for this session arrive here.
    std::function<void(const ClientSession&, const matp::VolumeConfig&)> onVolumeConfig;

    // Section 57: the room's buttons and encoders and what each gesture is
    // bound to, once the session is established and again on every change;
    // and a gesture that happened. Only well-formed messages for this session
    // arrive here; one that does not parse is logged and dropped. The flag is
    // whether the client can run deepSleep.
    std::function<void(const ClientSession&, const std::vector<matp::InputControl>&, bool)>
        onInputControls;
    std::function<void(const ClientSession&, const matp::InputActionEvent&)> onInputAction;

    // The session is over, for any reason. Its room's output stops with it.
    std::function<void(const ClientSession&)> onClosed;
};

struct ClientSessionSettings
{
    // Liveness. The client fails its own session after 20 s of total silence
    // (its Session's silence timeout), and the server works to the same order
    // for the same reason: a half-open TCP connection -- a cut cable, a NAT
    // that dropped the mapping, a peer killed without its socket being closed
    // -- never ends and never errors, so a server that only waited would keep
    // a room that is not there. The ping is the nudge that turns silence into
    // an answer or into a certainty.
    uint64_t pingIntervalNs = 5ULL * 1000000000ULL;
    uint64_t silenceTimeoutNs = 20ULL * 1000000000ULL;

    // Memory is bounded, and this is where a client that stopped reading its
    // socket stops costing anything. Sixty-four frames is well past anything
    // the control path produces in a burst -- the handshake is four messages
    // and the steady state is one ping every five seconds -- so reaching it
    // means the peer is not draining, which fails *that* session and no other.
    size_t maxOutboundFrames = 64;

    // Where this room's audio connection goes (section 3, and PREPARE_STREAM's
    // one field). The client's fallback when a server does not say is "control
    // port plus one", and the default pair 4711/4712 makes that fallback
    // right -- but it is sent explicitly all the same, because a fallback that
    // happens to be correct is how a non-default port becomes a silent
    // failure.
    uint16_t audioPort = 4712;

    ClientSessionHooks hooks;
};

// The format SESSION_CONFIG names for a room that has no zone.
//
// Section 10 has SESSION_CONFIG answer AUDIO_CAPABILITIES, and the client
// waits for it: a server that stayed silent because it had no source yet would
// hang every handshake until a zone happened to exist. So one is always sent,
// and with no source there is nothing to be right about -- the announcement is
// a placeholder that the re-negotiation of section 12 replaces the moment a
// zone assigns this room.
//
// The rule is the room's **ceiling**, because with no source the most useful
// thing an announcement can say is what this room is capable of:
//
//   1. the rate is `preferredSampleRate` when the client offers a format at
//      it, and otherwise the highest rate it offers -- the preference is a
//      hint from the client's own configuration and section 12 says the server
//      may ignore it, but nothing here has a better reason to;
//   2. at that rate, the most channels it offers;
//   3. at that rate and channel count, the most valid bits, integer before
//      float at equal bits, and the narrower container at equal bits.
//
// **Not the client's own list order**, which was the first thing tried and is
// wrong: the real client builds its list by looping layout, then channel
// count, then rate, so "first offered" is the first thing a probe happened to
// accept -- mono 16-bit on a stereo device. Section 12 makes that order a
// tie-break between candidates that already rank equally, not a preference.
//
// This is the only format decision outside OutputPlanner, and it is only one
// because there is no source for the planner to reason about. The moment a
// zone assigns this room, the planner's answer replaces it.
//
// Null when the list is empty, which the parser already refuses.
[[nodiscard]] const AudioFormat* idleStreamFormat(const ClientCapabilities& capabilities);

class ClientSession
{
public:
    // The session id is generated by the owner rather than here, so that a
    // test can name one and a reconnect can be given a new one deliberately.
    // It is 128 bits and a hex string at the same time (section 13), which is
    // what keeps SESSION_CONFIG's string and the audio header's two uint64s
    // from ever disagreeing.
    ClientSession(SessionId id, const IClock& clock, ClientSessionSettings settings = {});

    ClientSession(const ClientSession&) = delete;
    ClientSession& operator=(const ClientSession&) = delete;

    // Bytes as they came off the socket, in any grouping: one read is not one
    // message. Whole messages are decoded, answered, and the answers left in
    // the outbound queue.
    //
    // A failure means this connection is over: the session is already Closed
    // and has queued whatever ERROR the peer is owed, so the caller flushes
    // the queue and then closes the socket.
    Status<SessionError> receive(std::span<const std::byte> bytes);

    // Time passing, and nothing else: the ping and the silence timeout. Safe
    // to call as often as the owner likes.
    Status<SessionError> poll();

    // The next frame to write, if any. Frames are handed over one at a time
    // because a partial write has to be resumed, and a caller that took them
    // all would have to buffer what it could not send.
    [[nodiscard]] bool nextOutbound(std::string& frame);

    [[nodiscard]] size_t pendingOutbound() const noexcept { return outbound_.size(); }

    // Section 20's START_AT: make `timelineNs` audible at `serverTimeNs`. The
    // pair is the zone's -- which room, which position and which instant are
    // decided by whatever owns the timeline -- and the session id is filled in
    // here, because it is the one field of the message this class owns.
    //
    // Refused before the device has granted a stream: a client answers a
    // START_AT it cannot place by logging to itself (section 20 gives the
    // server no way to hear about it), so a start this side knows is early is
    // better not sent than sent and silently dropped.
    //
    // **The owner's thread, like every other call here.** A zone reaches it
    // through ControlServer::send, which is the thread boundary.
    Status<SessionError> startPlayback(const matp::StartAt& start);

    // Sections 37 and 38, which carry what START_AT carries and differ only in
    // what a room does about them: a pause keeps what it holds for the resume,
    // a stop drops it. Refused before the device has granted a stream, for the
    // same reason a start is -- there is nothing there to pause or to stop.
    Status<SessionError> pausePlayback(const matp::TransportAt& at);
    Status<SessionError> stopPlayback(const matp::TransportAt& at);

    // Section 54 (phase 26): the same two numbers once more, and a third thing
    // a room does with them -- it drops what it holds from the cut on and keeps
    // its start mapping, so the audio that follows plays on the same line
    // without a START_AT. Refused before a granted stream, like the other two.
    Status<SessionError> flushPlayback(const matp::TransportAt& at);

    // Section 39, forwarded and never applied: the gain belongs to the
    // client's own render path, and a server that scaled a sample would end
    // bitperfect transport for a room that only wanted to be quieter.
    //
    // Refused where it would ask for nothing -- neither a value nor a mute --
    // which is the rule the wire library states and the client's parser
    // enforces; a message its own parser rejects is not one to send.
    Status<SessionError> setVolume(const matp::SetVolume& request);

    // Sections 23 and 44's metadata, which changes nothing about playback. A
    // boundary says where the next track begins and is announced before the
    // room gets there; the info says what is playing at the moment it arrives,
    // and exists for the room that joined mid-track and never received the
    // boundary that named it.
    //
    // Both need a prepared stream and no more: they belong to a stream rather
    // than to a device, and a room whose driver refused is still a room this
    // zone has something to say to. A TRACK_INFO naming neither a track id nor
    // a title is refused here, as the wire library refuses it.
    Status<SessionError> sendTrackBoundary(const matp::TrackBoundary& boundary);
    Status<SessionError> sendTrackInfo(const matp::TrackInfo& info);

    // Section 12's re-negotiation, asked for by this end (phase 17): another
    // SESSION_CONFIG and PREPARE_STREAM pair, with the format chosen afresh
    // through `onChooseFormat`. It is what a room whose zone or settings the
    // control API changed needs, because the format its device holds open was
    // chosen for the old answer and the client reopens it only when asked.
    //
    // A room that has not reported its capabilities yet has nothing to be
    // re-negotiated: the exchange still to come asks the same question, so
    // this succeeds and sends nothing.
    Status<SessionError> renegotiate();

    // SET_AUDIO_DEVICE (section 56): the client persists `deviceId` (empty for
    // its system default), reports its devices again and, when the device it
    // plays on changed, its capabilities -- which is section 12's
    // re-negotiation and needs nothing more from this end. Needs an
    // established session, because the message names it.
    Status<SessionError> setAudioDevice(const std::string& deviceId);

    // RESCAN_AUDIO (section 56): the client scans its cards again and reports
    // what it found the same way.
    Status<SessionError> rescanAudio();

    // SET_BUFFER (section 58): the client validates, persists and applies the
    // size of its own buffer and answers with BUFFER_CONFIG, or refuses it with an
    // ERROR (code "setBuffer") and keeps the one it had. It never clamps, so
    // what `onBufferConfig` reports is exactly what was asked or unchanged.
    Status<SessionError> setBuffer(uint32_t bufferMs);

    // SET_ROOM_NAME (section 60): the client validates and persists the room's
    // new name and answers with ROOM_NAME, or refuses it with an ERROR (code
    // "setRoomName") and keeps the one it had. Nothing else about the session
    // changes: the name is a label, the client id the identity.
    Status<SessionError> setRoomName(const std::string& name);

    // SET_VOLUME_CONTROL (section 59): the client validates and persists how
    // it plays the room's volume and answers with VOLUME_CONFIG, or refuses it
    // with an ERROR (code "setVolumeControl") -- "software" under bitPerfect
    // always is -- and keeps the one it had.
    Status<SessionError> setVolumeControl(matp::VolumeControlMode mode);

    // SET_DRIFT_CORRECTION: the client persists the actuator it asks for and
    // answers with AUDIO_CAPABILITIES, which renegotiates this session -- or
    // refuses with an ERROR (code "setDriftCorrection"). Only for a client
    // that advertised driftFeedback.
    Status<SessionError> setDriftCorrection(matp::DriftCorrectionMode mode);

    // DRIFT_FEEDBACK, numbered here. Sent only to a client that advertised
    // it; its hardware target only while this session's hardware clock is
    // the selected and confirmed actuator. A client that never said it reads
    // the message gets nothing, which is not an error.
    Status<SessionError> sendDriftFeedback(matp::DriftFeedback feedback);

    // SET_INPUT_BINDINGS (section 57): the client validates and persists the
    // bindings and answers with INPUT_CONTROLS, or with an ERROR it refuses.
    // The session id is filled here.
    Status<SessionError> setInputBindings(const matp::SetInputBindings& request);

    // Ends the session with a reason, queuing an ERROR for the peer. Calling
    // it twice keeps the first reason: the first thing that went wrong is the
    // one worth reporting.
    void close(std::string_view reason);

    [[nodiscard]] SessionState state() const noexcept { return state_; }
    [[nodiscard]] const std::string& lastError() const noexcept { return lastError_; }

    [[nodiscard]] const SessionId& sessionId() const noexcept { return sessionId_; }

    // Empty until HELLO has been accepted.
    [[nodiscard]] const ClientId& clientId() const noexcept { return clientId_; }
    [[nodiscard]] const matp::Identity& identity() const noexcept { return identity_; }

    // What the client last reported it can play (section 12), and the format
    // this server told it to expect. Both are empty until AUDIO_CAPABILITIES.
    [[nodiscard]] bool hasCapabilities() const noexcept { return hasCapabilities_; }

    // The device's own volume control, as the last AUDIO_CAPABILITIES said
    // (section 12); unknown while hasHardwareVolume() is false. Reported on
    // only: SET_VOLUME is the same whichever path the client plays it on.
    [[nodiscard]] bool hasHardwareVolume() const noexcept { return hasHardwareVolume_; }
    [[nodiscard]] const matp::HardwareVolumeCapability& hardwareVolume() const noexcept
    {
        return hardwareVolume_;
    }
    // The client's drift-correction setting and its device's clock trim, as
    // AUDIO_CAPABILITIES reported them, whether or not this session selected
    // it. capabilities().hardwareClock is the trim in use: the one selected
    // in SESSION_CONFIG and confirmed in STREAM_READY.
    [[nodiscard]] bool driftFeedback() const noexcept { return capabilities_.driftFeedback; }
    [[nodiscard]] matp::DriftCorrectionMode driftCorrection() const noexcept
    {
        return driftCorrection_;
    }
    [[nodiscard]] const matp::HardwareClockCapability& hardwareClock() const noexcept
    {
        return hardwareClock_;
    }
    [[nodiscard]] uint64_t driftFeedbacksSent() const noexcept { return driftSequence_; }

    [[nodiscard]] const ClientCapabilities& capabilities() const noexcept
    {
        return capabilities_;
    }
    [[nodiscard]] const std::string& deviceName() const noexcept { return deviceName_; }
    // The client's IP as its control connection came from, set by the listener
    // that accepted it; empty where there was no socket (the tests).
    [[nodiscard]] const std::string& peerAddress() const noexcept { return peerAddress_; }
    void setPeerAddress(std::string address) { peerAddress_ = std::move(address); }
    [[nodiscard]] const AudioFormat& announcedFormat() const noexcept
    {
        return announcedFormat_;
    }

    // What the device actually granted, and whether it granted anything
    // (section 12). The two are kept apart from the announced format above
    // because a driver that answers with something else is the case section 12
    // exists for: the server must send what was confirmed here and not what it
    // asked for, and phase 6's re-plan is what turns the difference into a
    // room's plan. Empty until STREAM_READY.
    [[nodiscard]] bool streamReady() const noexcept { return streamReady_; }
    [[nodiscard]] bool streamStandby() const noexcept { return streamStandby_; }
    [[nodiscard]] const std::string& powerState() const noexcept { return powerState_; }
    // With powerState "sleep": the MAC a Wake-on-LAN magic packet wakes.
    [[nodiscard]] const std::string& wakeOnLan() const noexcept { return wakeOnLan_; }
    Status<SessionError> wakeStream();
    [[nodiscard]] const AudioFormat& grantedFormat() const noexcept { return grantedFormat_; }
    [[nodiscard]] uint32_t grantedPeriodFrames() const noexcept { return grantedPeriodFrames_; }

    // Why a device did not open, straight from the client. Empty while it did.
    [[nodiscard]] const std::string& streamNotReadyReason() const noexcept
    {
        return streamNotReadyReason_;
    }

    // The last figure this room reported (section 19). `hasContent` is false
    // until its first packet has arrived.
    [[nodiscard]] const matp::BufferStatus& bufferStatus() const noexcept
    {
        return bufferStatus_;
    }

    // The model of this client's clock, fed by CLOCK_SYNC_RESULT. It is built
    // from the first exchange because the data only arrives while the session
    // is up; phase 12's sync controller is what reads it, and travels with
    // every PLAYBACK_REPORT into the zone to do so.
    [[nodiscard]] const ClientClockModel& clock() const noexcept { return clockModel_; }

    // Diagnostics, and what the tests assert on.
    [[nodiscard]] uint64_t clockSyncReplies() const noexcept { return clockSyncReplies_; }
    [[nodiscard]] uint64_t clockSyncResults() const noexcept { return clockSyncResults_; }
    [[nodiscard]] uint64_t sessionConfigs() const noexcept { return sessionConfigs_; }
    [[nodiscard]] uint64_t prepareStreams() const noexcept { return prepareStreams_; }
    [[nodiscard]] uint64_t streamReadies() const noexcept { return streamReadies_; }
    [[nodiscard]] uint64_t bufferStatuses() const noexcept { return bufferStatuses_; }
    [[nodiscard]] uint64_t playbackReports() const noexcept { return playbackReports_; }
    [[nodiscard]] uint64_t systemReports() const noexcept { return systemReports_; }
    [[nodiscard]] uint64_t systemReportsRejected() const noexcept
    {
        return systemReportsRejected_;
    }
    [[nodiscard]] uint64_t audioDeviceReports() const noexcept { return audioDeviceReports_; }
    [[nodiscard]] uint64_t bufferConfigReports() const noexcept { return bufferConfigReports_; }
    [[nodiscard]] uint64_t roomNameReports() const noexcept { return roomNameReports_; }
    [[nodiscard]] uint64_t inputControlReports() const noexcept { return inputControlReports_; }
    [[nodiscard]] uint64_t inputActions() const noexcept { return inputActions_; }
    [[nodiscard]] uint64_t startsSent() const noexcept { return startsSent_; }
    [[nodiscard]] uint64_t pausesSent() const noexcept { return pausesSent_; }
    [[nodiscard]] uint64_t stopsSent() const noexcept { return stopsSent_; }
    [[nodiscard]] uint64_t flushesSent() const noexcept { return flushesSent_; }
    [[nodiscard]] uint64_t volumeChangesSent() const noexcept { return volumeChangesSent_; }
    [[nodiscard]] uint64_t trackBoundariesSent() const noexcept
    {
        return trackBoundariesSent_;
    }
    [[nodiscard]] uint64_t trackInfosSent() const noexcept { return trackInfosSent_; }
    [[nodiscard]] uint64_t pingsSent() const noexcept { return pingsSent_; }
    [[nodiscard]] uint64_t pongsReceived() const noexcept { return pongsReceived_; }
    [[nodiscard]] uint64_t lastHeardFromNs() const noexcept { return lastHeardFromNs_; }

private:
    Status<SessionError> handle(const matp::Message& message, uint64_t arrivalNs);
    Status<SessionError> handleHello(const matp::Message& message);
    Status<SessionError> handleCapabilities(const matp::Message& message);
    Status<SessionError> handleClockSyncRequest(const matp::Message& message,
                                                uint64_t arrivalNs);
    Status<SessionError> handleClockSyncResult(const matp::Message& message);
    Status<SessionError> handlePong(const matp::Message& message);
    Status<SessionError> handleStreamReady(const matp::Message& message);
    Status<SessionError> handleBufferStatus(const matp::Message& message);
    Status<SessionError> handlePlaybackReport(const matp::Message& message);
    Status<SessionError> handleSystemReport(const matp::Message& message);
    Status<SessionError> handleAudioDevices(const matp::Message& message);
    Status<SessionError> handleBufferConfig(const matp::Message& message);
    Status<SessionError> handleRoomName(const matp::Message& message);
    Status<SessionError> handleVolumeConfig(const matp::Message& message);
    Status<SessionError> handleInputControls(const matp::Message& message);
    Status<SessionError> handleInputAction(const matp::Message& message);

    // Section 12's three messages in order: what this room will be sent, and
    // then the instruction to open its device for it. They go out together
    // because the client waits for the first and acts on the second, and a
    // second pair is a re-negotiation rather than an error -- which is what
    // phase 15's format transition reuses.
    Status<SessionError> sendSessionConfig();
    Status<SessionError> sendPrepareStream();

    // What every message this server sends about a running stream has in
    // common: it belongs to a session, and a session that has not prepared a
    // stream has nothing for one to be about. `needsDevice` adds section 20's
    // second condition, which the two transport messages need and the two
    // metadata ones do not.
    [[nodiscard]] Status<SessionError> streamMessageAllowed(const char* what,
                                                            bool needsDevice) const;

    // Every message a client sends after PREPARE_STREAM carries the session it
    // answers for, and one that names another session is not this room's.
    [[nodiscard]] bool isThisSession(const std::string& text) const noexcept;
    Status<SessionError> send(matp::MessageType type, const json::Value& payload);

    // Records the reason, queues the ERROR, and returns the failure the caller
    // propagates. Every path that ends a session goes through it, so there is
    // one place where "closed" and "the peer was told why" are the same event.
    [[nodiscard]] std::unexpected<Failure<SessionError>> failSession(SessionError code,
                                                                     std::string_view wireCode,
                                                                     std::string detail);

    SessionId sessionId_;
    const IClock& clock_;
    ClientSessionSettings settings_;

    matp::FrameReader reader_;
    std::deque<std::string> outbound_;

    SessionState state_ = SessionState::Connected;
    std::string lastError_;

    matp::Identity identity_;
    ClientId clientId_;

    bool hasCapabilities_ = false;
    bool hasHardwareVolume_ = false;
    matp::HardwareVolumeCapability hardwareVolume_;
    matp::DriftCorrectionMode driftCorrection_ = matp::DriftCorrectionMode::Resampling;
    matp::HardwareClockCapability hardwareClock_;
    // Selected in the last SESSION_CONFIG; failed once the client reported a
    // write it could not make, after which this session resamples.
    bool hardwareClockSelected_ = false;
    bool hardwareClockFailed_ = false;
    uint64_t driftSequence_ = 0;
    ClientCapabilities capabilities_;
    std::string deviceName_;
    std::string peerAddress_;
    AudioFormat announcedFormat_;

    bool streamPrepared_ = false; // PREPARE_STREAM is out; its answers are expected
    bool streamReady_ = false;
    bool streamStandby_ = false;
    std::string powerState_;
    std::string wakeOnLan_;
    AudioFormat grantedFormat_;
    uint32_t grantedPeriodFrames_ = 0;
    std::string streamNotReadyReason_;
    matp::BufferStatus bufferStatus_;

    ClientClockModel clockModel_;

    uint64_t lastHeardFromNs_ = 0;
    uint64_t pingSequence_ = 0;
    bool pingOutstanding_ = false;

    uint64_t clockSyncReplies_ = 0;
    uint64_t clockSyncResults_ = 0;
    uint64_t sessionConfigs_ = 0;
    uint64_t prepareStreams_ = 0;
    uint64_t streamReadies_ = 0;
    uint64_t bufferStatuses_ = 0;
    uint64_t playbackReports_ = 0;
    uint64_t systemReports_ = 0;
    uint64_t systemReportsRejected_ = 0;
    uint64_t audioDeviceReports_ = 0;
    uint64_t bufferConfigReports_ = 0;
    uint64_t roomNameReports_ = 0;
    uint64_t inputControlReports_ = 0;
    uint64_t inputActions_ = 0;
    uint64_t startsSent_ = 0;
    uint64_t pausesSent_ = 0;
    uint64_t stopsSent_ = 0;
    uint64_t flushesSent_ = 0;
    uint64_t volumeChangesSent_ = 0;
    uint64_t trackBoundariesSent_ = 0;
    uint64_t trackInfosSent_ = 0;
    uint64_t pingsSent_ = 0;
    uint64_t pongsReceived_ = 0;
};

} // namespace multiroom
