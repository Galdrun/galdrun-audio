#include "TestListener.h"
#include "TestSupport.h"

#include "net/ServerConnection.h"
#include "net/Socket.h" // not a public header; the test plays the server with it

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#if defined(__linux__) && !defined(ESP_PLATFORM)
#include <fcntl.h>
#endif

using namespace multiroom;
using multiroomtest::TestListener;
using multiroomtest::waitUntil;

namespace {

// Collects what the connection hands up, from the connection's thread.
class Inbox
{
public:
    ServerConnection::DataHandler handler()
    {
        return [this](const void* data, size_t size) {
            std::lock_guard<std::mutex> lock(mutex_);
            text_.append(static_cast<const char*>(data), size);
        };
    }

    std::string text() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return text_;
    }

private:
    mutable std::mutex mutex_;
    std::string text_;
};

} // namespace

#if defined(__linux__) && !defined(ESP_PLATFORM)
MULTIROOM_TEST(client_sockets_are_not_inherited_by_a_board_helper)
{
    const auto socket = ::socket(AF_INET, SOCK_STREAM, 0);
    CHECK(socket != net::kInvalidSocket);
    CHECK(net::setNonBlocking(socket));
    CHECK((::fcntl(socket, F_GETFD, 0) & FD_CLOEXEC) != 0);
    CHECK((::fcntl(socket, F_GETFL, 0) & O_NONBLOCK) != 0);
    net::closeSocket(socket);
}
#endif

MULTIROOM_TEST(server_connection_stays_idle_without_an_address)
{
    net::Library library;
    CHECK(library.isReady());

    ServerConnection connection;
    std::string error;
    CHECK(connection.start(ServerConnection::DataHandler(), error));

    // Nothing configured means nothing attempted: a client without a server
    // must not produce connection traffic at all.
    CHECK(waitUntil([&] { return connection.state() == ServerConnectionState::NotConfigured; },
                    1000));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK_EQ(connection.connectAttempts(), static_cast<uint64_t>(0));
    CHECK_EQ(connection.server(), std::string());

    // A typo is refused outright rather than turned into a connection attempt.
    CHECK(!connection.setServer("not-an-address", error));
    CHECK(!error.empty());
    CHECK(!connection.setServer("host:0", error));
    CHECK_EQ(connection.server(), std::string());
    CHECK_EQ(connection.connectAttempts(), static_cast<uint64_t>(0));

    connection.stop();
    connection.stop(); // idempotent
}

MULTIROOM_TEST(server_connection_connects_and_carries_bytes_both_ways)
{
    net::Library library;
    TestListener listener;
    CHECK(listener.ready());

    Inbox inbox;
    ServerConnection connection;
    std::string error;
    CHECK(connection.setServer(listener.address(), error));
    CHECK(connection.start(inbox.handler(), error));

    CHECK(listener.acceptOne(3000));
    CHECK(waitUntil([&] { return connection.state() == ServerConnectionState::Connected; }, 3000));
    CHECK_EQ(connection.connectionCount(), static_cast<uint64_t>(1));
    CHECK_EQ(connection.lastError(), std::string());
    // Loopback is neither Ethernet nor Wi-Fi.
    CHECK_EQ(connection.networkLink(), std::string());

    const std::string request = "{\"type\":\"HELLO\"}\n";
    CHECK(connection.send(request.data(), request.size()));
    CHECK_EQ(listener.receive(3000), request);

    CHECK(listener.send("HELLO_ACK"));
    CHECK(waitUntil([&] { return inbox.text() == "HELLO_ACK"; }, 3000));

    connection.stop();
}

MULTIROOM_TEST(server_connection_reconnects_after_the_server_drops_it)
{
    net::Library library;
    TestListener listener;
    CHECK(listener.ready());

    ServerConnection connection;
    std::string error;
    CHECK(connection.setServer(listener.address(), error));
    CHECK(connection.start(ServerConnection::DataHandler(), error));

    CHECK(listener.acceptOne(3000));
    CHECK(waitUntil([&] { return connection.state() == ServerConnectionState::Connected; }, 3000));

    // The server goes away mid-session, which is the common case: a restart.
    listener.dropClient();
    CHECK(waitUntil([&] { return connection.state() != ServerConnectionState::Connected; }, 3000));

    // The first backoff step is a second, so this must not take much longer.
    CHECK(listener.acceptOne(5000));
    CHECK(waitUntil([&] { return connection.state() == ServerConnectionState::Connected; }, 3000));
    CHECK_EQ(connection.connectionCount(), static_cast<uint64_t>(2));

    connection.stop();
}

MULTIROOM_TEST(server_connection_backs_off_instead_of_retrying_in_a_loop)
{
    net::Library library;

    // A port that was bound and then released: connecting to it is refused
    // immediately, which is exactly the case a naive retry loop spins on.
    std::string address;
    {
        TestListener listener;
        CHECK(listener.ready());
        address = listener.address();
    }

    ServerConnection connection;
    std::string error;
    CHECK(connection.setServer(address, error));
    CHECK(connection.start(ServerConnection::DataHandler(), error));

    CHECK(waitUntil([&] { return connection.connectAttempts() >= 1; }, 3000));
    CHECK(waitUntil([&] { return !connection.lastError().empty(); }, 3000));

    // Two and a half seconds allow the first two backoff steps (1 s and 2 s),
    // so anything above three attempts means the delay is not being honoured.
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));
    CHECK(connection.connectAttempts() <= 3);
    CHECK(connection.state() != ServerConnectionState::Connected);
    CHECK_EQ(connection.connectionCount(), static_cast<uint64_t>(0));

    // Sending without a connection fails rather than queueing up for a server
    // that may never arrive.
    CHECK(!connection.send("x", 1));

    connection.stop();
}

MULTIROOM_TEST(server_connection_switches_to_a_new_address_at_once)
{
    net::Library library;
    TestListener first;
    TestListener second;
    CHECK(first.ready());
    CHECK(second.ready());

    ServerConnection connection;
    std::string error;
    CHECK(connection.setServer(first.address(), error));
    CHECK(connection.start(ServerConnection::DataHandler(), error));

    CHECK(first.acceptOne(3000));
    CHECK(waitUntil([&] { return connection.state() == ServerConnectionState::Connected; }, 3000));

    // Setting the address it already has must not disturb the connection.
    CHECK(connection.setServer(first.address(), error));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(connection.state() == ServerConnectionState::Connected);
    CHECK_EQ(connection.connectionCount(), static_cast<uint64_t>(1));

    // A new address drops what is open and goes there without waiting out a
    // backoff, which is what the control API's setServer needs.
    CHECK(connection.setServer(second.address(), error));
    CHECK(second.acceptOne(3000));
    CHECK(waitUntil([&] { return connection.state() == ServerConnectionState::Connected; }, 3000));
    CHECK_EQ(connection.server(), second.address());
    CHECK_EQ(connection.connectionCount(), static_cast<uint64_t>(2));

    // Clearing it disconnects and goes back to idle.
    CHECK(connection.setServer("", error));
    CHECK(waitUntil([&] { return connection.state() == ServerConnectionState::NotConfigured; },
                    3000));
    CHECK(!connection.send("x", 1));

    connection.stop();
}

MULTIROOM_TEST(server_connection_state_names_are_stable)
{
    // These strings are part of the control API's status response.
    CHECK_EQ(std::string(serverConnectionStateName(ServerConnectionState::NotConfigured)),
             std::string("notConfigured"));
    CHECK_EQ(std::string(serverConnectionStateName(ServerConnectionState::Disconnected)),
             std::string("disconnected"));
    CHECK_EQ(std::string(serverConnectionStateName(ServerConnectionState::Connecting)),
             std::string("connecting"));
    CHECK_EQ(std::string(serverConnectionStateName(ServerConnectionState::Connected)),
             std::string("connected"));
}
