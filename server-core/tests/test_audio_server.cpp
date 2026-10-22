// The audio listener, over a real loopback socket.
//
// It is the second place in this suite that opens one, and for the same reason
// the first does: what is being proven here is that a bare TCP connection
// becomes a room's stream, and there is no way to prove that without a
// connection. Everything the listener does *after* the handover belongs to
// ClientOutput and is tested with no socket at all.
//
// The port is always zero -- the system picks -- and every wait is on an event
// with a deadline rather than on a duration the server also uses.

#include "TestSupport.h"

#include "core/Units.h"
#include "matp/AudioServer.h"
#include "matp/Protocol.h"
#include "net/Socket.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace multiroom;

namespace {

constexpr auto kDeadline = std::chrono::seconds(5);

// Winsock is reference counted per process, and a test that outlives a
// listener would otherwise be calling send() after the last WSACleanup.
net::Library g_socketLibrary;

matp::Identity identityOf(const std::string& clientId)
{
    matp::Identity identity;
    identity.clientId = clientId;
    identity.clientVersion = "0.1.0";
    identity.platform = "test";
    return identity;
}

SessionId testSessionId()
{
    return SessionId(0x0123456789abcdefULL, 0xfedcba9876543210ULL);
}

// What the handler was handed, recorded so a test can assert on it after the
// listener's thread has moved on.
struct Attachments
{
    std::mutex mutex;
    std::vector<StreamAttachment> taken;
    std::vector<std::unique_ptr<IPacketSink>> sinks;

    size_t count()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return taken.size();
    }
};

// A client socket on 127.0.0.1, connected blocking and read non-blocking so
// that a failed expectation is a failure rather than a hung run.
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

        if (::connect(socket_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) !=
            0)
        {
            multiroomtest::failAt("could not connect to the audio listener: " +
                                net::errorMessage(net::lastError()),
                            __FILE__, __LINE__);
        }
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

    void send(const std::string& frame)
    {
        size_t written = 0;
        const auto until = std::chrono::steady_clock::now() + kDeadline;
        while (written < frame.size())
        {
            const int sent = net::transmit(socket_, frame.data() + written,
                                           static_cast<int>(frame.size() - written));
            if (sent > 0)
            {
                written += static_cast<size_t>(sent);
                continue;
            }
            if (sent < 0 && net::wouldBlock(net::lastError()) &&
                std::chrono::steady_clock::now() < until)
            {
                continue;
            }
            multiroomtest::failAt("the listener stopped reading mid-frame", __FILE__, __LINE__);
        }
    }

    void send(matp::MessageType type, const json::Value& payload)
    {
        send(matp::encode(type, payload));
    }

    // Appends whatever has arrived, and says whether anything did.
    bool receiveInto(std::string& out, size_t want)
    {
        const auto until = std::chrono::steady_clock::now() + kDeadline;
        while (out.size() < want && std::chrono::steady_clock::now() < until)
        {
            char buffer[256];
            const int received = net::receive(socket_, buffer, int(sizeof(buffer)));
            if (received > 0)
            {
                out.append(buffer, static_cast<size_t>(received));
                continue;
            }
            if (received < 0 && net::wouldBlock(net::lastError()))
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            break;
        }
        return out.size() >= want;
    }

    // True once the far end has closed this connection.
    bool closedByPeer()
    {
        const auto until = std::chrono::steady_clock::now() + kDeadline;
        while (std::chrono::steady_clock::now() < until)
        {
            char byte = 0;
            const int received = net::receive(socket_, &byte, 1);
            if (received == 0)
                return true;
            if (received < 0 && !net::wouldBlock(net::lastError()))
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return false;
    }

private:
    net::Socket socket_ = net::kInvalidSocket;
};

AudioServerSettings settingsFor(Attachments& attachments, bool accept = true)
{
    AudioServerSettings settings;
    settings.port = 0;
    settings.onAttach = [&attachments, accept](const StreamAttachment& attachment,
                                               std::unique_ptr<IPacketSink> sink) {
        std::lock_guard<std::mutex> lock(attachments.mutex);
        attachments.taken.push_back(attachment);
        if (!accept)
            return false;
        attachments.sinks.push_back(std::move(sink));
        return true;
    };
    return settings;
}

template <typename Predicate>
bool waitFor(Predicate predicate)
{
    const auto until = std::chrono::steady_clock::now() + kDeadline;
    while (std::chrono::steady_clock::now() < until)
    {
        if (predicate())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return predicate();
}

} // namespace

MULTIROOM_TEST(audio_server_refuses_to_start_without_somewhere_to_put_a_connection)
{
    // A component that is not implemented yet fails explicitly: a listener
    // with no handler would accept rooms and drop them without saying why.
    const SystemClock clock;
    AudioServerSettings settings;
    settings.port = 0;
    CHECK_ERR(AudioServer::start(clock, settings), AudioServerError::NoHandler);

    Attachments attachments;
    AudioServerSettings tooMany = settingsFor(attachments);
    tooMany.maxConnections = 0;
    CHECK_ERR(AudioServer::start(clock, tooMany), AudioServerError::Unsupported);
}

MULTIROOM_TEST(audio_server_binds_a_connection_to_the_session_it_names)
{
    // Section 13 assumes an audio connection "already bound to exactly one
    // session" and nothing in the protocol binds one. STREAM_ATTACH is that
    // binding, and this is the whole of what the listener does with it.
    const SystemClock clock;
    Attachments attachments;

    auto server = AudioServer::start(clock, settingsFor(attachments));
    CHECK_OK(server);
    CHECK((*server)->port() != 0);

    LoopbackClient client((*server)->port());
    client.send(matp::MessageType::StreamAttach,
                matp::makeStreamAttach(testSessionId().toHex(), identityOf("livingroom")));

    CHECK(waitFor([&] { return attachments.count() == 1; }));

    std::lock_guard<std::mutex> lock(attachments.mutex);
    CHECK(attachments.taken[0].session == testSessionId());
    CHECK_EQ(attachments.taken[0].client.str(), std::string("livingroom"));
    CHECK_EQ((*server)->attachedCount(), 1ULL);

    // The listener has forgotten the socket: it belongs to the room now, and
    // there is no moment where two threads would have to agree who closes it.
    CHECK(waitFor([&] { return (*server)->connectionCount() == 0; }));
}

MULTIROOM_TEST(audio_server_closes_a_connection_no_room_claims)
{
    // A session this server does not have. The handler says no and the
    // connection goes, rather than being kept for a room that may never exist.
    const SystemClock clock;
    Attachments attachments;

    auto server = AudioServer::start(clock, settingsFor(attachments, false));
    CHECK_OK(server);

    LoopbackClient client((*server)->port());
    client.send(matp::MessageType::StreamAttach,
                matp::makeStreamAttach(SessionId(7, 9).toHex(), identityOf("kitchen")));

    CHECK(waitFor([&] { return (*server)->rejectedCount() == 1ULL; }));
    CHECK_EQ((*server)->attachedCount(), 0ULL);
    CHECK(client.closedByPeer());
}

MULTIROOM_TEST(audio_server_refuses_anything_that_is_not_a_stream_attach)
{
    // The audio connection carries one message and then audio in the other
    // direction. A client talking on the wrong socket is told nothing and
    // dropped, because there is no session to attribute an answer to.
    const SystemClock clock;
    Attachments attachments;

    auto server = AudioServer::start(clock, settingsFor(attachments));
    CHECK_OK(server);

    LoopbackClient hello((*server)->port());
    hello.send(matp::MessageType::Hello, matp::makeHello(identityOf("livingroom")));
    CHECK(hello.closedByPeer());

    LoopbackClient nonsense((*server)->port());
    nonsense.send(std::string("this is not a MATP frame at all, not even close"));
    CHECK(nonsense.closedByPeer());

    // And an attach that names something that is not a session id.
    LoopbackClient unnamed((*server)->port());
    unnamed.send(matp::MessageType::StreamAttach,
                 matp::makeStreamAttach("not-a-session", identityOf("livingroom")));
    CHECK(unnamed.closedByPeer());

    CHECK(waitFor([&] { return (*server)->failedCount() == 3ULL; }));
    CHECK_EQ(attachments.count(), size_t(0));
}

MULTIROOM_TEST(audio_server_closes_a_connection_that_never_speaks)
{
    // The one thing a stranger can cost this process on this port. A fresh
    // connection carries no identity, so until STREAM_ATTACH arrives there is
    // nothing to attribute it to and nothing to tell it.
    const SystemClock clock;
    Attachments attachments;

    AudioServerSettings settings = settingsFor(attachments);
    settings.attachTimeoutNs = 50000000ULL; // 50 ms, far outside anything else here

    auto server = AudioServer::start(clock, settings);
    CHECK_OK(server);

    LoopbackClient silent((*server)->port());
    CHECK(silent.closedByPeer());
    CHECK(waitFor([&] { return (*server)->failedCount() == 1ULL; }));
    CHECK_EQ(attachments.count(), size_t(0));
}

MULTIROOM_TEST(audio_server_refuses_more_unattached_connections_than_it_will_watch)
{
    // Bounded, like every other queue here, and bounded at the door: sockets
    // that have attached have left, so this ceiling is only ever about peers
    // that have not spoken yet.
    const SystemClock clock;
    Attachments attachments;

    AudioServerSettings settings = settingsFor(attachments);
    settings.maxConnections = 2;
    settings.attachTimeoutNs = 3600ULL * kNanosecondsPerSecond; // never, during this test

    auto server = AudioServer::start(clock, settings);
    CHECK_OK(server);

    LoopbackClient first((*server)->port());
    LoopbackClient second((*server)->port());
    CHECK(waitFor([&] { return (*server)->connectionCount() == 2; }));

    LoopbackClient third((*server)->port());
    CHECK(waitFor([&] { return (*server)->refusedCount() == 1ULL; }));
    CHECK_EQ((*server)->acceptedCount(), 2ULL);

    // And attaching one frees the slot, because an attached connection is no
    // longer this listener's.
    first.send(matp::MessageType::StreamAttach,
               matp::makeStreamAttach(testSessionId().toHex(), identityOf("livingroom")));
    CHECK(waitFor([&] { return (*server)->connectionCount() == 1; }));
}

MULTIROOM_TEST(audio_server_hands_over_a_sink_that_reaches_the_client)
{
    // The handover is a socket, and what comes out the other end of it is
    // whatever the room writes. This is the one place that proves the two are
    // the same connection.
    const SystemClock clock;
    Attachments attachments;

    auto server = AudioServer::start(clock, settingsFor(attachments));
    CHECK_OK(server);

    LoopbackClient client((*server)->port());
    client.send(matp::MessageType::StreamAttach,
                matp::makeStreamAttach(testSessionId().toHex(), identityOf("livingroom")));
    CHECK(waitFor([&] { return attachments.count() == 1; }));

    const std::string payload = "the room's own bytes";
    {
        std::lock_guard<std::mutex> lock(attachments.mutex);
        const Result<size_t, PacketSinkError> written = attachments.sinks[0]->write(
            std::span<const std::byte>(reinterpret_cast<const std::byte*>(payload.data()),
                                       payload.size()));
        CHECK_OK(written);
        CHECK_EQ(*written, payload.size());
    }

    // Read it back off the socket the client is still holding.
    std::string received;
    CHECK(client.receiveInto(received, payload.size()));
    CHECK_EQ(received, payload);

    // And closing the sink closes that connection, which is the room's end of
    // it and the only end that decides.
    {
        std::lock_guard<std::mutex> lock(attachments.mutex);
        attachments.sinks[0]->close();
    }
    CHECK(client.closedByPeer());
}

// --- phase 35: an attach that comes before its room is held waits ------------

namespace {

// A probe the test turns: what the listener is told about every attachment.
struct Readiness
{
    std::atomic<AttachReadiness> answer{AttachReadiness::Later};

    StreamAttachProbe probe()
    {
        return [this](const StreamAttachment&) { return answer.load(); };
    }
};

} // namespace

MULTIROOM_TEST(audio_server_keeps_an_early_attach_open_until_its_room_is_held)
{
    // A client attaches as soon as it holds a prepared stream, which may be
    // before a zone has made its room a member -- or while it is in no zone.
    // That is "not yet": the connection waits, past the attach deadline, and
    // is handed over whole the moment the room is held.
    const SystemClock clock;
    Attachments attachments;
    Readiness readiness;

    AudioServerSettings settings = settingsFor(attachments);
    settings.attachTimeoutNs = 50000000ULL; // 50 ms: a waiting connection is not held to it
    settings.readiness = readiness.probe();

    auto server = AudioServer::start(clock, settings);
    CHECK_OK(server);

    LoopbackClient client((*server)->port());
    client.send(matp::MessageType::StreamAttach,
                matp::makeStreamAttach(testSessionId().toHex(), identityOf("livingroom")));
    CHECK(waitFor([&] { return (*server)->waitingCount() == 1; }));

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK_EQ((*server)->waitingCount(), size_t(1));
    CHECK_EQ((*server)->failedCount(), 0ULL);
    CHECK_EQ((*server)->rejectedCount(), 0ULL);
    CHECK_EQ(attachments.count(), size_t(0));

    readiness.answer = AttachReadiness::Now;
    CHECK(waitFor([&] { return attachments.count() == 1; }));
    CHECK(waitFor([&] { return (*server)->waitingCount() == 0; }));
    CHECK_EQ((*server)->attachedCount(), 1ULL);

    // The same connection: what the room writes reaches the client.
    const std::string payload = "after waiting";
    {
        std::lock_guard<std::mutex> lock(attachments.mutex);
        const Result<size_t, PacketSinkError> written = attachments.sinks[0]->write(
            std::span<const std::byte>(reinterpret_cast<const std::byte*>(payload.data()),
                                       payload.size()));
        CHECK_OK(written);
    }
    std::string received;
    CHECK(client.receiveInto(received, payload.size()));
    CHECK_EQ(received, payload);
}

MULTIROOM_TEST(audio_server_closes_an_attach_whose_session_is_gone_even_after_it_waited)
{
    // Never is still never: a session this server does not have is closed at
    // once, and one that ends while its connection waits is closed then.
    const SystemClock clock;
    Attachments attachments;
    Readiness readiness;
    readiness.answer = AttachReadiness::Never;

    AudioServerSettings settings = settingsFor(attachments);
    settings.readiness = readiness.probe();

    auto server = AudioServer::start(clock, settings);
    CHECK_OK(server);

    LoopbackClient stranger((*server)->port());
    stranger.send(matp::MessageType::StreamAttach,
                  matp::makeStreamAttach(SessionId(7, 9).toHex(), identityOf("kitchen")));
    CHECK(waitFor([&] { return (*server)->rejectedCount() == 1ULL; }));
    CHECK(stranger.closedByPeer());

    readiness.answer = AttachReadiness::Later;
    LoopbackClient waiting((*server)->port());
    waiting.send(matp::MessageType::StreamAttach,
                 matp::makeStreamAttach(testSessionId().toHex(), identityOf("livingroom")));
    CHECK(waitFor([&] { return (*server)->waitingCount() == 1; }));

    readiness.answer = AttachReadiness::Never;
    CHECK(waitFor([&] { return (*server)->rejectedCount() == 2ULL; }));
    CHECK(waiting.closedByPeer());
    CHECK_EQ((*server)->waitingCount(), size_t(0));
    CHECK_EQ(attachments.count(), size_t(0));
}

MULTIROOM_TEST(audio_server_drops_a_waiting_connection_the_client_closes)
{
    // A client that prepares a new stream opens a new connection for it and
    // closes the one that was waiting. That is not a failure.
    const SystemClock clock;
    Attachments attachments;
    Readiness readiness;

    AudioServerSettings settings = settingsFor(attachments);
    settings.readiness = readiness.probe();

    auto server = AudioServer::start(clock, settings);
    CHECK_OK(server);

    LoopbackClient client((*server)->port());
    client.send(matp::MessageType::StreamAttach,
                matp::makeStreamAttach(testSessionId().toHex(), identityOf("livingroom")));
    CHECK(waitFor([&] { return (*server)->waitingCount() == 1; }));

    client.close();
    CHECK(waitFor([&] { return (*server)->connectionCount() == 0; }));
    CHECK_EQ((*server)->waitingCount(), size_t(0));
    CHECK_EQ((*server)->failedCount(), 0ULL);
    CHECK_EQ((*server)->rejectedCount(), 0ULL);
}
