#include "discovery/ClientControl.h"

#include "net/Socket.h"
#include "util/Json.h"

#include <chrono>

namespace multiroom {
namespace {

// Attachment and public network/LED settings fit comfortably in this cap.
constexpr size_t kMaxResponseBytes = 4096;

bool parseIpv4(const std::string& text, uint16_t port, sockaddr_in& out)
{
    out = sockaddr_in{};
    out.sin_family = AF_INET;
    out.sin_port = htons(port);
    return ::inet_pton(AF_INET, text.c_str(), &out.sin_addr) == 1;
}

// Waits for `socket` to be readable or writable, up to `deadline`. False on
// the deadline or an error.
bool waitFor(net::Socket socket, bool forWrite,
             std::chrono::steady_clock::time_point deadline)
{
    const auto left = std::chrono::duration_cast<std::chrono::microseconds>(
        deadline - std::chrono::steady_clock::now());
    if (left.count() <= 0 || !net::selectable(socket))
        return false;
    timeval timeout{};
    timeout.tv_sec = static_cast<long>(left.count() / 1000000);
    timeout.tv_usec = static_cast<long>(left.count() % 1000000);

    fd_set ready;
    fd_set failed; // Windows reports a refused connect here, not as writable
    FD_ZERO(&ready);
    FD_ZERO(&failed);
    FD_SET(socket, &ready);
    FD_SET(socket, &failed);
    const int count = net::waitReady(socket, forWrite ? nullptr : &ready,
                                     forWrite ? &ready : nullptr, &failed, timeout);
    return count > 0 && FD_ISSET(socket, &ready) && !FD_ISSET(socket, &failed);
}

// Whether `target` is one of this machine's own addresses: only those can be
// bound to.
bool isOwnAddress(const sockaddr_in& target)
{
    const net::Socket probe = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (probe == net::kInvalidSocket)
        return false;
    sockaddr_in local = target;
    local.sin_port = 0;
    const bool own = ::bind(probe, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) == 0;
    net::closeSocket(probe);
    return own;
}

} // namespace

bool sendClientControl(const std::string& address, uint16_t controlPort,
                       const std::string& command, const json::Value& params,
                       uint32_t timeoutMs, std::string& response, std::string& error)
{
    net::Library library;
    if (!library.isReady())
    {
        error = library.error();
        return false;
    }

    sockaddr_in target{};
    if (!parseIpv4(address, controlPort, target))
    {
        error = "\"" + address + "\" is not a numeric IPv4 address";
        return false;
    }

    // A client on this machine beacons from one of its addresses (any
    // interface it broadcasts on, e.g. WSL's), where a control API bound to
    // loopback does not answer; loopback reaches it however it is bound.
    if (isOwnAddress(target))
        target.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const std::string where = address + ":" + std::to_string(controlPort);
    const net::Socket socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket == net::kInvalidSocket || !net::setNonBlocking(socket))
    {
        error = net::errorMessage(net::lastError());
        net::closeSocket(socket);
        return false;
    }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    if (::connect(socket, reinterpret_cast<const sockaddr*>(&target), sizeof(target)) != 0)
    {
        const int code = net::lastError();
#if defined(_WIN32)
        const bool pending = code == WSAEWOULDBLOCK;
#else
        const bool pending = code == EINPROGRESS;
#endif
        std::string why = pending ? "no answer" : net::errorMessage(code);
        if (pending && waitFor(socket, true, deadline))
        {
            // Writable is not connected on POSIX: a refusal is reported as
            // writable with the error pending on the socket.
            int failure = 0;
#if defined(_WIN32)
            int length = static_cast<int>(sizeof(failure));
#else
            socklen_t length = static_cast<socklen_t>(sizeof(failure));
#endif
            if (::getsockopt(socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&failure),
                             &length) == 0 &&
                failure == 0)
                why.clear();
            else
                why = net::errorMessage(failure);
        }
        if (!why.empty())
        {
            error = "could not connect to " + where + ": " + why;
            net::closeSocket(socket);
            return false;
        }
    }

    json::Value request = json::Value::makeObject();
    request["command"] = json::Value(command);
    request["params"] = params;
    const std::string line = json::serialize(request) + "\n";

    // The whole request, then the answer, both within a fresh deadline.
    deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    size_t sent = 0;
    while (sent < line.size())
    {
        const int count = net::transmit(socket, line.data() + sent,
                                        static_cast<int>(line.size() - sent));
        if (count > 0)
            sent += static_cast<size_t>(count);
        else if (count < 0 && net::wouldBlock(net::lastError()) && waitFor(socket, true, deadline))
            continue;
        else
        {
            error = "could not send to " + where;
            net::closeSocket(socket);
            return false;
        }
    }

    std::string received;
    char buffer[512];
    while (received.find('\n') == std::string::npos)
    {
        if (received.size() > kMaxResponseBytes)
        {
            error = where + " answered with something that is not a response";
            net::closeSocket(socket);
            return false;
        }
        if (!waitFor(socket, false, deadline))
        {
            error = where + " did not answer";
            net::closeSocket(socket);
            return false;
        }
        const int count = net::receive(socket, buffer, sizeof(buffer));
        if (count > 0)
            received.append(buffer, static_cast<size_t>(count));
        else if (count == 0 || !net::wouldBlock(net::lastError()))
        {
            error = where + " closed the connection without an answer";
            net::closeSocket(socket);
            return false;
        }
    }
    net::closeSocket(socket);

    response = received.substr(0, received.find('\n'));
    if (response.size() > kMaxResponseBytes)
    {
        error = where + " answered with something that is not a response";
        return false;
    }
    if (!response.empty() && response.back() == '\r')
        response.pop_back();
    return true;
}

std::string localAddressToward(const std::string& address)
{
    net::Library library;
    sockaddr_in target{};
    if (!library.isReady() || !parseIpv4(address, 9, target))
        return {};

    // A UDP connect sends nothing; it only has the system pick the route, and
    // with it the local address a datagram would leave from.
    const net::Socket socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket == net::kInvalidSocket)
        return {};
    std::string out;
    sockaddr_in local{};
#if defined(_WIN32)
    int length = static_cast<int>(sizeof(local));
#else
    socklen_t length = static_cast<socklen_t>(sizeof(local));
#endif
    char text[INET_ADDRSTRLEN] = {};
    if (::connect(socket, reinterpret_cast<const sockaddr*>(&target), sizeof(target)) == 0 &&
        ::getsockname(socket, reinterpret_cast<sockaddr*>(&local), &length) == 0 &&
        ::inet_ntop(AF_INET, &local.sin_addr, text, sizeof(text)) != nullptr)
        out = text;
    net::closeSocket(socket);
    return out;
}

} // namespace multiroom
