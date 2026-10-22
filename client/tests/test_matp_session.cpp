#include "TestListener.h"
#include "TestSupport.h"

#include "matp/Session.h"
#include "util/Time.h"

#include <chrono>
#include <string>
#include <thread>
#include <vector>

using namespace multiroom;
using multiroomtest::TestListener;
using multiroomtest::waitUntil;

namespace {

matp::Identity testIdentity(const char* clientId = "livingroom")
{
    matp::Identity identity;
    identity.clientId = clientId;
    identity.clientVersion = "0.1.0";
    identity.platform = "linux-armv6";
    return identity;
}

AudioFormat format(uint32_t rate, uint16_t channels, AudioFormat::SampleType type)
{
    AudioFormat result;
    result.sampleRate = rate;
    result.channels = channels;
    result.sampleType = type;
    return result;
}

matp::CapabilityReport testReport()
{
    matp::CapabilityReport report;
    report.deviceId = "device-1";
    report.deviceName = "Test DAC";
    report.preferredSampleRate = 48000;
    report.formats.push_back(format(48000, 2, AudioFormat::SampleType::Int24In32));
    report.formats.push_back(format(44100, 2, AudioFormat::SampleType::Int16));
    return report;
}

json::Value helloAck(bool withVersion, uint32_t version)
{
    json::Value payload = json::Value::makeObject();
    payload["type"] = json::Value("helloAck");
    if (withVersion)
        payload["protocolVersion"] = json::Value(version);
    return payload;
}

json::Value sessionConfig(const char* sessionId, uint32_t sampleRate, const char* sampleFormat)
{
    json::Value formatValue = json::Value::makeObject();
    formatValue["sampleRate"] = json::Value(sampleRate);
    formatValue["channels"] = json::Value(static_cast<uint32_t>(2));
    formatValue["sampleFormat"] = json::Value(sampleFormat);

    json::Value payload = json::Value::makeObject();
    payload["type"] = json::Value("sessionConfig");
    payload["sessionId"] = json::Value(sessionId);
    payload["format"] = formatValue;
    return payload;
}

// The server's side of the connection: it decodes what the client sends and
// answers it, polling the session in between exactly the way the client's main
// loop does. Nothing here sleeps through a fixed delay.
class Peer
{
public:
    Peer(matp::Session& session, TestListener& listener) : session_(session), listener_(listener)
    {
    }

    // A clock sync request is answered where it is decoded, from this
    // machine's own clock, and never handed to the caller: an established
    // session sends them continuously, and no test but the clock ones cares.
    // Turn it off to inspect the exchange itself.
    void setAnswersClockSync(bool answers) { answersClockSync_ = answers; }
    uint32_t clockSyncRequests() const { return clockSyncRequests_; }
    uint32_t clockSyncResults() const { return clockSyncResults_; }

    // The server clock jumps by `ns` from the next reply on, as a restarted
    // server or a resumed client would make it.
    void stepServerClock(int64_t ns) { serverStepNs_ += ns; }

    // Moves the session's schedule on without waiting for it: every poll from
    // here on passes a time that much later. The exchanges themselves still
    // measure the real clock.
    void skipAhead(uint64_t ns) { scheduleShiftNs_ += ns; }

    bool next(matp::Message& out, int timeoutMs)
    {
        for (int waited = 0; waited <= timeoutMs; waited += 20)
        {
            step();
            if (take(out))
                return true;

            const std::string bytes = listener_.receive(20);
            if (!bytes.empty())
                reader_.append(bytes.data(), bytes.size());
        }
        step();
        return take(out);
    }

    // Drives the session without expecting anything back, for the cases where
    // silence is the thing under test. Anything that did arrive is kept for
    // the next() that follows rather than swallowed.
    void pump(int milliseconds)
    {
        for (int waited = 0; waited < milliseconds; waited += 20)
        {
            step();
            const std::string bytes = listener_.receive(20);
            if (!bytes.empty())
                reader_.append(bytes.data(), bytes.size());
        }
        step();
    }

    bool send(matp::MessageType type, const json::Value& payload)
    {
        return listener_.send(matp::encode(type, payload));
    }

    bool sendRaw(const std::string& bytes) { return listener_.send(bytes); }

private:
    // Polls the session and decodes everything that has arrived, answering the
    // clock sync requests and queueing the rest.
    void step()
    {
        session_.poll(monotonicNowNs() + scheduleShiftNs_);

        // Whatever that poll just sent is taken off the socket here rather
        // than a loop turn later. A clock exchange measures each leg, and a
        // peer that only looks every twenty milliseconds puts all of the
        // delay on the inbound one: the offsets then jump by half of it,
        // which is more than the model accepts, and a session that is also
        // sending buffer status never establishes a mapping at all.
        for (;;)
        {
            const std::string ready = listener_.receive(0);
            if (ready.empty())
                break;
            reader_.append(ready.data(), ready.size());
        }

        matp::Message message;
        std::string error;
        while (reader_.next(message, error) == matp::FrameReader::Result::Message)
        {
            if (message.type == matp::MessageType::ClockSyncRequest)
            {
                ++clockSyncRequests_;
                if (answersClockSync_)
                {
                    answerClockSync(message);
                    continue;
                }
            }
            else if (message.type == matp::MessageType::ClockSyncResult)
            {
                // The four timestamps coming back (section 8), one per reply
                // the client accepted. Swallowed for the same reason the
                // request is: every established session sends them, and only
                // the clock tests are about them.
                ++clockSyncResults_;
                if (answersClockSync_)
                    continue;
            }
            pending_.push_back(message);
        }
    }

    void answerClockSync(const matp::Message& request)
    {
        // S2 is taken where a server would take it: the moment the request was
        // decoded, not the moment the answer goes out.
        const uint64_t receivedNs = monotonicNowNs();

        int64_t clientSendNs = 0;
        json::getInt(request.payload, "clientSendNs", clientSendNs);

        json::Value payload = json::Value::makeObject();
        payload["type"] = json::Value("clockSyncReply");
        payload["clientSendNs"] = json::Value(static_cast<uint64_t>(clientSendNs));
        payload["serverReceiveNs"] = json::Value(static_cast<uint64_t>(static_cast<int64_t>(receivedNs) + serverStepNs_));
        payload["serverSendNs"] = json::Value(static_cast<uint64_t>(static_cast<int64_t>(monotonicNowNs()) + serverStepNs_));
        send(matp::MessageType::ClockSyncReply, payload);
    }

    bool take(matp::Message& out)
    {
        if (pending_.empty())
            return false;
        out = pending_.front();
        pending_.erase(pending_.begin());
        return true;
    }

    matp::Session& session_;
    TestListener& listener_;
    matp::FrameReader reader_;
    std::vector<matp::Message> pending_;
    bool answersClockSync_ = true;
    uint32_t clockSyncRequests_ = 0;
    uint32_t clockSyncResults_ = 0;
    int64_t serverStepNs_ = 0;
    uint64_t scheduleShiftNs_ = 0;
};

json::Value prepareStream(const char* sessionId, bool withAudioPort, uint32_t audioPort)
{
    json::Value payload = json::Value::makeObject();
    payload["type"] = json::Value("prepareStream");
    if (sessionId != nullptr)
        payload["sessionId"] = json::Value(sessionId);
    if (withAudioPort)
        payload["audioPort"] = json::Value(audioPort);
    return payload;
}

// A stand-in for the layer that owns the device and the ring buffer: it
// records what PREPARE_STREAM asked for and answers with whatever the test
// wants the driver to have granted. No sound card is involved, which is the
// point of the hooks being hooks.
struct StreamStub
{
    bool ready = true;
    AudioFormat granted = format(48000, 2, AudioFormat::SampleType::Int24In32);
    uint32_t periodFrames = 480;
    std::string reason = "the device is in use by another application";

    uint32_t prepares = 0;
    uint32_t releases = 0;
    matp::PrepareStream lastRequest;
    AudioFormat lastNegotiated;

    bool hasBuffer = true;
    matp::BufferStatus buffer;

    // What the playback layer would say about the sound card. No device is
    // involved here either, so the report is whatever the test sets.
    bool hasPlayback = true;
    matp::PlaybackReport playback;

    // What the playback layer would say about a scheduled start. The session
    // never touches a device, so this is all there is to it here.
    bool startAccepted = true;
    std::string startRefusal = "the audio device is not running";
    uint32_t starts = 0;
    matp::StartAt lastStart;
    uint64_t lastStartLocalNs = 0;

    // The rest of the transport (sections 37 to 39) and the track metadata of
    // section 23. Like the start, none of it touches a device here.
    bool transportAccepted = true;
    std::string transportRefusal = "the audio device is not running";
    uint32_t pauses = 0;
    uint32_t stops = 0;
    uint32_t flushes = 0;
    matp::TransportAt lastTransport;

    bool volumeAccepted = true;
    uint32_t volumes = 0;
    matp::SetVolume lastVolume;

    uint32_t boundaries = 0;
    matp::TrackBoundary lastBoundary;

    uint32_t infos = 0;
    matp::TrackInfo lastInfo;

    matp::Session::StreamHooks hooks()
    {
        matp::Session::StreamHooks result;
        result.prepare = [this](const matp::PrepareStream& request,
                                const AudioFormat& negotiated, matp::StreamReady& out) {
            ++prepares;
            lastRequest = request;
            lastNegotiated = negotiated;
            out.ready = ready;
            if (ready)
            {
                out.format = granted;
                out.periodFrames = periodFrames;
            }
            else
            {
                out.reason = reason;
            }
        };
        result.release = [this]() { ++releases; };
        result.start = [this](const matp::StartAt& request, uint64_t localStartNs,
                              std::string& error) {
            lastStart = request;
            lastStartLocalNs = localStartNs;
            if (!startAccepted)
            {
                error = startRefusal;
                return false;
            }
            ++starts;
            return true;
        };
        result.bufferStatus = [this](matp::BufferStatus& out) {
            if (!hasBuffer)
                return false;
            out = buffer;
            return true;
        };
        result.playbackReport = [this](matp::PlaybackReport& out) {
            if (!hasPlayback)
                return false;
            out = playback;
            return true;
        };
        result.pause = [this](const matp::TransportAt& request, std::string& error) {
            lastTransport = request;
            if (!transportAccepted)
            {
                error = transportRefusal;
                return false;
            }
            ++pauses;
            return true;
        };
        result.stop = [this](const matp::TransportAt& request, std::string& error) {
            lastTransport = request;
            if (!transportAccepted)
            {
                error = transportRefusal;
                return false;
            }
            ++stops;
            return true;
        };
        result.flush = [this](const matp::TransportAt& request, std::string& error) {
            lastTransport = request;
            if (!transportAccepted)
            {
                error = transportRefusal;
                return false;
            }
            ++flushes;
            return true;
        };
        result.volume = [this](const matp::SetVolume& request, std::string& error) {
            lastVolume = request;
            if (!volumeAccepted)
            {
                error = "the audio device is not running";
                return false;
            }
            ++volumes;
            return true;
        };
        result.trackBoundary = [this](const matp::TrackBoundary& boundary) {
            ++boundaries;
            lastBoundary = boundary;
        };
        result.trackInfo = [this](const matp::TrackInfo& info) {
            ++infos;
            lastInfo = info;
        };
        return result;
    }
};

// Everything a handshake test needs, wired the way main() wires it.
struct Fixture
{
    explicit Fixture(const matp::Identity& identity)
        : report(testReport()), session(connection, identity,
                                        [this](matp::CapabilityReport& out, std::string& error) {
                                            if (!reportAvailable)
                                            {
                                                error = "no usable audio playback device";
                                                return false;
                                            }
                                            out = report;
                                            return true;
                                        })
    {
    }

    bool start(TestListener& listener)
    {
        std::string error;
        if (!connection.setServer(listener.address(), error))
            return false;
        return connection.start(
            [this](const void* data, size_t size) { session.onData(data, size); }, error);
    }

    net::Library library;
    ServerConnection connection;
    matp::CapabilityReport report;
    bool reportAvailable = true;
    matp::Session session;
};

} // namespace

MULTIROOM_TEST(matp_session_handshakes_and_records_the_stream_format)
{
    TestListener listener;
    CHECK(listener.ready());

    Fixture fixture(testIdentity());
    CHECK(fixture.start(listener));
    CHECK(listener.acceptOne(2000));

    Peer peer(fixture.session, listener);

    matp::Message message;
    CHECK(peer.next(message, 3000));
    CHECK(message.type == matp::MessageType::Hello);

    std::string text;
    CHECK(json::getString(message.payload, "clientId", text));
    CHECK_EQ(text, std::string("livingroom"));
    CHECK(fixture.session.state() == matp::SessionState::HelloSent);

    CHECK(peer.send(matp::MessageType::HelloAck, helloAck(true, matp::kProtocolVersion)));

    // Mandatory, and immediately after the acknowledgement: the server may not
    // send audio before it knows what this client can play (section 12).
    CHECK(peer.next(message, 3000));
    CHECK(message.type == matp::MessageType::AudioCapabilities);
    CHECK(json::getString(message.payload, "deviceId", text));
    CHECK_EQ(text, std::string("device-1"));
    const json::Value* formats = message.payload.find("formats");
    CHECK(formats != nullptr && formats->isArray());
    CHECK_EQ(formats->size(), static_cast<size_t>(2));
    CHECK(fixture.session.state() == matp::SessionState::CapabilitiesSent);

    CHECK(peer.send(matp::MessageType::SessionConfig, sessionConfig("s-42", 48000, "s24le")));

    CHECK(waitUntil(
        [&] {
            fixture.session.poll(monotonicNowNs());
            return fixture.session.state() == matp::SessionState::Established;
        },
        3000));

    CHECK_EQ(fixture.session.sessionId(), std::string("s-42"));
    CHECK(fixture.session.hasStreamFormat());
    CHECK_EQ(fixture.session.streamFormat().sampleRate, static_cast<uint32_t>(48000));
    CHECK(fixture.session.streamFormat().sampleType == AudioFormat::SampleType::Int24In32);
    CHECK(fixture.session.streamFormatPlayable());
    CHECK_EQ(fixture.session.handshakes(), static_cast<uint32_t>(1));
    CHECK_EQ(fixture.session.lastError(), std::string());
}

MULTIROOM_TEST(matp_session_records_a_stream_format_the_device_did_not_report)
{
    TestListener listener;
    CHECK(listener.ready());

    Fixture fixture(testIdentity());
    CHECK(fixture.start(listener));
    CHECK(listener.acceptOne(2000));

    Peer peer(fixture.session, listener);
    matp::Message message;
    CHECK(peer.next(message, 3000));
    CHECK(peer.send(matp::MessageType::HelloAck, helloAck(false, 0)));
    CHECK(peer.next(message, 3000));
    CHECK(message.type == matp::MessageType::AudioCapabilities);

    // A rate the device never accepted. The client records the session and
    // says so rather than substituting a format of its own -- the wrong 24-bit
    // layout is noise, not a quiet defect.
    CHECK(peer.send(matp::MessageType::SessionConfig, sessionConfig("s-1", 96000, "s32le")));

    CHECK(waitUntil(
        [&] {
            fixture.session.poll(monotonicNowNs());
            return fixture.session.state() == matp::SessionState::Established;
        },
        3000));

    CHECK(fixture.session.hasStreamFormat());
    CHECK(!fixture.session.streamFormatPlayable());
}

MULTIROOM_TEST(matp_session_reports_capabilities_again_only_when_they_changed)
{
    TestListener listener;
    CHECK(listener.ready());

    Fixture fixture(testIdentity());
    CHECK(fixture.start(listener));
    CHECK(listener.acceptOne(2000));

    Peer peer(fixture.session, listener);
    matp::Message message;
    CHECK(peer.next(message, 3000));
    CHECK(peer.send(matp::MessageType::HelloAck, helloAck(false, 0)));
    CHECK(peer.next(message, 3000));
    CHECK(peer.send(matp::MessageType::SessionConfig, sessionConfig("s-1", 48000, "s24le")));
    CHECK(waitUntil(
        [&] {
            fixture.session.poll(monotonicNowNs());
            return fixture.session.state() == matp::SessionState::Established;
        },
        3000));

    // A rescan that landed on the same device is not a renegotiation.
    fixture.session.capabilitiesChanged();
    peer.pump(200);
    CHECK(!peer.next(message, 100));

    // A different device is.
    fixture.report.deviceId = "device-2";
    fixture.report.deviceName = "Other DAC";
    fixture.report.formats.clear();
    fixture.report.formats.push_back(format(44100, 2, AudioFormat::SampleType::Int16));
    fixture.session.capabilitiesChanged();

    CHECK(peer.next(message, 3000));
    CHECK(message.type == matp::MessageType::AudioCapabilities);
    std::string text;
    CHECK(json::getString(message.payload, "deviceId", text));
    CHECK_EQ(text, std::string("device-2"));

    // The stream the server chose was chosen from the old list, so it stands
    // until the server answers -- but it is no longer playable, and the client
    // must not claim otherwise.
    CHECK(fixture.session.state() == matp::SessionState::Established);
    CHECK(!fixture.session.streamFormatPlayable());

    CHECK(peer.send(matp::MessageType::SessionConfig, sessionConfig("s-2", 44100, "s16le")));
    CHECK(waitUntil(
        [&] {
            fixture.session.poll(monotonicNowNs());
            return fixture.session.sessionId() == "s-2";
        },
        3000));
    CHECK(fixture.session.streamFormatPlayable());
    CHECK_EQ(fixture.session.handshakes(), static_cast<uint32_t>(2));
}

MULTIROOM_TEST(matp_session_drops_a_connection_it_can_no_longer_follow)
{
    TestListener listener;
    CHECK(listener.ready());

    Fixture fixture(testIdentity());
    CHECK(fixture.start(listener));
    CHECK(listener.acceptOne(2000));

    Peer peer(fixture.session, listener);
    matp::Message message;
    CHECK(peer.next(message, 3000));

    // A length prefix that cannot be believed. Reading on would take the next
    // length out of the middle of a message, so the connection is the thing
    // that has to go.
    std::string broken("\x00\x00\x00\x01\x00\x02", 6);
    CHECK(peer.sendRaw(broken));

    CHECK(waitUntil(
        [&] {
            fixture.session.poll(monotonicNowNs());
            return fixture.session.state() == matp::SessionState::Failed;
        },
        3000));
    CHECK(!fixture.session.lastError().empty());

    // And the client comes back on a new connection rather than sitting on a
    // stream it cannot read.
    CHECK(listener.acceptOne(5000));
    CHECK(peer.next(message, 3000));
    CHECK(message.type == matp::MessageType::Hello);
}

MULTIROOM_TEST(matp_session_drops_a_server_that_outruns_the_owner_thread)
{
    TestListener listener;
    CHECK(listener.ready());

    Fixture fixture(testIdentity());
    CHECK(fixture.start(listener));
    CHECK(listener.acceptOne(2000));

    Peer peer(fixture.session, listener);
    matp::Message message;
    CHECK(peer.next(message, 3000));

    // Six maximum-size frames' worth of bytes with nobody polling: without a
    // limit the inbox holds all of it, at whatever rate the server can send.
    std::string frame("\x00\x10\x00\x00\x00\x01", 6);
    frame.resize(4 + matp::kMaxMessageBytes, 'x');
    for (int i = 0; i < 6; ++i)
        CHECK(peer.sendRaw(frame));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    fixture.session.poll(monotonicNowNs());
    CHECK(fixture.session.state() == matp::SessionState::Failed);
    CHECK(fixture.session.lastError().find("between two polls") != std::string::npos);

    CHECK(listener.acceptOne(5000));
    CHECK(peer.next(message, 3000));
    CHECK(message.type == matp::MessageType::Hello);
}

MULTIROOM_TEST(matp_session_gives_up_on_a_server_that_never_finishes_the_handshake)
{
    TestListener listener;
    CHECK(listener.ready());

    Fixture fixture(testIdentity());
    CHECK(fixture.start(listener));
    CHECK(listener.acceptOne(2000));

    Peer peer(fixture.session, listener);
    matp::Message message;
    CHECK(peer.next(message, 3000));
    CHECK(message.type == matp::MessageType::Hello);

    // poll() takes the time rather than reading the clock, so the timeout can
    // be walked without waiting it out.
    fixture.session.poll(monotonicNowNs() + 11ULL * 1000000000ULL);
    CHECK(fixture.session.state() == matp::SessionState::Failed);
    CHECK(!fixture.session.lastError().empty());
}

MULTIROOM_TEST(matp_session_refuses_a_server_that_speaks_another_protocol_version)
{
    TestListener listener;
    CHECK(listener.ready());

    Fixture fixture(testIdentity());
    CHECK(fixture.start(listener));
    CHECK(listener.acceptOne(2000));

    Peer peer(fixture.session, listener);
    matp::Message message;
    CHECK(peer.next(message, 3000));
    // Version 1 expressed every position as a canonical frame and is
    // withdrawn (section 51): the two disagree about what every number on the
    // wire means, so the session ends rather than being interpreted.
    CHECK(peer.send(matp::MessageType::HelloAck, helloAck(true, 1)));

    CHECK(waitUntil(
        [&] {
            fixture.session.poll(monotonicNowNs());
            return fixture.session.state() == matp::SessionState::Failed;
        },
        3000));
}

MULTIROOM_TEST(matp_session_needs_an_id_and_a_device_before_it_says_hello)
{
    TestListener listener;
    CHECK(listener.ready());

    // No client id and no host name to fall back on. Connecting as a new room
    // on every start is worse than not connecting at all, so nothing is sent.
    Fixture fixture(testIdentity(""));
    CHECK(fixture.start(listener));
    CHECK(listener.acceptOne(2000));

    Peer peer(fixture.session, listener);
    matp::Message message;
    CHECK(!peer.next(message, 500));
    CHECK(fixture.session.state() == matp::SessionState::Failed);
    CHECK(!fixture.session.lastError().empty());
}

MULTIROOM_TEST(matp_session_without_a_device_reports_no_formats_and_stays_up)
{
    TestListener listener;
    CHECK(listener.ready());

    Fixture fixture(testIdentity());
    fixture.reportAvailable = false;
    CHECK(fixture.start(listener));
    CHECK(listener.acceptOne(2000));

    Peer peer(fixture.session, listener);
    matp::Message message;
    CHECK(peer.next(message, 3000));
    CHECK(message.type == matp::MessageType::Hello);
    CHECK(peer.send(matp::MessageType::HelloAck, helloAck(true, matp::kProtocolVersion)));

    // Still sent, with an empty list, so the server sees the room and can ask
    // it to rescan or switch device.
    CHECK(peer.next(message, 3000));
    CHECK(message.type == matp::MessageType::AudioCapabilities);
    matp::CapabilityReport report;
    std::string error;
    CHECK(matp::parseAudioCapabilities(message.payload, report, error));
    CHECK(report.formats.empty());

    // A SESSION_CONFIG with no format establishes the session without a stream.
    json::Value config = json::Value::makeObject();
    config["type"] = json::Value("sessionConfig");
    config["sessionId"] = json::Value("s-no-device");
    CHECK(peer.send(matp::MessageType::SessionConfig, config));
    CHECK(waitUntil(
        [&] {
            fixture.session.poll(monotonicNowNs());
            return fixture.session.state() == matp::SessionState::Established;
        },
        3000));
}

// Brings a session up to Established and leaves the peer ready to answer.
static void establish(Fixture& fixture, Peer& peer, const char* sessionId)
{
    matp::Message message;
    CHECK(peer.next(message, 3000));
    CHECK(message.type == matp::MessageType::Hello);
    CHECK(peer.send(matp::MessageType::HelloAck, helloAck(true, matp::kProtocolVersion)));
    CHECK(peer.next(message, 3000));
    CHECK(message.type == matp::MessageType::AudioCapabilities);
    CHECK(peer.send(matp::MessageType::SessionConfig, sessionConfig(sessionId, 48000, "s24le")));
    CHECK(waitUntil(
        [&] {
            fixture.session.poll(monotonicNowNs());
            return fixture.session.state() == matp::SessionState::Established;
        },
        3000));
}

// C4 is the arrival of the read that completed the reply, never of an earlier
// read still waiting in the inbox. Here that earlier read is a PING landing
// while the poll that sends the clock request is busy handling SET_BUFFER --
// the connect burst's shape, where a device opening takes the same place. A
// reply stamped with the PING's arrival would say it came back before it was
// asked for, and the server drops the session for it.
MULTIROOM_TEST(matp_session_stamps_a_clock_reply_with_the_read_that_brought_it)
{
    TestListener listener;
    CHECK(listener.ready());

    Fixture fixture(testIdentity());
    uint32_t bufferMs = 1500;
    fixture.session.setBufferHooks([&]() { return bufferMs; },
                                   [&](uint32_t requested, std::string&) {
                                       listener.send(matp::encode(matp::MessageType::Ping,
                                                                  matp::makePing(1)));
                                       std::this_thread::sleep_for(std::chrono::milliseconds(100));
                                       bufferMs = requested;
                                       return true;
                                   });
    CHECK(fixture.start(listener));
    CHECK(listener.acceptOne(2000));

    Peer peer(fixture.session, listener);
    peer.setAnswersClockSync(false);
    matp::Message message;
    CHECK(peer.next(message, 3000));
    CHECK(message.type == matp::MessageType::Hello);
    CHECK(peer.send(matp::MessageType::HelloAck, helloAck(true, matp::kProtocolVersion)));
    CHECK(peer.next(message, 3000));
    CHECK(message.type == matp::MessageType::AudioCapabilities);

    // One write, one poll: the session is established, SET_BUFFER runs, and
    // the first clock request goes out after the PING has arrived.
    matp::SetBuffer request;
    request.bufferMs = 2000;
    CHECK(peer.sendRaw(matp::encode(matp::MessageType::SessionConfig,
                                    sessionConfig("s-stamp", 48000, "s24le")) +
                       matp::encode(matp::MessageType::SetBuffer, matp::makeSetBuffer(request))));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    fixture.session.poll(monotonicNowNs());
    CHECK(fixture.session.state() == matp::SessionState::Established);
    CHECK_EQ(bufferMs, static_cast<uint32_t>(2000));

    matp::FrameReader reader;
    const auto nextOf = [&](matp::MessageType type, matp::Message& out) {
        for (int waited = 0; waited < 2000; waited += 20)
        {
            std::string error;
            while (reader.next(out, error) == matp::FrameReader::Result::Message)
            {
                if (out.type == type)
                    return true;
            }
            const std::string bytes = listener.receive(20);
            reader.append(bytes.data(), bytes.size());
        }
        return false;
    };
    CHECK(nextOf(matp::MessageType::ClockSyncRequest, message));
    int64_t clientSendNs = 0;
    CHECK(json::getInt(message.payload, "clientSendNs", clientSendNs));

    json::Value reply = json::Value::makeObject();
    reply["type"] = json::Value("clockSyncReply");
    reply["clientSendNs"] = json::Value(static_cast<uint64_t>(clientSendNs));
    reply["serverReceiveNs"] = json::Value(monotonicNowNs());
    reply["serverSendNs"] = json::Value(monotonicNowNs());
    CHECK(peer.send(matp::MessageType::ClockSyncReply, reply));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    fixture.session.poll(monotonicNowNs());

    CHECK(nextOf(matp::MessageType::ClockSyncResult, message));
    matp::ClockSyncResult result;
    std::string error;
    CHECK(matp::parseClockSyncResult(message.payload, result, error));
    CHECK(result.clientReceiveNs >= result.clientSendNs);
    CHECK_EQ(result.clientSendNs, static_cast<uint64_t>(clientSendNs));
}

MULTIROOM_TEST(matp_session_maps_the_server_clock_after_the_handshake)
{
    TestListener listener;
    CHECK(listener.ready());

    Fixture fixture(testIdentity());
    CHECK(fixture.start(listener));
    CHECK(listener.acceptOne(2000));

    Peer peer(fixture.session, listener);

    // Nothing before there is a session for it to belong to (section 10).
    CHECK(!fixture.session.clock().hasEstimate());
    CHECK_EQ(fixture.session.clockSyncRequests(), static_cast<uint32_t>(0));

    establish(fixture, peer, "s-clock");

    CHECK(waitUntil(
        [&] {
            peer.pump(20);
            return fixture.session.clock().hasEstimate();
        },
        5000));

    // The peer is this process, so the two clocks are the same one: the offset
    // has to come out as nothing, and the round trip as a loopback round trip.
    const matp::ClockModel& clock = fixture.session.clock();
    CHECK_NEAR(clock.offsetNs(), 0.0, 20000000.0);
    CHECK(clock.bestRoundTripNs() < 100000000ULL);
    CHECK(clock.sampleCount() >= 4);
    CHECK(fixture.session.clockSyncReplies() >= 4);

    // A mapping is only worth having if it converts, and a conversion of the
    // present has to land near the present.
    const uint64_t now = monotonicNowNs();
    uint64_t serverNs = 0;
    CHECK(clock.serverFromLocal(now, serverNs));
    uint64_t backToLocal = 0;
    CHECK(clock.localFromServer(serverNs, backToLocal));
    CHECK_NEAR(static_cast<double>(backToLocal), static_cast<double>(now), 1.0);
}

MULTIROOM_TEST(matp_session_resynchronizes_at_once_after_a_clock_step)
{
    TestListener listener;
    CHECK(listener.ready());

    Fixture fixture(testIdentity());
    CHECK(fixture.start(listener));
    CHECK(listener.acceptOne(2000));

    Peer peer(fixture.session, listener);
    establish(fixture, peer, "s-step");

    // Through the initial burst, into the five second interval: a quiet
    // stretch without a single request.
    CHECK(waitUntil(
        [&] {
            peer.pump(20);
            return fixture.session.clockSyncReplies() >= 20;
        },
        5000));
    peer.pump(300);
    const uint32_t settled = peer.clockSyncRequests();
    peer.pump(300);
    CHECK_EQ(peer.clockSyncRequests(), settled);
    CHECK(fixture.session.clock().hasEstimate());

    // The server clock jumps by half a second. Each interval's sample is an
    // outlier until the model concedes and starts its window over.
    peer.stepServerClock(500000000LL);
    for (int interval = 0; interval < 3; ++interval)
    {
        const uint32_t replies = fixture.session.clockSyncReplies();
        peer.skipAhead(5ULL * 1000000000ULL);
        CHECK(waitUntil(
            [&] {
                peer.pump(20);
                return fixture.session.clockSyncReplies() > replies;
            },
            3000));
    }
    CHECK(!fixture.session.clock().hasEstimate());

    // Without skipping ahead again: only a burst brings the mapping back
    // within a second, the five second interval never would.
    CHECK(waitUntil(
        [&] {
            peer.pump(20);
            return fixture.session.clock().hasEstimate();
        },
        1500));
    CHECK_NEAR(fixture.session.clock().offsetNs(), 500000000.0, 20000000.0);
    CHECK(fixture.session.state() == matp::SessionState::Established);
}

MULTIROOM_TEST(matp_session_keeps_one_clock_exchange_outstanding_at_a_time)
{
    TestListener listener;
    CHECK(listener.ready());

    Fixture fixture(testIdentity());
    CHECK(fixture.start(listener));
    CHECK(listener.acceptOne(2000));

    Peer peer(fixture.session, listener);
    peer.setAnswersClockSync(false);
    establish(fixture, peer, "s-quiet");

    matp::Message message;
    CHECK(peer.next(message, 3000));
    CHECK(message.type == matp::MessageType::ClockSyncRequest);

    int64_t clientSendNs = 0;
    CHECK(json::getInt(message.payload, "clientSendNs", clientSendNs));
    CHECK(clientSendNs > 0);

    // A server that says nothing gets no second question: twenty requests in
    // flight together would measure the queue they formed, not the network.
    peer.pump(500);
    CHECK_EQ(peer.clockSyncRequests(), static_cast<uint32_t>(1));

    // A reply to an exchange that was never asked about is not a measurement
    // and not an error either -- it is ignored, and the session stays up.
    json::Value stray = json::Value::makeObject();
    stray["type"] = json::Value("clockSyncReply");
    stray["clientSendNs"] = json::Value(static_cast<uint64_t>(clientSendNs) + 1);
    stray["serverReceiveNs"] = json::Value(static_cast<uint64_t>(clientSendNs) + 100);
    stray["serverSendNs"] = json::Value(static_cast<uint64_t>(clientSendNs) + 200);
    CHECK(peer.send(matp::MessageType::ClockSyncReply, stray));
    peer.pump(300);

    CHECK(fixture.session.state() == matp::SessionState::Established);
    CHECK_EQ(fixture.session.clockSyncReplies(), static_cast<uint32_t>(0));
    CHECK(!fixture.session.clock().hasEstimate());
}

MULTIROOM_TEST(matp_session_drops_a_server_that_contradicts_its_own_clock)
{
    TestListener listener;
    CHECK(listener.ready());

    Fixture fixture(testIdentity());
    CHECK(fixture.start(listener));
    CHECK(listener.acceptOne(2000));

    Peer peer(fixture.session, listener);
    peer.setAnswersClockSync(false);
    establish(fixture, peer, "s-broken");

    matp::Message message;
    CHECK(peer.next(message, 3000));
    CHECK(message.type == matp::MessageType::ClockSyncRequest);

    int64_t clientSendNs = 0;
    CHECK(json::getInt(message.payload, "clientSendNs", clientSendNs));

    // Answered before it was asked. Every later phase schedules playback
    // against what the server says here, so this is a protocol error rather
    // than a sample to discard.
    json::Value payload = json::Value::makeObject();
    payload["type"] = json::Value("clockSyncReply");
    payload["clientSendNs"] = json::Value(static_cast<uint64_t>(clientSendNs));
    payload["serverReceiveNs"] = json::Value(static_cast<uint64_t>(clientSendNs) + 500);
    payload["serverSendNs"] = json::Value(static_cast<uint64_t>(clientSendNs) + 100);
    CHECK(peer.send(matp::MessageType::ClockSyncReply, payload));

    CHECK(waitUntil(
        [&] {
            fixture.session.poll(monotonicNowNs());
            return fixture.session.state() == matp::SessionState::Failed;
        },
        3000));
    CHECK(!fixture.session.lastError().empty());
}

// Brings a prepared stream up on an established session and leaves the answer
// to PREPARE_STREAM in `ready` for the caller to inspect.
static void prepare(Peer& peer, matp::Message& ready, bool withAudioPort = false,
                    uint32_t audioPort = 0)
{
    CHECK(peer.send(matp::MessageType::PrepareStream,
                    prepareStream(nullptr, withAudioPort, audioPort)));
    CHECK(peer.next(ready, 3000));
    CHECK(ready.type == matp::MessageType::StreamReady);
}

MULTIROOM_TEST(matp_session_parks_audio_and_prepares_again_on_automatic_wake)
{
    TestListener listener;
    CHECK(listener.ready());
    Fixture fixture(testIdentity());
    StreamStub stub;
    stub.hasBuffer = false;
    stub.hasPlayback = false;
    fixture.session.setStreamHooks(stub.hooks());
    CHECK(fixture.start(listener));
    CHECK(listener.acceptOne(2000));
    Peer peer(fixture.session, listener);
    establish(fixture, peer, "s-power");
    matp::Message message;
    prepare(peer, message, true, 4712);
    const auto released = stub.releases;
    std::string error, id;
    CHECK(fixture.session.setPower(false, error, true));
    CHECK_EQ(stub.releases, released + 1);
    CHECK_EQ(std::string(fixture.session.powerState()), std::string("standby"));
    CHECK(peer.next(message, 3000));
    CHECK(message.type == matp::MessageType::StreamReady);
    matp::StreamReady ready;
    CHECK(matp::parseStreamReady(message.payload, id, ready, error));
    CHECK(!ready.ready && ready.standby);
    CHECK_EQ(ready.powerState, std::string("standby"));
    CHECK(fixture.session.state() == matp::SessionState::Established);
    CHECK(fixture.session.setPower(false, error, true));
    CHECK_EQ(stub.releases, released + 1);
    prepare(peer, message, true, 4712);
    CHECK(matp::parseStreamReady(message.payload, id, ready, error));
    CHECK(ready.ready && !ready.standby);
    CHECK_EQ(ready.powerState, std::string("on"));
    CHECK_EQ(stub.prepares, uint32_t(2));
    CHECK_EQ(std::string(fixture.session.powerState()), std::string("on"));
}

MULTIROOM_TEST(matp_session_manual_power_off_survives_prepare_until_power_on)
{
    TestListener listener;
    CHECK(listener.ready());
    Fixture fixture(testIdentity());
    StreamStub stub;
    stub.hasBuffer = false;
    stub.hasPlayback = false;
    fixture.session.setStreamHooks(stub.hooks());
    CHECK(fixture.start(listener));
    CHECK(listener.acceptOne(2000));
    Peer peer(fixture.session, listener);
    establish(fixture, peer, "s-power");
    matp::Message message;
    prepare(peer, message, true, 4712);
    std::string error, id;
    CHECK(fixture.session.setPower(false, error));
    CHECK(peer.next(message, 3000));
    CHECK(message.type == matp::MessageType::StreamReady);
    matp::StreamReady ready;
    CHECK(matp::parseStreamReady(message.payload, id, ready, error));
    CHECK(!ready.ready && !ready.standby);
    CHECK_EQ(ready.powerState, std::string("standbyManual"));
    prepare(peer, message, true, 4712);
    CHECK(matp::parseStreamReady(message.payload, id, ready, error));
    CHECK(!ready.ready && !ready.standby);
    CHECK_EQ(stub.prepares, uint32_t(1));
    CHECK_EQ(std::string(fixture.session.powerState()), std::string("standbyManual"));
    fixture.connection.stop();
    fixture.session.poll(monotonicNowNs());
    CHECK_EQ(std::string(fixture.session.powerState()), std::string("standbyManual"));
    TestListener reconnect;
    CHECK(reconnect.ready());
    CHECK(fixture.start(reconnect));
    CHECK(reconnect.acceptOne(2000));
    Peer returned(fixture.session, reconnect);
    establish(fixture, returned, "s-returned");
    prepare(returned, message, true, 4712);
    CHECK_EQ(stub.prepares, uint32_t(1));
    CHECK_EQ(std::string(fixture.session.powerState()), std::string("standbyManual"));
    CHECK(fixture.session.setPower(true, error));
    CHECK(returned.next(message, 3000));
    CHECK(matp::parseStreamReady(message.payload, id, ready, error));
    CHECK(ready.ready);
    CHECK_EQ(ready.powerState, std::string("on"));
    CHECK_EQ(stub.prepares, uint32_t(2));
    CHECK_EQ(stub.lastRequest.audioPort, uint16_t(4712));
    CHECK(fixture.session.setPower(true, error));
    CHECK_EQ(stub.prepares, uint32_t(2));
}

MULTIROOM_TEST(matp_session_announces_deep_sleep_and_reports_again_when_it_failed)
{
    TestListener listener;
    CHECK(listener.ready());
    Fixture fixture(testIdentity());
    StreamStub stub;
    stub.hasBuffer = false; // no BUFFER_CONFIG or playback reports in between
    stub.hasPlayback = false;
    fixture.session.setStreamHooks(stub.hooks());
    CHECK(!fixture.session.announceSleep()); // nothing to tell yet
    CHECK(fixture.start(listener));
    CHECK(listener.acceptOne(2000));
    Peer peer(fixture.session, listener);
    establish(fixture, peer, "s-sleep");
    matp::Message message;
    prepare(peer, message, true, 4712);
    CHECK(fixture.session.announceSleep());
    CHECK(peer.next(message, 3000));
    CHECK(message.type == matp::MessageType::StreamReady);
    matp::StreamReady ready;
    std::string error, id;
    CHECK(matp::parseStreamReady(message.payload, id, ready, error));
    CHECK(!ready.ready && !ready.standby);
    CHECK_EQ(ready.powerState, std::string("sleepButtonOnly"));
    CHECK(ready.wakeOnLan.empty());
    fixture.session.resumeAfterFailedSleep();
    CHECK(peer.next(message, 3000));
    CHECK(matp::parseStreamReady(message.payload, id, ready, error));
    CHECK(ready.ready);
    CHECK_EQ(ready.powerState, std::string("on"));
    // Over Ethernet the server can wake it, and is told the address.
    CHECK(fixture.session.announceSleep("98:5d:ad:b2:c5:66"));
    CHECK(peer.next(message, 3000));
    CHECK(matp::parseStreamReady(message.payload, id, ready, error));
    CHECK_EQ(ready.powerState, std::string("sleep"));
    CHECK_EQ(ready.wakeOnLan, std::string("98:5d:ad:b2:c5:66"));
}

MULTIROOM_TEST(matp_session_answers_prepare_stream_with_what_the_device_granted)
{
    TestListener listener;
    CHECK(listener.ready());

    Fixture fixture(testIdentity());
    StreamStub stub;
    fixture.session.setStreamHooks(stub.hooks());
    CHECK(fixture.start(listener));
    CHECK(listener.acceptOne(2000));

    Peer peer(fixture.session, listener);
    establish(fixture, peer, "s-stream");

    matp::Message ready;
    prepare(peer, ready, true, 4712);

    CHECK_EQ(stub.prepares, static_cast<uint32_t>(1));
    CHECK(stub.lastRequest.hasAudioPort);
    CHECK_EQ(stub.lastRequest.audioPort, static_cast<uint16_t>(4712));
    CHECK(stub.lastNegotiated == format(48000, 2, AudioFormat::SampleType::Int24In32));

    bool readyFlag = false;
    std::string sessionId;
    CHECK(json::getBool(ready.payload, "ready", readyFlag));
    CHECK(readyFlag);
    CHECK(json::getString(ready.payload, "sessionId", sessionId));
    CHECK_EQ(sessionId, std::string("s-stream"));

    const json::Value* formatValue = ready.payload.find("format");
    CHECK(formatValue != nullptr);
    std::string sampleFormat;
    int64_t sampleRate = 0;
    CHECK(json::getString(*formatValue, "sampleFormat", sampleFormat));
    CHECK(json::getInt(*formatValue, "sampleRate", sampleRate));
    CHECK_EQ(sampleFormat, std::string("s24le"));
    CHECK_EQ(sampleRate, static_cast<int64_t>(48000));

    int64_t periodFrames = 0;
    CHECK(json::getInt(ready.payload, "periodFrames", periodFrames));
    CHECK_EQ(periodFrames, static_cast<int64_t>(480));

    CHECK(fixture.session.streamPrepared());
}

MULTIROOM_TEST(matp_session_reports_a_granted_format_that_is_not_the_one_asked_for)
{
    TestListener listener;
    CHECK(listener.ready());

    Fixture fixture(testIdentity());
    StreamStub stub;
    // A driver that grants something other than what was asked for is the
    // normal case on both platforms, and the server has to be told which one
    // it is: it must not send audio in a format the client has not confirmed.
    stub.granted = format(48000, 2, AudioFormat::SampleType::Int16);
    fixture.session.setStreamHooks(stub.hooks());
    CHECK(fixture.start(listener));
    CHECK(listener.acceptOne(2000));

    Peer peer(fixture.session, listener);
    establish(fixture, peer, "s-granted");

    matp::Message ready;
    prepare(peer, ready);

    const json::Value* formatValue = ready.payload.find("format");
    CHECK(formatValue != nullptr);
    std::string sampleFormat;
    CHECK(json::getString(*formatValue, "sampleFormat", sampleFormat));
    CHECK_EQ(sampleFormat, std::string("s16le"));
}

MULTIROOM_TEST(matp_session_reports_a_device_it_cannot_open_and_asks_the_server_to_choose_again)
{
    TestListener listener;
    CHECK(listener.ready());

    Fixture fixture(testIdentity());
    StreamStub stub;
    stub.ready = false;
    fixture.session.setStreamHooks(stub.hooks());
    CHECK(fixture.start(listener));
    CHECK(listener.acceptOne(2000));

    Peer peer(fixture.session, listener);
    establish(fixture, peer, "s-busy");

    matp::Message ready;
    prepare(peer, ready);

    bool readyFlag = true;
    std::string reason;
    CHECK(json::getBool(ready.payload, "ready", readyFlag));
    CHECK(!readyFlag);
    CHECK(json::getString(ready.payload, "reason", reason));
    CHECK_EQ(reason, stub.reason);
    CHECK(ready.payload.find("format") == nullptr);

    // Section 12: the client does not improvise a format nobody chose. It says
    // it cannot play and reports what it can, so the server chooses again.
    matp::Message again;
    CHECK(peer.next(again, 3000));
    CHECK(again.type == matp::MessageType::AudioCapabilities);
    CHECK(!fixture.session.streamPrepared());
}

MULTIROOM_TEST(matp_session_reports_the_buffer_status_while_a_stream_is_prepared)
{
    TestListener listener;
    CHECK(listener.ready());

    Fixture fixture(testIdentity());
    StreamStub stub;
    stub.buffer.hasContent = true;
    stub.buffer.playheadTimelineNs = 180000000000ULL;
    stub.buffer.bufferedUntilTimelineNs = 181500000000ULL;
    stub.buffer.bufferedDurationNs = 1500000000ULL;
    // This room's own frames for that 1.5 s, which is a different number in
    // every room and is never sent.
    stub.buffer.streamFrames = 72001;
    fixture.session.setStreamHooks(stub.hooks());
    CHECK(fixture.start(listener));
    CHECK(listener.acceptOne(2000));

    Peer peer(fixture.session, listener);
    establish(fixture, peer, "s-buffer");

    // Nothing to report before there is a stream: section 18's readiness
    // signal belongs to the stream, not to the session.
    peer.pump(300);
    CHECK_EQ(fixture.session.bufferStatusReports(), static_cast<uint32_t>(0));

    matp::Message ready;
    prepare(peer, ready);

    matp::Message status;
    CHECK(peer.next(status, 3000));
    CHECK(status.type == matp::MessageType::BufferStatus);

    std::string sessionId;
    int64_t first = 0;
    int64_t last = 0;
    int64_t buffered = 0;
    CHECK(json::getString(status.payload, "sessionId", sessionId));
    CHECK(json::getInt(status.payload, "playheadTimelineNs", first));
    CHECK(json::getInt(status.payload, "bufferedUntilTimelineNs", last));
    CHECK(json::getInt(status.payload, "bufferedDurationNs", buffered));
    CHECK_EQ(sessionId, std::string("s-buffer"));
    CHECK_EQ(first, static_cast<int64_t>(180000000000ULL));
    CHECK_EQ(last, static_cast<int64_t>(181500000000ULL));
    CHECK_EQ(buffered, static_cast<int64_t>(1500000000ULL));
    CHECK(status.payload.find("streamFrames") == nullptr);

    // And it keeps reporting: the server decides when every room is ready, so
    // this is a measurement and not an announcement.
    CHECK(waitUntil(
        [&] {
            peer.pump(20);
            return fixture.session.bufferStatusReports() >= 3;
        },
        3000));
}

MULTIROOM_TEST(matp_session_leaves_the_timeline_out_of_a_buffer_status_with_no_audio_in_it)
{
    TestListener listener;
    CHECK(listener.ready());

    Fixture fixture(testIdentity());
    StreamStub stub; // hasContent stays false: nothing has arrived yet
    fixture.session.setStreamHooks(stub.hooks());
    CHECK(fixture.start(listener));
    CHECK(listener.acceptOne(2000));

    Peer peer(fixture.session, listener);
    establish(fixture, peer, "s-empty");

    matp::Message ready;
    prepare(peer, ready);

    matp::Message status;
    CHECK(peer.next(status, 3000));
    CHECK(status.type == matp::MessageType::BufferStatus);

    // 0 ns is a real position on the media timeline, so an empty buffer
    // cannot be reported as sitting at it.
    CHECK(status.payload.find("playheadTimelineNs") == nullptr);
    CHECK(status.payload.find("bufferedUntilTimelineNs") == nullptr);
    int64_t buffered = -1;
    CHECK(json::getInt(status.payload, "bufferedDurationNs", buffered));
    CHECK_EQ(buffered, static_cast<int64_t>(0));
}

// A prepared stream sends buffer status and playback reports side by side, so
// a test that wants one of them has to step over the other.
static bool nextOfType(Peer& peer, matp::MessageType type, matp::Message& out, int timeoutMs)
{
    for (int waited = 0; waited <= timeoutMs; waited += 100)
    {
        if (!peer.next(out, 100))
            continue;
        if (out.type == type)
            return true;
    }
    return false;
}

MULTIROOM_TEST(matp_session_reports_what_the_sound_card_played_while_a_stream_is_prepared)
{
    TestListener listener;
    CHECK(listener.ready());

    Fixture fixture(testIdentity());
    StreamStub stub;
    stub.playback.localMonotonicTimeNs = 821773812345ULL;
    stub.playback.deviceFramePosition = 23849231;
    stub.playback.clockQuality = AudioClockQuality::Hardware;
    stub.playback.hasTimelineNsAudible = true;
    stub.playback.timelineNsAudible = 195349312000ULL;
    stub.playback.bufferedDurationNs = 1470000000ULL;
    stub.playback.hasDrift = true;
    stub.playback.measuredSampleRate = 48001.44;
    stub.playback.measuredDriftPpm = 30.0;
    stub.playback.driftConfidence = 0.97;
    fixture.session.setStreamHooks(stub.hooks());
    CHECK(fixture.start(listener));
    CHECK(listener.acceptOne(2000));

    Peer peer(fixture.session, listener);
    establish(fixture, peer, "s-telemetry");

    // Nothing to report before there is a device: section 25 is about what the
    // sound card played, and there is no sound card until PREPARE_STREAM.
    peer.pump(600);
    CHECK_EQ(fixture.session.playbackReports(), static_cast<uint32_t>(0));

    matp::Message ready;
    prepare(peer, ready);

    matp::Message report;
    CHECK(nextOfType(peer, matp::MessageType::PlaybackReport, report, 3000));

    std::string sessionId;
    std::string quality;
    int64_t instant = 0;
    int64_t position = 0;
    int64_t audible = 0;
    double ppm = 0.0;
    CHECK(json::getString(report.payload, "sessionId", sessionId));
    CHECK(json::getString(report.payload, "clockQuality", quality));
    CHECK(json::getInt(report.payload, "localMonotonicTimeNs", instant));
    CHECK(json::getInt(report.payload, "deviceFramePosition", position));
    CHECK(json::getInt(report.payload, "timelineNsAudible", audible));
    CHECK(json::getDouble(report.payload, "measuredDriftPpm", ppm));
    CHECK_EQ(sessionId, std::string("s-telemetry"));
    CHECK_EQ(quality, std::string("hardware"));
    CHECK_EQ(instant, static_cast<int64_t>(821773812345ULL));
    CHECK_EQ(position, static_cast<int64_t>(23849231));
    CHECK_EQ(audible, static_cast<int64_t>(195349312000ULL));
    CHECK_NEAR(ppm, 30.0, 1e-9);

    // And it keeps reporting: the server's sync controller runs on a series,
    // not on an announcement.
    CHECK(waitUntil(
        [&] {
            peer.pump(20);
            return fixture.session.playbackReports() >= 3;
        },
        4000));
}

MULTIROOM_TEST(matp_session_sends_no_playback_report_a_client_cannot_make)
{
    TestListener listener;
    CHECK(listener.ready());

    Fixture fixture(testIdentity());
    StreamStub stub;
    stub.hasPlayback = false; // the device is not running
    fixture.session.setStreamHooks(stub.hooks());
    CHECK(fixture.start(listener));
    CHECK(listener.acceptOne(2000));

    Peer peer(fixture.session, listener);
    establish(fixture, peer, "s-silent");

    matp::Message ready;
    prepare(peer, ready);

    // A suppressed report rather than one full of zeros: a position of zero
    // is a position, and the server would resample against it.
    peer.pump(1500);
    CHECK_EQ(fixture.session.playbackReports(), static_cast<uint32_t>(0));
    CHECK(fixture.session.bufferStatusReports() > 0); // the stream itself is fine
}

MULTIROOM_TEST(matp_session_refuses_a_prepare_stream_for_another_session)
{
    TestListener listener;
    CHECK(listener.ready());

    Fixture fixture(testIdentity());
    StreamStub stub;
    fixture.session.setStreamHooks(stub.hooks());
    CHECK(fixture.start(listener));
    CHECK(listener.acceptOne(2000));

    Peer peer(fixture.session, listener);
    establish(fixture, peer, "s-mine");

    CHECK(peer.send(matp::MessageType::PrepareStream,
                    prepareStream("s-somebody-else", false, 0)));
    CHECK(waitUntil(
        [&] {
            fixture.session.poll(monotonicNowNs());
            return fixture.session.state() == matp::SessionState::Failed;
        },
        3000));
    CHECK_EQ(stub.prepares, static_cast<uint32_t>(0));
    CHECK(!fixture.session.lastError().empty());
}

MULTIROOM_TEST(matp_session_releases_the_stream_with_the_session)
{
    TestListener listener;
    CHECK(listener.ready());

    Fixture fixture(testIdentity());
    StreamStub stub;
    fixture.session.setStreamHooks(stub.hooks());
    CHECK(fixture.start(listener));
    CHECK(listener.acceptOne(2000));

    Peer peer(fixture.session, listener);
    establish(fixture, peer, "s-gone");

    matp::Message ready;
    prepare(peer, ready);
    CHECK(fixture.session.streamPrepared());

    // The audio connection is bound to the session (section 13), so a session
    // that ends takes the stream with it rather than leaving a device open on
    // a timeline nobody is feeding.
    listener.dropClient();
    CHECK(waitUntil(
        [&] {
            fixture.session.poll(monotonicNowNs());
            return !fixture.session.streamPrepared();
        },
        3000));
    CHECK(stub.releases != 0);
}

MULTIROOM_TEST(matp_session_releases_the_stream_when_it_fails)
{
    TestListener listener;
    CHECK(listener.ready());

    Fixture fixture(testIdentity());
    StreamStub stub;
    fixture.session.setStreamHooks(stub.hooks());
    CHECK(fixture.start(listener));
    CHECK(listener.acceptOne(2000));

    Peer peer(fixture.session, listener);
    establish(fixture, peer, "s-mine");

    matp::Message ready;
    prepare(peer, ready);
    CHECK(fixture.session.streamPrepared());
    const uint32_t released = stub.releases;

    // A failed session waits for the next connection, which may be a long
    // backoff away; the device must not keep playing what it buffered.
    CHECK(peer.send(matp::MessageType::PrepareStream,
                    prepareStream("s-somebody-else", false, 0)));
    CHECK(waitUntil(
        [&] {
            fixture.session.poll(monotonicNowNs());
            return fixture.session.state() == matp::SessionState::Failed;
        },
        3000));
    CHECK(!fixture.session.streamPrepared());
    CHECK_EQ(stub.releases, released + 1);
    CHECK(!fixture.session.lastError().empty());
}

// The instant a START_AT names is on the server's clock, and turning it into
// this machine's is the only part of section 20 that belongs to the session.
static void establishAndPrepare(Fixture& fixture, Peer& peer, const char* sessionId)
{
    establish(fixture, peer, sessionId);
    matp::Message ready;
    prepare(peer, ready);
    CHECK(fixture.session.streamPrepared());

    // The clock exchange the peer has been answering all along has to have
    // produced a mapping before an instant on the server's clock means
    // anything (section 10 puts it before streaming for this reason).
    CHECK(waitUntil(
        [&] {
            peer.pump(20);
            return fixture.session.clock().hasEstimate();
        },
        5000));
}

json::Value startAt(const char* sessionId, uint64_t serverTimeNs, uint64_t timelineNs)
{
    json::Value payload = json::Value::makeObject();
    payload["type"] = json::Value("startAt");
    if (sessionId != nullptr)
        payload["sessionId"] = json::Value(sessionId);
    payload["serverTimeNs"] = json::Value(serverTimeNs);
    payload["timelineNs"] = json::Value(timelineNs);
    return payload;
}

MULTIROOM_TEST(matp_session_schedules_the_start_on_this_machines_clock)
{
    TestListener listener;
    CHECK(listener.ready());

    Fixture fixture(testIdentity());
    StreamStub stub;
    fixture.session.setStreamHooks(stub.hooks());
    CHECK(fixture.start(listener));
    CHECK(listener.acceptOne(2000));

    Peer peer(fixture.session, listener);
    establishAndPrepare(fixture, peer, "s-start");

    // The peer answers the clock exchange from this machine's own clock, so
    // the mapping is close to the identity and the converted instant has to
    // land near the one that was sent.
    const uint64_t serverTimeNs = monotonicNowNs() + 500ULL * 1000000ULL;
    CHECK(peer.send(matp::MessageType::StartAt, startAt(nullptr, serverTimeNs, 180000000000ULL)));
    CHECK(waitUntil(
        [&] {
            peer.pump(20);
            return stub.starts != 0;
        },
        3000));

    CHECK_EQ(stub.lastStart.timelineNs, static_cast<uint64_t>(180000000000ULL));
    CHECK_EQ(stub.lastStart.serverTimeNs, serverTimeNs);
    const int64_t delta =
        static_cast<int64_t>(stub.lastStartLocalNs) - static_cast<int64_t>(serverTimeNs);
    CHECK(delta > -50000000 && delta < 50000000); // within 50 ms of the identity
    CHECK_EQ(fixture.session.scheduledStarts(), static_cast<uint32_t>(1));
    CHECK(fixture.session.state() == matp::SessionState::Established);
}

MULTIROOM_TEST(matp_session_refuses_a_start_for_another_session)
{
    TestListener listener;
    CHECK(listener.ready());

    Fixture fixture(testIdentity());
    StreamStub stub;
    fixture.session.setStreamHooks(stub.hooks());
    CHECK(fixture.start(listener));
    CHECK(listener.acceptOne(2000));

    Peer peer(fixture.session, listener);
    establishAndPrepare(fixture, peer, "s-mine");

    // Starting somebody else's timeline is exactly what section 13's identity
    // rules exist to prevent.
    CHECK(peer.send(matp::MessageType::StartAt,
                    startAt("s-somebody-else", monotonicNowNs() + 500000000ULL, 0)));
    CHECK(waitUntil(
        [&] {
            fixture.session.poll(monotonicNowNs());
            return fixture.session.state() == matp::SessionState::Failed;
        },
        3000));
    CHECK_EQ(stub.starts, static_cast<uint32_t>(0));
}

MULTIROOM_TEST(matp_session_keeps_the_connection_when_it_cannot_honour_a_start)
{
    TestListener listener;
    CHECK(listener.ready());

    Fixture fixture(testIdentity());
    StreamStub stub;
    stub.startAccepted = false;
    fixture.session.setStreamHooks(stub.hooks());
    CHECK(fixture.start(listener));
    CHECK(listener.acceptOne(2000));

    Peer peer(fixture.session, listener);
    establishAndPrepare(fixture, peer, "s-late");

    // A start this client cannot place is its own inability, not the server's
    // mistake: dropping the connection over it would cost the clock model and
    // the prepared device, which are what the next START_AT needs.
    CHECK(peer.send(matp::MessageType::StartAt,
                    startAt(nullptr, monotonicNowNs() + 500000000ULL, 4000)));
    CHECK(waitUntil(
        [&] {
            peer.pump(20);
            return stub.lastStart.timelineNs == 4000;
        },
        3000));
    peer.pump(200);
    CHECK_EQ(stub.starts, static_cast<uint32_t>(0));
    CHECK_EQ(fixture.session.scheduledStarts(), static_cast<uint32_t>(0));
    CHECK(fixture.session.state() == matp::SessionState::Established);
    CHECK(fixture.session.streamPrepared());
}

MULTIROOM_TEST(matp_session_does_not_start_a_stream_it_never_prepared)
{
    TestListener listener;
    CHECK(listener.ready());

    Fixture fixture(testIdentity());
    StreamStub stub;
    fixture.session.setStreamHooks(stub.hooks());
    CHECK(fixture.start(listener));
    CHECK(listener.acceptOne(2000));

    Peer peer(fixture.session, listener);
    establish(fixture, peer, "s-unprepared");

    // Section 20 has the server wait for streamReady before it schedules
    // anything. A client that never reported ready has nothing to start.
    CHECK(peer.send(matp::MessageType::StartAt,
                    startAt(nullptr, monotonicNowNs() + 500000000ULL, 100)));
    peer.pump(300);
    CHECK_EQ(stub.starts, static_cast<uint32_t>(0));
    CHECK(fixture.session.state() == matp::SessionState::Established);
}

json::Value transportAt(const char* type, const char* sessionId, uint64_t serverTimeNs,
                        uint64_t timelineNs)
{
    json::Value payload = json::Value::makeObject();
    payload["type"] = json::Value(type);
    if (sessionId != nullptr)
        payload["sessionId"] = json::Value(sessionId);
    payload["serverTimeNs"] = json::Value(serverTimeNs);
    payload["timelineNs"] = json::Value(timelineNs);
    return payload;
}

MULTIROOM_TEST(matp_session_hands_pause_and_stop_to_the_playback_layer)
{
    TestListener listener;
    CHECK(listener.ready());

    Fixture fixture(testIdentity());
    StreamStub stub;
    fixture.session.setStreamHooks(stub.hooks());
    CHECK(fixture.start(listener));
    CHECK(listener.acceptOne(2000));

    Peer peer(fixture.session, listener);
    establishAndPrepare(fixture, peer, "s-transport");

    // Sections 37 and 38: the timeline frame is what places both, so neither
    // needs the clock mapping a start does -- the room already knows where
    // that frame falls on its own axis.
    CHECK(peer.send(matp::MessageType::PauseAt,
                    transportAt("pauseAt", nullptr, 145000000000ULL, 240000000000ULL)));
    CHECK(waitUntil(
        [&] {
            peer.pump(20);
            return stub.pauses != 0;
        },
        3000));
    CHECK_EQ(stub.lastTransport.timelineNs, static_cast<uint64_t>(240000000000ULL));
    CHECK_EQ(stub.lastTransport.serverTimeNs, static_cast<uint64_t>(145000000000ULL));
    CHECK_EQ(fixture.session.scheduledPauses(), static_cast<uint32_t>(1));

    CHECK(peer.send(matp::MessageType::StopAt,
                    transportAt("stopAt", "s-transport", 150000000000ULL, 260000000000ULL)));
    CHECK(waitUntil(
        [&] {
            peer.pump(20);
            return stub.stops != 0;
        },
        3000));
    CHECK_EQ(stub.lastTransport.timelineNs, static_cast<uint64_t>(260000000000ULL));
    CHECK_EQ(fixture.session.scheduledStops(), static_cast<uint32_t>(1));
    CHECK(fixture.session.state() == matp::SessionState::Established);
}

MULTIROOM_TEST(matp_session_hands_a_flush_to_the_playback_layer)
{
    TestListener listener;
    CHECK(listener.ready());

    Fixture fixture(testIdentity());
    StreamStub stub;
    fixture.session.setStreamHooks(stub.hooks());
    CHECK(fixture.start(listener));
    CHECK(listener.acceptOne(2000));

    Peer peer(fixture.session, listener);
    establishAndPrepare(fixture, peer, "s-seek");

    // Section 54: dispatched like STOP_AT, the timeline position being the cut.
    CHECK(peer.send(matp::MessageType::Flush,
                    transportAt("flush", "s-seek", 150000000000ULL, 250000000000ULL)));
    CHECK(waitUntil(
        [&] {
            peer.pump(20);
            return stub.flushes != 0;
        },
        3000));
    CHECK_EQ(stub.lastTransport.timelineNs, static_cast<uint64_t>(250000000000ULL));
    CHECK_EQ(fixture.session.scheduledFlushes(), static_cast<uint32_t>(1));
    CHECK_EQ(stub.stops, static_cast<uint32_t>(0));

    // One without a cut is malformed, and like any malformed message it ends
    // the session.
    json::Value broken = json::Value::makeObject();
    broken["type"] = json::Value("flush");
    broken["serverTimeNs"] = json::Value(static_cast<uint64_t>(150000000000ULL));
    CHECK(peer.send(matp::MessageType::Flush, broken));
    CHECK(waitUntil(
        [&] {
            peer.pump(20);
            return fixture.session.state() == matp::SessionState::Failed;
        },
        3000));
    CHECK_EQ(stub.flushes, static_cast<uint32_t>(1));
}

MULTIROOM_TEST(matp_session_drops_a_flush_when_no_stream_is_prepared)
{
    TestListener listener;
    CHECK(listener.ready());

    Fixture fixture(testIdentity());
    StreamStub stub;
    fixture.session.setStreamHooks(stub.hooks());
    CHECK(fixture.start(listener));
    CHECK(listener.acceptOne(2000));

    Peer peer(fixture.session, listener);
    establish(fixture, peer, "s-no-stream");

    CHECK(peer.send(matp::MessageType::Flush,
                    transportAt("flush", nullptr, 150000000000ULL, 250000000000ULL)));
    peer.pump(300);
    CHECK_EQ(stub.flushes, static_cast<uint32_t>(0));
    CHECK_EQ(fixture.session.scheduledFlushes(), static_cast<uint32_t>(0));
    CHECK(fixture.session.state() == matp::SessionState::Established);
}

MULTIROOM_TEST(matp_session_does_not_drop_the_connection_over_a_pause_it_cannot_honour)
{
    TestListener listener;
    CHECK(listener.ready());

    Fixture fixture(testIdentity());
    StreamStub stub;
    stub.transportAccepted = false;
    fixture.session.setStreamHooks(stub.hooks());
    CHECK(fixture.start(listener));
    CHECK(listener.acceptOne(2000));

    Peer peer(fixture.session, listener);
    establishAndPrepare(fixture, peer, "s-refused");

    // A room that cannot pause is this client's own state, not a protocol
    // error: a reconnect would cost the clock model and the prepared device,
    // which are what the next message needs.
    CHECK(peer.send(matp::MessageType::PauseAt,
                    transportAt("pauseAt", nullptr, 145000000000ULL, 240000000000ULL)));
    peer.pump(300);
    CHECK_EQ(stub.pauses, static_cast<uint32_t>(0));
    CHECK_EQ(fixture.session.scheduledPauses(), static_cast<uint32_t>(0));
    CHECK(fixture.session.state() == matp::SessionState::Established);
    CHECK(fixture.session.streamPrepared());
}

MULTIROOM_TEST(matp_session_refuses_a_stop_for_another_session)
{
    TestListener listener;
    CHECK(listener.ready());

    Fixture fixture(testIdentity());
    StreamStub stub;
    fixture.session.setStreamHooks(stub.hooks());
    CHECK(fixture.start(listener));
    CHECK(listener.acceptOne(2000));

    Peer peer(fixture.session, listener);
    establishAndPrepare(fixture, peer, "s-mine");

    // Section 13: guessing which session a message belongs to is exactly what
    // the identity exists to prevent.
    CHECK(peer.send(matp::MessageType::StopAt,
                    transportAt("stopAt", "s-somebody-else", 150000000000ULL, 260000000000ULL)));
    CHECK(waitUntil(
        [&] {
            peer.pump(20);
            return fixture.session.state() == matp::SessionState::Failed;
        },
        3000));
    CHECK_EQ(stub.stops, static_cast<uint32_t>(0));
}

MULTIROOM_TEST(matp_session_hands_the_volume_to_the_playback_layer)
{
    TestListener listener;
    CHECK(listener.ready());

    Fixture fixture(testIdentity());
    StreamStub stub;
    fixture.session.setStreamHooks(stub.hooks());
    CHECK(fixture.start(listener));
    CHECK(listener.acceptOne(2000));

    Peer peer(fixture.session, listener);
    establishAndPrepare(fixture, peer, "s-volume");

    json::Value payload = json::Value::makeObject();
    payload["type"] = json::Value("setVolume");
    payload["value"] = json::Value(0.75);
    payload["applyAtTimelineNs"] = json::Value(static_cast<uint64_t>(142000000000ULL));
    CHECK(peer.send(matp::MessageType::SetVolume, payload));
    CHECK(waitUntil(
        [&] {
            peer.pump(20);
            return stub.volumes != 0;
        },
        3000));
    CHECK(stub.lastVolume.hasValue);
    CHECK_NEAR(stub.lastVolume.value, 0.75, 1e-12);
    CHECK(stub.lastVolume.hasApplyAtTimelineNs);
    CHECK_EQ(stub.lastVolume.applyAtTimelineNs, static_cast<uint64_t>(142000000000ULL));
    CHECK_EQ(fixture.session.volumeChanges(), static_cast<uint32_t>(1));

    // A setVolume that asks for nothing is a server bug; the session says so
    // rather than counting a change that did not happen.
    json::Value empty = json::Value::makeObject();
    empty["type"] = json::Value("setVolume");
    CHECK(peer.send(matp::MessageType::SetVolume, empty));
    CHECK(waitUntil(
        [&] {
            peer.pump(20);
            return fixture.session.state() == matp::SessionState::Failed;
        },
        3000));
    CHECK_EQ(fixture.session.volumeChanges(), static_cast<uint32_t>(1));
}

MULTIROOM_TEST(matp_session_hands_a_volume_set_in_standby_to_the_playback_layer)
{
    TestListener listener;
    CHECK(listener.ready());

    Fixture fixture(testIdentity());
    StreamStub stub;
    fixture.session.setStreamHooks(stub.hooks());
    CHECK(fixture.start(listener));
    CHECK(listener.acceptOne(2000));

    Peer peer(fixture.session, listener);
    establishAndPrepare(fixture, peer, "s-parked");
    std::string error;
    CHECK(fixture.session.setPower(false, error));

    // Standby released the stream; the playback layer keeps the change for
    // the device it opens on waking, so it is not the session's to drop.
    json::Value payload = json::Value::makeObject();
    payload["type"] = json::Value("setVolume");
    payload["sessionId"] = json::Value("s-parked");
    payload["value"] = json::Value(0.0);
    payload["muted"] = json::Value(true);
    CHECK(peer.send(matp::MessageType::SetVolume, payload));
    CHECK(waitUntil(
        [&] {
            peer.pump(20);
            return stub.volumes != 0;
        },
        3000));
    CHECK(stub.lastVolume.hasMuted && stub.lastVolume.muted);
    CHECK_NEAR(stub.lastVolume.value, 0.0, 1e-12);
    CHECK_EQ(fixture.session.volumeChanges(), static_cast<uint32_t>(1));
    CHECK(fixture.session.state() == matp::SessionState::Established);

    // One for another session still fails it, prepared stream or not.
    payload["sessionId"] = json::Value("s-other");
    CHECK(peer.send(matp::MessageType::SetVolume, payload));
    CHECK(waitUntil(
        [&] {
            peer.pump(20);
            return fixture.session.state() == matp::SessionState::Failed;
        },
        3000));
    CHECK_EQ(stub.volumes, static_cast<uint32_t>(1));
}

MULTIROOM_TEST(matp_session_treats_a_track_boundary_as_metadata_only)
{
    TestListener listener;
    CHECK(listener.ready());

    Fixture fixture(testIdentity());
    StreamStub stub;
    fixture.session.setStreamHooks(stub.hooks());
    CHECK(fixture.start(listener));
    CHECK(listener.acceptOne(2000));

    Peer peer(fixture.session, listener);
    establishAndPrepare(fixture, peer, "s-track");
    const uint32_t preparesBefore = stub.prepares;
    const uint32_t releasesBefore = stub.releases;

    json::Value payload = json::Value::makeObject();
    payload["type"] = json::Value("trackBoundary");
    payload["timelineNs"] = json::Value(static_cast<uint64_t>(180000000000ULL));
    payload["trackId"] = json::Value("track-B");
    payload["title"] = json::Value("Second Song");
    CHECK(peer.send(matp::MessageType::TrackBoundary, payload));
    CHECK(waitUntil(
        [&] {
            peer.pump(20);
            return stub.boundaries != 0;
        },
        3000));

    CHECK_EQ(stub.lastBoundary.timelineNs, static_cast<uint64_t>(180000000000ULL));
    CHECK_EQ(stub.lastBoundary.trackId, std::string("track-B"));
    CHECK_EQ(stub.lastBoundary.title, std::string("Second Song"));
    CHECK_EQ(fixture.session.trackBoundaries(), static_cast<uint32_t>(1));

    // Section 22: the stream has no gap at a boundary, so nothing may reopen
    // the device for one. The stream that was prepared is the same stream.
    CHECK_EQ(stub.prepares, preparesBefore);
    CHECK_EQ(stub.releases, releasesBefore);
    CHECK(fixture.session.streamPrepared());
    CHECK(fixture.session.state() == matp::SessionState::Established);
}

MULTIROOM_TEST(matp_session_answers_a_ping_with_the_pings_own_body)
{
    TestListener listener;
    CHECK(listener.ready());

    Fixture fixture(testIdentity());
    CHECK(fixture.start(listener));
    CHECK(listener.acceptOne(2000));

    Peer peer(fixture.session, listener);
    establish(fixture, peer, "s-ping");

    json::Value payload = json::Value::makeObject();
    payload["type"] = json::Value("ping");
    payload["seq"] = json::Value(static_cast<uint64_t>(17));
    CHECK(peer.send(matp::MessageType::Ping, payload));

    matp::Message pong;
    CHECK(peer.next(pong, 3000));
    CHECK(pong.type == matp::MessageType::Pong);
    CHECK_EQ(pong.wireType, static_cast<uint16_t>(20));

    // Whatever the server correlates on comes back to it: the section gives
    // PING no fields, so the client cannot know which one that is.
    int64_t sequence = 0;
    CHECK(json::getInt(pong.payload, "seq", sequence));
    CHECK_EQ(sequence, static_cast<int64_t>(17));
    CHECK_EQ(fixture.session.pongs(), static_cast<uint32_t>(1));
    CHECK(fixture.session.state() == matp::SessionState::Established);
}

MULTIROOM_TEST(matp_session_hands_track_info_to_the_playback_layer)
{
    TestListener listener;
    CHECK(listener.ready());

    Fixture fixture(testIdentity());
    StreamStub stub;
    fixture.session.setStreamHooks(stub.hooks());
    CHECK(fixture.start(listener));
    CHECK(listener.acceptOne(2000));

    Peer peer(fixture.session, listener);
    establishAndPrepare(fixture, peer, "s-info");
    const uint32_t preparesBefore = stub.prepares;

    json::Value payload = json::Value::makeObject();
    payload["type"] = json::Value("trackInfo");
    payload["trackId"] = json::Value("track-A");
    payload["title"] = json::Value("First Song");
    CHECK(peer.send(matp::MessageType::TrackInfo, payload));
    CHECK(waitUntil(
        [&] {
            peer.pump(20);
            return stub.infos != 0;
        },
        3000));

    CHECK_EQ(stub.lastInfo.trackId, std::string("track-A"));
    CHECK_EQ(stub.lastInfo.title, std::string("First Song"));
    CHECK_EQ(fixture.session.trackInfos(), static_cast<uint32_t>(1));

    // Metadata, like a boundary: nothing about the device or the stream may
    // change because the room was told what it is playing (section 22).
    CHECK_EQ(stub.prepares, preparesBefore);
    CHECK(fixture.session.streamPrepared());
    CHECK(fixture.session.state() == matp::SessionState::Established);
}

MULTIROOM_TEST(matp_session_drops_a_server_that_has_gone_silent)
{
    TestListener listener;
    CHECK(listener.ready());

    Fixture fixture(testIdentity());
    CHECK(fixture.start(listener));
    CHECK(listener.acceptOne(2000));

    Peer peer(fixture.session, listener);
    establish(fixture, peer, "s-silent");

    // A half-open connection from the client's side: the socket is still
    // there, the writes still succeed, and nothing ever comes back. Stopping
    // the clock sync answers is the whole of it -- they are the only thing an
    // established session with no stream expects.
    peer.setAnswersClockSync(false);
    peer.pump(100); // anything already in flight lands here, not later

    const uint64_t connectionsBefore = fixture.connection.connectionCount();

    // poll() takes the time rather than reading the clock, so the deadline can
    // be walked without waiting it out.
    fixture.session.poll(monotonicNowNs() + 21ULL * 1000000000ULL);
    CHECK(fixture.session.state() == matp::SessionState::Failed);
    CHECK(!fixture.session.lastError().empty());

    // Dropped, not merely reported: only a new connection can put the room
    // back in the session (section 43).
    CHECK(waitUntil([&] { return fixture.connection.connectionCount() > connectionsBefore; },
                    5000));
}

MULTIROOM_TEST(matp_session_stays_up_while_the_server_still_answers)
{
    TestListener listener;
    CHECK(listener.ready());

    Fixture fixture(testIdentity());
    CHECK(fixture.start(listener));
    CHECK(listener.acceptOne(2000));

    Peer peer(fixture.session, listener);
    establish(fixture, peer, "s-alive");

    // A few exchanges answered, then the deadline walked to just short of
    // where it fires. A session that failed here would drop a room every
    // twenty seconds for no reason at all.
    peer.pump(200);
    CHECK(fixture.session.clockSyncReplies() != 0);

    // All the way to the end of the test: poll() takes the time, so a later
    // poll at the real clock would be a poll that went backwards, which is
    // something the client's own loop cannot do.
    fixture.session.poll(monotonicNowNs() + 19ULL * 1000000000ULL);
    CHECK(fixture.session.state() == matp::SessionState::Established);
}

MULTIROOM_TEST(matp_session_reports_the_machine_every_two_seconds_once_established)
{
    TestListener listener;
    CHECK(listener.ready());

    Fixture fixture(testIdentity());
    SystemMetrics sample;
    sample.hasMemoryBytes = true;
    sample.memoryBytes = 14680064;
    sample.hasTemperatureC = true;
    sample.temperatureC = 51.5;
    sample.hasWifiRssiDbm = true;
    sample.wifiRssiDbm = -60;
    uint32_t asked = 0;
    fixture.session.setSystemProvider([&](SystemMetrics& out) {
        ++asked;
        out = sample;
        return true;
    });
    CHECK(fixture.start(listener));
    CHECK(listener.acceptOne(2000));

    Peer peer(fixture.session, listener);

    // Nothing before the session exists: the report names it (section 55).
    matp::Message message;
    CHECK(peer.next(message, 3000));
    CHECK(message.type == matp::MessageType::Hello);
    peer.pump(300);
    CHECK(peer.send(matp::MessageType::HelloAck, helloAck(true, matp::kProtocolVersion)));
    CHECK(peer.next(message, 3000));
    CHECK(message.type == matp::MessageType::AudioCapabilities);
    peer.pump(300);
    CHECK_EQ(fixture.session.systemReports(), static_cast<uint32_t>(0));
    CHECK_EQ(asked, static_cast<uint32_t>(0));

    CHECK(peer.send(matp::MessageType::SessionConfig, sessionConfig("s-system", 48000, "s24le")));

    // Whether the room plays or not: no stream is ever prepared here.
    CHECK(nextOfType(peer, matp::MessageType::SystemReport, message, 3000));
    const uint64_t firstNs = monotonicNowNs();
    std::string sessionId;
    SystemMetrics read;
    std::string error;
    CHECK(matp::parseSystemReport(message.payload, sessionId, read, error));
    CHECK_EQ(sessionId, std::string("s-system"));
    CHECK(read.hasMemoryBytes);
    CHECK_EQ(read.memoryBytes, static_cast<uint64_t>(14680064));
    CHECK(read.hasTemperatureC);
    CHECK(!read.hasCpuPercent); // what the sampler did not say is not sent
    // Over loopback the link is unknown, so the Wi-Fi figures stay.
    CHECK(read.networkLink.empty());
    CHECK(read.hasWifiRssiDbm);

    CHECK(nextOfType(peer, matp::MessageType::SystemReport, message, 3000));
    const uint64_t spacingMs = (monotonicNowNs() - firstNs) / 1000000ULL;
    CHECK(spacingMs >= 1800 && spacingMs <= 2600);
    CHECK_EQ(fixture.session.systemReports(), static_cast<uint32_t>(2));
}

MULTIROOM_TEST(matp_session_sends_no_system_report_before_there_is_a_sample)
{
    TestListener listener;
    CHECK(listener.ready());

    Fixture fixture(testIdentity());
    fixture.session.setSystemProvider([](SystemMetrics&) { return false; });
    CHECK(fixture.start(listener));
    CHECK(listener.acceptOne(2000));

    Peer peer(fixture.session, listener);
    establish(fixture, peer, "s-unsampled");
    peer.pump(500);
    CHECK_EQ(fixture.session.systemReports(), static_cast<uint32_t>(0));
}

MULTIROOM_TEST(matp_session_reports_its_devices_and_applies_a_device_the_server_picks)
{
    TestListener listener;
    CHECK(listener.ready());

    Fixture fixture(testIdentity());
    matp::AudioDeviceList devices;
    devices.selectedDeviceId = "onboard";
    matp::AudioDeviceEntry entry;
    entry.id = "onboard";
    entry.isDefault = true;
    devices.devices.push_back(entry);
    entry.id = "usb";
    entry.isDefault = false;
    devices.devices.push_back(entry);

    std::string asked = "unset";
    fixture.session.setAudioDeviceHooks(
        [&](matp::AudioDeviceList& out) { out = devices; },
        [&](const std::string& deviceId, std::string& error) {
            if (deviceId == "gone")
            {
                error = "no audio device \"gone\"";
                return false;
            }
            asked = deviceId;
            devices.configuredDeviceId = deviceId;
            devices.selectedDeviceId = deviceId;
            fixture.session.audioDevicesChanged();
            return true;
        });
    CHECK(fixture.start(listener));
    CHECK(listener.acceptOne(2000));

    Peer peer(fixture.session, listener);
    establish(fixture, peer, "s-devices");

    // The first list comes with the session (section 56).
    matp::Message message;
    CHECK(nextOfType(peer, matp::MessageType::AudioDevices, message, 3000));
    std::string sessionId;
    matp::AudioDeviceList read;
    std::string error;
    CHECK(matp::parseAudioDevices(message.payload, sessionId, read, error));
    CHECK_EQ(sessionId, std::string("s-devices"));
    CHECK_EQ(read.selectedDeviceId, std::string("onboard"));
    CHECK_EQ(read.devices.size(), static_cast<size_t>(2));

    matp::SetAudioDevice request;
    request.sessionId = "s-devices";
    request.deviceId = "usb";
    CHECK(peer.send(matp::MessageType::SetAudioDevice, matp::makeSetAudioDevice(request)));
    CHECK(nextOfType(peer, matp::MessageType::AudioDevices, message, 3000));
    CHECK(matp::parseAudioDevices(message.payload, sessionId, read, error));
    CHECK_EQ(asked, std::string("usb"));
    CHECK_EQ(read.selectedDeviceId, std::string("usb"));
    CHECK_EQ(read.configuredDeviceId, std::string("usb"));
    CHECK_EQ(fixture.session.audioDeviceChanges(), static_cast<uint32_t>(1));

    // Refused: an ERROR back, and the session stays up.
    request.deviceId = "gone";
    CHECK(peer.send(matp::MessageType::SetAudioDevice, matp::makeSetAudioDevice(request)));
    CHECK(nextOfType(peer, matp::MessageType::Error, message, 3000));
    CHECK(fixture.session.state() == matp::SessionState::Established);
    CHECK_EQ(fixture.session.audioDeviceReports(), static_cast<uint32_t>(2));
}

MULTIROOM_TEST(matp_session_answers_set_buffer_with_the_target_it_accepted_or_an_error)
{
    TestListener listener;
    CHECK(listener.ready());

    Fixture fixture(testIdentity());
    uint32_t bufferMs = 1500;
    fixture.session.setBufferHooks([&]() { return bufferMs; },
                                   [&](uint32_t requested, std::string& error) {
                                       if (requested > 10000)
                                       {
                                           error = "bufferMs must be between 1000 and 11000";
                                           return false;
                                       }
                                       bufferMs = requested;
                                       fixture.session.bufferConfigChanged();
                                       return true;
                                   });
    CHECK(fixture.start(listener));
    CHECK(listener.acceptOne(2000));

    Peer peer(fixture.session, listener);
    establish(fixture, peer, "s-buffer");

    // The first report comes with the session (section 58).
    matp::Message message;
    CHECK(nextOfType(peer, matp::MessageType::BufferConfig, message, 3000));
    std::string sessionId;
    uint32_t reported = 0;
    std::string error;
    CHECK(matp::parseBufferConfig(message.payload, sessionId, reported, error));
    CHECK_EQ(sessionId, std::string("s-buffer"));
    CHECK_EQ(reported, static_cast<uint32_t>(1500));

    matp::SetBuffer request;
    request.sessionId = "s-buffer";
    request.bufferMs = 2500;
    CHECK(peer.send(matp::MessageType::SetBuffer, matp::makeSetBuffer(request)));
    CHECK(nextOfType(peer, matp::MessageType::BufferConfig, message, 3000));
    CHECK(matp::parseBufferConfig(message.payload, sessionId, reported, error));
    CHECK_EQ(reported, static_cast<uint32_t>(2500));

    // The same value again is still answered: the answer is the receipt.
    CHECK(peer.send(matp::MessageType::SetBuffer, matp::makeSetBuffer(request)));
    CHECK(nextOfType(peer, matp::MessageType::BufferConfig, message, 3000));
    CHECK(matp::parseBufferConfig(message.payload, sessionId, reported, error));
    CHECK_EQ(reported, static_cast<uint32_t>(2500));
    CHECK_EQ(fixture.session.bufferChanges(), static_cast<uint32_t>(2));
    CHECK_EQ(fixture.session.bufferConfigReports(), static_cast<uint32_t>(3));

    // Refused, not clamped: an ERROR with the code, the old value kept.
    request.bufferMs = 60000;
    CHECK(peer.send(matp::MessageType::SetBuffer, matp::makeSetBuffer(request)));
    CHECK(nextOfType(peer, matp::MessageType::Error, message, 3000));
    std::string code;
    CHECK(json::getString(message.payload, "code", code));
    CHECK_EQ(code, std::string("setBuffer"));
    CHECK_EQ(bufferMs, static_cast<uint32_t>(2500));
    CHECK(fixture.session.state() == matp::SessionState::Established);
    CHECK_EQ(fixture.session.bufferConfigReports(), static_cast<uint32_t>(3));
}

MULTIROOM_TEST(matp_session_rescans_when_the_server_asks_and_reports_what_it_found)
{
    TestListener listener;
    CHECK(listener.ready());

    Fixture fixture(testIdentity());
    matp::AudioDeviceList devices;
    devices.selectedDeviceId = "onboard";
    matp::AudioDeviceEntry entry;
    entry.id = "onboard";
    entry.isDefault = true;
    devices.devices.push_back(entry);
    fixture.session.setAudioDeviceHooks([&](matp::AudioDeviceList& out) { out = devices; },
                                        [](const std::string&, std::string&) { return true; });
    bool fails = false;
    fixture.session.setAudioRescanHook([&](std::string& error) {
        if (fails)
        {
            error = "no audio backend";
            return false;
        }
        entry.id = "usb";
        entry.isDefault = false;
        devices.devices.push_back(entry); // plugged in since the last scan
        fixture.session.audioDevicesChanged();
        return true;
    });
    CHECK(fixture.start(listener));
    CHECK(listener.acceptOne(2000));

    Peer peer(fixture.session, listener);
    establish(fixture, peer, "s-rescan");
    matp::Message message;
    CHECK(nextOfType(peer, matp::MessageType::AudioDevices, message, 3000));

    CHECK(peer.send(matp::MessageType::RescanAudio, matp::makeRescanAudio("s-rescan")));
    CHECK(nextOfType(peer, matp::MessageType::AudioDevices, message, 3000));
    std::string sessionId;
    matp::AudioDeviceList read;
    std::string error;
    CHECK(matp::parseAudioDevices(message.payload, sessionId, read, error));
    CHECK_EQ(read.devices.size(), static_cast<size_t>(2));
    CHECK_EQ(fixture.session.audioRescans(), static_cast<uint32_t>(1));

    fails = true;
    CHECK(peer.send(matp::MessageType::RescanAudio, matp::makeRescanAudio("")));
    CHECK(nextOfType(peer, matp::MessageType::Error, message, 3000));
    CHECK(fixture.session.state() == matp::SessionState::Established);
}

MULTIROOM_TEST(matp_session_reports_its_inputs_sends_actions_and_rebinds)
{
    TestListener listener;
    CHECK(listener.ready());

    Fixture fixture(testIdentity());
    std::vector<matp::InputControl> inputs(1);
    inputs[0].id = "play";
    inputs[0].bindings = matp::defaultInputBindings(matp::InputKind::Button);

    fixture.session.setInputHooks(
        [&](std::vector<matp::InputControl>& out) { out = inputs; },
        [&](const matp::SetInputBindings& request, std::string& error) {
            if (request.inputId != "play")
            {
                error = "no input \"" + request.inputId + "\"";
                return false;
            }
            if (!matp::inputBindingsFromJson(request.bindings, inputs[0].kind, inputs[0].bindings,
                                             error))
                return false;
            fixture.session.inputControlsChanged();
            return true;
        },
        true);

    // Before the session exists, an action is dropped rather than queued.
    matp::InputActionEvent event;
    event.inputId = "play";
    event.binding = inputs[0].bindings[0];
    CHECK(!fixture.session.sendInputAction(event));

    CHECK(fixture.start(listener));
    CHECK(listener.acceptOne(2000));
    Peer peer(fixture.session, listener);
    establish(fixture, peer, "s-inputs");

    // The first list comes with the session (section 57).
    matp::Message message;
    CHECK(nextOfType(peer, matp::MessageType::InputControls, message, 3000));
    std::string sessionId;
    std::vector<matp::InputControl> read;
    bool deepSleep = false;
    std::string error;
    CHECK(matp::parseInputControls(message.payload, sessionId, read, deepSleep, error));
    CHECK(deepSleep);
    CHECK_EQ(sessionId, std::string("s-inputs"));
    CHECK_EQ(read.size(), static_cast<size_t>(1));

    CHECK(fixture.session.sendInputAction(event));
    CHECK(nextOfType(peer, matp::MessageType::InputAction, message, 3000));
    matp::InputActionEvent readEvent;
    CHECK(matp::parseInputAction(message.payload, sessionId, readEvent, error));
    CHECK(readEvent.binding.action == matp::InputAction::TogglePause);
    CHECK_EQ(fixture.session.inputActions(), static_cast<uint32_t>(1));

    matp::SetInputBindings request;
    request.sessionId = "s-inputs";
    request.inputId = "play";
    json::parse("{\"doubleClick\":{\"action\":\"next\"}}", request.bindings, error);
    CHECK(peer.send(matp::MessageType::SetInputBindings, matp::makeSetInputBindings(request)));
    CHECK(nextOfType(peer, matp::MessageType::InputControls, message, 3000));
    CHECK(matp::parseInputControls(message.payload, sessionId, read, deepSleep, error));
    CHECK(read[0].bindings[static_cast<size_t>(matp::InputGesture::DoubleClick)].action ==
          matp::InputAction::Next);
    CHECK_EQ(fixture.session.inputBindingChanges(), static_cast<uint32_t>(1));

    // Refused: an ERROR back, and the session stays up.
    json::parse("{\"clockwise\":{\"action\":\"next\"}}", request.bindings, error);
    CHECK(peer.send(matp::MessageType::SetInputBindings, matp::makeSetInputBindings(request)));
    CHECK(nextOfType(peer, matp::MessageType::Error, message, 3000));
    CHECK(fixture.session.state() == matp::SessionState::Established);
    CHECK_EQ(fixture.session.inputControlReports(), static_cast<uint32_t>(2));
}

MULTIROOM_TEST(matp_session_hands_on_the_audio_control_and_answers_set_volume_control)
{
    TestListener listener;
    CHECK(listener.ready());

    Fixture fixture(testIdentity());
    matp::AudioControl audio = matp::AudioControl::Auto;
    bool audioKnown = true;
    matp::VolumeControlMode configured = matp::VolumeControlMode::Auto;
    fixture.session.setVolumeControlHooks(
        [&](matp::AudioControl control, bool known) {
            audio = control;
            audioKnown = known;
        },
        [&]() {
            matp::VolumeConfig report;
            report.volumeControl = configured;
            report.effective = configured == matp::VolumeControlMode::Software
                                   ? matp::VolumeControlMode::Software
                                   : matp::VolumeControlMode::Disabled;
            return report;
        },
        [&](matp::VolumeControlMode mode, std::string& error) {
            if (mode == matp::VolumeControlMode::Software &&
                audio == matp::AudioControl::BitPerfect)
            {
                error = "software volume is not available";
                return false;
            }
            configured = mode;
            return true;
        });
    CHECK(fixture.start(listener));
    CHECK(listener.acceptOne(2000));

    Peer peer(fixture.session, listener);
    establish(fixture, peer, "s-volume");

    // A SESSION_CONFIG that names none is read as the strict one.
    CHECK(audio == matp::AudioControl::BitPerfect);
    CHECK(!audioKnown);

    matp::Message message;
    CHECK(nextOfType(peer, matp::MessageType::VolumeConfig, message, 3000));
    matp::VolumeConfig report;
    std::string error;
    CHECK(matp::parseVolumeConfig(message.payload, report, error));
    CHECK_EQ(report.sessionId, std::string("s-volume"));
    CHECK(report.volumeControl == matp::VolumeControlMode::Auto);

    // Refused under bitPerfect: an ERROR with the code, the setting kept.
    matp::SetVolumeControl request;
    request.volumeControl = matp::VolumeControlMode::Software;
    CHECK(peer.send(matp::MessageType::SetVolumeControl, matp::makeSetVolumeControl(request)));
    CHECK(nextOfType(peer, matp::MessageType::Error, message, 3000));
    std::string code;
    CHECK(json::getString(message.payload, "code", code));
    CHECK_EQ(code, std::string("setVolumeControl"));
    CHECK(configured == matp::VolumeControlMode::Auto);

    // Allowed once the room is not bitPerfect, and answered.
    json::Value config = sessionConfig("s-volume", 48000, "s24le");
    config["audioControl"] = json::Value("auto");
    CHECK(peer.send(matp::MessageType::SessionConfig, config));
    CHECK(waitUntil(
        [&] {
            fixture.session.poll(monotonicNowNs());
            return audio == matp::AudioControl::Auto;
        },
        3000));
    CHECK(audioKnown);
    CHECK(peer.send(matp::MessageType::SetVolumeControl, matp::makeSetVolumeControl(request)));
    CHECK(nextOfType(peer, matp::MessageType::VolumeConfig, message, 3000));
    CHECK(matp::parseVolumeConfig(message.payload, report, error));
    CHECK(report.volumeControl == matp::VolumeControlMode::Software);
    CHECK(report.effective == matp::VolumeControlMode::Software);
    CHECK_EQ(fixture.session.volumeControlChanges(), static_cast<uint32_t>(1));
}

MULTIROOM_TEST(matp_session_reports_its_name_and_takes_a_new_one_without_reconnecting)
{
    TestListener listener;
    CHECK(listener.ready());

    Fixture fixture(testIdentity());
    std::string name = "Kitchen";
    fixture.session.setRoomNameHooks([&]() { return name; },
                                     [&](const std::string& requested, std::string& error) {
                                         if (requested == "Refused")
                                         {
                                             error = "could not write client.json";
                                             return false;
                                         }
                                         name = requested;
                                         return true;
                                     });
    CHECK(fixture.start(listener));
    CHECK(listener.acceptOne(2000));

    Peer peer(fixture.session, listener);
    establish(fixture, peer, "s-name");

    // The first report comes with the session (section 60).
    matp::Message message;
    CHECK(nextOfType(peer, matp::MessageType::RoomName, message, 3000));
    std::string sessionId;
    std::string reported;
    std::string error;
    CHECK(matp::parseRoomName(message.payload, sessionId, reported, error));
    CHECK_EQ(sessionId, std::string("s-name"));
    CHECK_EQ(reported, std::string("Kitchen"));

    matp::SetRoomName request;
    request.sessionId = "s-name";
    request.name = "Arbeitsraum";
    CHECK(peer.send(matp::MessageType::SetRoomName, matp::makeSetRoomName(request)));
    CHECK(nextOfType(peer, matp::MessageType::RoomName, message, 3000));
    CHECK(matp::parseRoomName(message.payload, sessionId, reported, error));
    CHECK_EQ(reported, std::string("Arbeitsraum"));

    // The same name again is still answered: the answer is the receipt.
    CHECK(peer.send(matp::MessageType::SetRoomName, matp::makeSetRoomName(request)));
    CHECK(nextOfType(peer, matp::MessageType::RoomName, message, 3000));
    CHECK_EQ(fixture.session.roomNameReports(), static_cast<uint32_t>(3));

    // The owner refusing, and a name against the rule: an ERROR, the session
    // goes on and the name stays.
    request.name = "Refused";
    CHECK(peer.send(matp::MessageType::SetRoomName, matp::makeSetRoomName(request)));
    CHECK(nextOfType(peer, matp::MessageType::Error, message, 3000));
    std::string code;
    CHECK(json::getString(message.payload, "code", code));
    CHECK_EQ(code, std::string("setRoomName"));
    json::Value bad = matp::makeSetRoomName(request);
    bad["name"] = json::Value(" padded");
    CHECK(peer.send(matp::MessageType::SetRoomName, bad));
    CHECK(nextOfType(peer, matp::MessageType::Error, message, 3000));
    CHECK_EQ(name, std::string("Arbeitsraum"));
    CHECK(fixture.session.state() == matp::SessionState::Established);

    // A rename by the control API goes out by itself, on the same session.
    name = "Wohnzimmer";
    fixture.session.roomNameChanged();
    CHECK(nextOfType(peer, matp::MessageType::RoomName, message, 3000));
    CHECK(matp::parseRoomName(message.payload, sessionId, reported, error));
    CHECK_EQ(sessionId, std::string("s-name"));
    CHECK_EQ(reported, std::string("Wohnzimmer"));
}

MULTIROOM_TEST(matp_session_selects_the_drift_actuator_and_passes_on_fresh_feedback_only)
{
    TestListener listener;
    CHECK(listener.ready());

    Fixture fixture(testIdentity());
    fixture.report.driftFeedback = true;
    matp::DriftCorrectionMode selected = matp::DriftCorrectionMode::HardwareClock;
    std::vector<uint64_t> applied;
    fixture.session.setDriftCorrectionHooks(
        [&](matp::DriftCorrectionMode mode) { selected = mode; },
        [&](const matp::DriftFeedback& feedback) { applied.push_back(feedback.sequence); },
        [&](matp::DriftCorrectionMode mode, std::string& error) {
            if (mode == matp::DriftCorrectionMode::HardwareClock &&
                !fixture.report.hardwareClock.available)
            {
                error = "the selected device has no verified hardware clock";
                return false;
            }
            fixture.report.driftCorrection = mode;
            return true;
        });
    CHECK(fixture.start(listener));
    CHECK(listener.acceptOne(2000));

    Peer peer(fixture.session, listener);
    establish(fixture, peer, "s-drift");

    // A SESSION_CONFIG that names no actuator is the server resampling.
    CHECK(selected == matp::DriftCorrectionMode::Resampling);

    // Refused where the device has no trim: an ERROR, the setting kept.
    matp::SetDriftCorrection request;
    request.driftCorrection = matp::DriftCorrectionMode::HardwareClock;
    CHECK(peer.send(matp::MessageType::SetDriftCorrection, matp::makeSetDriftCorrection(request)));
    matp::Message message;
    CHECK(nextOfType(peer, matp::MessageType::Error, message, 3000));
    std::string code;
    CHECK(json::getString(message.payload, "code", code));
    CHECK_EQ(code, std::string("setDriftCorrection"));

    // Accepted, it is answered with capabilities that carry it.
    fixture.report.hasHardwareClock = true;
    fixture.report.hardwareClock.available = true;
    fixture.report.hardwareClock.minPpm = -500;
    fixture.report.hardwareClock.maxPpm = 500;
    CHECK(peer.send(matp::MessageType::SetDriftCorrection, matp::makeSetDriftCorrection(request)));
    CHECK(nextOfType(peer, matp::MessageType::AudioCapabilities, message, 3000));
    matp::CapabilityReport capabilities;
    std::string error;
    CHECK(matp::parseAudioCapabilities(message.payload, capabilities, error));
    CHECK(capabilities.driftCorrection == matp::DriftCorrectionMode::HardwareClock);
    CHECK(capabilities.hardwareClock.available);

    json::Value config = sessionConfig("s-drift", 48000, "s24le");
    config["driftCorrection"] = json::Value("hardwareClock");
    CHECK(peer.send(matp::MessageType::SessionConfig, config));
    CHECK(waitUntil(
        [&] {
            fixture.session.poll(monotonicNowNs());
            return selected == matp::DriftCorrectionMode::HardwareClock;
        },
        3000));

    // Feedback in sequence; a repeated or older one and another session's
    // are skipped rather than applied.
    matp::DriftFeedback feedback;
    feedback.sessionId = "s-drift";
    for (const uint64_t sequence : {2ULL, 2ULL, 1ULL, 3ULL})
    {
        feedback.sequence = sequence;
        CHECK(peer.send(matp::MessageType::DriftFeedback, matp::makeDriftFeedback(feedback)));
    }
    feedback.sessionId = "s-other";
    feedback.sequence = 9;
    CHECK(peer.send(matp::MessageType::DriftFeedback, matp::makeDriftFeedback(feedback)));
    CHECK(waitUntil(
        [&] {
            fixture.session.poll(monotonicNowNs());
            return applied.size() >= 2;
        },
        3000));
    for (int i = 0; i < 5; ++i)
        fixture.session.poll(monotonicNowNs());
    CHECK_EQ(applied.size(), static_cast<size_t>(2));
    CHECK_EQ(applied[0], static_cast<uint64_t>(2));
    CHECK_EQ(applied[1], static_cast<uint64_t>(3));
    CHECK(fixture.session.state() == matp::SessionState::Established);
}
