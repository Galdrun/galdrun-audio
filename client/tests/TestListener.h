#pragma once

// The other end of a ServerConnection: a loopback listener on an ephemeral
// port, so a test never collides with a running client or with another test.
//
// Shared by the connection tests and the MATP handshake tests, the same way
// StubBackend.h is shared by the engine and supervisor tests.

#include "net/Socket.h" // not a public header; these tests play the server with it

#include <chrono>
#include <cstring>
#include <string>
#include <thread>

namespace multiroomtest {

// Polls a condition rather than sleeping for a fixed time: a slow machine then
// costs patience instead of a spurious failure.
template <typename Predicate>
bool waitUntil(Predicate predicate, int timeoutMs)
{
    for (int waited = 0; waited < timeoutMs; waited += 10)
    {
        if (predicate())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return predicate();
}

class TestListener
{
public:
    TestListener()
    {
        listener_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener_ == multiroom::net::kInvalidSocket)
            return;

        sockaddr_in address;
        std::memset(&address, 0, sizeof(address));
        address.sin_family = AF_INET;
        address.sin_port = 0;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        if (::bind(listener_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
            ::listen(listener_, 4) != 0)
        {
            multiroom::net::closeSocket(listener_);
            listener_ = multiroom::net::kInvalidSocket;
            return;
        }

        sockaddr_in bound;
        std::memset(&bound, 0, sizeof(bound));
#if defined(_WIN32)
        int length = static_cast<int>(sizeof(bound));
#else
        socklen_t length = static_cast<socklen_t>(sizeof(bound));
#endif
        if (::getsockname(listener_, reinterpret_cast<sockaddr*>(&bound), &length) == 0)
            port_ = ntohs(bound.sin_port);
    }

    ~TestListener()
    {
        dropClient();
        closeListener();
    }

    TestListener(const TestListener&) = delete;
    TestListener& operator=(const TestListener&) = delete;

    bool ready() const
    {
        return library_.isReady() && listener_ != multiroom::net::kInvalidSocket && port_ != 0;
    }
    uint16_t port() const { return port_; }

    std::string address() const { return "127.0.0.1:" + std::to_string(port_); }

    // Accepts one connection, waiting up to timeoutMs for it.
    bool acceptOne(int timeoutMs)
    {
        if (!waitReadable(listener_, timeoutMs))
            return false;
        dropClient();
        client_ = ::accept(listener_, nullptr, nullptr);
        return client_ != multiroom::net::kInvalidSocket;
    }

    bool send(const std::string& text)
    {
        return multiroom::net::transmit(client_, text.data(), static_cast<int>(text.size())) ==
               static_cast<int>(text.size());
    }

    // Reads whatever has arrived within the timeout; empty on timeout. Sized
    // for a MATP capability report, which is the largest thing a client sends
    // on this connection.
    std::string receive(int timeoutMs)
    {
        if (!waitReadable(client_, timeoutMs))
            return std::string();
        char buffer[8192];
        const int received =
            multiroom::net::receive(client_, buffer, static_cast<int>(sizeof(buffer)));
        if (received <= 0)
            return std::string();
        return std::string(buffer, static_cast<size_t>(received));
    }

    void dropClient()
    {
        if (client_ != multiroom::net::kInvalidSocket)
        {
            multiroom::net::closeSocket(client_);
            client_ = multiroom::net::kInvalidSocket;
        }
    }

    // Frees the port, so a later connect to it is refused rather than accepted.
    void closeListener()
    {
        if (listener_ != multiroom::net::kInvalidSocket)
        {
            multiroom::net::closeSocket(listener_);
            listener_ = multiroom::net::kInvalidSocket;
        }
    }

private:
    static bool waitReadable(multiroom::net::Socket socket, int timeoutMs)
    {
        if (socket == multiroom::net::kInvalidSocket)
            return false;
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(socket, &readable);

        timeval timeout;
        timeout.tv_sec = timeoutMs / 1000;
        timeout.tv_usec = (timeoutMs % 1000) * 1000;

        int highest = 0;
#if !defined(_WIN32)
        highest = static_cast<int>(socket);
#endif
        return ::select(highest + 1, &readable, nullptr, nullptr, &timeout) > 0;
    }

    // First, and owned here: on Windows a socket cannot be created before
    // WSAStartup, and this class is constructed before the client under test
    // brings its own copy up. WSAStartup is reference counted, so nesting it
    // with the connection's own library is safe.
    multiroom::net::Library library_;

    multiroom::net::Socket listener_ = multiroom::net::kInvalidSocket;
    multiroom::net::Socket client_ = multiroom::net::kInvalidSocket;
    uint16_t port_ = 0;
};

} // namespace multiroomtest
