#include "client/ClientSession.h"

#include "util/Logger.h"

#include <algorithm>
#include <cmath>
#include <optional>

namespace multiroom {
namespace {

// The wire `code` of an ERROR body. Short, machine-readable, and stable: the
// human-readable half is the detail beside it.
constexpr const char* kCodeVersionMismatch = "versionMismatch";
constexpr const char* kCodeProtocol = "protocol";
constexpr const char* kCodeUnsolicited = "unsolicited";

// Types this server sends and therefore never receives. One of these arriving
// from a client is not a message out of order -- it is a peer playing the
// wrong half of the protocol, and continuing would mean guessing which half it
// thinks this server is.
bool isServerToClient(matp::MessageType type)
{
    switch (type)
    {
    case matp::MessageType::HelloAck:
    case matp::MessageType::SessionConfig:
    case matp::MessageType::ClockSyncReply:
    case matp::MessageType::PrepareStream:
    case matp::MessageType::StartAt:
    case matp::MessageType::PauseAt:
    case matp::MessageType::StopAt:
    case matp::MessageType::SetVolume:
    case matp::MessageType::TrackInfo:
    case matp::MessageType::TrackBoundary:
    case matp::MessageType::SetAudioDevice:
    case matp::MessageType::RescanAudio:
    case matp::MessageType::SetInputBindings:
    case matp::MessageType::SetBuffer:
    case matp::MessageType::SetVolumeControl:
    case matp::MessageType::SetRoomName:
    case matp::MessageType::SetDriftCorrection:
    case matp::MessageType::DriftFeedback:
        return true;
    default:
        return false;
    }
}

// A room's volume is a slider position; SET_VOLUME carries a linear gain
// (section 39). The slider is linear in decibels over kVolumeRangeDb, which is
// how the ear hears level: every step is the same change, 0.5 is -30 dB, and 0
// stays silence. A cube put half the travel into the top 18 dB, so rooms got
// loud early. The server's DesiredState combines positions on this scale.
constexpr double kVolumeRangeDb = 60.0;

double gainForPosition(double position)
{
    if (!(position > 0.0))
        return 0.0;
    return std::pow(10.0, kVolumeRangeDb * (position - 1.0) / 20.0);
}

} // namespace

const char* sessionStateName(SessionState state)
{
    switch (state)
    {
    case SessionState::Connected:
        return "connected";
    case SessionState::Identified:
        return "identified";
    case SessionState::Configured:
        return "configured";
    case SessionState::Closed:
        return "closed";
    }
    return "unknown";
}

const char* errorName(SessionError error)
{
    switch (error)
    {
    case SessionError::NotIdentified:
        return "NotIdentified";
    case SessionError::VersionMismatch:
        return "VersionMismatch";
    case SessionError::MalformedMessage:
        return "MalformedMessage";
    case SessionError::UnexpectedMessage:
        return "UnexpectedMessage";
    case SessionError::Unsolicited:
        return "Unsolicited";
    case SessionError::WrongSession:
        return "WrongSession";
    case SessionError::OutboundFull:
        return "OutboundFull";
    case SessionError::PeerSilent:
        return "PeerSilent";
    case SessionError::Closed:
        return "Closed";
    }
    return "Unknown";
}

namespace {

// Which of two layouts carries more of a sample, for the ceiling rule in
// ClientSession.h. Integer before float at equal bits because a float32
// mantissa carries 24 where an int32 carries 32, and the narrower container at
// equal bits because it is equally exact and smaller on the wire.
bool layoutOutranks(AudioFormat::SampleType candidate, AudioFormat::SampleType best)
{
    const uint32_t candidateBits = AudioFormat::validBits(candidate);
    const uint32_t bestBits = AudioFormat::validBits(best);
    if (candidateBits != bestBits)
        return candidateBits > bestBits;

    const bool candidateIsFloat = candidate == AudioFormat::SampleType::Float32;
    const bool bestIsFloat = best == AudioFormat::SampleType::Float32;
    if (candidateIsFloat != bestIsFloat)
        return bestIsFloat;

    return AudioFormat::bytesPerSample(candidate) < AudioFormat::bytesPerSample(best);
}

} // namespace

const AudioFormat* idleStreamFormat(const ClientCapabilities& capabilities)
{
    if (capabilities.formats.empty())
        return nullptr;

    // 1. The rate. The preference is used only where the device offers it; a
    //    preferredSampleRate the client cannot play is not a rate.
    uint32_t rate = 0;
    if (capabilities.preferredSampleRate != 0)
    {
        for (const AudioFormat& format : capabilities.formats)
        {
            if (format.sampleRate == capabilities.preferredSampleRate)
            {
                rate = format.sampleRate;
                break;
            }
        }
    }
    if (rate == 0)
    {
        for (const AudioFormat& format : capabilities.formats)
            rate = std::max(rate, format.sampleRate);
    }

    // 2 and 3, in one pass. Every comparison is strict, so equal candidates
    // leave the one the client listed first -- the tie-break section 12 does
    // give the list order.
    const AudioFormat* best = nullptr;
    for (const AudioFormat& format : capabilities.formats)
    {
        if (format.sampleRate != rate)
            continue;
        if (best == nullptr || format.channels > best->channels ||
            (format.channels == best->channels &&
             layoutOutranks(format.sampleType, best->sampleType)))
        {
            best = &format;
        }
    }

    return best;
}

ClientSession::ClientSession(SessionId id, const IClock& clock, ClientSessionSettings settings)
    : sessionId_(id), clock_(clock), settings_(settings),
      lastHeardFromNs_(clock.monotonicNowNs())
{
}

Status<SessionError> ClientSession::receive(std::span<const std::byte> bytes)
{
    if (state_ == SessionState::Closed)
        return fail(SessionError::Closed, lastError_);

    // The instant the bytes came off the socket. S2 of every clock exchange
    // completed out of this batch: the last byte of a message cannot have
    // arrived before the first byte of the batch that completed it, and this
    // is as close to a receive timestamp as a byte stream has.
    const uint64_t arrivalNs = clock_.monotonicNowNs();

    if (!bytes.empty())
        reader_.append(bytes.data(), bytes.size());

    for (;;)
    {
        matp::Message message;
        std::string error;
        const matp::FrameReader::Result result = reader_.next(message, error);
        if (result == matp::FrameReader::Result::Incomplete)
            break;
        if (result == matp::FrameReader::Result::Invalid)
        {
            // Framing, not content: the stream is out of step and only a new
            // connection fixes it, so there is nothing to skip past.
            return failSession(SessionError::MalformedMessage, kCodeProtocol,
                               std::move(error));
        }

        lastHeardFromNs_ = arrivalNs;

        Status<SessionError> handled = handle(message, arrivalNs);
        if (!handled)
            return handled;
    }

    return {};
}

Status<SessionError> ClientSession::handle(const matp::Message& message, uint64_t arrivalNs)
{
    // A type this build does not know is carried by name and ignored, which is
    // the same rule the client applies in the other direction: a later message
    // type must not break a peer that predates it.
    if (message.type == matp::MessageType::Unknown)
    {
        MULTIROOM_DEBUG("session {}: ignoring unknown message type \"{}\"", sessionId_.toHex(),
                  message.typeName);
        return {};
    }

    if (isServerToClient(message.type))
    {
        return failSession(SessionError::UnexpectedMessage, kCodeProtocol,
                           message.typeName + " is sent by a server, not to one");
    }

    if (state_ == SessionState::Connected && message.type != matp::MessageType::Hello)
    {
        // Section 10: HELLO is first and mandatory. Until it has arrived there
        // is no room to attribute anything to.
        return failSession(SessionError::NotIdentified, kCodeProtocol,
                           message.typeName + " arrived before hello");
    }

    switch (message.type)
    {
    case matp::MessageType::Hello:
        return handleHello(message);
    case matp::MessageType::AudioCapabilities:
        return handleCapabilities(message);
    case matp::MessageType::ClockSyncRequest:
        return handleClockSyncRequest(message, arrivalNs);
    case matp::MessageType::ClockSyncResult:
        return handleClockSyncResult(message);
    case matp::MessageType::Pong:
        return handlePong(message);
    case matp::MessageType::Ping:
        // The protocol gives PING no body and no direction, so one from a
        // client is answered the way the client answers ours: its own body
        // with the type changed.
        return send(matp::MessageType::Pong, matp::makePong(message.payload));
    case matp::MessageType::Error:
        // The peer is telling this server something went wrong at its end.
        // Recorded and not answered -- answering an error with an error is how
        // two implementations talk past each other forever.
        MULTIROOM_WARN("session {}: client reported {}", sessionId_.toHex(),
                 matp::errorText(message.payload));
        return {};
    case matp::MessageType::StreamReady:
        return handleStreamReady(message);
    case matp::MessageType::BufferStatus:
        return handleBufferStatus(message);
    case matp::MessageType::PlaybackReport:
        return handlePlaybackReport(message);
    case matp::MessageType::SystemReport:
        return handleSystemReport(message);
    case matp::MessageType::AudioDevices:
        return handleAudioDevices(message);
    case matp::MessageType::BufferConfig:
        return handleBufferConfig(message);
    case matp::MessageType::RoomName:
        return handleRoomName(message);
    case matp::MessageType::VolumeConfig:
        return handleVolumeConfig(message);
    case matp::MessageType::InputControls:
        return handleInputControls(message);
    case matp::MessageType::InputAction:
        return handleInputAction(message);
    case matp::MessageType::StreamAttach:
        // The one message that belongs on the other socket. A client that
        // sends it here has bound nothing, and a server that accepted it would
        // be binding a connection that carries no audio.
        return failSession(SessionError::Unsolicited, kCodeUnsolicited,
                           "streamAttach belongs on the audio connection");
    default:
        return failSession(SessionError::UnexpectedMessage, kCodeProtocol,
                           message.typeName + " is not a message this server handles");
    }
}

Status<SessionError> ClientSession::handleHello(const matp::Message& message)
{
    if (state_ != SessionState::Connected)
    {
        // A second HELLO would re-identify a connection that is already a
        // room, which is a reconnect and belongs on a new socket (section 43).
        return failSession(SessionError::UnexpectedMessage, kCodeProtocol,
                           "hello arrived twice on one connection");
    }

    matp::Hello hello;
    std::string error;
    if (!matp::parseHello(message.payload, hello, error))
        return failSession(SessionError::MalformedMessage, kCodeProtocol, std::move(error));

    if (hello.protocolVersion != matp::kProtocolVersion)
    {
        // Section 51: version 1 and version 2 disagree about what every
        // position on the wire means, so a half-understood session is worse
        // than no session. Refused here rather than later.
        return failSession(SessionError::VersionMismatch, kCodeVersionMismatch,
                           "client speaks protocol version " +
                               std::to_string(hello.protocolVersion) + ", this server speaks " +
                               std::to_string(matp::kProtocolVersion));
    }

    identity_ = hello.identity;
    clientId_ = ClientId(hello.identity.clientId);
    state_ = SessionState::Identified;

    MULTIROOM_INFO("session {}: hello from {} ({} on {})", sessionId_.toHex(), clientId_.str(),
             identity_.clientVersion.empty() ? "unknown version" : identity_.clientVersion,
             identity_.platform.empty() ? "unknown platform" : identity_.platform);

    return send(matp::MessageType::HelloAck, matp::makeHelloAck(matp::kProtocolVersion));
}

Status<SessionError> ClientSession::handleCapabilities(const matp::Message& message)
{
    matp::CapabilityReport report;
    std::string error;
    if (!matp::parseAudioCapabilities(message.payload, report, error))
        return failSession(SessionError::MalformedMessage, kCodeProtocol, std::move(error));

    const bool again = hasCapabilities_;
    capabilities_.formats = report.formats;
    capabilities_.preferredSampleRate = report.preferredSampleRate;
    capabilities_.accessMode = report.accessMode;
    deviceName_ = report.deviceName.empty() ? report.deviceId : report.deviceName;
    hasCapabilities_ = true;
    hasHardwareVolume_ = report.hasHardwareVolume;
    hardwareVolume_ = report.hardwareVolume;
    capabilities_.driftFeedback = report.driftFeedback;
    driftCorrection_ = report.driftCorrection;
    hardwareClock_ = report.hasHardwareClock ? report.hardwareClock
                                             : matp::HardwareClockCapability();

    // Re-negotiation rather than an error, in both directions (section 12): a
    // client whose device changed sends this again, and the answer is another
    // SESSION_CONFIG on the same session.
    MULTIROOM_INFO("session {}: {} offers {} format(s) on {}", sessionId_.toHex(), clientId_.str(),
             capabilities_.formats.size(), deviceName_.empty() ? "an unnamed device"
                                                               : deviceName_.c_str());

    Status<SessionError> configured = sendSessionConfig();
    if (again && settings_.hooks.onCapabilitiesChanged)
        settings_.hooks.onCapabilitiesChanged(*this);
    return configured;
}

Status<SessionError> ClientSession::sendSessionConfig()
{
    // A zone's planner first, because a room a zone owns is sent that zone's
    // source and not its own ceiling. A zone that has no source yet, or no
    // zone at all, falls through to the ceiling -- which section 12's
    // re-negotiation replaces the moment there is something better to say.
    AudioFormat fromZone;
    if (settings_.hooks.onChooseFormat && settings_.hooks.onChooseFormat(*this, fromZone) &&
        fromZone.isValid())
    {
        announcedFormat_ = fromZone;
    }
    else
    {
        const AudioFormat* chosen = idleStreamFormat(capabilities_);
        announcedFormat_ = chosen != nullptr ? *chosen : AudioFormat();
    }

    matp::SessionConfig config;
    config.sessionId = sessionId_.toHex();
    config.hasFormat = announcedFormat_.isValid();
    config.format = announcedFormat_;
    if (settings_.hooks.onAudioControl)
    {
        config.hasAudioControl = true;
        config.audioControl = settings_.hooks.onAudioControl(*this);
    }

    // The hardware clock only where the client asks for it (or for auto), reads the
    // feedback and has a verified trim -- and not again in a session where
    // a write to it failed. Planned on from here; withdrawn again if the
    // opened device does not confirm it (handleStreamReady).
    hardwareClockSelected_ = capabilities_.driftFeedback &&
                             driftCorrection_ != matp::DriftCorrectionMode::Resampling &&
                             hardwareClock_.available && !hardwareClockFailed_;
    capabilities_.hardwareClock =
        hardwareClockSelected_ ? hardwareClock_ : matp::HardwareClockCapability();
    if (capabilities_.driftFeedback)
    {
        config.hasDriftCorrection = true;
        config.driftCorrection = hardwareClockSelected_ ? matp::DriftCorrectionMode::HardwareClock
                                                        : matp::DriftCorrectionMode::Resampling;
    }

    ++sessionConfigs_;
    state_ = SessionState::Configured;

    if (!config.hasFormat)
    {
        // A client with no playable device: a room the server can still ask
        // to rescan or switch device, but nothing to prepare a stream on.
        MULTIROOM_INFO("session {}: {} reports no playable format; no stream", sessionId_.toHex(),
                 clientId_.str());
        Status<SessionError> sent =
            send(matp::MessageType::SessionConfig, matp::makeSessionConfig(config));
        if (!sent)
            return sent;

        streamPrepared_ = false;
        streamReady_ = false;
        streamStandby_ = false;
        grantedFormat_ = AudioFormat();
        grantedPeriodFrames_ = 0;
        streamNotReadyReason_ = "the client reports no playable audio device";
        bufferStatus_ = matp::BufferStatus();
        if (settings_.hooks.onStreamReady)
            settings_.hooks.onStreamReady(*this);
        return {};
    }

    MULTIROOM_INFO("session {}: stream format {}", sessionId_.toHex(),
             describeFormat(announcedFormat_));

    Status<SessionError> sent =
        send(matp::MessageType::SessionConfig, matp::makeSessionConfig(config));
    if (!sent)
        return sent;

    return sendPrepareStream();
}

Status<SessionError> ClientSession::sendPrepareStream()
{
    // Section 10's next step, and section 21's reason for it: the device opens
    // now and runs on silence, so that its clock is established and its buffer
    // primed by the time anything names an instant. A room with no zone is
    // served its own ceiling and prepares for that -- the re-negotiation of
    // section 12 is what replaces both the moment a zone assigns it, and the
    // client is built to reopen rather than to stack.
    matp::PrepareStream request;
    request.sessionId = sessionId_.toHex();
    request.hasAudioPort = true;
    request.audioPort = settings_.audioPort;

    // Whatever the previous stream reported is about a device that is being
    // reopened, so it is not this stream's answer any more.
    streamPrepared_ = true;
    streamReady_ = false;
    streamStandby_ = false;
    grantedFormat_ = AudioFormat();
    grantedPeriodFrames_ = 0;
    streamNotReadyReason_.clear();
    bufferStatus_ = matp::BufferStatus();
    ++prepareStreams_;

    MULTIROOM_INFO("session {}: prepare stream, audio port {}", sessionId_.toHex(),
             settings_.audioPort);

    if (settings_.hooks.onPrepareStream)
        settings_.hooks.onPrepareStream(*this);
    return send(matp::MessageType::PrepareStream, matp::makePrepareStream(request));
}

Status<SessionError> ClientSession::wakeStream()
{
    if (state_ != SessionState::Configured)
        return fail(SessionError::UnexpectedMessage, "wake before the room has a session");
    return streamStandby_ ? sendPrepareStream() : Status<SessionError>{};
}

bool ClientSession::isThisSession(const std::string& text) const noexcept
{
    const std::optional<SessionId> parsed = SessionId::fromHex(text);
    return parsed.has_value() && *parsed == sessionId_;
}

Status<SessionError> ClientSession::handleStreamReady(const matp::Message& message)
{
    if (!streamPrepared_)
    {
        return failSession(SessionError::Unsolicited, kCodeUnsolicited,
                           "streamReady answers a prepareStream this server has not sent");
    }

    std::string sessionText;
    matp::StreamReady ready;
    std::string error;
    if (!matp::parseStreamReady(message.payload, sessionText, ready, error))
        return failSession(SessionError::MalformedMessage, kCodeProtocol, std::move(error));
    if (!isThisSession(sessionText))
    {
        return failSession(SessionError::WrongSession, kCodeProtocol,
                           "streamReady names session " + sessionText + ", this is " +
                               sessionId_.toHex());
    }

    ++streamReadies_;
    streamReady_ = ready.ready;
    streamStandby_ = ready.standby;
    powerState_ = ready.powerState;
    wakeOnLan_ = ready.wakeOnLan;
    grantedFormat_ = ready.ready ? ready.format : AudioFormat();
    grantedPeriodFrames_ = ready.ready ? ready.periodFrames : 0;
    streamNotReadyReason_ = ready.ready ? std::string() : ready.reason;

    if (!ready.ready)
    {
        // Not a protocol error: section 12 has the client answer late and
        // honestly rather than optimistically, and a device that will not open
        // is a room this server has to wait for or give up on -- which is
        // phase 11's readiness timeout, not this class's.
        if (ready.standby)
            MULTIROOM_INFO("session {}: {} in standby", sessionId_.toHex(), clientId_.str());
        else
            MULTIROOM_WARN("session {}: {} is not ready ({})", sessionId_.toHex(), clientId_.str(),
                     streamNotReadyReason_.empty() ? "no reason given" : streamNotReadyReason_);

        // Still told: section 20's table drops a room that is not ready from
        // scheduling, and phase 24 says it is silent -- both the owner's.
        if (settings_.hooks.onStreamReady)
            settings_.hooks.onStreamReady(*this);
        return {};
    }

    MULTIROOM_INFO("session {}: {} granted {}, {} frame(s) per period", sessionId_.toHex(),
             clientId_.str(), describeFormat(grantedFormat_), grantedPeriodFrames_);
    if (hardwareClockSelected_ &&
        !(ready.hasDriftCorrection &&
          ready.driftCorrection == matp::DriftCorrectionMode::HardwareClock))
    {
        // The device opened without confirming its trim at nominal: this
        // stream is planned for resampling instead, before anything plays.
        MULTIROOM_WARN("session {}: {} did not confirm its hardware clock; resampling instead",
                 sessionId_.toHex(), clientId_.str());
        capabilities_.hardwareClock = matp::HardwareClockCapability();
    }
    else if (hardwareClockSelected_)
    {
        capabilities_.hardwareClock = hardwareClock_;
    }
    if (grantedFormat_ != announcedFormat_)
    {
        // Reported rather than corrected here. Section 12 forbids sending
        // audio in a format the client has not confirmed, and OutputPlanner's
        // replanForGrantedFormat is what turns the difference into this room's
        // plan.
        MULTIROOM_WARN("session {}: the driver granted {}, not the {} this server asked for",
                 sessionId_.toHex(), describeFormat(grantedFormat_),
                 describeFormat(announcedFormat_));
    }

    if (settings_.hooks.onStreamReady)
        settings_.hooks.onStreamReady(*this);
    return {};
}

Status<SessionError> ClientSession::handleBufferStatus(const matp::Message& message)
{
    if (!streamPrepared_)
    {
        return failSession(SessionError::Unsolicited, kCodeUnsolicited,
                           "bufferStatus reports on a stream this server has not prepared");
    }

    std::string sessionText;
    matp::BufferStatus status;
    std::string error;
    if (!matp::parseBufferStatus(message.payload, sessionText, status, error))
        return failSession(SessionError::MalformedMessage, kCodeProtocol, std::move(error));
    if (!isThisSession(sessionText))
    {
        return failSession(SessionError::WrongSession, kCodeProtocol,
                           "bufferStatus names session " + sessionText + ", this is " +
                               sessionId_.toHex());
    }

    ++bufferStatuses_;
    bufferStatus_ = status;

    // Four of these a second per room, so nothing here logs and nothing here
    // allocates: the hook hands the figure to that room's output and returns.
    if (settings_.hooks.onBufferStatus)
        settings_.hooks.onBufferStatus(*this, status);
    return {};
}

Status<SessionError> ClientSession::handlePlaybackReport(const matp::Message& message)
{
    if (!streamPrepared_)
    {
        return failSession(SessionError::Unsolicited, kCodeUnsolicited,
                           "playbackReport reports on a stream this server has not prepared");
    }

    std::string sessionText;
    matp::PlaybackReport report;
    std::string error;
    if (!matp::parsePlaybackReport(message.payload, sessionText, report, error))
        return failSession(SessionError::MalformedMessage, kCodeProtocol, std::move(error));
    if (!isThisSession(sessionText))
    {
        return failSession(SessionError::WrongSession, kCodeProtocol,
                           "playbackReport names session " + sessionText + ", this is " +
                               sessionId_.toHex());
    }

    // Checked, counted and handed on. The client sends these from the moment
    // its device opens (section 25), so refusing them would end every session
    // the moment a stream was prepared. Phase 11 reads one field: a report
    // that names an audible timeline position is this room confirming that its
    // START_AT took. Phase 12 reads the rest, in the zone: the drift
    // regression, the offset and the correction of sections 26 to 31.
    ++playbackReports_;

    if (settings_.hooks.onPlaybackReport)
        settings_.hooks.onPlaybackReport(*this, report);

    // A trim the client could not program: it has returned the clock to
    // nominal where it could, and the room falls back to resampling for the
    // rest of this session. Where it could not, the renegotiation's prepare
    // finds out and the room is held rather than guessed at.
    if (hardwareClockSelected_ && report.hasHardwareClock && !report.hardwareClockError.empty() &&
        !hardwareClockFailed_)
    {
        MULTIROOM_WARN("session {}: {}'s hardware clock failed ({}); resampling instead",
                 sessionId_.toHex(), clientId_.str(), report.hardwareClockError);
        hardwareClockFailed_ = true;
        return renegotiate();
    }
    return {};
}

Status<SessionError> ClientSession::handleSystemReport(const matp::Message& message)
{
    // Diagnostics, so nothing about one ends the session (section 55): one
    // that does not parse, or names another session, is counted and dropped.
    std::string sessionText;
    SystemMetrics metrics;
    std::string error;
    if (!matp::parseSystemReport(message.payload, sessionText, metrics, error))
    {
        ++systemReportsRejected_;
        MULTIROOM_DEBUG("session {}: ignoring a systemReport: {}", sessionId_.toHex(), error);
        return {};
    }
    if (!isThisSession(sessionText))
    {
        ++systemReportsRejected_;
        MULTIROOM_DEBUG("session {}: ignoring a systemReport for session {}", sessionId_.toHex(),
                  sessionText);
        return {};
    }

    ++systemReports_;
    if (settings_.hooks.onSystemReport)
        settings_.hooks.onSystemReport(*this, metrics);
    return {};
}

Status<SessionError> ClientSession::handleAudioDevices(const matp::Message& message)
{
    // A report like SYSTEM_REPORT: one that does not parse is logged and
    // dropped, never the end of a room that plays.
    std::string sessionText;
    matp::AudioDeviceList devices;
    std::string error;
    if (!matp::parseAudioDevices(message.payload, sessionText, devices, error))
    {
        MULTIROOM_WARN("session {}: ignoring an audioDevices: {}", sessionId_.toHex(), error);
        return {};
    }
    if (!isThisSession(sessionText))
    {
        MULTIROOM_DEBUG("session {}: ignoring an audioDevices for session {}", sessionId_.toHex(),
                  sessionText);
        return {};
    }

    ++audioDeviceReports_;
    if (settings_.hooks.onAudioDevices)
        settings_.hooks.onAudioDevices(*this, devices);
    return {};
}

Status<SessionError> ClientSession::handleBufferConfig(const matp::Message& message)
{
    // A report like AUDIO_DEVICES: one that does not parse is logged and
    // dropped, never the end of a room that plays.
    std::string sessionText;
    uint32_t bufferMs = 0;
    std::string error;
    if (!matp::parseBufferConfig(message.payload, sessionText, bufferMs, error))
    {
        MULTIROOM_WARN("session {}: ignoring a bufferConfig: {}", sessionId_.toHex(), error);
        return {};
    }
    if (!isThisSession(sessionText))
    {
        MULTIROOM_DEBUG("session {}: ignoring a bufferConfig for session {}", sessionId_.toHex(),
                  sessionText);
        return {};
    }

    ++bufferConfigReports_;
    MULTIROOM_DEBUG("session {}: {} holds a {} ms buffer", sessionId_.toHex(), clientId_.str(),
              bufferMs);
    if (settings_.hooks.onBufferConfig)
        settings_.hooks.onBufferConfig(*this, bufferMs);
    return {};
}

Status<SessionError> ClientSession::handleRoomName(const matp::Message& message)
{
    // A report like BUFFER_CONFIG: one that does not parse is logged and
    // dropped, never the end of a room that plays.
    std::string sessionText;
    std::string name;
    std::string error;
    if (!matp::parseRoomName(message.payload, sessionText, name, error))
    {
        MULTIROOM_WARN("session {}: ignoring a roomName: {}", sessionId_.toHex(), error);
        return {};
    }
    if (!isThisSession(sessionText))
    {
        MULTIROOM_DEBUG("session {}: ignoring a roomName for session {}", sessionId_.toHex(),
                  sessionText);
        return {};
    }

    ++roomNameReports_;
    MULTIROOM_DEBUG("session {}: {} is called \"{}\"", sessionId_.toHex(), clientId_.str(), name);
    if (settings_.hooks.onRoomName)
        settings_.hooks.onRoomName(*this, name);
    return {};
}

Status<SessionError> ClientSession::handleVolumeConfig(const matp::Message& message)
{
    // A report like BUFFER_CONFIG: one that does not parse is logged and
    // dropped, never the end of a room that plays.
    matp::VolumeConfig config;
    std::string error;
    if (!matp::parseVolumeConfig(message.payload, config, error))
    {
        MULTIROOM_WARN("session {}: ignoring a volumeConfig: {}", sessionId_.toHex(), error);
        return {};
    }
    if (!isThisSession(config.sessionId))
    {
        MULTIROOM_DEBUG("session {}: ignoring a volumeConfig for session {}", sessionId_.toHex(),
                  config.sessionId);
        return {};
    }

    MULTIROOM_INFO("session {}: {} volume control {}, playing {}{}{}", sessionId_.toHex(),
             clientId_.str(), matp::volumeControlModeName(config.volumeControl),
             matp::volumeControlModeName(config.effective), config.reason.empty() ? "" : " -- ",
             config.reason);
    if (settings_.hooks.onVolumeConfig)
        settings_.hooks.onVolumeConfig(*this, config);
    return {};
}

Status<SessionError> ClientSession::handleInputControls(const matp::Message& message)
{
    // A report like AUDIO_DEVICES: one that does not parse costs the room its
    // buttons in the UI, not its music.
    std::string sessionText;
    std::vector<matp::InputControl> inputs;
    bool deepSleep = false;
    std::string error;
    if (!matp::parseInputControls(message.payload, sessionText, inputs, deepSleep, error))
    {
        MULTIROOM_WARN("session {}: ignoring an inputControls: {}", sessionId_.toHex(), error);
        return {};
    }
    if (!isThisSession(sessionText))
    {
        MULTIROOM_DEBUG("session {}: ignoring an inputControls for session {}", sessionId_.toHex(),
                  sessionText);
        return {};
    }

    ++inputControlReports_;
    if (settings_.hooks.onInputControls)
        settings_.hooks.onInputControls(*this, inputs, deepSleep);
    return {};
}

Status<SessionError> ClientSession::handleInputAction(const matp::Message& message)
{
    std::string sessionText;
    matp::InputActionEvent event;
    std::string error;
    if (!matp::parseInputAction(message.payload, sessionText, event, error))
    {
        MULTIROOM_WARN("session {}: ignoring an inputAction: {}", sessionId_.toHex(), error);
        return {};
    }
    if (!isThisSession(sessionText))
    {
        MULTIROOM_DEBUG("session {}: ignoring an inputAction for session {}", sessionId_.toHex(),
                  sessionText);
        return {};
    }

    ++inputActions_;
    MULTIROOM_DEBUG("session {}: {} {} {} x{}", sessionId_.toHex(), event.inputId,
              matp::inputGestureName(event.gesture), matp::inputActionName(event.binding.action),
              event.count);
    if (settings_.hooks.onInputAction)
        settings_.hooks.onInputAction(*this, event);
    return {};
}

Status<SessionError> ClientSession::setInputBindings(const matp::SetInputBindings& request)
{
    if (state_ == SessionState::Closed)
        return fail(SessionError::Closed, "the session is over");
    if (state_ != SessionState::Configured)
        return fail(SessionError::UnexpectedMessage,
                    "setInputBindings before " + clientId_.str() + " has a session");

    matp::SetInputBindings named = request;
    named.sessionId = sessionId_.toHex();
    MULTIROOM_INFO("session {}: rebinding input {} of {}", sessionId_.toHex(), request.inputId,
             clientId_.str());
    return send(matp::MessageType::SetInputBindings, matp::makeSetInputBindings(named));
}

Status<SessionError> ClientSession::setAudioDevice(const std::string& deviceId)
{
    if (state_ == SessionState::Closed)
        return fail(SessionError::Closed, "the session is over");
    if (state_ != SessionState::Configured)
        return fail(SessionError::UnexpectedMessage,
                    "setAudioDevice before " + clientId_.str() + " has a session");

    matp::SetAudioDevice request;
    request.sessionId = sessionId_.toHex();
    request.deviceId = deviceId;
    MULTIROOM_INFO("session {}: asking {} to play on {}", sessionId_.toHex(), clientId_.str(),
             deviceId.empty() ? "its default device" : deviceId);
    return send(matp::MessageType::SetAudioDevice, matp::makeSetAudioDevice(request));
}

Status<SessionError> ClientSession::rescanAudio()
{
    if (state_ == SessionState::Closed)
        return fail(SessionError::Closed, "the session is over");
    if (state_ != SessionState::Configured)
        return fail(SessionError::UnexpectedMessage,
                    "rescanAudio before " + clientId_.str() + " has a session");

    MULTIROOM_INFO("session {}: asking {} to rescan its audio devices", sessionId_.toHex(),
             clientId_.str());
    return send(matp::MessageType::RescanAudio, matp::makeRescanAudio(sessionId_.toHex()));
}

Status<SessionError> ClientSession::setBuffer(uint32_t bufferMs)
{
    if (state_ == SessionState::Closed)
        return fail(SessionError::Closed, "the session is over");
    if (state_ != SessionState::Configured)
        return fail(SessionError::UnexpectedMessage,
                    "setBuffer before " + clientId_.str() + " has a session");

    matp::SetBuffer request;
    request.sessionId = sessionId_.toHex();
    request.bufferMs = bufferMs;
    MULTIROOM_INFO("session {}: asking {} for a {} ms buffer", sessionId_.toHex(), clientId_.str(),
             bufferMs);
    return send(matp::MessageType::SetBuffer, matp::makeSetBuffer(request));
}

Status<SessionError> ClientSession::setRoomName(const std::string& name)
{
    if (state_ == SessionState::Closed)
        return fail(SessionError::Closed, "the session is over");
    if (state_ != SessionState::Configured)
        return fail(SessionError::UnexpectedMessage,
                    "setRoomName before " + clientId_.str() + " has a session");

    matp::SetRoomName request;
    request.sessionId = sessionId_.toHex();
    request.name = name;
    MULTIROOM_INFO("session {}: asking {} to be called \"{}\"", sessionId_.toHex(), clientId_.str(),
             name);
    return send(matp::MessageType::SetRoomName, matp::makeSetRoomName(request));
}

Status<SessionError> ClientSession::setVolumeControl(matp::VolumeControlMode mode)
{
    if (state_ == SessionState::Closed)
        return fail(SessionError::Closed, "the session is over");
    if (state_ != SessionState::Configured)
        return fail(SessionError::UnexpectedMessage,
                    "setVolumeControl before " + clientId_.str() + " has a session");

    matp::SetVolumeControl request;
    request.sessionId = sessionId_.toHex();
    request.volumeControl = mode;
    MULTIROOM_INFO("session {}: asking {} for volume control {}", sessionId_.toHex(), clientId_.str(),
             matp::volumeControlModeName(mode));
    return send(matp::MessageType::SetVolumeControl, matp::makeSetVolumeControl(request));
}

Status<SessionError> ClientSession::setDriftCorrection(matp::DriftCorrectionMode mode)
{
    if (state_ == SessionState::Closed)
        return fail(SessionError::Closed, "the session is over");
    if (state_ != SessionState::Configured)
        return fail(SessionError::UnexpectedMessage,
                    "setDriftCorrection before " + clientId_.str() + " has a session");
    if (!capabilities_.driftFeedback)
        return fail(SessionError::UnexpectedMessage,
                    clientId_.str() + " does not support drift-correction settings");

    // A new request is a new chance for a clock that failed before.
    hardwareClockFailed_ = false;
    matp::SetDriftCorrection request;
    request.sessionId = sessionId_.toHex();
    request.driftCorrection = mode;
    MULTIROOM_INFO("session {}: asking {} for drift correction {}", sessionId_.toHex(),
             clientId_.str(), matp::driftCorrectionModeName(mode));
    return send(matp::MessageType::SetDriftCorrection, matp::makeSetDriftCorrection(request));
}

Status<SessionError> ClientSession::sendDriftFeedback(matp::DriftFeedback feedback)
{
    if (state_ != SessionState::Configured || !capabilities_.driftFeedback || !streamPrepared_)
        return {};
    feedback.sessionId = sessionId_.toHex();
    feedback.sequence = ++driftSequence_;
    feedback.hasHardwareTarget = feedback.hasHardwareTarget && capabilities_.hardwareClock.available;
    return send(matp::MessageType::DriftFeedback, matp::makeDriftFeedback(feedback));
}

Status<SessionError> ClientSession::streamMessageAllowed(const char* what,
                                                         bool needsDevice) const
{
    if (state_ == SessionState::Closed)
        return fail(SessionError::Closed, "the session is over");
    if (!streamPrepared_)
    {
        // The client's own rule, from the other end of the socket: it ignores
        // a message about a stream it has not been asked to prepare.
        return fail(SessionError::UnexpectedMessage,
                    std::string(what) + " before " + clientId_.str() +
                        " has been asked for a stream");
    }
    if (needsDevice && !streamReady_)
    {
        // Not a protocol failure and not this room's fault: it is a command
        // that arrived before the device did, and the scheduler will offer
        // another once the device has answered.
        return fail(SessionError::UnexpectedMessage,
                    std::string(what) + " before " + clientId_.str() +
                        " has granted a stream");
    }
    return {};
}

Status<SessionError> ClientSession::startPlayback(const matp::StartAt& start)
{
    if (const Status<SessionError> allowed = streamMessageAllowed("startAt", true); !allowed)
        return allowed;

    matp::StartAt request = start;
    request.sessionId = sessionId_.toHex();
    ++startsSent_;

    MULTIROOM_INFO("session {}: start timeline {} ns at server time {} ns", sessionId_.toHex(),
             request.timelineNs, request.serverTimeNs);

    return send(matp::MessageType::StartAt, matp::makeStartAt(request));
}

Status<SessionError> ClientSession::pausePlayback(const matp::TransportAt& at)
{
    if (const Status<SessionError> allowed = streamMessageAllowed("pauseAt", true); !allowed)
        return allowed;

    matp::TransportAt request = at;
    request.sessionId = sessionId_.toHex();
    ++pausesSent_;

    MULTIROOM_INFO("session {}: pause at timeline {} ns", sessionId_.toHex(), request.timelineNs);

    return send(matp::MessageType::PauseAt, matp::makePauseAt(request));
}

Status<SessionError> ClientSession::stopPlayback(const matp::TransportAt& at)
{
    if (const Status<SessionError> allowed = streamMessageAllowed("stopAt", true); !allowed)
        return allowed;

    matp::TransportAt request = at;
    request.sessionId = sessionId_.toHex();
    ++stopsSent_;

    MULTIROOM_INFO("session {}: stop at timeline {} ns", sessionId_.toHex(), request.timelineNs);

    return send(matp::MessageType::StopAt, matp::makeStopAt(request));
}

Status<SessionError> ClientSession::flushPlayback(const matp::TransportAt& at)
{
    if (const Status<SessionError> allowed = streamMessageAllowed("flush", true); !allowed)
        return allowed;

    matp::TransportAt request = at;
    request.sessionId = sessionId_.toHex();
    ++flushesSent_;

    MULTIROOM_INFO("session {}: flush at timeline {} ns", sessionId_.toHex(), request.timelineNs);

    return send(matp::MessageType::Flush, matp::makeFlush(request));
}

Status<SessionError> ClientSession::setVolume(const matp::SetVolume& request)
{
    if (const Status<SessionError> allowed = streamMessageAllowed("setVolume", false); !allowed)
        return allowed;

    matp::SetVolume outgoing = request;
    outgoing.sessionId = sessionId_.toHex();
    outgoing.value = gainForPosition(request.value);

    json::Value payload;
    std::string error;
    if (!matp::makeSetVolume(outgoing, payload, error))
    {
        // Section 39 asks for nothing when neither a value nor a mute is set,
        // and the client's parser refuses exactly that. A message this
        // server's own wire library would not write is a caller's mistake
        // rather than a room's, so the session stays up.
        return fail(SessionError::MalformedMessage, std::move(error));
    }

    ++volumeChangesSent_;
    return send(matp::MessageType::SetVolume, payload);
}

Status<SessionError> ClientSession::sendTrackBoundary(const matp::TrackBoundary& boundary)
{
    if (const Status<SessionError> allowed = streamMessageAllowed("trackBoundary", false);
        !allowed)
    {
        return allowed;
    }

    matp::TrackBoundary outgoing = boundary;
    outgoing.sessionId = sessionId_.toHex();
    ++trackBoundariesSent_;

    return send(matp::MessageType::TrackBoundary, matp::makeTrackBoundary(outgoing));
}

Status<SessionError> ClientSession::sendTrackInfo(const matp::TrackInfo& info)
{
    if (const Status<SessionError> allowed = streamMessageAllowed("trackInfo", false); !allowed)
        return allowed;

    matp::TrackInfo outgoing = info;
    outgoing.sessionId = sessionId_.toHex();

    json::Value payload;
    std::string error;
    if (!matp::makeTrackInfo(outgoing, payload, error))
        return fail(SessionError::MalformedMessage, std::move(error));

    ++trackInfosSent_;
    return send(matp::MessageType::TrackInfo, payload);
}

Status<SessionError> ClientSession::renegotiate()
{
    if (state_ == SessionState::Closed)
        return fail(SessionError::Closed, "the session is over");
    if (!hasCapabilities_)
        return {};

    MULTIROOM_INFO("session {}: re-negotiating {}'s stream format", sessionId_.toHex(),
             clientId_.str());
    return sendSessionConfig();
}

Status<SessionError> ClientSession::handleClockSyncRequest(const matp::Message& message,
                                                           uint64_t arrivalNs)
{
    uint64_t clientSendNs = 0;
    std::string error;
    if (!matp::parseClockSyncRequest(message.payload, clientSendNs, error))
        return failSession(SessionError::MalformedMessage, kCodeProtocol, std::move(error));

    matp::ClockSyncReply reply;
    reply.clientSendNs = clientSendNs;
    reply.serverReceiveNs = arrivalNs;
    // Read now, not with S2: the gap between them is this server's own
    // processing delay, which is the part of the round trip the client
    // subtracts rather than halves (section 8). One timestamp for both would
    // charge that delay to the network in both directions.
    reply.serverSendNs = clock_.monotonicNowNs();

    ++clockSyncReplies_;
    return send(matp::MessageType::ClockSyncReply, matp::makeClockSyncReply(reply));
}

Status<SessionError> ClientSession::handleClockSyncResult(const matp::Message& message)
{
    matp::ClockSyncResult result;
    std::string error;
    if (!matp::parseClockSyncResult(message.payload, result, error))
        return failSession(SessionError::MalformedMessage, kCodeProtocol, std::move(error));

    ++clockSyncResults_;

    // Unanswered, best-effort telemetry: a sample this model rejects is not a
    // protocol error, and the client deliberately sends the ones its own model
    // rejected too.
    const bool wasValid = clockModel_.valid();
    const bool kept = clockModel_.addSample(result);

    if (!wasValid && clockModel_.valid())
    {
        // Until there is a status API to read it out of, this line is the only
        // way to compare the server's figures against the client's own --
        // which is exactly what the phase-8 milestone against the real binary
        // asks for.
        MULTIROOM_INFO("session {}: client clock mapped, offset {} us, best round trip {} us, "
                 "{} sample(s)",
                 sessionId_.toHex(), clockModel_.offsetNs() / 1000,
                 clockModel_.bestRoundTripNs() / 1000, clockModel_.sampleCount());
    }
    else if (!kept)
    {
        MULTIROOM_DEBUG("session {}: clock sample rejected ({} of {} so far)", sessionId_.toHex(),
                  clockModel_.rejectedCount(), clockSyncResults_);
    }

    return {};
}

Status<SessionError> ClientSession::handlePong(const matp::Message& message)
{
    uint64_t sequence = 0;
    std::string error;
    if (!matp::parsePong(message.payload, sequence, error))
        return failSession(SessionError::MalformedMessage, kCodeProtocol, std::move(error));

    ++pongsReceived_;
    // Only the newest ping clears the outstanding flag. An older sequence is a
    // late answer to a ping that has already been superseded -- it still
    // counts as the peer being alive, which lastHeardFromNs_ has recorded, but
    // it does not say the current ping was answered.
    if (pingOutstanding_ && sequence == pingSequence_)
        pingOutstanding_ = false;

    return {};
}

Status<SessionError> ClientSession::poll()
{
    if (state_ == SessionState::Closed)
        return fail(SessionError::Closed, lastError_);

    const uint64_t nowNs = clock_.monotonicNowNs();
    const uint64_t silentForNs = nowNs - lastHeardFromNs_;

    if (silentForNs >= settings_.silenceTimeoutNs)
    {
        return failSession(SessionError::PeerSilent, kCodeProtocol,
                           "no message for " + std::to_string(silentForNs / 1000000ULL) +
                               " ms");
    }

    // One ping outstanding at a time. A second would measure the queue the
    // first is sitting in, and the silence timeout above is what actually
    // decides the room is gone -- this is only the nudge that makes a live
    // peer say so.
    if (!pingOutstanding_ && silentForNs >= settings_.pingIntervalNs)
    {
        ++pingSequence_;
        pingOutstanding_ = true;
        ++pingsSent_;
        return send(matp::MessageType::Ping, matp::makePing(pingSequence_));
    }

    return {};
}

Status<SessionError> ClientSession::send(matp::MessageType type, const json::Value& payload)
{
    if (outbound_.size() >= settings_.maxOutboundFrames)
    {
        // The peer is not draining its socket. Failing this session is the
        // whole isolation argument: one client can never block another, and
        // the alternative -- growing the queue -- is one room costing the
        // process memory without limit.
        return failSession(SessionError::OutboundFull, kCodeProtocol,
                           "outbound queue reached " +
                               std::to_string(settings_.maxOutboundFrames) +
                               " frames; the client is not reading");
    }

    outbound_.push_back(matp::encode(type, payload));
    return {};
}

bool ClientSession::nextOutbound(std::string& frame)
{
    if (outbound_.empty())
        return false;
    frame = std::move(outbound_.front());
    outbound_.pop_front();
    return true;
}

void ClientSession::close(std::string_view reason)
{
    if (state_ == SessionState::Closed)
        return;

    lastError_ = std::string(reason);
    state_ = SessionState::Closed;
    MULTIROOM_INFO("session {}: closed ({})", sessionId_.toHex(), lastError_);

    // Last, and after the state is already Closed: a room's output stops with
    // its session, and the hook must not be able to reach a session that is
    // half closed.
    if (settings_.hooks.onClosed)
        settings_.hooks.onClosed(*this);
}

std::unexpected<Failure<SessionError>> ClientSession::failSession(SessionError code,
                                                                  std::string_view wireCode,
                                                                  std::string detail)
{
    const std::string reason = std::string(errorName(code)) + ": " + detail;

    // Queued directly rather than through send(): this is the last thing the
    // peer will hear, and the queue limit is exactly the condition under which
    // it most needs to be told why.
    if (state_ != SessionState::Closed)
    {
        outbound_.push_back(matp::encode(matp::MessageType::Error,
                                         matp::makeError(std::string(wireCode), detail)));
    }

    close(reason);
    return fail(code, std::move(detail));
}

} // namespace multiroom
