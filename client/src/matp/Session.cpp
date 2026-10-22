#include "matp/Session.h"

#include "util/Logger.h"
#include "util/Time.h"
#include <cstdlib>

namespace multiroom {
namespace matp {
namespace {

// A server that accepts the connection and then says nothing. Generous
// compared to the round trip it covers, because a busy server starting up is
// not a broken one -- but finite, because a client stuck here is mute and
// reports nothing about why.
const uint64_t kHandshakeTimeoutNs = 10ULL * 1000000000ULL;

// How long an established session may hear nothing at all from the server
// before the connection is treated as dead.
//
// The protocol names no keepalive interval and no liveness rule, but it does
// guarantee the prompt: section 46 has the client ask for a clock sync every
// five seconds for as long as the session lasts, and a server that answers
// none of four consecutive ones is not slow. That is where this figure comes
// from, and it is why no PING of this client's own was added -- a second
// heartbeat beside the clock exchange would measure the same silence twice
// (see makePong).
//
// It only matters against a connection that has half-opened. A socket that is
// closed, reset or refused is reported by the transport in milliseconds and
// never reaches this; one whose path has gone away reports nothing at all, and
// writes into it keep succeeding into the kernel's send buffer for as long as
// it has room.
const uint64_t kServerSilenceTimeoutNs = 20ULL * 1000000000ULL;

// Section 46's initial values: twenty samples on connect, then one every five
// seconds. Samples, not requests -- an exchange the model refuses because it
// spent milliseconds in a queue has measured nothing, and stopping at twenty
// attempts would leave a client on a busy network with a mapping built from
// the four that happened to get through.
//
// The burst spacing is this client's; the section says how many, not how fast.
// It is wide enough that each request finds an idle socket rather than the
// tail of the previous one.
const uint32_t kClockSyncBurstSamples = 20;
const uint64_t kClockSyncBurstIntervalNs = 50ULL * 1000000ULL;
const uint64_t kClockSyncIntervalNs = 5ULL * 1000000000ULL;

// ...but a network that refuses to produce twenty usable samples must not keep
// the client asking twenty times a second forever. Once the attempts run out
// the interval settles regardless of how many samples were had.
const uint32_t kClockSyncBurstAttempts = 60;

// An exchange this old is not going to be answered usefully any more: even if
// the reply arrives, the round trip it measured says nothing about the path.
// Abandoning it frees the slot for a request that can.
const uint64_t kClockSyncTimeoutNs = 2ULL * 1000000000ULL;

// Section 19 says what BUFFER_STATUS carries and section 18 what the server
// does with it -- it holds playback back until every room has enough -- but
// neither says how often. Four times a second: the figure decides when the
// whole system may start, so a server waiting on the slowest room should not
// also be waiting on this client's next report, and at a handful of bytes it
// costs nothing next to the audio it describes.
const uint64_t kBufferStatusIntervalNs = 250ULL * 1000000ULL;

// Section 25 asks for a PLAYBACK_REPORT every 250 to 1000 ms and leaves the
// choice inside that range open. Half a second: the server's sync controller
// works over seconds of drift rather than over single reports, and the owner
// thread's own period stretches the spacing a little anyway, so a figure from
// the middle of the range stays inside it at both ends.
const uint64_t kPlaybackReportIntervalNs = 500ULL * 1000000ULL;

// Section 55. The figures move over seconds, and the server polls its core on
// the same tick (its phase 18).
const uint64_t kSystemReportIntervalNs = 2000ULL * 1000000ULL;

// What the connection thread may queue between two polls. Room for a message
// at the size limit and ordinary traffic around it; beyond that the owner
// thread is not keeping up, and on a 512 MB board a sender must not be able to
// grow this at network speed for as long as that lasts.
const size_t kMaxInboxBytes = 4 * static_cast<size_t>(kMaxMessageBytes);

} // namespace

const char* sessionStateName(SessionState state)
{
    switch (state)
    {
    case SessionState::Idle:
        return "idle";
    case SessionState::HelloSent:
        return "helloSent";
    case SessionState::CapabilitiesSent:
        return "capabilitiesSent";
    case SessionState::Established:
        return "established";
    case SessionState::Failed:
        return "failed";
    }
    return "unknown";
}

Session::Session(ServerConnection& connection, Identity identity,
                 CapabilityProvider capabilities)
    : connection_(connection), identity_(std::move(identity)),
      capabilities_(std::move(capabilities))
{
}

void Session::setStreamHooks(StreamHooks hooks)
{
    stream_ = std::move(hooks);
}

void Session::setSystemProvider(SystemProvider provider)
{
    system_ = std::move(provider);
}

void Session::setAudioDeviceHooks(AudioDeviceProvider provider, AudioDeviceSetter setter)
{
    audioDevices_ = std::move(provider);
    setAudioDevice_ = std::move(setter);
}

void Session::setAudioRescanHook(AudioRescanner rescan)
{
    rescanAudio_ = std::move(rescan);
}

void Session::onData(const void* data, size_t size)
{
    std::lock_guard<std::mutex> lock(mutex_);

    // Tagged with the connection the bytes arrived on. The count is bumped
    // before a connection delivers anything, so bytes from a connection that
    // has already been replaced can be told apart from current ones instead of
    // being decoded as the middle of somebody else's message.
    const uint64_t generation = connection_.connectionCount();
    if (generation != inboxGeneration_)
    {
        inbox_.clear();
        inboxChunks_.clear();
        inboxOverflow_ = false;
        inboxGeneration_ = generation;
    }

    if (inboxOverflow_)
        return; // poll() fails the session; nothing after the gap can be framed
    if (size > kMaxInboxBytes - inbox_.size())
    {
        inbox_.clear();
        inboxChunks_.clear();
        inboxOverflow_ = true;
        return;
    }

    // C4 for anything completed by these bytes. Read here rather than in
    // poll(): between the two lies the whole polling interval, which is orders
    // of magnitude larger than the round trip being measured.
    inbox_.append(static_cast<const char*>(data), size);
    inboxChunks_.push_back(InboxChunk{inbox_.size(), monotonicNowNs()});
}

void Session::poll(uint64_t nowNs)
{
    // Drained before the generation is read, not after: the count only grows,
    // so bytes taken here can be older than the connection read below, never
    // newer.
    std::string bytes;
    std::vector<InboxChunk> chunks;
    uint64_t bytesGeneration = 0;
    bool overflow = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        bytes.swap(inbox_);
        chunks.swap(inboxChunks_);
        bytesGeneration = inboxGeneration_;
        overflow = inboxOverflow_;
        inboxOverflow_ = false;
    }

    // The state before the count: the connection counts a socket before it
    // says Connected, so a Connected read here is never paired with the count
    // of the socket before it -- which would send this poll's HELLO on the new
    // socket under the old count, and the next poll a second one.
    const ServerConnectionState connectionState = connection_.state();
    const uint64_t generation = connection_.connectionCount();
    if (generation != generation_)
    {
        // A different socket than the one this handshake was made on. Nothing
        // it established survives: the server treats a reconnect as a new
        // control connection and sends a new SESSION_CONFIG.
        resetSession();
        generation_ = generation;
    }

    if (connectionState != ServerConnectionState::Connected)
    {
        if (state_ != SessionState::Idle && state_ != SessionState::Failed)
            resetSession();
        return;
    }

    if (state_ == SessionState::Failed)
        return; // the connection was dropped; the reconnect starts over

    if (state_ == SessionState::Idle)
    {
        startHandshake(nowNs);
        if (state_ == SessionState::Failed)
            return;
    }

    if (bytesGeneration != generation_)
        chunks.clear();
    else if (overflow)
    {
        fail("the server sent more than " + std::to_string(kMaxInboxBytes) +
             " bytes between two polls");
        return;
    }

    // One read at a time, so each message carries the arrival of the read
    // that completed it.
    size_t begin = 0;
    for (const InboxChunk& chunk : chunks)
    {
        reader_.append(bytes.data() + begin, chunk.end - begin);
        begin = chunk.end;

        for (;;)
        {
            Message message;
            std::string error;
            const FrameReader::Result result = reader_.next(message, error);
            if (result == FrameReader::Result::Incomplete)
                break;
            if (result == FrameReader::Result::Invalid)
            {
                // The byte stream is out of step, and nothing but a new
                // connection can put it back: the next length prefix would be
                // read out of the middle of a message.
                fail(error);
                return;
            }

            // Any message at all, including one this client does not
            // implement: what it proves is that the path is still there, which
            // is what the silence deadline below asks about.
            lastHeardFromNs_ = nowNs;

            handleMessage(message, chunk.arrivalNs);
            if (state_ == SessionState::Failed)
                return;
        }
    }

    if ((state_ == SessionState::HelloSent || state_ == SessionState::CapabilitiesSent) &&
        nowNs - handshakeStartedNs_ > kHandshakeTimeoutNs)
    {
        fail("the server did not finish the handshake within " +
             std::to_string(kHandshakeTimeoutNs / 1000000000ULL) + " s");
        return;
    }

    if (state_ == SessionState::Established && nowNs - lastHeardFromNs_ > kServerSilenceTimeoutNs)
    {
        // Dropped rather than probed again: the clock sync requests have been
        // going out unanswered the whole time, so there is nothing left to ask
        // that has not already been asked. The reconnect is the repair, and
        // section 43 is what happens after it.
        fail("the server has said nothing for " +
             std::to_string(kServerSilenceTimeoutNs / 1000000000ULL) +
             " s; the connection is dead");
        return;
    }

    pollClockSync(nowNs);
    pollBufferStatus(nowNs);
    pollPlaybackReport(nowNs);
    pollSystemReport(nowNs);
}

// One exchange at a time, on a schedule that starts as a burst and settles
// into section 46's five second interval. The schedule runs on the caller's
// clock so a test can walk it; the timestamps inside the exchange do not.
void Session::pollClockSync(uint64_t nowNs)
{
    if (state_ != SessionState::Established)
        return;

    if (clockRequestPending_)
    {
        if (nowNs < clockRequestDeadlineNs_)
            return;

        // Abandoned rather than retried with the same C1: the reply, if it
        // ever comes, no longer matches anything and is ignored.
        clockRequestPending_ = false;
        MULTIROOM_DEBUG("clock sync request went unanswered for %llu ms",
                  static_cast<unsigned long long>(kClockSyncTimeoutNs / 1000000ULL));
    }
    else if (nowNs < clockNextRequestNs_)
    {
        return;
    }

    // C1 is read as late as possible, so the outbound leg the measurement
    // attributes to the network is the network.
    const uint64_t sendNs = monotonicNowNs();
    if (!send(MessageType::ClockSyncRequest, makeClockSyncRequest(sendNs)))
    {
        // The connection is gone; the next one starts the whole exchange over.
        clockNextRequestNs_ = nowNs + kClockSyncIntervalNs;
        return;
    }

    clockRequestPending_ = true;
    clockRequestSendNs_ = sendNs;
    clockRequestDeadlineNs_ = nowNs + kClockSyncTimeoutNs;
    ++clockSyncRequests_;

    const bool bursting = clockBurstRemaining_ > 0 && clockBurstAttemptsLeft_ > 0;
    if (clockBurstAttemptsLeft_ > 0)
        --clockBurstAttemptsLeft_;
    clockNextRequestNs_ = nowNs + (bursting ? kClockSyncBurstIntervalNs : kClockSyncIntervalNs);
}

// Section 18's readiness signal: the server holds playback back until every
// room reports enough buffered audio, so this runs for as long as there is a
// stream, not only while it is filling.
void Session::pollBufferStatus(uint64_t nowNs)
{
    if (!streamPrepared_ || state_ != SessionState::Established)
        return;
    if (nowNs < bufferStatusNextNs_)
        return;

    // Set before the report is built, so a provider that has nothing to say
    // costs one call per interval rather than one per poll.
    bufferStatusNextNs_ = nowNs + kBufferStatusIntervalNs;

    BufferStatus status;
    if (!stream_.bufferStatus || !stream_.bufferStatus(status))
        return;

    if (send(MessageType::BufferStatus, makeBufferStatus(sessionId_, status)))
        ++bufferStatusReports_;
}

// Section 25: the positions the server's sync controller runs on, and this
// client's own measurement of its sound card beside them.
//
// It reports from PREPARE_STREAM on rather than from the start. The device
// runs from then (section 21), so its clock is worth measuring before anything
// is audible, and the server gets a drift estimate that has already settled by
// the time it schedules one.
void Session::pollPlaybackReport(uint64_t nowNs)
{
    if (!streamPrepared_ || state_ != SessionState::Established)
        return;
    if (nowNs < playbackReportNextNs_)
        return;

    // Set before the report is built, for the same reason as above: a provider
    // with nothing to say costs one call per interval, not one per poll.
    playbackReportNextNs_ = nowNs + kPlaybackReportIntervalNs;

    PlaybackReport report;
    if (!stream_.playbackReport || !stream_.playbackReport(report))
        return;

    if (send(MessageType::PlaybackReport, makePlaybackReport(sessionId_, report)))
        ++playbackReports_;
}

// Section 55: how this machine is doing, whether the room plays or not. From
// SESSION_CONFIG on rather than from HELLO_ACK, because the report names the
// session and the server has not chosen one before then.
void Session::pollSystemReport(uint64_t nowNs)
{
    if (state_ != SessionState::Established || nowNs < systemReportNextNs_)
        return;
    systemReportNextNs_ = nowNs + kSystemReportIntervalNs;

    SystemMetrics metrics;
    if (!system_ || !system_(metrics))
        return;
    // The sampler reports the first associated Wi-Fi interface; on a wired
    // link that one carries no audio, so its figures would only mislead.
    metrics.networkLink = connection_.networkLink();
    if (metrics.networkLink == "ethernet")
    {
        metrics.hasWifiRssiDbm = false;
        metrics.hasWifiChannel = false;
        metrics.wifiBssid.clear();
        metrics.hasWifiRxRateKbps = false;
    }

    if (send(MessageType::SystemReport, makeSystemReport(sessionId_, metrics)))
        ++systemReports_;
}

void Session::capabilitiesChanged()
{
    if (state_ != SessionState::CapabilitiesSent && state_ != SessionState::Established)
        return;

    json::Value payload;
    std::string serialized;
    if (!buildCapabilities(payload, serialized))
        return;

    if (serialized == lastCapabilities_)
        return; // a rescan that landed on the same device is not a change

    if (!send(MessageType::AudioCapabilities, payload))
        return;
    lastCapabilities_ = serialized;

    // The stream the server chose was chosen from the old list; it stands
    // until the new SESSION_CONFIG arrives, which is why it is only
    // re-checked rather than discarded here.
    if (hasStreamFormat_)
        streamFormatPlayable_ = formatIsPlayable(streamFormat_);

    MULTIROOM_INFO("audio capabilities changed; reported again to the server");
}

void Session::audioDevicesChanged()
{
    // It names the session, so there is nothing to send before one exists;
    // establishing it sends the first list.
    if (state_ != SessionState::Established || !audioDevices_)
        return;

    AudioDeviceList list;
    audioDevices_(list);
    const json::Value payload = makeAudioDevices(sessionId_, list);
    sendIfChanged(MessageType::AudioDevices, payload, lastAudioDevices_, audioDeviceReports_);
}

// Reports are state, not events: one the server already has is not sent again.
void Session::sendIfChanged(MessageType type, const json::Value& payload, std::string& last,
                            uint32_t& reports)
{
    const std::string serialized = json::serialize(payload);
    if (serialized == last || !send(type, payload))
        return;
    last = serialized;
    ++reports;
}

void Session::setInputHooks(InputControlProvider provider, InputBindingSetter setter,
                            bool deepSleep)
{
    inputControls_ = std::move(provider);
    deepSleep_ = deepSleep;
    setInputBindings_ = std::move(setter);
}

void Session::inputControlsChanged()
{
    if (state_ != SessionState::Established || !inputControls_)
        return;

    std::vector<InputControl> inputs;
    inputControls_(inputs);
    if (inputs.empty())
        return;
    const json::Value payload = makeInputControls(sessionId_, inputs, deepSleep_);
    sendIfChanged(MessageType::InputControls, payload, lastInputControls_, inputControlReports_);
}

void Session::setBufferHooks(BufferProvider provider, BufferSetter setter)
{
    buffer_ = std::move(provider);
    setBuffer_ = std::move(setter);
}

void Session::bufferConfigChanged()
{
    if (state_ != SessionState::Established || !buffer_)
        return;

    const json::Value payload = makeBufferConfig(sessionId_, buffer_());
    sendIfChanged(MessageType::BufferConfig, payload, lastBufferConfig_, bufferConfigReports_);
}

void Session::setRoomNameHooks(RoomNameProvider provider, RoomNameSetter setter)
{
    roomName_ = std::move(provider);
    setRoomName_ = std::move(setter);
}

void Session::roomNameChanged()
{
    if (state_ != SessionState::Established || !roomName_)
        return;

    const json::Value payload = makeRoomName(sessionId_, roomName_());
    sendIfChanged(MessageType::RoomName, payload, lastRoomName_, roomNameReports_);
}

void Session::setVolumeControlHooks(AudioControlHook audioControl,
                                    VolumeConfigProvider provider, VolumeControlSetter setter)
{
    audioControl_ = std::move(audioControl);
    volumeConfig_ = std::move(provider);
    setVolumeControl_ = std::move(setter);
}

void Session::setDriftCorrectionHooks(DriftCorrectionHook negotiated, DriftFeedbackHook feedback,
                                      DriftCorrectionSetter setter)
{
    driftCorrection_ = std::move(negotiated);
    driftFeedback_ = std::move(feedback);
    setDriftCorrection_ = std::move(setter);
}

void Session::volumeConfigChanged()
{
    if (state_ != SessionState::Established || !volumeConfig_)
        return;

    VolumeConfig config = volumeConfig_();
    config.sessionId = sessionId_;
    const json::Value payload = makeVolumeConfig(config);
    sendIfChanged(MessageType::VolumeConfig, payload, lastVolumeConfig_, volumeConfigReports_);
}

bool Session::sendInputAction(const InputActionEvent& event)
{
    if (state_ != SessionState::Established)
    {
        MULTIROOM_INFO("input %s %s (%s) dropped: no session", event.inputId.c_str(),
                 inputGestureName(event.gesture), inputActionName(event.binding.action));
        return false;
    }
    if (!send(MessageType::InputAction, makeInputAction(sessionId_, event)))
        return false;
    MULTIROOM_DEBUG("input %s %s x%u: %s", event.inputId.c_str(), inputGestureName(event.gesture),
              static_cast<unsigned>(event.count), inputActionName(event.binding.action));
    ++inputActions_;
    return true;
}

const char* Session::powerState() const
{
    return manualStandby_ ? "standbyManual" : standby_ ? "standby" : "on";
}

bool Session::announceSleep(const std::string& wakeMac)
{
    if (state_ != SessionState::Established || !hasPrepareRequest_)
        return false;
    StreamReady ready;
    ready.powerState = wakeMac.empty() ? "sleepButtonOnly" : "sleep";
    ready.wakeOnLan = wakeMac;
    ready.reason = "deep sleep";
    return send(MessageType::StreamReady, makeStreamReady(sessionId_, ready));
}

void Session::resumeAfterFailedSleep()
{
    if (state_ == SessionState::Established && hasPrepareRequest_)
        prepareStream();
}

bool Session::setPower(bool on, std::string& error, bool automatic)
{
    if (on)
    {
        if (!standby_ && !manualStandby_)
            return true;
        standby_ = false;
        manualStandby_ = false;
        if (state_ == SessionState::Established && hasPrepareRequest_ && !prepareStream())
        {
            error = "the stream could not be prepared after standby";
            return false;
        }
        return true;
    }

    if (standby_ && manualStandby_ == !automatic)
        return true;
    standby_ = true;
    manualStandby_ = !automatic;
    releaseStream();
    if (state_ == SessionState::Established && hasPrepareRequest_)
    {
        StreamReady ready;
        ready.standby = automatic;
        ready.powerState = powerState();
        ready.reason = automatic ? "automatic standby" : "manual standby";
        if (!send(MessageType::StreamReady, makeStreamReady(sessionId_, ready)))
        {
            error = "could not report standby to the server";
            return false;
        }
    }
    return true;
}

void Session::setClientId(const std::string& clientId)
{
    if (clientId == identity_.clientId)
        return;
    identity_.clientId = clientId;
    if (state_ != SessionState::Idle && state_ != SessionState::Failed)
        connection_.dropConnection("client id changed");
}

void Session::startHandshake(uint64_t nowNs)
{
    if (identity_.clientId.empty())
    {
        // The server keys this client's stream, drift model and resampler on
        // the id (section 11). An invented one would make every restart a new
        // room, which is worse than not connecting at all.
        fail("no client id: set clientId in client.json");
        return;
    }

    handshakeStartedNs_ = nowNs;
    lastHeardFromNs_ = nowNs;
    if (!send(MessageType::Hello, makeHello(identity_)))
        return;

    state_ = SessionState::HelloSent;
    MULTIROOM_INFO("MATP handshake started as \"%s\" (%s)", identity_.clientId.c_str(),
             identity_.platform.c_str());
}

void Session::releaseStream()
{
    streamPrepared_ = false;
    bufferStatusNextNs_ = 0;
    playbackReportNextNs_ = 0;

    // Unconditional, and idempotent on the other side: a prepare that failed
    // half way through has left something open just as surely as one that
    // succeeded, and only the layer that opened it knows what.
    if (stream_.release)
        stream_.release();
}

void Session::resetSession()
{
    releaseStream();
    hasPrepareRequest_ = false;
    reader_.reset();
    state_ = SessionState::Idle;
    handshakeStartedNs_ = 0;
    lastHeardFromNs_ = 0;
    systemReportNextNs_ = 0;
    sessionId_.clear();
    hasStreamFormat_ = false;
    streamFormat_ = AudioFormat();
    streamFormatPlayable_ = false;
    lastCapabilities_.clear();
    lastAudioDevices_.clear();
    lastInputControls_.clear();
    lastBufferConfig_.clear();
    lastVolumeConfig_.clear();
    lastRoomName_.clear();
    lastDriftSequence_ = 0;

    // Clock synchronization is re-run from scratch on every connection
    // (section 43). The server on the other end of the next one may be a
    // restarted process whose monotonic clock began again at zero, and a
    // mapping carried across that is not stale, it is wrong.
    clock_.reset();
    clockRequestPending_ = false;
    clockRequestSendNs_ = 0;
    clockRequestDeadlineNs_ = 0;
    clockNextRequestNs_ = 0;
    clockBurstRemaining_ = 0;
    clockBurstAttemptsLeft_ = 0;

    // lastError_ is kept: it is why the last attempt failed, and a client that
    // keeps reconnecting would otherwise report nothing at all.
}

bool Session::buildCapabilities(json::Value& payload, std::string& serialized)
{
    CapabilityReport report;
    std::string error;
    if (!capabilities_ || !capabilities_(report, error))
    {
        // Still sent, with no formats: the session goes on without a stream,
        // so the server can see this room and ask it to rescan or switch
        // device. A rescan that finds one reports again.
        MULTIROOM_WARN("reporting no playable format: %s",
                 error.empty() ? "no capability report" : error.c_str());
        report = CapabilityReport();
    }

    payload = makeAudioCapabilities(report);
    serialized = json::serialize(payload);
    return true;
}

bool Session::sendCapabilities()
{
    json::Value payload;
    std::string serialized;
    if (!buildCapabilities(payload, serialized))
        return false;

    if (!send(MessageType::AudioCapabilities, payload))
        return false;

    lastCapabilities_ = serialized;
    return true;
}

bool Session::send(MessageType type, const json::Value& payload)
{
    const std::string frame = encode(type, payload);
    if (connection_.send(frame.data(), frame.size()))
        return true;

    // Not a protocol error: the connection went away underneath, and the
    // reconnect path already handles that. The next connection starts a fresh
    // handshake.
    MULTIROOM_DEBUG("could not send %s: the connection is gone", messageTypeName(type));
    return false;
}

void Session::handleMessage(const Message& message, uint64_t arrivalNs)
{
    switch (message.type)
    {
    case MessageType::HelloAck:
        handleHelloAck(message);
        break;

    case MessageType::SessionConfig:
        handleSessionConfig(message);
        break;

    case MessageType::ClockSyncReply:
        handleClockSyncReply(message, arrivalNs);
        break;

    case MessageType::PrepareStream:
        handlePrepareStream(message);
        break;

    case MessageType::StartAt:
        handleStartAt(message);
        break;

    case MessageType::PauseAt:
        handleTransportAt(message, stream_.pause, scheduledPauses_);
        break;

    case MessageType::StopAt:
        handleTransportAt(message, stream_.stop, scheduledStops_);
        break;

    case MessageType::Flush:
        handleTransportAt(message, stream_.flush, scheduledFlushes_);
        break;

    case MessageType::SetVolume:
        handleSetVolume(message);
        break;

    case MessageType::TrackBoundary:
        handleTrackBoundary(message);
        break;

    case MessageType::TrackInfo:
        handleTrackInfo(message);
        break;

    case MessageType::SetAudioDevice:
        handleSetAudioDevice(message);
        break;

    case MessageType::RescanAudio:
        handleRescanAudio(message);
        break;

    case MessageType::SetInputBindings:
        handleSetInputBindings(message);
        break;

    case MessageType::SetBuffer:
        handleSetBuffer(message);
        break;

    case MessageType::SetRoomName:
        handleSetRoomName(message);
        break;

    case MessageType::SetVolumeControl:
        handleSetVolumeControl(message);
        break;

    case MessageType::SetDriftCorrection:
        handleSetDriftCorrection(message);
        break;

    case MessageType::DriftFeedback:
        handleDriftFeedback(message);
        break;

    case MessageType::Ping:
        handlePing(message);
        break;

    case MessageType::Pong:
        // This client sends no ping (see makePong), so a pong answers nothing
        // it asked. Not an error -- a server that pongs unprompted is being
        // talkative, not wrong -- and it has already counted as the server
        // being heard from, which is all a pong is worth here.
        MULTIROOM_DEBUG("ignoring an unsolicited pong: this client sends no ping");
        break;

    case MessageType::Error:
    {
        const std::string text = errorText(message.payload);
        if (state_ == SessionState::Established)
        {
            // A session already exists, so this is about something inside it
            // rather than about the handshake.
            MULTIROOM_WARN("server error: %s", text.c_str());
        }
        else
        {
            fail("the server refused the handshake: " + text);
        }
        break;
    }

    case MessageType::Hello:
    case MessageType::AudioCapabilities:
    case MessageType::ClockSyncRequest:
    case MessageType::ClockSyncResult:
    case MessageType::StreamReady:
    case MessageType::BufferStatus:
    case MessageType::PlaybackReport:
    case MessageType::SystemReport:
    case MessageType::StreamAttach:
    case MessageType::AudioDevices:
    case MessageType::InputControls:
    case MessageType::InputAction:
    case MessageType::BufferConfig:
    case MessageType::VolumeConfig:
    case MessageType::RoomName:
        fail("the server sent " + message.typeName + ", which only a client sends");
        break;

    case MessageType::Unknown:
        // Every later phase adds message types to this connection. Ignoring
        // them keeps a client usable against a server that is ahead of it;
        // they are named in the log so it is visible which ones were skipped.
        MULTIROOM_DEBUG("ignoring %s from the server: not implemented in this client",
                  message.typeName.c_str());
        break;
    }
}

void Session::handleHelloAck(const Message& message)
{
    if (state_ != SessionState::HelloSent)
    {
        fail("helloAck arrived while the session was " + std::string(sessionStateName(state_)));
        return;
    }

    HelloAck ack;
    std::string error;
    if (!parseHelloAck(message.payload, ack, error))
    {
        fail("invalid helloAck: " + error);
        return;
    }

    if (ack.hasProtocolVersion && ack.protocolVersion != kProtocolVersion)
    {
        fail("the server speaks MATP version " + std::to_string(ack.protocolVersion) +
             ", this client speaks " + std::to_string(kProtocolVersion));
        return;
    }

    // Mandatory, and immediately: the server may not send audio before it
    // knows what this client can play (section 12).
    if (!sendCapabilities())
        return;

    state_ = SessionState::CapabilitiesSent;
}

void Session::handleSessionConfig(const Message& message)
{
    if (state_ != SessionState::CapabilitiesSent && state_ != SessionState::Established)
    {
        fail("sessionConfig arrived while the session was " +
             std::string(sessionStateName(state_)));
        return;
    }

    SessionConfig config;
    std::string error;
    if (!parseSessionConfig(message.payload, config, error))
    {
        fail("invalid sessionConfig: " + error);
        return;
    }

    const bool renegotiated = state_ == SessionState::Established;

    // No format: this client reported none, and whatever stream it had is
    // over until a rescan finds a device.
    if (!config.hasFormat)
        releaseStream();

    sessionId_ = config.sessionId;
    streamFormat_ = config.hasFormat ? config.format : AudioFormat();
    hasStreamFormat_ = config.hasFormat;
    streamFormatPlayable_ = config.hasFormat && formatIsPlayable(config.format);
    state_ = SessionState::Established;
    lastError_.clear();
    ++handshakes_;

    // Before anything is played or reported: the Audio Control bounds what
    // may be done to the samples, and every SESSION_CONFIG says it again.
    if (audioControl_)
        audioControl_(config.hasAudioControl ? config.audioControl : AudioControl::BitPerfect,
                      config.hasAudioControl);

    // So is the actuator: a stream prepared after this is opened for it, and
    // one that is playing stops taking hardware commands at once if it is no
    // longer selected -- never both actuators at the same time.
    if (driftCorrection_)
        driftCorrection_(config.hasDriftCorrection ? config.driftCorrection
                                                   : DriftCorrectionMode::Resampling);

    if (!renegotiated)
    {
        // Section 10 puts clock synchronization directly after SESSION_CONFIG,
        // and section 46 wants twenty samples out of the way before playback
        // is scheduled against the mapping. A renegotiated format changes
        // nothing about either clock, so it does not start over.
        clockBurstRemaining_ = kClockSyncBurstSamples;
        clockBurstAttemptsLeft_ = kClockSyncBurstAttempts;
        clockNextRequestNs_ = 0; // the next poll sends the first request
    }

    MULTIROOM_INFO("MATP session %s %s: %s", sessionId_.c_str(),
             renegotiated ? "renegotiated" : "established",
             hasStreamFormat_ ? describeFormat(streamFormat_).c_str() : "no stream format");

    // The first lists of this session; after that, only a change is sent.
    audioDevicesChanged();
    inputControlsChanged();
    bufferConfigChanged();
    volumeConfigChanged();
    roomNameChanged();

    if (hasStreamFormat_ && !streamFormatPlayable_)
    {
        // Reported, not worked around. Answering with a format the server did
        // not choose is how the 24-bit layouts of section 12 turn into noise;
        // the client says what it can do and the server chooses again, which
        // is what STREAM_READY is for once there is a stream to prepare.
        MULTIROOM_WARN("the server chose %s, which the selected device did not report as playable",
                 describeFormat(streamFormat_).c_str());
    }
}

// PREPARE_STREAM is the point where the protocol stops being a conversation
// and touches hardware: the answer to it must be what the device actually
// granted, and it must not be sent before the device has granted it
// (section 12).
void Session::handlePrepareStream(const Message& message)
{
    if (state_ != SessionState::Established)
    {
        fail("prepareStream arrived while the session was " +
             std::string(sessionStateName(state_)));
        return;
    }
    if (!hasStreamFormat_)
    {
        fail("prepareStream arrived before the server had chosen a stream format");
        return;
    }

    PrepareStream request;
    std::string error;
    if (!parsePrepareStream(message.payload, request, error))
    {
        fail("invalid prepareStream: " + error);
        return;
    }
    if (!request.sessionId.empty() && request.sessionId != sessionId_)
    {
        // A stream for a session this client is not in. Guessing which of the
        // two the audio would belong to is exactly what section 13 exists to
        // prevent.
        fail("prepareStream names session " + request.sessionId + ", this client is in " +
             sessionId_);
        return;
    }

    prepareRequest_ = request;
    hasPrepareRequest_ = true;
    prepareStream();
}

bool Session::prepareStream()
{
    // A second PREPARE_STREAM reopens rather than stacks: the format may have
    // been renegotiated since the first one, and the device has to follow it.
    releaseStream();

    StreamReady ready;
    if (manualStandby_)
        ready.reason = "manual standby";
    else if (stream_.prepare)
    {
        standby_ = false;
        stream_.prepare(prepareRequest_, streamFormat_, ready);
    }
    else
        ready.reason = "this client has no playback path";

    ready.powerState = powerState();
    if (!send(MessageType::StreamReady, makeStreamReady(sessionId_, ready)))
        return false;

    streamPrepared_ = ready.ready;
    if (ready.ready)
    {
        // The next poll reports, rather than an interval from now.
        bufferStatusNextNs_ = 0;
        playbackReportNextNs_ = 0;
        MULTIROOM_INFO("stream prepared: %s, %lu frames per period",
                 describeFormat(ready.format).c_str(),
                 static_cast<unsigned long>(ready.periodFrames));
        if (ready.format != streamFormat_)
        {
            // Reported rather than corrected. The server compares the two and
            // sends what was confirmed here or renegotiates; a client that
            // quietly played the granted format while the server sent the
            // requested one would produce noise (section 12).
            MULTIROOM_WARN("the device granted %s, not the %s the server asked for",
                     describeFormat(ready.format).c_str(), describeFormat(streamFormat_).c_str());
        }
        return true;
    }

    MULTIROOM_WARN("cannot prepare the stream: %s",
             ready.reason.empty() ? "no reason given" : ready.reason.c_str());

    // Section 12: a client that cannot open the negotiated format does not
    // improvise. It says so and reports what it can play, so the server
    // chooses again -- unconditionally, because the server has just been told
    // that the choice it made from the last list does not work.
    if (!manualStandby_)
        sendCapabilities();
    return false;
}

// START_AT is the one message that has to be right to the frame (sections 20
// and 21), and the only thing this class contributes to that is the mapping:
// the server's instant becomes this machine's instant here, and where that
// lands on the sound card's own frame axis is the playback layer's business.
void Session::handleStartAt(const Message& message)
{
    if (state_ != SessionState::Established)
    {
        fail("startAt arrived while the session was " +
             std::string(sessionStateName(state_)));
        return;
    }

    StartAt request;
    std::string error;
    if (!parseStartAt(message.payload, request, error))
    {
        fail("invalid startAt: " + error);
        return;
    }
    if (!request.sessionId.empty() && request.sessionId != sessionId_)
    {
        fail("startAt names session " + request.sessionId + ", this client is in " +
             sessionId_);
        return;
    }

    // The rest is this client's own inability rather than the server's
    // mistake, so it is reported and not made into a dropped connection: a
    // reconnect would cost the clock model and the prepared device, which are
    // exactly what the next START_AT needs.
    if (!streamPrepared_)
    {
        MULTIROOM_ERROR("cannot start: no stream is prepared (section 20 waits for streamReady)");
        return;
    }

    uint64_t localStartNs = 0;
    if (!clock_.localFromServer(request.serverTimeNs, localStartNs))
    {
        // Without the mapping the instant is a number from another clock.
        // Section 10 puts clock synchronization before streaming for this
        // reason; the server reschedules once the exchange has produced one.
        MULTIROOM_ERROR("cannot start at server time %llu: no clock mapping yet",
                  static_cast<unsigned long long>(request.serverTimeNs));
        return;
    }

    if (!stream_.start)
    {
        MULTIROOM_ERROR("cannot start: this client has no playback path");
        return;
    }
    if (!stream_.start(request, localStartNs, error))
    {
        MULTIROOM_ERROR("cannot start timeline %llu ns: %s",
                  static_cast<unsigned long long>(request.timelineNs), error.c_str());
        return;
    }

    ++scheduledStarts_;
    const uint64_t nowNs = monotonicNowNs();
    MULTIROOM_INFO("start scheduled: timeline %llu ns at server time %llu, in %llu ms",
             static_cast<unsigned long long>(request.timelineNs),
             static_cast<unsigned long long>(request.serverTimeNs),
             static_cast<unsigned long long>(
                 localStartNs > nowNs ? (localStartNs - nowNs) / 1000000ULL : 0));
}


// PAUSE_AT, STOP_AT and FLUSH (sections 37, 38 and 54). None is a handshake
// message and none can fail the session: a room that cannot pause is this
// client's problem, and dropping the connection over it would cost the clock
// model and the prepared device -- the two things the next message needs.
void Session::handleTransportAt(const Message& message,
                                const std::function<bool(const TransportAt&, std::string&)>& hook,
                                uint32_t& counter)
{
    TransportAt request;
    std::string error;
    if (!parseTransportAt(message.payload, request, error))
    {
        fail(std::string("invalid ") + message.typeName + ": " + error);
        return;
    }
    if (!streamMessageApplies(message, request.sessionId))
        return;

    if (!hook)
    {
        MULTIROOM_ERROR("cannot %s: this client has no playback path", message.typeName.c_str());
        return;
    }
    if (!hook(request, error))
    {
        MULTIROOM_ERROR("cannot %s at timeline %llu ns: %s", message.typeName.c_str(),
                  static_cast<unsigned long long>(request.timelineNs), error.c_str());
        return;
    }

    ++counter;

    // The server's instant is logged rather than used: the timeline position
    // is what places the event (see TransportAt), and the two are the server's
    // own two spellings of one moment.
    MULTIROOM_INFO("%s scheduled: timeline %llu ns, server time %llu",
             message.typeName.c_str(),
             static_cast<unsigned long long>(request.timelineNs),
             static_cast<unsigned long long>(request.serverTimeNs));
}

// SET_VOLUME (section 39). Nothing about it is synchronized through the clock:
// an unscheduled change applies at once and a scheduled one at the timeline
// position the server named, which every room reaches at the same moment
// because that is what the timeline is.
void Session::handleSetVolume(const Message& message)
{
    SetVolume request;
    std::string error;
    if (!parseSetVolume(message.payload, request, error))
    {
        fail("invalid setVolume: " + error);
        return;
    }
    // No prepared stream needed: in standby, or before the first PREPARE, the
    // playback layer keeps the volume for the next device it opens.
    if (!sessionMessageApplies(message, request.sessionId))
        return;

    if (!stream_.volume)
    {
        MULTIROOM_ERROR("cannot set the volume: this client has no playback path");
        return;
    }
    if (!stream_.volume(request, error))
    {
        MULTIROOM_ERROR("cannot set the volume: %s", error.c_str());
        return;
    }

    ++volumeChanges_;
}

// TRACK_BOUNDARY (section 23): where the next track begins on a timeline that
// has no gap at that point (section 22). The client is told so it can say what
// is playing; nothing about playback changes here, and the hook is the only
// one that cannot refuse.
void Session::handleTrackBoundary(const Message& message)
{
    TrackBoundary boundary;
    std::string error;
    if (!parseTrackBoundary(message.payload, boundary, error))
    {
        fail("invalid trackBoundary: " + error);
        return;
    }
    if (!streamMessageApplies(message, boundary.sessionId))
        return;

    ++trackBoundaries_;
    if (stream_.trackBoundary)
        stream_.trackBoundary(boundary);
}

// TRACK_INFO: the same metadata for the track that is playing now rather than
// for one that begins later. Like a boundary it changes nothing about
// playback, and like a boundary it belongs to a stream -- a room with no
// stream has nothing that could be playing.
void Session::handleTrackInfo(const Message& message)
{
    TrackInfo info;
    std::string error;
    if (!parseTrackInfo(message.payload, info, error))
    {
        fail("invalid trackInfo: " + error);
        return;
    }
    if (!streamMessageApplies(message, info.sessionId))
        return;

    ++trackInfos_;
    if (stream_.trackInfo)
        stream_.trackInfo(info);
}

// PING (section 17), answered in any state and without a session: it asks
// whether this client is there, and the answer is the same whether the
// handshake has finished or not. A server that goes unanswered concludes the
// room is gone, which is the one thing a client that *is* there must not let
// happen.
// SET_AUDIO_DEVICE (section 56). Not a stream message: a room with no stream
// yet may still be told which card to open when one is prepared. A request the
// client cannot honour is answered with an ERROR rather than failing the
// session -- the room still plays on the device it had.
void Session::handleSetAudioDevice(const Message& message)
{
    SetAudioDevice request;
    std::string error;
    if (!parseSetAudioDevice(message.payload, request, error))
    {
        fail("invalid setAudioDevice: " + error);
        return;
    }
    if (!settingsMessageApplies(message, request.sessionId))
        return;

    if (!setAudioDevice_)
        error = "this client cannot change its audio device";
    else if (setAudioDevice_(request.deviceId, error))
    {
        ++audioDeviceChanges_;
        return;
    }

    MULTIROOM_ERROR("cannot change the audio device: %s", error.c_str());
    send(MessageType::Error, makeError("setAudioDevice", error));
}

// RESCAN_AUDIO (section 56). The scan takes seconds on the owner thread, like
// the control API's rescanAudio; the hook reports what it found.
void Session::handleRescanAudio(const Message& message)
{
    std::string sessionId;
    std::string error;
    if (!parseRescanAudio(message.payload, sessionId, error))
    {
        fail("invalid rescanAudio: " + error);
        return;
    }
    if (!settingsMessageApplies(message, sessionId))
        return;

    if (!rescanAudio_)
        error = "this client cannot rescan its audio devices";
    else if (rescanAudio_(error))
    {
        ++audioRescans_;
        return;
    }

    MULTIROOM_ERROR("cannot rescan audio devices: %s", error.c_str());
    send(MessageType::Error, makeError("rescanAudio", error));
}

// SET_INPUT_BINDINGS (section 57), shaped like SET_AUDIO_DEVICE: a message
// that does not parse is a broken server, a binding the client refuses -- an
// unknown input, a gesture it does not have, a step out of range -- is
// answered with an ERROR and the room plays on.
void Session::handleSetInputBindings(const Message& message)
{
    SetInputBindings request;
    std::string error;
    if (!parseSetInputBindings(message.payload, request, error))
    {
        fail("invalid setInputBindings: " + error);
        return;
    }
    if (!settingsMessageApplies(message, request.sessionId))
        return;

    if (!setInputBindings_)
        error = "this client has no inputs";
    else if (setInputBindings_(request, error))
    {
        ++inputBindingChanges_;
        return;
    }

    MULTIROOM_WARN("cannot change the bindings of input %s: %s", request.inputId.c_str(), error.c_str());
    send(MessageType::Error, makeError("setInputBindings", error));
}

// SET_BUFFER (section 58), shaped like SET_AUDIO_DEVICE. Accepted, it is
// always answered with BUFFER_CONFIG -- also when the value did not change,
// because the answer is what tells the server the request was taken. The owner
// re-reports the capabilities itself when the target changed which formats fit.
void Session::handleSetBuffer(const Message& message)
{
    SetBuffer request;
    std::string error;
    if (!parseSetBuffer(message.payload, request, error))
    {
        fail("invalid setBuffer: " + error);
        return;
    }
    if (!settingsMessageApplies(message, request.sessionId))
        return;

    if (!setBuffer_ || !buffer_)
    {
        error = "this client cannot change its buffer";
    }
    else
    {
        // Forgotten first, so the owner's own report of a change -- which it
        // sends before the capabilities that change may have moved -- goes
        // out, and an unchanged value is still answered below.
        const std::string lastSent = lastBufferConfig_;
        lastBufferConfig_.clear();
        if (setBuffer_(request.bufferMs, error))
        {
            ++bufferChanges_;
            bufferConfigChanged();
            return;
        }
        lastBufferConfig_ = lastSent;
    }

    MULTIROOM_WARN("cannot set the buffer to %lu ms: %s",
             static_cast<unsigned long>(request.bufferMs), error.c_str());
    send(MessageType::Error, makeError("setBuffer", error));
}

// SET_ROOM_NAME (section 60), shaped like SET_BUFFER. Accepted, it is always
// answered with ROOM_NAME -- also when the name did not change. Nothing else
// about the session moves: the name is a label, not the identity.
void Session::handleSetRoomName(const Message& message)
{
    SetRoomName request;
    std::string error;
    if (!parseSetRoomName(message.payload, request, error))
    {
        // A name that breaks section 60's rule is the request's fault, not
        // the session's: refused, and the session goes on.
        MULTIROOM_WARN("refusing setRoomName: %s", error.c_str());
        send(MessageType::Error, makeError("setRoomName", error));
        return;
    }
    if (!settingsMessageApplies(message, request.sessionId))
        return;

    if (!setRoomName_ || !roomName_)
    {
        error = "this client cannot change its name";
    }
    else
    {
        const std::string lastSent = lastRoomName_;
        lastRoomName_.clear();
        if (setRoomName_(request.name, error))
        {
            roomNameChanged();
            return;
        }
        lastRoomName_ = lastSent;
    }

    MULTIROOM_WARN("cannot rename the room to \"%s\": %s", request.name.c_str(), error.c_str());
    send(MessageType::Error, makeError("setRoomName", error));
}

// SET_VOLUME_CONTROL (section 59), shaped like SET_BUFFER: accepted, it is
// always answered with VOLUME_CONFIG, also when nothing changed.
void Session::handleSetVolumeControl(const Message& message)
{
    SetVolumeControl request;
    std::string error;
    if (!parseSetVolumeControl(message.payload, request, error))
    {
        fail("invalid setVolumeControl: " + error);
        return;
    }
    if (!settingsMessageApplies(message, request.sessionId))
        return;

    if (!setVolumeControl_ || !volumeConfig_)
    {
        error = "this client cannot change its volume control";
    }
    else
    {
        const std::string lastSent = lastVolumeConfig_;
        lastVolumeConfig_.clear();
        if (setVolumeControl_(request.volumeControl, error))
        {
            ++volumeControlChanges_;
            volumeConfigChanged();
            return;
        }
        lastVolumeConfig_ = lastSent;
    }

    MULTIROOM_WARN("cannot set the volume control to %s: %s",
             volumeControlModeName(request.volumeControl), error.c_str());
    send(MessageType::Error, makeError("setVolumeControl", error));
}

// SET_DRIFT_CORRECTION: persisted by the setter, and answered with the
// capabilities it changed -- the server renegotiates from those, which is
// what reopens the device for the other actuator.
void Session::handleSetDriftCorrection(const Message& message)
{
    SetDriftCorrection request;
    std::string error;
    if (!parseSetDriftCorrection(message.payload, request, error))
    {
        fail("invalid setDriftCorrection: " + error);
        return;
    }
    if (!settingsMessageApplies(message, request.sessionId))
        return;

    if (!setDriftCorrection_)
        error = "this client cannot change its drift correction";
    else if (setDriftCorrection_(request.driftCorrection, error))
    {
        ++driftCorrectionChanges_;
        capabilitiesChanged();
        return;
    }

    MULTIROOM_WARN("cannot set the drift correction to %s: %s",
             driftCorrectionModeName(request.driftCorrection), error.c_str());
    send(MessageType::Error, makeError("setDriftCorrection", error));
}

// DRIFT_FEEDBACK: the server's measurement, and while the hardware clock is
// negotiated its command. Telemetry first, so one that arrives out of turn is
// skipped rather than a reason to drop the session; only a malformed one is.
void Session::handleDriftFeedback(const Message& message)
{
    DriftFeedback feedback;
    std::string error;
    if (!parseDriftFeedback(message.payload, feedback, error))
    {
        fail("invalid driftFeedback: " + error);
        return;
    }
    if (state_ != SessionState::Established || feedback.sessionId != sessionId_)
    {
        MULTIROOM_DEBUG("ignoring driftFeedback for session %s", feedback.sessionId.c_str());
        return;
    }
    if (feedback.sequence <= lastDriftSequence_)
    {
        MULTIROOM_DEBUG("ignoring stale driftFeedback %llu",
                  static_cast<unsigned long long>(feedback.sequence));
        return;
    }
    lastDriftSequence_ = feedback.sequence;
    ++driftFeedbacks_;
    if (driftFeedback_)
        driftFeedback_(feedback);
}

void Session::handlePing(const Message& message)
{
    if (!send(MessageType::Pong, makePong(message.payload)))
        return;

    ++pongs_;
    MULTIROOM_TRACE("answered a ping");
}

bool Session::sessionMessageApplies(const Message& message, const std::string& sessionId)
{
    if (state_ != SessionState::Established)
    {
        // Before SESSION_CONFIG there is no session for it to belong to, which
        // is a server talking out of turn rather than a bad message.
        fail(message.typeName + " arrived while the session was " +
             std::string(sessionStateName(state_)));
        return false;
    }
    if (!sessionId.empty() && sessionId != sessionId_)
    {
        fail(message.typeName + " names session " + sessionId + ", this client is in " +
             sessionId_);
        return false;
    }
    return true;
}

bool Session::streamMessageApplies(const Message& message, const std::string& sessionId)
{
    if (!sessionMessageApplies(message, sessionId))
        return false;
    if (!streamPrepared_)
    {
        // This client's own state rather than the server's mistake: there is
        // nothing to pause, stop or turn down until a stream is prepared.
        MULTIROOM_ERROR("ignoring %s: no stream is prepared", message.typeName.c_str());
        return false;
    }
    return true;
}

bool Session::settingsMessageApplies(const Message& message, const std::string& sessionId)
{
    if (state_ != SessionState::Established)
    {
        fail(message.typeName + " arrived while the session was " +
             std::string(sessionStateName(state_)));
        return false;
    }
    if (!sessionId.empty() && sessionId != sessionId_)
    {
        MULTIROOM_WARN("ignoring %s for session %s, this is %s", message.typeName.c_str(),
                 sessionId.c_str(), sessionId_.c_str());
        return false;
    }
    return true;
}

void Session::handleClockSyncReply(const Message& message, uint64_t arrivalNs)
{
    ClockSyncReply reply;
    std::string error;
    if (!parseClockSyncReply(message.payload, reply, error))
    {
        // The server contradicting itself about its own clock is a protocol
        // error, not a bad measurement: every later phase schedules against
        // what it says here.
        fail("invalid clockSyncReply: " + error);
        return;
    }

    if (!clockRequestPending_ || reply.clientSendNs != clockRequestSendNs_)
    {
        // A reply to an exchange this client has already given up on. Not an
        // error -- the server answered a question that was asked -- but its
        // round trip no longer means anything, so it is not measured.
        MULTIROOM_DEBUG("ignoring a clockSyncReply for an exchange that is no longer outstanding");
        return;
    }

    clockRequestPending_ = false;
    ++clockSyncReplies_;

    ClockSyncSample sample;
    sample.clientSendNs = clockRequestSendNs_;
    sample.serverReceiveNs = reply.serverReceiveNs;
    sample.serverSendNs = reply.serverSendNs;
    sample.clientReceiveNs = arrivalNs;

    // Section 8: the server holds three of these four and cannot know C4, so
    // without this it has no client-to-server clock model and cannot place
    // PLAYBACK_REPORT's instants in its own time -- which is the whole offset
    // half of its correction. Sent for every exchange that completed,
    // including one the model below rejects as an outlier: the two sides
    // filter independently, and pre-filtering here would hide exactly the
    // samples the server's own outlier test exists for.
    //
    // Best-effort. A send that fails takes the connection with it soon enough
    // through everything else on it, and a lost result costs one sample.
    if (send(MessageType::ClockSyncResult,
             makeClockSyncResult(sample.clientSendNs, sample.serverReceiveNs,
                                 sample.serverSendNs, sample.clientReceiveNs)))
        ++clockSyncResults_;

    const bool hadEstimate = clock_.hasEstimate();
    if (!clock_.addSample(sample))
        return;

    if (clockBurstRemaining_ > 0)
        --clockBurstRemaining_;

    if (hadEstimate && !clock_.hasEstimate())
    {
        // The model restarted its window: one of the clocks stepped. At the
        // five second interval the samples a new mapping needs would take
        // fifteen seconds, and every START_AT until then is refused, so the
        // burst of section 46 is run again, from the next poll.
        clockBurstRemaining_ = kClockSyncBurstSamples;
        clockBurstAttemptsLeft_ = kClockSyncBurstAttempts;
        clockNextRequestNs_ = 0;
        MULTIROOM_WARN("server clock mapping lost: a clock stepped; resynchronizing");
        return;
    }

    if (hadEstimate || !clock_.hasEstimate())
        return;

    // Once only, when the mapping first exists: it is the point from which
    // scheduled playback becomes possible at all.
    MULTIROOM_INFO("server clock mapped: offset %lld ms, round trip %llu us, +/- %llu us",
             static_cast<long long>(clock_.offsetNs() / 1000000),
             static_cast<unsigned long long>(clock_.bestRoundTripNs() / 1000),
             static_cast<unsigned long long>(clock_.uncertaintyNs() / 1000));
}

bool Session::formatIsPlayable(const AudioFormat& format) const
{
    CapabilityReport report;
    std::string error;
    if (!capabilities_ || !capabilities_(report, error))
        return false;

    for (size_t i = 0; i < report.formats.size(); ++i)
    {
        if (report.formats[i] == format)
            return true;
    }
    return false;
}

void Session::fail(const std::string& reason)
{
    lastError_ = reason;
    state_ = SessionState::Failed;
    // "session" rather than "handshake": most of what fails here fails long
    // after the handshake did, and a silence timeout is not a handshake.
    MULTIROOM_ERROR("MATP session failed: %s", reason.c_str());

    // The session is kept as Failed until the next connection, which may be a
    // long backoff away; the stream goes now, as with any other session end.
    releaseStream();

    // Only a new connection resynchronizes a control stream, so the drop is
    // the repair: the connection's backoff decides when to try again.
    connection_.dropConnection(reason);
}

} // namespace matp
} // namespace multiroom
