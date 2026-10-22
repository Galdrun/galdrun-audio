// The control listener, over a real loopback socket.
//
// Everything about the protocol is already covered by test_client_session.cpp,
// which needs no port at all. What is left here is the part that only a socket
// can prove: that a connection is accepted, that the bytes go both ways
// unaltered, that a room appears in the registry when it names itself, and
// that two clients cannot block each other.
//
// The port is always zero -- the system picks -- because a test on a fixed
// port fails whenever anything else on the machine happens to hold it.
//
// This is the one place in the suite that waits on another thread. It waits
// for an *event* with a deadline rather than sleeping for a duration the
// server also uses, and the listener's own timers are set far outside the
// test, so nothing here is a race against a designed interval.

#include "TestSupport.h"

#include "matp/ControlServer.h"
#include "net/Socket.h"

#include <chrono>
#include <string>
#include <thread>
#include <vector>

using namespace multiroom;

namespace {

// How long an assertion about another thread is given before it is a failure.
// Generous on purpose: it bounds a hang, it does not measure anything.
constexpr auto kDeadline = std::chrono::seconds(5);

// Winsock is reference counted per process. The listener starts it for its own
// lifetime, so a test that outlives one -- the shutdown test does, by design --
// would otherwise be calling recv() after the last WSACleanup. One here keeps
// it up for the whole binary, and on POSIX it is nothing at all.
net::Library g_socketLibrary;

// Settings whose timers cannot fire during a test: the liveness path is walked
// by ManualClock in test_client_session.cpp, where it costs nothing.
ControlServerSettings loopbackSettings()
{
    ControlServerSettings settings;
    settings.port = 0;
    settings.session.pingIntervalNs = 3600ULL * 1000000000ULL;
    settings.session.silenceTimeoutNs = 7200ULL * 1000000000ULL;
    return settings;
}

// A client socket, connected to 127.0.0.1 on `port`, with the frame reader the
// real client uses on the other side of it.
class LoopbackClient
{
public:
    explicit LoopbackClient(uint16_t port)
    {
        socket_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (socket_ == net::kInvalidSocket)
            multiroomtest::failAt("could not create a client socket", __FILE__, __LINE__);

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        if (::connect(socket_, reinterpret_cast<const sockaddr*>(&address),
                      sizeof(address)) != 0)
        {
            multiroomtest::failAt("could not connect to the listener: " +
                                net::errorMessage(net::lastError()),
                            __FILE__, __LINE__);
        }

        // Connected blocking, read non-blocking: every wait in this file is a
        // deadline around a predicate, and a blocking recv() would turn a
        // failed expectation into a hung test run.
        if (!net::setNonBlocking(socket_))
            multiroomtest::failAt("could not make the client socket non-blocking", __FILE__,
                            __LINE__);
    }

    ~LoopbackClient() { close(); }

    LoopbackClient(const LoopbackClient&) = delete;
    LoopbackClient& operator=(const LoopbackClient&) = delete;

    void close()
    {
        if (socket_ == net::kInvalidSocket)
            return;
        net::closeSocket(socket_);
        socket_ = net::kInvalidSocket;
    }

    void send(matp::MessageType type, const json::Value& payload)
    {
        const std::string frame = matp::encode(type, payload);
        size_t written = 0;
        while (written < frame.size())
        {
            const int sent = net::transmit(socket_, frame.data() + written,
                                           static_cast<int>(frame.size() - written));
            if (sent > 0)
            {
                written += static_cast<size_t>(sent);
                continue;
            }
            if (sent < 0 && net::wouldBlock(net::lastError()))
                continue;
            multiroomtest::failAt("the listener stopped reading mid-frame", __FILE__, __LINE__);
        }
    }

    // The next whole message, or a failure once the deadline passes. A closed
    // connection is reported rather than waited out.
    matp::Message receive()
    {
        const auto until = std::chrono::steady_clock::now() + kDeadline;
        for (;;)
        {
            matp::Message message;
            std::string error;
            const matp::FrameReader::Result result = reader_.next(message, error);
            if (result == matp::FrameReader::Result::Message)
                return message;
            if (result == matp::FrameReader::Result::Invalid)
                multiroomtest::failAt("the listener sent a frame the client cannot read: " + error,
                                __FILE__, __LINE__);

            if (std::chrono::steady_clock::now() > until)
                multiroomtest::failAt("timed out waiting for a message", __FILE__, __LINE__);

            char buffer[4096];
            const int received =
                net::receive(socket_, buffer, static_cast<int>(sizeof(buffer)));
            if (received > 0)
            {
                reader_.append(buffer, static_cast<size_t>(received));
                continue;
            }
            if (received == 0)
                multiroomtest::failAt("the listener closed the connection", __FILE__, __LINE__);
            if (!net::wouldBlock(net::lastError()))
                multiroomtest::failAt("read failed: " + net::errorMessage(net::lastError()),
                                __FILE__, __LINE__);
            std::this_thread::yield();
        }
    }

    // True once the far end has closed, which is how a refused handshake ends.
    bool waitForClose()
    {
        const auto until = std::chrono::steady_clock::now() + kDeadline;
        while (std::chrono::steady_clock::now() <= until)
        {
            char buffer[512];
            const int received =
                net::receive(socket_, buffer, static_cast<int>(sizeof(buffer)));
            if (received == 0)
                return true;
            if (received < 0 && !net::wouldBlock(net::lastError()))
                return true; // a reset is a close as far as this test cares
            std::this_thread::yield();
        }
        return false;
    }

private:
    net::Socket socket_ = net::kInvalidSocket;
    matp::FrameReader reader_;
};

// Spins on a predicate with the same deadline. Used only for counters the
// listener thread publishes; nothing here depends on how long it takes.
template <typename Predicate>
bool waitFor(Predicate predicate)
{
    const auto until = std::chrono::steady_clock::now() + kDeadline;
    while (std::chrono::steady_clock::now() <= until)
    {
        if (predicate())
            return true;
        std::this_thread::yield();
    }
    return predicate();
}

json::Value helloFor(const std::string& clientId)
{
    matp::Identity identity;
    identity.clientId = clientId;
    identity.clientVersion = "0.1.0";
    identity.platform = "test";
    return matp::makeHello(identity);
}

json::Value capabilitiesAt(uint32_t rate)
{
    matp::CapabilityReport report;
    report.deviceName = "loopback";
    AudioFormat format;
    format.sampleRate = rate;
    format.channels = 2;
    format.sampleType = AudioFormat::SampleType::Int24In32;
    report.formats.push_back(format);
    return matp::makeAudioCapabilities(report);
}

} // namespace

MULTIROOM_TEST(control_server_binds_a_port_the_system_chose)
{
    const SystemClock clock;
    auto server = ControlServer::start(clock, loopbackSettings());
    CHECK_OK(server);
    // Zero asked for any free port; the listener has to say which one it got,
    // or nothing could connect to it.
    CHECK((*server)->port() != 0);
    CHECK_EQ((*server)->connectionCount(), size_t(0));
    CHECK_EQ((*server)->clientCount(), size_t(0));
}

MULTIROOM_TEST(control_server_refuses_to_start_beyond_what_select_can_watch)
{
    const SystemClock clock;
    ControlServerSettings settings = loopbackSettings();
    settings.maxConnections = FD_SETSIZE;

    CHECK_ERR(ControlServer::start(clock, settings), ControlServerError::Unsupported);
}

MULTIROOM_TEST(control_server_completes_a_handshake_over_a_real_socket)
{
    const SystemClock clock;
    auto server = ControlServer::start(clock, loopbackSettings());
    CHECK_OK(server);

    LoopbackClient client((*server)->port());

    client.send(matp::MessageType::Hello, helloFor("livingroom"));

    const matp::Message ack = client.receive();
    CHECK(ack.type == matp::MessageType::HelloAck);

    matp::HelloAck parsedAck;
    std::string error;
    CHECK(matp::parseHelloAck(ack.payload, parsedAck, error));
    CHECK_EQ(parsedAck.protocolVersion, matp::kProtocolVersion);

    // Named, so it is a room now.
    CHECK(waitFor([&] { return (*server)->clientCount() == 1; }));
    CHECK_EQ((*server)->identifiedCount(), 1ULL);

    client.send(matp::MessageType::AudioCapabilities, capabilitiesAt(44100));

    const matp::Message configured = client.receive();
    CHECK(configured.type == matp::MessageType::SessionConfig);

    matp::SessionConfig config;
    CHECK(matp::parseSessionConfig(configured.payload, config, error));
    CHECK_EQ(config.sessionId.size(), size_t(32)); // 128 bits as hex
    CHECK_EQ(config.format.sampleRate, 44100u);
    CHECK(config.format.sampleType == AudioFormat::SampleType::Int24In32);

    // And straight after it, the instruction to open a device for that format,
    // carrying the audio port rather than leaving the client to its "control
    // port plus one" fallback.
    const matp::Message prepared = client.receive();
    CHECK(prepared.type == matp::MessageType::PrepareStream);

    matp::PrepareStream prepare;
    CHECK(matp::parsePrepareStream(prepared.payload, prepare, error));
    CHECK_EQ(prepare.sessionId, config.sessionId);
    CHECK(prepare.hasAudioPort);

    // And the clock exchange, which section 10 has running alongside.
    client.send(matp::MessageType::ClockSyncRequest, matp::makeClockSyncRequest(123456789));

    const matp::Message reply = client.receive();
    CHECK(reply.type == matp::MessageType::ClockSyncReply);

    matp::ClockSyncReply parsedReply;
    CHECK(matp::parseClockSyncReply(reply.payload, parsedReply, error));
    CHECK_EQ(parsedReply.clientSendNs, 123456789ULL);
    CHECK(parsedReply.serverSendNs >= parsedReply.serverReceiveNs);
}

MULTIROOM_TEST(control_server_tells_a_wrong_version_why_before_dropping_it)
{
    // Section 51 again, this time over a socket: what the real client sees is
    // an ERROR and then a closed connection, not a connection that just went
    // away.
    const SystemClock clock;
    auto server = ControlServer::start(clock, loopbackSettings());
    CHECK_OK(server);

    LoopbackClient client((*server)->port());

    json::Value hello = helloFor("livingroom");
    hello["protocolVersion"] = json::Value(uint32_t(1));
    client.send(matp::MessageType::Hello, hello);

    const matp::Message refused = client.receive();
    CHECK(refused.type == matp::MessageType::Error);
    CHECK(matp::errorText(refused.payload).find("versionMismatch") != std::string::npos);

    CHECK(client.waitForClose());
    CHECK(waitFor([&] { return (*server)->closedCount() == 1; }));
    // It never became a room: the id was never accepted.
    CHECK_EQ((*server)->clientCount(), size_t(0));
}

MULTIROOM_TEST(control_server_serves_one_client_while_another_stops_reading)
{
    // The rule that applies to every phase touching the network path: one
    // client can never block another. This is the control path's share of it:
    // a room that speaks and then never reads a byte of the answer costs the
    // listener one bounded queue and one non-blocking write per pass, and the
    // other room's whole handshake and fifty clock exchanges run beside it.
    // The audio path's version -- where a full socket is the normal case
    // rather than a broken one -- is phase 9's mandatory test.
    const SystemClock clock;
    auto server = ControlServer::start(clock, loopbackSettings());
    CHECK_OK(server);

    LoopbackClient mute((*server)->port());
    mute.send(matp::MessageType::Hello, helloFor("mute"));
    mute.send(matp::MessageType::AudioCapabilities, capabilitiesAt(48000));
    // ... and never reads a byte of the two answers waiting for it.

    LoopbackClient attentive((*server)->port());
    attentive.send(matp::MessageType::Hello, helloFor("attentive"));
    CHECK(attentive.receive().type == matp::MessageType::HelloAck);

    attentive.send(matp::MessageType::AudioCapabilities, capabilitiesAt(44100));
    CHECK(attentive.receive().type == matp::MessageType::SessionConfig);
    CHECK(attentive.receive().type == matp::MessageType::PrepareStream);

    // Fifty exchanges while the other room says nothing.
    for (uint64_t i = 1; i <= 50; ++i)
    {
        attentive.send(matp::MessageType::ClockSyncRequest, matp::makeClockSyncRequest(i));
        const matp::Message reply = attentive.receive();
        CHECK(reply.type == matp::MessageType::ClockSyncReply);

        matp::ClockSyncReply parsed;
        std::string error;
        CHECK(matp::parseClockSyncReply(reply.payload, parsed, error));
        CHECK_EQ(parsed.clientSendNs, i);
    }

    CHECK(waitFor([&] { return (*server)->clientCount() == 2; }));
}

MULTIROOM_TEST(control_server_replaces_a_client_that_reconnects)
{
    // Section 43: the same room on a new socket. The old connection is gone
    // whether or not its TCP connection has noticed.
    const SystemClock clock;
    auto server = ControlServer::start(clock, loopbackSettings());
    CHECK_OK(server);

    LoopbackClient first((*server)->port());
    first.send(matp::MessageType::Hello, helloFor("livingroom"));
    CHECK(first.receive().type == matp::MessageType::HelloAck);
    CHECK(waitFor([&] { return (*server)->clientCount() == 1; }));

    LoopbackClient second((*server)->port());
    second.send(matp::MessageType::Hello, helloFor("livingroom"));
    CHECK(second.receive().type == matp::MessageType::HelloAck);

    // The first connection is told why and closed; the room is still one room.
    CHECK(first.waitForClose());
    CHECK(waitFor([&] { return (*server)->clientCount() == 1; }));
    CHECK_EQ((*server)->acceptedCount(), 2ULL);
}

MULTIROOM_TEST(control_server_refuses_more_connections_than_it_will_hold)
{
    // Bounded, so that a peer opening sockets without speaking cannot cost
    // this process a session each.
    const SystemClock clock;
    ControlServerSettings settings = loopbackSettings();
    settings.maxConnections = 2;

    auto server = ControlServer::start(clock, settings);
    CHECK_OK(server);

    std::vector<std::unique_ptr<LoopbackClient>> clients;
    for (int i = 0; i < 2; ++i)
    {
        clients.push_back(std::make_unique<LoopbackClient>((*server)->port()));
        clients.back()->send(matp::MessageType::Hello,
                             helloFor("room" + std::to_string(i)));
        CHECK(clients.back()->receive().type == matp::MessageType::HelloAck);
    }

    CHECK(waitFor([&] { return (*server)->connectionCount() == 2; }));

    // TCP accepts the third into the backlog; the listener closes it without
    // ever giving it a session.
    LoopbackClient extra((*server)->port());
    CHECK(waitFor([&] { return (*server)->refusedCount() >= 1; }));
    CHECK(extra.waitForClose());
    CHECK_EQ((*server)->connectionCount(), size_t(2));
}

MULTIROOM_TEST(control_server_stops_cleanly_with_connections_still_open)
{
    // Shutdown order, step 3: the listener's destructor stops its thread and
    // closes every connection before returning. A client that was mid-session
    // sees a closed socket rather than a process that hung.
    const SystemClock clock;
    auto server = ControlServer::start(clock, loopbackSettings());
    CHECK_OK(server);

    LoopbackClient client((*server)->port());
    client.send(matp::MessageType::Hello, helloFor("livingroom"));
    CHECK(client.receive().type == matp::MessageType::HelloAck);

    server.value().reset();

    CHECK(client.waitForClose());
}

MULTIROOM_TEST(control_server_carries_every_room_message_across_the_thread_boundary)
{
    // Phase 11's mailbox, phase 14's five more message types. The zone's side
    // of it is another thread entirely; what is asserted here is that a
    // RoomCommand queued from one reaches the right session on the other, with
    // its session id filled in and its kind intact. Which messages a session
    // will accept when is the session's own rule and is tested there.
    const SystemClock clock;
    auto server = ControlServer::start(clock, loopbackSettings());
    CHECK_OK(server);

    LoopbackClient client((*server)->port());

    client.send(matp::MessageType::Hello, helloFor("livingroom"));
    CHECK(client.receive().type == matp::MessageType::HelloAck);
    CHECK(waitFor([&] { return (*server)->clientCount() == 1; }));

    client.send(matp::MessageType::AudioCapabilities, capabilitiesAt(44100));

    const matp::Message configured = client.receive();
    CHECK(configured.type == matp::MessageType::SessionConfig);
    matp::SessionConfig config;
    std::string error;
    CHECK(matp::parseSessionConfig(configured.payload, config, error));
    CHECK(client.receive().type == matp::MessageType::PrepareStream);

    const ClientId room("livingroom");

    RoomCommand boundary;
    boundary.kind = RoomCommandKind::TrackBoundary;
    boundary.client = room;
    boundary.boundary.timelineNs = 180000000000ULL;
    boundary.boundary.trackId = "track-B";
    CHECK((*server)->send(boundary));

    RoomCommand volume;
    volume.kind = RoomCommandKind::Volume;
    volume.client = room;
    volume.volume.hasValue = true;
    volume.volume.value = 0.5;
    CHECK((*server)->send(volume));

    const matp::Message gotBoundary = client.receive();
    CHECK(gotBoundary.type == matp::MessageType::TrackBoundary);
    matp::TrackBoundary parsedBoundary;
    CHECK(matp::parseTrackBoundary(gotBoundary.payload, parsedBoundary, error));
    CHECK_EQ(parsedBoundary.sessionId, config.sessionId);
    CHECK_EQ(parsedBoundary.timelineNs, 180000000000ULL);
    CHECK_EQ(parsedBoundary.trackId, std::string("track-B"));

    const matp::Message gotVolume = client.receive();
    CHECK(gotVolume.type == matp::MessageType::SetVolume);
    matp::SetVolume parsedVolume;
    CHECK(matp::parseSetVolume(gotVolume.payload, parsedVolume, error));
    CHECK_EQ(parsedVolume.sessionId, config.sessionId);
    CHECK(parsedVolume.hasValue);
    CHECK_NEAR(parsedVolume.value, 0.031622777, 1e-9); // the position 0.5 as a gain: -30 dB

    CHECK(waitFor([&] { return (*server)->commandsSentCount() == 2ULL; }));

    // Starts are counted apart from the rest, because "was this room ever told
    // to play?" is a different question from "did it get any messages?".
    CHECK_EQ((*server)->startsSentCount(), 0ULL);

    // A room this listener does not hold is queued and then dropped, which is
    // a message that went nowhere rather than a listener that failed.
    RoomCommand elsewhere = volume;
    elsewhere.client = ClientId("kitchen");
    CHECK((*server)->send(elsewhere));
    CHECK(waitFor([&] { return (*server)->connectionCount() == 1; }));
    CHECK_EQ((*server)->commandsSentCount(), 2ULL);
}
