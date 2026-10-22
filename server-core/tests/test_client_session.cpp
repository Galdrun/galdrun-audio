// Phase 8's handshake, from this server's end.
//
// Every test here drives the session with frames built by the *client's* half
// of the shared wire library and reads its answers with the client's parsers
// -- `makeHello`, `parseHelloAck`, `parseSessionConfig`, `parseClockSyncReply`
// are the exact calls the real binary makes. That is what makes these loopback
// tests rather than a server talking to a mirror of itself: the only way they
// pass with a field name wrong is if it is wrong in one place, which is the
// whole point of the move into `shared`.
//
// Nothing here opens a socket or sleeps. The clock is `multiroomtest::ManualClock`,
// so a twenty-second silence timeout is an addition.

#include "ManualClock.h"
#include "TestSupport.h"

#include "client/ClientSession.h"

#include <string>
#include <vector>

using namespace multiroom;

namespace {

// Bytes a client would put on the wire.
std::string frameOf(matp::MessageType type, const json::Value& payload)
{
    return matp::encode(type, payload);
}

void feed(ClientSession& session, const std::string& frame)
{
    CHECK_OK(session.receive(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(frame.data()), frame.size())));
}

Status<SessionError> feedRaw(ClientSession& session, const std::string& frame)
{
    return session.receive(std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(frame.data()), frame.size()));
}

// Everything the session has queued, decoded with the client's own reader.
std::vector<matp::Message> drain(ClientSession& session)
{
    matp::FrameReader reader;
    std::string frame;
    while (session.nextOutbound(frame))
        reader.append(frame.data(), frame.size());

    std::vector<matp::Message> messages;
    for (;;)
    {
        matp::Message message;
        std::string error;
        const matp::FrameReader::Result result = reader.next(message, error);
        if (result == matp::FrameReader::Result::Message)
        {
            messages.push_back(message);
            continue;
        }
        if (result == matp::FrameReader::Result::Invalid)
            multiroomtest::failAt("the session produced a frame the client cannot read: " + error,
                            __FILE__, __LINE__);
        break;
    }
    return messages;
}

matp::Identity clientIdentity(const std::string& id = "livingroom")
{
    matp::Identity identity;
    identity.clientId = id;
    identity.clientVersion = "0.1.0";
    identity.platform = "linux-armv6";
    return identity;
}

matp::CapabilityReport usbDac(uint32_t preferredRate = 0)
{
    matp::CapabilityReport report;
    report.deviceId = "hw:CARD=USB,DEV=0";
    report.deviceName = "USB DAC";
    report.preferredSampleRate = preferredRate;

    AudioFormat format;
    format.channels = 2;
    format.sampleType = AudioFormat::SampleType::Int24In32;
    for (const uint32_t rate : {44100u, 48000u, 96000u})
    {
        format.sampleRate = rate;
        report.formats.push_back(format);
    }
    return report;
}

SessionId testSessionId()
{
    return SessionId(0x0123456789abcdefULL, 0xfedcba9876543210ULL);
}

// HELLO and AUDIO_CAPABILITIES in, HELLO_ACK, SESSION_CONFIG and
// PREPARE_STREAM out and discarded: the state every phase-9 test starts from.
void handshake(ClientSession& session)
{
    feed(session, frameOf(matp::MessageType::Hello, matp::makeHello(clientIdentity())));
    feed(session, frameOf(matp::MessageType::AudioCapabilities,
                          matp::makeAudioCapabilities(usbDac())));
    drain(session);
}

} // namespace

MULTIROOM_TEST(session_reprepares_automatic_standby_but_keeps_manual_standby)
{
    multiroomtest::ManualClock clock(1000000000ULL);
    ClientSession session(testSessionId(), clock);
    handshake(session);
    matp::StreamReady ready;
    ready.reason = "automatic standby";
    ready.standby = true;
    ready.powerState = "standby";
    feed(session, frameOf(matp::MessageType::StreamReady, matp::makeStreamReady(testSessionId().toHex(), ready)));
    CHECK(session.streamStandby());
    CHECK_EQ(session.powerState(), std::string("standby"));
    CHECK(!session.streamReady());
    CHECK_OK(session.wakeStream());
    const auto frames = drain(session);
    CHECK_EQ(frames.size(), size_t(1));
    CHECK(frames[0].type == matp::MessageType::PrepareStream);
    CHECK(!session.streamStandby());
    CHECK_OK(session.wakeStream());
    CHECK(drain(session).empty());
    ready.standby = false;
    ready.powerState = "standbyManual";
    ready.reason = "manual standby";
    feed(session, frameOf(matp::MessageType::StreamReady, matp::makeStreamReady(testSessionId().toHex(), ready)));
    CHECK_OK(session.wakeStream());
    CHECK(drain(session).empty());
    CHECK_EQ(session.powerState(), std::string("standbyManual"));
}

MULTIROOM_TEST(session_completes_the_handshake_of_section_10)
{
    multiroomtest::ManualClock clock(1000000000ULL);
    ClientSession session(testSessionId(), clock);
    CHECK(session.state() == SessionState::Connected);

    feed(session, frameOf(matp::MessageType::Hello, matp::makeHello(clientIdentity())));
    CHECK(session.state() == SessionState::Identified);
    CHECK_EQ(session.clientId().str(), std::string("livingroom"));

    std::vector<matp::Message> out = drain(session);
    CHECK_EQ(out.size(), size_t(1));
    CHECK(out[0].type == matp::MessageType::HelloAck);

    matp::HelloAck ack;
    std::string error;
    CHECK(matp::parseHelloAck(out[0].payload, ack, error));
    CHECK(ack.hasProtocolVersion);
    CHECK_EQ(ack.protocolVersion, matp::kProtocolVersion);

    feed(session, frameOf(matp::MessageType::AudioCapabilities,
                          matp::makeAudioCapabilities(usbDac())));
    CHECK(session.state() == SessionState::Configured);
    CHECK(session.hasCapabilities());
    CHECK_EQ(session.capabilities().formats.size(), size_t(3));
    CHECK(!session.hasHardwareVolume()); // the report did not say

    // Section 10's order: the format, and then the instruction to open a
    // device for it. They go out together because the client waits for the
    // first and acts on the second.
    out = drain(session);
    CHECK_EQ(out.size(), size_t(2));
    CHECK(out[0].type == matp::MessageType::SessionConfig);
    CHECK(out[1].type == matp::MessageType::PrepareStream);

    matp::SessionConfig config;
    CHECK(matp::parseSessionConfig(out[0].payload, config, error));
    // Section 13: the string and the 128 bits are two forms of one value, and
    // the audio header carries the bits. They can never disagree.
    CHECK_EQ(config.sessionId, testSessionId().toHex());
    CHECK(config.format == session.announcedFormat());
    CHECK(config.format.isValid());

    // The audio port is announced rather than left to the client's "control
    // port plus one" fallback, which is right for the default pair and silent
    // for any other.
    matp::PrepareStream prepare;
    CHECK(matp::parsePrepareStream(out[1].payload, prepare, error));
    CHECK_EQ(prepare.sessionId, testSessionId().toHex());
    CHECK(prepare.hasAudioPort);
    CHECK_EQ(prepare.audioPort, uint16_t(4712));
    CHECK_EQ(session.prepareStreams(), 1ULL);
}

MULTIROOM_TEST(session_refuses_a_version_mismatch_at_hello)
{
    // Section 51: version 1 and version 2 disagree about what every position
    // on the wire means, so a half-understood session is worse than none.
    multiroomtest::ManualClock clock;
    ClientSession session(testSessionId(), clock);

    json::Value hello = matp::makeHello(clientIdentity());
    hello["protocolVersion"] = json::Value(uint32_t(1));

    CHECK_ERR(feedRaw(session, frameOf(matp::MessageType::Hello, hello)),
              SessionError::VersionMismatch);
    CHECK(session.state() == SessionState::Closed);

    // The peer is told why rather than seeing a socket close.
    const std::vector<matp::Message> out = drain(session);
    CHECK_EQ(out.size(), size_t(1));
    CHECK(out[0].type == matp::MessageType::Error);
    CHECK(matp::errorText(out[0].payload).find("versionMismatch") != std::string::npos);
}

MULTIROOM_TEST(session_refuses_anything_before_hello)
{
    multiroomtest::ManualClock clock;
    ClientSession session(testSessionId(), clock);

    CHECK_ERR(feedRaw(session, frameOf(matp::MessageType::AudioCapabilities,
                                       matp::makeAudioCapabilities(usbDac()))),
              SessionError::NotIdentified);
    CHECK(session.state() == SessionState::Closed);
}

MULTIROOM_TEST(session_tells_its_owner_before_every_prepare_stream)
{
    // The client lets go of its audio connection on every PREPARE_STREAM, so
    // the owner is told first -- before the message is queued -- and stops
    // writing into that connection while there is still nothing to lose.
    multiroomtest::ManualClock clock;
    uint64_t told = 0;
    uint64_t queuedWhenTold = 0;
    ClientSessionSettings settings;
    ClientSession* self = nullptr;
    settings.hooks.onPrepareStream = [&](const ClientSession& preparing) {
        ++told;
        queuedWhenTold = preparing.prepareStreams();
        CHECK(&preparing == self);
    };

    ClientSession session(testSessionId(), clock, settings);
    self = &session;
    feed(session, frameOf(matp::MessageType::Hello, matp::makeHello(clientIdentity())));
    feed(session, frameOf(matp::MessageType::AudioCapabilities,
                          matp::makeAudioCapabilities(usbDac(48000))));
    CHECK_EQ(told, 1ULL);
    CHECK_EQ(queuedWhenTold, 1ULL);

    // A re-negotiation reopens the device, and so the connection, again.
    CHECK(session.renegotiate().has_value());
    CHECK_EQ(told, 2ULL);
    CHECK_EQ(session.prepareStreams(), 2ULL);
}

MULTIROOM_TEST(session_re_negotiates_when_capabilities_are_sent_again)
{
    // Section 12: the client re-sends AUDIO_CAPABILITIES when its device
    // changes, and a SESSION_CONFIG on an established session is a
    // re-negotiation rather than a protocol error. This is also the machinery
    // phase 15's format transition reuses, which is why Configured is a state
    // the session can re-enter.
    multiroomtest::ManualClock clock;
    ClientSession session(testSessionId(), clock);

    feed(session, frameOf(matp::MessageType::Hello, matp::makeHello(clientIdentity())));
    feed(session, frameOf(matp::MessageType::AudioCapabilities,
                          matp::makeAudioCapabilities(usbDac(48000))));
    drain(session);

    CHECK_EQ(session.sessionConfigs(), 1ULL);
    CHECK_EQ(session.announcedFormat().sampleRate, 48000u);

    // A different device: one rate, packed 24-bit.
    matp::CapabilityReport replaced;
    replaced.deviceName = "Cheap DAC";
    AudioFormat only;
    only.sampleRate = 44100;
    only.channels = 2;
    only.sampleType = AudioFormat::SampleType::Int24;
    replaced.formats.push_back(only);

    feed(session, frameOf(matp::MessageType::AudioCapabilities,
                          matp::makeAudioCapabilities(replaced)));

    CHECK(session.state() == SessionState::Configured);
    CHECK_EQ(session.sessionConfigs(), 2ULL);
    CHECK(session.announcedFormat() == only);

    // A second pair, not a second session: the client reopens its device
    // rather than stacking a stream, which is what phase 15's format
    // transition reuses instead of needing new client machinery.
    const std::vector<matp::Message> out = drain(session);
    CHECK_EQ(out.size(), size_t(2));
    CHECK(out[0].type == matp::MessageType::SessionConfig);
    CHECK(out[1].type == matp::MessageType::PrepareStream);
    CHECK_EQ(session.prepareStreams(), 2ULL);

    matp::SessionConfig config;
    std::string error;
    CHECK(matp::parseSessionConfig(out[0].payload, config, error));
    // The same session: a re-negotiation is not a new one, which is exactly
    // what keeps the client's audio stream from reading it as a restart.
    CHECK_EQ(config.sessionId, testSessionId().toHex());
    CHECK(config.format.sampleType == AudioFormat::SampleType::Int24);
}

MULTIROOM_TEST(session_announces_the_preferred_rate_when_the_client_offers_it)
{
    ClientCapabilities capabilities;
    AudioFormat format;
    format.channels = 2;
    format.sampleType = AudioFormat::SampleType::Int16;
    for (const uint32_t rate : {44100u, 96000u})
    {
        format.sampleRate = rate;
        capabilities.formats.push_back(format);
    }
    capabilities.preferredSampleRate = 96000;

    const AudioFormat* chosen = idleStreamFormat(capabilities);
    CHECK(chosen != nullptr);
    CHECK_EQ(chosen->sampleRate, 96000u);

    // A preference the device does not offer is not a rate, so the ceiling
    // rule takes the highest it does offer.
    capabilities.preferredSampleRate = 192000;
    chosen = idleStreamFormat(capabilities);
    CHECK(chosen != nullptr);
    CHECK_EQ(chosen->sampleRate, 96000u);

    capabilities.preferredSampleRate = 0;
    chosen = idleStreamFormat(capabilities);
    CHECK(chosen != nullptr);
    CHECK_EQ(chosen->sampleRate, 96000u);
}

MULTIROOM_TEST(session_announces_the_rooms_ceiling_from_a_real_probe_order)
{
    // This list is shaped the way the real client builds one: layout outermost,
    // then channel count, then rate. "The first format offered" is therefore
    // mono 16-bit at the lowest rate, which is what this rule exists not to
    // announce to a stereo device.
    ClientCapabilities capabilities;
    for (const AudioFormat::SampleType type :
         {AudioFormat::SampleType::Int16, AudioFormat::SampleType::Int24In32,
          AudioFormat::SampleType::Float32})
    {
        for (const uint16_t channels : {uint16_t(1), uint16_t(2)})
        {
            for (const uint32_t rate : {44100u, 48000u})
            {
                AudioFormat format;
                format.sampleRate = rate;
                format.channels = channels;
                format.sampleType = type;
                capabilities.formats.push_back(format);
            }
        }
    }

    const AudioFormat* chosen = idleStreamFormat(capabilities);
    CHECK(chosen != nullptr);
    CHECK_EQ(chosen->sampleRate, 48000u); // the highest offered
    CHECK_EQ(chosen->channels, uint16_t(2)); // the most offered
    // 32 valid bits beats 24, and integer beats float at the same width: a
    // float32 mantissa carries 24 bits where an int32 carries 32.
    CHECK(chosen->sampleType == AudioFormat::SampleType::Float32);

    // With int32 in the list as well, the integer layout wins outright.
    AudioFormat wide;
    wide.sampleRate = 48000;
    wide.channels = 2;
    wide.sampleType = AudioFormat::SampleType::Int32;
    capabilities.formats.push_back(wide);

    chosen = idleStreamFormat(capabilities);
    CHECK(chosen != nullptr);
    CHECK(chosen->sampleType == AudioFormat::SampleType::Int32);
}

MULTIROOM_TEST(session_prefers_the_narrower_container_at_the_same_depth)
{
    // Int24 and Int24In32 carry the same 24 bits; the packed one is three
    // bytes rather than four and is equally exact.
    ClientCapabilities capabilities;
    AudioFormat inContainer;
    inContainer.sampleRate = 44100;
    inContainer.channels = 2;
    inContainer.sampleType = AudioFormat::SampleType::Int24In32;
    AudioFormat packed = inContainer;
    packed.sampleType = AudioFormat::SampleType::Int24;

    capabilities.formats.push_back(inContainer);
    capabilities.formats.push_back(packed);

    const AudioFormat* chosen = idleStreamFormat(capabilities);
    CHECK(chosen != nullptr);
    CHECK(chosen->sampleType == AudioFormat::SampleType::Int24);
}

MULTIROOM_TEST(session_answers_a_clock_sync_request_with_all_three_timestamps)
{
    multiroomtest::ManualClock clock(5000000000ULL);
    ClientSession session(testSessionId(), clock);

    feed(session, frameOf(matp::MessageType::Hello, matp::makeHello(clientIdentity())));
    feed(session, frameOf(matp::MessageType::AudioCapabilities,
                          matp::makeAudioCapabilities(usbDac())));
    drain(session);

    feed(session, frameOf(matp::MessageType::ClockSyncRequest,
                          matp::makeClockSyncRequest(1234567890ULL)));

    const std::vector<matp::Message> out = drain(session);
    CHECK_EQ(out.size(), size_t(1));
    CHECK(out[0].type == matp::MessageType::ClockSyncReply);

    matp::ClockSyncReply reply;
    std::string error;
    CHECK(matp::parseClockSyncReply(out[0].payload, reply, error));
    // C1 echoed: it is the correlator, and a monotonic clock does not produce
    // the same nanosecond twice.
    CHECK_EQ(reply.clientSendNs, 1234567890ULL);
    CHECK_EQ(reply.serverReceiveNs, 5000000000ULL);
    CHECK(reply.serverSendNs >= reply.serverReceiveNs);
    CHECK_EQ(session.clockSyncReplies(), 1ULL);
}

MULTIROOM_TEST(session_builds_a_clock_model_from_twenty_exchanges)
{
    // The done-when of phase 8: twenty exchanges yielding an offset inside the
    // injected value's uncertainty. The client's clock is 3.0 s behind the
    // server's and the round trip is 1 ms, so half of it -- 500 us -- is the
    // most the midpoint can be wrong by.
    constexpr int64_t kInjectedOffsetNs = 3000000000LL;
    constexpr uint64_t kRoundTripNs = 1000000ULL;

    multiroomtest::ManualClock clock;
    ClientSession session(testSessionId(), clock);

    feed(session, frameOf(matp::MessageType::Hello, matp::makeHello(clientIdentity())));
    feed(session, frameOf(matp::MessageType::AudioCapabilities,
                          matp::makeAudioCapabilities(usbDac())));
    drain(session);

    uint64_t clientNs = 100000000000ULL;
    for (int i = 0; i < 20; ++i)
    {
        const uint64_t c1 = clientNs;
        const uint64_t s2 = static_cast<uint64_t>(static_cast<int64_t>(c1) +
                                                  kInjectedOffsetNs + 400000);
        const uint64_t s3 = s2 + 50000;
        const uint64_t c4 = c1 + kRoundTripNs + 50000;

        feed(session, frameOf(matp::MessageType::ClockSyncResult,
                              matp::makeClockSyncResult(c1, s2, s3, c4)));
        clientNs += 5000000000ULL; // section 46's five-second interval
    }

    CHECK_EQ(session.clockSyncResults(), 20ULL);

    const ClientClockModel& model = session.clock();
    CHECK(model.valid());
    CHECK_EQ(model.acceptedCount(), 20ULL);
    CHECK_EQ(model.rejectedCount(), 0ULL);
    CHECK_NEAR(static_cast<double>(model.offsetNs()), static_cast<double>(kInjectedOffsetNs),
               static_cast<double>(kRoundTripNs) / 2.0);
}

MULTIROOM_TEST(session_pings_a_quiet_peer_and_drops_one_that_never_answers)
{
    ClientSessionSettings settings;
    settings.pingIntervalNs = 5000000000ULL;
    settings.silenceTimeoutNs = 20000000000ULL;

    multiroomtest::ManualClock clock(1000000000ULL);
    ClientSession session(testSessionId(), clock, settings);

    feed(session, frameOf(matp::MessageType::Hello, matp::makeHello(clientIdentity())));
    drain(session);

    // Nothing is due yet.
    CHECK_OK(session.poll());
    CHECK_EQ(session.pingsSent(), 0ULL);

    clock.advanceNs(settings.pingIntervalNs);
    CHECK_OK(session.poll());
    CHECK_EQ(session.pingsSent(), 1ULL);

    std::vector<matp::Message> out = drain(session);
    CHECK_EQ(out.size(), size_t(1));
    CHECK(out[0].type == matp::MessageType::Ping);

    // One at a time: a second ping would measure the queue the first is in.
    clock.advanceNs(settings.pingIntervalNs);
    CHECK_OK(session.poll());
    CHECK_EQ(session.pingsSent(), 1ULL);
    CHECK_EQ(drain(session).size(), size_t(0));

    // A half-open TCP connection never ends and never errors. This is what
    // notices one.
    clock.advanceNs(settings.silenceTimeoutNs);
    CHECK_ERR(session.poll(), SessionError::PeerSilent);
    CHECK(session.state() == SessionState::Closed);
}

MULTIROOM_TEST(session_stays_alive_while_the_peer_answers_its_pings)
{
    ClientSessionSettings settings;
    settings.pingIntervalNs = 5000000000ULL;
    settings.silenceTimeoutNs = 20000000000ULL;

    multiroomtest::ManualClock clock(1000000000ULL);
    ClientSession session(testSessionId(), clock, settings);

    feed(session, frameOf(matp::MessageType::Hello, matp::makeHello(clientIdentity())));
    drain(session);

    // Five minutes of a room that says nothing but pong.
    for (int i = 0; i < 60; ++i)
    {
        clock.advanceNs(settings.pingIntervalNs);
        CHECK_OK(session.poll());

        std::vector<matp::Message> out = drain(session);
        CHECK_EQ(out.size(), size_t(1));
        CHECK(out[0].type == matp::MessageType::Ping);

        // The client answers with the ping's own body, type changed.
        feed(session, frameOf(matp::MessageType::Pong, matp::makePong(out[0].payload)));
        CHECK_EQ(drain(session).size(), size_t(0));
    }

    CHECK(session.state() == SessionState::Identified);
    CHECK_EQ(session.pingsSent(), 60ULL);
    CHECK_EQ(session.pongsReceived(), 60ULL);
}

MULTIROOM_TEST(session_refuses_an_answer_to_a_question_it_never_asked)
{
    // PREPARE_STREAM goes out with SESSION_CONFIG, so before capabilities have
    // arrived a STREAM_READY answers nothing. Said out loud rather than
    // dropped: a component that is not implemented yet fails explicitly.
    multiroomtest::ManualClock clock;
    ClientSession session(testSessionId(), clock);

    feed(session, frameOf(matp::MessageType::Hello, matp::makeHello(clientIdentity())));
    drain(session);

    matp::StreamReady ready;
    ready.ready = true;
    ready.format.sampleRate = 44100;
    ready.format.channels = 2;

    CHECK_ERR(feedRaw(session, frameOf(matp::MessageType::StreamReady,
                                       matp::makeStreamReady("abc", ready))),
              SessionError::Unsolicited);
    CHECK(session.state() == SessionState::Closed);
}

MULTIROOM_TEST(session_refuses_a_stream_attach_on_the_control_connection)
{
    // The one message that belongs on the other socket (section 3). A client
    // that sends it here has bound nothing, and a server that accepted it
    // would be binding a connection that carries no audio.
    multiroomtest::ManualClock clock;
    ClientSession session(testSessionId(), clock);

    feed(session, frameOf(matp::MessageType::Hello, matp::makeHello(clientIdentity())));
    drain(session);

    CHECK_ERR(feedRaw(session,
                      frameOf(matp::MessageType::StreamAttach,
                              matp::makeStreamAttach(testSessionId().toHex(),
                                                     clientIdentity()))),
              SessionError::Unsolicited);
}

MULTIROOM_TEST(session_records_what_the_device_granted)
{
    // Section 12: STREAM_READY carries what the *driver* granted, which is not
    // always what SESSION_CONFIG asked for, and the server must send what was
    // confirmed here rather than what it asked for.
    multiroomtest::ManualClock clock;
    AudioFormat granted;
    std::string closedReason;
    ClientSessionSettings settings;
    settings.hooks.onStreamReady = [&granted](const ClientSession& ready) {
        granted = ready.grantedFormat();
    };
    settings.hooks.onClosed = [&closedReason](const ClientSession& closed) {
        closedReason = closed.lastError();
    };

    ClientSession session(testSessionId(), clock, settings);
    handshake(session);
    CHECK_EQ(session.prepareStreams(), 1ULL);
    CHECK(!session.streamReady());

    matp::StreamReady ready;
    ready.ready = true;
    ready.format.sampleRate = 48000;
    ready.format.channels = 2;
    ready.format.sampleType = AudioFormat::SampleType::Int16;
    ready.periodFrames = 480;

    feed(session, frameOf(matp::MessageType::StreamReady,
                          matp::makeStreamReady(testSessionId().toHex(), ready)));

    CHECK(session.streamReady());
    CHECK(session.grantedFormat() == ready.format);
    CHECK_EQ(session.grantedPeriodFrames(), 480u);
    CHECK_EQ(session.streamReadies(), 1ULL);
    CHECK(session.streamNotReadyReason().empty());
    // Nothing is answered: what to do about a granted format that differs is
    // the planner's, and what to do about a room that is ready is the zone's.
    CHECK_EQ(drain(session).size(), size_t(0));

    // The owner hears about it through the hooks and not by polling, which is
    // what lets a zone create this room's output on the listener's thread.
    CHECK(granted == ready.format);

    session.close("the test is over");
    CHECK(closedReason.find("the test is over") != std::string::npos);
}

MULTIROOM_TEST(session_keeps_a_room_alive_when_its_device_would_not_open)
{
    // A device that will not open is not a protocol error. Section 12 has the
    // client answer late and honestly, and giving up on the room is phase 11's
    // readiness timeout rather than this class's.
    multiroomtest::ManualClock clock;
    std::string told;
    ClientSessionSettings settings;
    settings.hooks.onStreamReady = [&told](const ClientSession& answered) {
        told = answered.streamReady() ? "ready" : answered.streamNotReadyReason();
    };
    ClientSession session(testSessionId(), clock, settings);
    handshake(session);

    matp::StreamReady ready;
    ready.ready = false;
    ready.reason = "the device is exclusive to another process";

    feed(session, frameOf(matp::MessageType::StreamReady,
                          matp::makeStreamReady(testSessionId().toHex(), ready)));

    CHECK(session.state() == SessionState::Configured);
    CHECK(!session.streamReady());
    CHECK_EQ(session.streamNotReadyReason(), ready.reason);
    CHECK(!session.grantedFormat().isValid());

    // The owner is told this answer too: a room that is silent is its
    // business, and it is not a room this class gives up on.
    CHECK_EQ(told, ready.reason);
}

MULTIROOM_TEST(session_hands_every_buffer_status_to_its_room)
{
    // Section 19, four times a second, and the only input pacing has. The hook
    // is what carries it to that room's output; the session itself keeps only
    // the last one for status.
    multiroomtest::ManualClock clock;
    std::vector<uint64_t> reported;
    ClientSessionSettings settings;
    settings.hooks.onBufferStatus = [&reported](const ClientSession&,
                                                const matp::BufferStatus& status) {
        reported.push_back(status.bufferedDurationNs);
    };

    ClientSession session(testSessionId(), clock, settings);
    handshake(session);

    matp::BufferStatus status;
    status.hasContent = true;
    status.playheadTimelineNs = 195000000000ULL;
    status.bufferedUntilTimelineNs = 197000000000ULL;
    status.bufferedDurationNs = 2000000000ULL;

    feed(session, frameOf(matp::MessageType::BufferStatus,
                          matp::makeBufferStatus(testSessionId().toHex(), status)));

    CHECK_EQ(session.bufferStatuses(), 1ULL);
    CHECK(session.bufferStatus().hasContent);
    CHECK_EQ(session.bufferStatus().bufferedDurationNs, 2000000000ULL);
    CHECK_EQ(reported.size(), size_t(1));
    CHECK_EQ(reported[0], 2000000000ULL);
}

MULTIROOM_TEST(session_refuses_a_report_for_another_session)
{
    // Section 13's purpose: a report that names a session this connection is
    // not in is a stale or crossed stream, and attributing it to this room
    // would pace this room on another one's buffer.
    multiroomtest::ManualClock clock;
    ClientSession session(testSessionId(), clock);
    handshake(session);

    matp::BufferStatus status;
    status.bufferedDurationNs = 1000;

    const SessionId other(1, 2);
    CHECK_ERR(feedRaw(session, frameOf(matp::MessageType::BufferStatus,
                                       matp::makeBufferStatus(other.toHex(), status))),
              SessionError::WrongSession);
}

MULTIROOM_TEST(session_accepts_the_playback_reports_a_prepared_client_sends)
{
    // The client reports from the moment its device opens (section 25), so a
    // server that refused them would end every session the moment a stream was
    // prepared. Counted and checked; what they are *for* is phase 12's.
    multiroomtest::ManualClock clock;
    ClientSession session(testSessionId(), clock);
    handshake(session);

    matp::PlaybackReport report;
    report.localMonotonicTimeNs = 5000000000ULL;
    report.deviceFramePosition = 441000;
    report.clockQuality = AudioClockQuality::Hardware;
    report.bufferedDurationNs = 1400000000ULL;

    feed(session, frameOf(matp::MessageType::PlaybackReport,
                          matp::makePlaybackReport(testSessionId().toHex(), report)));

    CHECK(session.state() == SessionState::Configured);
    CHECK_EQ(session.playbackReports(), 1ULL);
    CHECK_EQ(drain(session).size(), size_t(0));
}

MULTIROOM_TEST(session_refuses_a_message_only_a_server_sends)
{
    multiroomtest::ManualClock clock;
    ClientSession session(testSessionId(), clock);

    feed(session, frameOf(matp::MessageType::Hello, matp::makeHello(clientIdentity())));
    drain(session);

    CHECK_ERR(feedRaw(session, frameOf(matp::MessageType::HelloAck,
                                       matp::makeHelloAck(matp::kProtocolVersion))),
              SessionError::UnexpectedMessage);
}

MULTIROOM_TEST(session_ignores_a_message_type_it_does_not_know)
{
    // Forward compatibility, the same rule the client applies in the other
    // direction: a later message type must not break a peer that predates it.
    multiroomtest::ManualClock clock;
    ClientSession session(testSessionId(), clock);

    feed(session, frameOf(matp::MessageType::Hello, matp::makeHello(clientIdentity())));
    drain(session);

    json::Value future = json::Value::makeObject();
    future["type"] = json::Value("somethingFromVersionThree");
    future["value"] = json::Value(uint32_t(7));

    // Encoded with a wire number this build has never assigned.
    const std::string frame = matp::encode(static_cast<matp::MessageType>(9000), future);
    feed(session, frame);

    CHECK(session.state() == SessionState::Identified);
    CHECK_EQ(drain(session).size(), size_t(0));
}

MULTIROOM_TEST(session_decodes_a_handshake_arriving_one_byte_at_a_time)
{
    // One read is not one message. A handshake split to its worst case must
    // produce exactly the same three answers.
    multiroomtest::ManualClock clock;
    ClientSession session(testSessionId(), clock);

    std::string stream;
    stream += frameOf(matp::MessageType::Hello, matp::makeHello(clientIdentity()));
    stream += frameOf(matp::MessageType::AudioCapabilities,
                      matp::makeAudioCapabilities(usbDac()));
    stream += frameOf(matp::MessageType::ClockSyncRequest, matp::makeClockSyncRequest(42));

    for (size_t i = 0; i < stream.size(); ++i)
    {
        CHECK_OK(session.receive(std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(stream.data() + i), size_t(1))));
    }

    CHECK(session.state() == SessionState::Configured);

    const std::vector<matp::Message> out = drain(session);
    CHECK_EQ(out.size(), size_t(4));
    CHECK(out[0].type == matp::MessageType::HelloAck);
    CHECK(out[1].type == matp::MessageType::SessionConfig);
    CHECK(out[2].type == matp::MessageType::PrepareStream);
    CHECK(out[3].type == matp::MessageType::ClockSyncReply);
}

MULTIROOM_TEST(session_fails_itself_when_the_peer_stops_reading)
{
    // Memory is bounded. A room that never drains its socket fills its own
    // queue and ends, and the ERROR that says so is queued past the limit
    // because that is exactly when the peer most needs it.
    ClientSessionSettings settings;
    settings.maxOutboundFrames = 4;
    settings.pingIntervalNs = 1000000000ULL;
    settings.silenceTimeoutNs = 1000000000000ULL;

    multiroomtest::ManualClock clock(1000000000ULL);
    ClientSession session(testSessionId(), clock, settings);

    feed(session, frameOf(matp::MessageType::Hello, matp::makeHello(clientIdentity())));

    // HELLO_ACK is queued and nobody takes it; pings fill the rest. Each pong
    // clears the outstanding ping so the next one is due, without draining
    // anything -- which is exactly a peer that answers but never reads.
    Status<SessionError> polled;
    for (int i = 0; i < 8; ++i)
    {
        clock.advanceNs(settings.pingIntervalNs);
        polled = session.poll();
        if (!polled)
            break;

        const uint64_t sequence = static_cast<uint64_t>(i) + 1;
        feed(session, frameOf(matp::MessageType::Pong,
                              matp::makePong(matp::makePing(sequence))));
    }

    CHECK_ERR(polled, SessionError::OutboundFull);
    CHECK(session.state() == SessionState::Closed);
    CHECK(session.pendingOutbound() > settings.maxOutboundFrames);
}

MULTIROOM_TEST(session_refuses_everything_once_it_is_closed)
{
    multiroomtest::ManualClock clock;
    ClientSession session(testSessionId(), clock);

    session.close("the test said so");
    CHECK(session.state() == SessionState::Closed);
    CHECK_EQ(session.lastError(), std::string("the test said so"));

    CHECK_ERR(feedRaw(session, frameOf(matp::MessageType::Hello,
                                       matp::makeHello(clientIdentity()))),
              SessionError::Closed);
    CHECK_ERR(session.poll(), SessionError::Closed);

    // The first reason is kept: it is the one that explains the rest.
    session.close("and then something else");
    CHECK_EQ(session.lastError(), std::string("the test said so"));
}

MULTIROOM_TEST(session_sends_a_start_the_client_can_read_and_refuses_one_that_is_early)
{
    // Section 20 from this end: the pair is the zone's and the session id is
    // this class's, because it is the one field of the message it owns.
    multiroomtest::ManualClock clock;
    ClientSession session(testSessionId(), clock);
    handshake(session);

    matp::StartAt start;
    start.serverTimeNs = 102000000000ULL;
    start.timelineNs = 180000000000ULL;

    // A device that has not granted a stream has nothing to place a start on,
    // and the client answers one it cannot act on by logging to itself -- so a
    // start this side knows is early is better refused than sent.
    CHECK_ERR(session.startPlayback(start), SessionError::UnexpectedMessage);
    CHECK_EQ(drain(session).size(), size_t(0));

    matp::StreamReady ready;
    ready.ready = true;
    ready.format = session.announcedFormat();
    ready.periodFrames = 480;
    feed(session, frameOf(matp::MessageType::StreamReady,
                          matp::makeStreamReady(testSessionId().toHex(), ready)));

    CHECK_OK(session.startPlayback(start));
    CHECK_EQ(session.startsSent(), 1ULL);

    // Read back with the client's own parser, so a field that is wrong is
    // wrong according to the code on the other end of the socket.
    std::vector<matp::Message> out = drain(session);
    CHECK_EQ(out.size(), size_t(1));
    CHECK(out[0].type == matp::MessageType::StartAt);

    matp::StartAt parsed;
    std::string error;
    CHECK(matp::parseStartAt(out[0].payload, parsed, error));
    CHECK_EQ(parsed.sessionId, testSessionId().toHex());
    CHECK_EQ(parsed.serverTimeNs, start.serverTimeNs);
    CHECK_EQ(parsed.timelineNs, start.timelineNs);
}

MULTIROOM_TEST(session_hands_every_playback_report_to_its_room)
{
    // Phase 11 reads one field of it: a report that names an audible timeline
    // position is this room confirming that its START_AT took, and it is the
    // only acknowledgement the protocol has.
    multiroomtest::ManualClock clock;
    std::vector<matp::PlaybackReport> reported;
    ClientSessionSettings settings;
    settings.hooks.onPlaybackReport = [&reported](const ClientSession&,
                                                  const matp::PlaybackReport& report) {
        reported.push_back(report);
    };

    ClientSession session(testSessionId(), clock, settings);
    handshake(session);

    matp::PlaybackReport before;
    before.localMonotonicTimeNs = 5000000000ULL;
    before.deviceFramePosition = 441000;
    before.clockQuality = AudioClockQuality::Hardware;
    before.bufferedDurationNs = 1400000000ULL;
    feed(session, frameOf(matp::MessageType::PlaybackReport,
                          matp::makePlaybackReport(testSessionId().toHex(), before)));

    matp::PlaybackReport after = before;
    after.hasTimelineNsAudible = true;
    after.timelineNsAudible = 180000000000ULL;
    feed(session, frameOf(matp::MessageType::PlaybackReport,
                          matp::makePlaybackReport(testSessionId().toHex(), after)));

    CHECK_EQ(reported.size(), size_t(2));
    CHECK(!reported[0].hasTimelineNsAudible);
    CHECK(reported[1].hasTimelineNsAudible);
    CHECK_EQ(reported[1].timelineNsAudible, 180000000000ULL);
}

// --- phase 14: what a session says about a stream that is running ------------

MULTIROOM_TEST(session_sends_a_flush_the_client_can_read)
{
    // Section 54 (phase 26): the same three fields as a pause and a stop, and
    // the same refusal before the device has granted a stream.
    multiroomtest::ManualClock clock;
    ClientSession session(testSessionId(), clock);
    handshake(session);

    matp::TransportAt at;
    at.serverTimeNs = 150000000000ULL;
    at.timelineNs = 240000000000ULL;
    CHECK_ERR(session.flushPlayback(at), SessionError::UnexpectedMessage);
    CHECK_EQ(drain(session).size(), size_t(0));

    matp::StreamReady ready;
    ready.ready = true;
    ready.format = session.announcedFormat();
    ready.periodFrames = 480;
    feed(session, frameOf(matp::MessageType::StreamReady,
                          matp::makeStreamReady(testSessionId().toHex(), ready)));

    CHECK_OK(session.flushPlayback(at));
    CHECK_EQ(session.flushesSent(), 1ULL);

    std::vector<matp::Message> out = drain(session);
    CHECK_EQ(out.size(), size_t(1));
    CHECK(out[0].type == matp::MessageType::Flush);
    matp::TransportAt parsed;
    std::string error;
    CHECK(matp::parseTransportAt(out[0].payload, parsed, error));
    CHECK_EQ(parsed.sessionId, testSessionId().toHex());
    CHECK_EQ(parsed.serverTimeNs, at.serverTimeNs);
    CHECK_EQ(parsed.timelineNs, at.timelineNs);
}

MULTIROOM_TEST(session_sends_a_pause_and_a_stop_the_client_can_read)
{
    multiroomtest::ManualClock clock;
    ClientSession session(testSessionId(), clock);
    handshake(session);

    matp::TransportAt at;
    at.serverTimeNs = 150000000000ULL;
    at.timelineNs = 240000000000ULL;

    // Sections 37 and 38 act on playback, and a device that has not granted a
    // stream is not playing anything -- the same reason a start is refused.
    CHECK_ERR(session.pausePlayback(at), SessionError::UnexpectedMessage);
    CHECK_ERR(session.stopPlayback(at), SessionError::UnexpectedMessage);
    CHECK_EQ(drain(session).size(), size_t(0));

    matp::StreamReady ready;
    ready.ready = true;
    ready.format = session.announcedFormat();
    ready.periodFrames = 480;
    feed(session, frameOf(matp::MessageType::StreamReady,
                          matp::makeStreamReady(testSessionId().toHex(), ready)));

    CHECK_OK(session.pausePlayback(at));
    CHECK_OK(session.stopPlayback(at));
    CHECK_EQ(session.pausesSent(), 1ULL);
    CHECK_EQ(session.stopsSent(), 1ULL);

    // Read back with the client's own parser: one parser for both messages,
    // because the type string is the entire difference between them.
    std::vector<matp::Message> out = drain(session);
    CHECK_EQ(out.size(), size_t(2));
    CHECK(out[0].type == matp::MessageType::PauseAt);
    CHECK(out[1].type == matp::MessageType::StopAt);

    for (const matp::Message& message : out)
    {
        matp::TransportAt parsed;
        std::string error;
        CHECK(matp::parseTransportAt(message.payload, parsed, error));
        CHECK_EQ(parsed.sessionId, testSessionId().toHex());
        CHECK_EQ(parsed.serverTimeNs, at.serverTimeNs);
        CHECK_EQ(parsed.timelineNs, at.timelineNs);
    }
}

MULTIROOM_TEST(session_forwards_a_volume_and_refuses_one_that_asks_for_nothing)
{
    multiroomtest::ManualClock clock;
    ClientSession session(testSessionId(), clock);

    matp::SetVolume request;
    request.hasValue = true;
    request.value = 0.75;
    request.hasApplyAtTimelineNs = true;
    request.applyAtTimelineNs = 142000000000ULL;

    // Section 39 belongs to a stream rather than to a device -- the client
    // ignores a volume for a stream it has not been asked to prepare, and
    // applies one whether or not its driver has answered. So this is refused
    // before the handshake and accepted straight after it.
    CHECK_ERR(session.setVolume(request), SessionError::UnexpectedMessage);

    handshake(session);
    CHECK_OK(session.setVolume(request));
    CHECK_EQ(session.volumeChangesSent(), 1ULL);

    std::vector<matp::Message> out = drain(session);
    CHECK_EQ(out.size(), size_t(1));
    CHECK(out[0].type == matp::MessageType::SetVolume);

    matp::SetVolume parsed;
    std::string error;
    CHECK(matp::parseSetVolume(out[0].payload, parsed, error));
    CHECK_EQ(parsed.sessionId, testSessionId().toHex());
    CHECK(parsed.hasValue);
    CHECK_NEAR(parsed.value, 0.177827941, 1e-9); // the slider position 0.75 as a gain: -15 dB
    CHECK(!parsed.hasMuted); // absent means unchanged, so it is not written
    CHECK(parsed.hasApplyAtTimelineNs);
    CHECK_EQ(parsed.applyAtTimelineNs, request.applyAtTimelineNs);

    // A message naming neither a value nor a mute asks for nothing, and one
    // this server's own wire library would not write is one its peer would
    // refuse. The session stays up: it is the caller's mistake, not the room's.
    CHECK_ERR(session.setVolume(matp::SetVolume{}), SessionError::MalformedMessage);
    CHECK(session.state() == SessionState::Configured);
    CHECK_EQ(drain(session).size(), size_t(0));
    CHECK_EQ(session.volumeChangesSent(), 1ULL);
}

MULTIROOM_TEST(session_sends_the_two_metadata_messages_without_a_device)
{
    multiroomtest::ManualClock clock;
    ClientSession session(testSessionId(), clock);

    matp::TrackBoundary boundary;
    boundary.timelineNs = 180000000000ULL;
    boundary.trackId = "track-B";
    boundary.title = "Second Song";

    // Before the handshake there is no stream for either of them to be about.
    CHECK_ERR(session.sendTrackBoundary(boundary), SessionError::UnexpectedMessage);

    // After it, PREPARE_STREAM has gone out with SESSION_CONFIG, and that is
    // all either of these needs: they belong to a stream rather than to a
    // device, and a room whose driver refused is still a room this zone has
    // something to say to. What they never do is change playback.
    handshake(session);
    CHECK_OK(session.sendTrackBoundary(boundary));

    matp::TrackInfo info;
    info.trackId = "track-A";
    info.title = "First Song";
    CHECK_OK(session.sendTrackInfo(info));

    CHECK_EQ(session.trackBoundariesSent(), 1ULL);
    CHECK_EQ(session.trackInfosSent(), 1ULL);

    std::vector<matp::Message> out = drain(session);
    CHECK_EQ(out.size(), size_t(2));
    CHECK(out[0].type == matp::MessageType::TrackBoundary);
    CHECK(out[1].type == matp::MessageType::TrackInfo);

    std::string error;
    matp::TrackBoundary parsedBoundary;
    CHECK(matp::parseTrackBoundary(out[0].payload, parsedBoundary, error));
    CHECK_EQ(parsedBoundary.sessionId, testSessionId().toHex());
    CHECK_EQ(parsedBoundary.timelineNs, boundary.timelineNs);
    CHECK_EQ(parsedBoundary.trackId, std::string("track-B"));
    CHECK_EQ(parsedBoundary.title, std::string("Second Song"));

    matp::TrackInfo parsedInfo;
    CHECK(matp::parseTrackInfo(out[1].payload, parsedInfo, error));
    CHECK_EQ(parsedInfo.sessionId, testSessionId().toHex());
    CHECK_EQ(parsedInfo.trackId, std::string("track-A"));
    CHECK_EQ(parsedInfo.title, std::string("First Song"));

    // A boundary that names neither is still a boundary; a TRACK_INFO that
    // names neither is nothing at all.
    matp::TrackBoundary bare;
    bare.timelineNs = 200000000000ULL;
    CHECK_OK(session.sendTrackBoundary(bare));
    CHECK_ERR(session.sendTrackInfo(matp::TrackInfo{}), SessionError::MalformedMessage);
    CHECK(session.state() == SessionState::Configured);
    CHECK_EQ(drain(session).size(), size_t(1));
}

namespace {

// The USB DAC with a trim the client asks to be driven by, and the feedback
// that drives it.
matp::CapabilityReport trimmedDac(matp::DriftCorrectionMode mode)
{
    matp::CapabilityReport report = usbDac();
    report.driftFeedback = true;
    report.driftCorrection = mode;
    report.hasHardwareClock = true;
    report.hardwareClock.available = true;
    report.hardwareClock.control = "Drift compensator";
    report.hardwareClock.minPpm = -500;
    report.hardwareClock.maxPpm = 500;
    return report;
}

matp::StreamReady readyFor(matp::DriftCorrectionMode mode)
{
    matp::StreamReady ready;
    ready.ready = true;
    ready.format.sampleRate = 48000;
    ready.format.channels = 2;
    ready.format.sampleType = AudioFormat::SampleType::Int24In32;
    ready.hasDriftCorrection = true;
    ready.driftCorrection = mode;
    return ready;
}

const matp::Message* findType(const std::vector<matp::Message>& messages, matp::MessageType type)
{
    for (const matp::Message& message : messages)
        if (message.type == type)
            return &message;
    return nullptr;
}

} // namespace

// Auto is the hardware clock where the device has a verified trim, and
// resampling where it has none; SESSION_CONFIG names the actuator, never auto.
MULTIROOM_TEST(session_resolves_auto_drift_correction_by_the_hardware_clock)
{
    multiroomtest::ManualClock clock;
    ClientSession trimmed(testSessionId(), clock);
    feed(trimmed, frameOf(matp::MessageType::Hello, matp::makeHello(clientIdentity())));
    feed(trimmed, frameOf(matp::MessageType::AudioCapabilities,
                          matp::makeAudioCapabilities(trimmedDac(matp::DriftCorrectionMode::Auto))));
    const std::vector<matp::Message> sent = drain(trimmed);
    const matp::Message* config = findType(sent, matp::MessageType::SessionConfig);
    CHECK(config != nullptr);
    matp::SessionConfig parsed;
    std::string error;
    CHECK(matp::parseSessionConfig(config->payload, parsed, error));
    CHECK(parsed.hasDriftCorrection);
    CHECK(parsed.driftCorrection == matp::DriftCorrectionMode::HardwareClock);
    CHECK(trimmed.capabilities().hardwareClock.available);

    matp::CapabilityReport plain = trimmedDac(matp::DriftCorrectionMode::Auto);
    plain.hasHardwareClock = false;
    plain.hardwareClock = matp::HardwareClockCapability();
    ClientSession untrimmed(testSessionId(), clock);
    feed(untrimmed, frameOf(matp::MessageType::Hello, matp::makeHello(clientIdentity())));
    feed(untrimmed, frameOf(matp::MessageType::AudioCapabilities, matp::makeAudioCapabilities(plain)));
    const std::vector<matp::Message> sentPlain = drain(untrimmed);
    const matp::Message* configPlain = findType(sentPlain, matp::MessageType::SessionConfig);
    CHECK(configPlain != nullptr);
    CHECK(matp::parseSessionConfig(configPlain->payload, parsed, error));
    CHECK(parsed.hasDriftCorrection);
    CHECK(parsed.driftCorrection == matp::DriftCorrectionMode::Resampling);
    CHECK(!untrimmed.capabilities().hardwareClock.available);
}

MULTIROOM_TEST(session_selects_the_hardware_clock_only_where_the_client_asks_and_confirms)
{
    multiroomtest::ManualClock clock;
    ClientSession session(testSessionId(), clock);
    feed(session, frameOf(matp::MessageType::Hello, matp::makeHello(clientIdentity())));
    feed(session, frameOf(matp::MessageType::AudioCapabilities,
                          matp::makeAudioCapabilities(
                              trimmedDac(matp::DriftCorrectionMode::HardwareClock))));

    std::vector<matp::Message> sent = drain(session);
    const matp::Message* config = findType(sent, matp::MessageType::SessionConfig);
    CHECK(config != nullptr);
    matp::SessionConfig parsed;
    std::string error;
    CHECK(matp::parseSessionConfig(config->payload, parsed, error));
    CHECK(parsed.hasDriftCorrection);
    CHECK(parsed.driftCorrection == matp::DriftCorrectionMode::HardwareClock);
    CHECK(session.capabilities().hardwareClock.available);

    // The device opened without confirming its trim: planned as resampled.
    feed(session, frameOf(matp::MessageType::StreamReady,
                          matp::makeStreamReady(testSessionId().toHex(),
                                                readyFor(matp::DriftCorrectionMode::Resampling))));
    CHECK(!session.capabilities().hardwareClock.available);

    // Confirmed on the next stream, and fed a target from then on.
    CHECK_OK(session.renegotiate());
    drain(session);
    feed(session, frameOf(matp::MessageType::StreamReady,
                          matp::makeStreamReady(testSessionId().toHex(),
                                                readyFor(matp::DriftCorrectionMode::HardwareClock))));
    CHECK(session.capabilities().hardwareClock.available);

    matp::DriftFeedback feedback;
    feedback.hasDrift = true;
    feedback.measuredDriftPpm = 0.5;
    feedback.driftConfidence = 0.9;
    feedback.hasHardwareTarget = true;
    feedback.hardwareTargetPpm = 41.5;
    CHECK_OK(session.sendDriftFeedback(feedback));
    CHECK_OK(session.sendDriftFeedback(feedback));
    sent = drain(session);
    CHECK_EQ(sent.size(), size_t(2));
    matp::DriftFeedback first;
    matp::DriftFeedback second;
    CHECK(matp::parseDriftFeedback(sent[0].payload, first, error));
    CHECK(matp::parseDriftFeedback(sent[1].payload, second, error));
    CHECK_EQ(first.sessionId, testSessionId().toHex());
    CHECK(second.sequence > first.sequence);
    CHECK(first.hasHardwareTarget);
    CHECK_EQ(first.hardwareTargetPpm, 41.5);

    // A write the client could not make: the session resamples from now on.
    matp::PlaybackReport report;
    report.localMonotonicTimeNs = 5000000000ULL;
    report.deviceFramePosition = 441000;
    report.clockQuality = AudioClockQuality::Hardware;
    report.hasHardwareClock = true;
    report.hardwareClockError = "i2c write failed";
    feed(session, frameOf(matp::MessageType::PlaybackReport,
                          matp::makePlaybackReport(testSessionId().toHex(), report)));
    sent = drain(session);
    config = findType(sent, matp::MessageType::SessionConfig);
    CHECK(config != nullptr);
    CHECK(matp::parseSessionConfig(config->payload, parsed, error));
    CHECK(parsed.driftCorrection == matp::DriftCorrectionMode::Resampling);
    CHECK(!session.capabilities().hardwareClock.available);

    // Until the setting is asked for again.
    CHECK_OK(session.setDriftCorrection(matp::DriftCorrectionMode::HardwareClock));
    sent = drain(session);
    CHECK(findType(sent, matp::MessageType::SetDriftCorrection) != nullptr);
}

MULTIROOM_TEST(session_sends_drift_feedback_only_to_a_client_that_reads_it)
{
    multiroomtest::ManualClock clock;

    // An older client: no selection in SESSION_CONFIG, and no feedback.
    ClientSession old(testSessionId(), clock);
    handshake(old);
    CHECK(!old.driftFeedback());
    matp::DriftFeedback feedback;
    feedback.hasHardwareTarget = true;
    feedback.hardwareTargetPpm = 10.0;
    CHECK_OK(old.sendDriftFeedback(feedback));
    CHECK_EQ(drain(old).size(), size_t(0));
    CHECK_ERR(old.setDriftCorrection(matp::DriftCorrectionMode::HardwareClock),
              SessionError::UnexpectedMessage);

    // A resampled client that reads it gets the measurement and never a target.
    ClientSession resampled(testSessionId(), clock);
    feed(resampled, frameOf(matp::MessageType::Hello, matp::makeHello(clientIdentity())));
    feed(resampled, frameOf(matp::MessageType::AudioCapabilities,
                            matp::makeAudioCapabilities(
                                trimmedDac(matp::DriftCorrectionMode::Resampling))));
    std::vector<matp::Message> sent = drain(resampled);
    const matp::Message* config = findType(sent, matp::MessageType::SessionConfig);
    CHECK(config != nullptr);
    matp::SessionConfig parsed;
    std::string error;
    CHECK(matp::parseSessionConfig(config->payload, parsed, error));
    CHECK(parsed.hasDriftCorrection);
    CHECK(parsed.driftCorrection == matp::DriftCorrectionMode::Resampling);

    feedback.hasDrift = true;
    feedback.measuredDriftPpm = 12.0;
    feedback.driftConfidence = 0.8;
    CHECK_OK(resampled.sendDriftFeedback(feedback));
    sent = drain(resampled);
    CHECK_EQ(sent.size(), size_t(1));
    matp::DriftFeedback parsedFeedback;
    CHECK(matp::parseDriftFeedback(sent[0].payload, parsedFeedback, error));
    CHECK(parsedFeedback.hasDrift);
    CHECK(!parsedFeedback.hasHardwareTarget);
}
