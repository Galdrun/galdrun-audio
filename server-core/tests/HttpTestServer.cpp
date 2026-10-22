#include "HttpTestServer.h"

#include "TestSupport.h"
#include "net/Socket.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <string_view>
#include <utility>

namespace multiroomtest {

using multiroom::net::Socket;

struct HttpTestServer::Library
{
    multiroom::net::Library library;
};

namespace {

constexpr int kPollMs = 20;
constexpr auto kIdleLimit = std::chrono::seconds(10);

Socket socketOf(uintptr_t value)
{
    return static_cast<Socket>(value);
}

// Waits up to kPollMs for `socket` to become readable or writable.
bool ready(Socket socket, bool forWrite)
{
    fd_set set;
    FD_ZERO(&set);
    FD_SET(socket, &set);
    timeval timeout{};
    timeout.tv_usec = kPollMs * 1000;
#if defined(_WIN32)
    const int highest = 0;
#else
    const int highest = static_cast<int>(socket) + 1;
#endif
    return ::select(highest, forWrite ? nullptr : &set, forWrite ? &set : nullptr, nullptr,
                    &timeout) > 0;
}

bool sendAll(std::stop_token& stop, Socket socket, std::string_view bytes)
{
    size_t sent = 0;
    const auto giveUp = std::chrono::steady_clock::now() + kIdleLimit;
    while (sent < bytes.size())
    {
        if (stop.stop_requested() || std::chrono::steady_clock::now() > giveUp)
            return false;
        if (!ready(socket, true))
            continue;
        const int chunk = static_cast<int>(std::min<size_t>(bytes.size() - sent, 64 * 1024));
        const int written = multiroom::net::transmit(socket, bytes.data() + sent, chunk);
        if (written < 0)
        {
            if (multiroom::net::wouldBlock(multiroom::net::lastError()))
                continue;
            return false; // the client closed its end, which a seek does
        }
        sent += static_cast<size_t>(written);
    }
    return true;
}

// The value of `name:` in a header block, or empty. Case-insensitive on the
// name, as HTTP is.
std::string headerValue(const std::string& head, std::string_view name)
{
    size_t lineStart = head.find("\r\n");
    while (lineStart != std::string::npos && lineStart + 2 < head.size())
    {
        const size_t begin = lineStart + 2;
        const size_t end = head.find("\r\n", begin);
        const std::string line = head.substr(begin, end - begin);
        const size_t colon = line.find(':');
        if (colon == name.size())
        {
            bool same = true;
            for (size_t index = 0; index < name.size(); ++index)
                same = same && std::tolower(static_cast<unsigned char>(line[index])) ==
                                   std::tolower(static_cast<unsigned char>(name[index]));
            if (same)
            {
                const size_t value = line.find_first_not_of(' ', colon + 1);
                return value == std::string::npos ? std::string{} : line.substr(value);
            }
        }
        lineStart = end;
    }
    return {};
}

} // namespace

HttpTestServer::HttpTestServer() : library_(std::make_unique<Library>())
{
    CHECK(library_->library.isReady());

    std::string error;
    const Socket listener = multiroom::net::listenOnPort(0, 16, error);
    CHECK(listener != multiroom::net::kInvalidSocket);
    listener_ = static_cast<uintptr_t>(listener);
    CHECK(multiroom::net::boundPort(listener, port_));

    acceptor_ = std::jthread([this](std::stop_token stop) { accept(stop); });
}

HttpTestServer::~HttpTestServer()
{
    acceptor_.request_stop();
    acceptor_.join();

    std::vector<std::jthread> connections;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        connections.swap(connections_);
    }
    connections.clear(); // stops and joins each

    if (!shut_)
        multiroom::net::closeSocket(socketOf(listener_));
}

std::string HttpTestServer::url(std::string_view path) const
{
    return "http://127.0.0.1:" + std::to_string(port_) + std::string(path);
}

void HttpTestServer::serve(const std::string& path, std::vector<std::byte> body)
{
    serve(path, std::move(body), Faults{});
}

void HttpTestServer::serve(const std::string& path, std::vector<std::byte> body, Faults faults)
{
    std::lock_guard<std::mutex> lock(mutex_);
    routes_[path] = Route{std::move(body), {}, faults};
}

void HttpTestServer::redirect(const std::string& path, const std::string& location)
{
    std::lock_guard<std::mutex> lock(mutex_);
    routes_[path] = Route{{}, location, {}};
}

void HttpTestServer::requireBearer(std::string token)
{
    std::lock_guard<std::mutex> lock(mutex_);
    bearer_ = std::move(token);
}

size_t HttpTestServer::unauthorised() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return unauthorised_;
}

bool HttpTestServer::isShut() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return shut_;
}

void HttpTestServer::reopen()
{
    std::string error;
    const Socket listener = multiroom::net::listenOnPort(port_, 16, error);
    CHECK(listener != multiroom::net::kInvalidSocket);

    std::lock_guard<std::mutex> lock(mutex_);
    CHECK(shut_);
    listener_ = static_cast<uintptr_t>(listener);
    shutRequested_ = false;
    shut_ = false;
}

std::vector<std::string> HttpTestServer::requested() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return requested_;
}

std::vector<size_t> HttpTestServer::rangesAsked(const std::string& path) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = ranges_.find(path);
    return found == ranges_.end() ? std::vector<size_t>{} : found->second;
}

void HttpTestServer::accept(std::stop_token stop)
{
    while (!stop.stop_requested())
    {
        Socket listener = multiroom::net::kInvalidSocket;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (shutRequested_ && !shut_)
            {
                multiroom::net::closeSocket(socketOf(listener_));
                shut_ = true;
                shutDone_.notify_all();
            }
            if (!shut_)
                listener = socketOf(listener_);
        }
        if (listener == multiroom::net::kInvalidSocket)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(kPollMs));
            continue;
        }

        if (!ready(listener, false))
            continue;

        bool none = false;
        std::string error;
        const Socket connection = multiroom::net::acceptOne(listener, none, error);
        if (connection == multiroom::net::kInvalidSocket)
            continue;

        std::lock_guard<std::mutex> lock(mutex_);
        connections_.emplace_back([this, connection](std::stop_token own) {
            answer(own, static_cast<uintptr_t>(connection));
        });
    }
}

void HttpTestServer::answer(std::stop_token stop, uintptr_t value)
{
    const Socket socket = socketOf(value);

    std::string head;
    const auto giveUp = std::chrono::steady_clock::now() + kIdleLimit;
    while (head.find("\r\n\r\n") == std::string::npos)
    {
        if (stop.stop_requested() || std::chrono::steady_clock::now() > giveUp ||
            head.size() > 64 * 1024)
        {
            multiroom::net::closeSocket(socket);
            return;
        }
        if (!ready(socket, false))
            continue;
        char buffer[4096];
        const int got = multiroom::net::receive(socket, buffer, sizeof(buffer));
        if (got == 0 || (got < 0 && !multiroom::net::wouldBlock(multiroom::net::lastError())))
        {
            multiroom::net::closeSocket(socket);
            return;
        }
        if (got > 0)
            head.append(buffer, static_cast<size_t>(got));
    }

    // "GET /path HTTP/1.1"
    const size_t firstSpace = head.find(' ');
    const size_t secondSpace = head.find(' ', firstSpace + 1);
    const std::string path = head.substr(firstSpace + 1, secondSpace - firstSpace - 1);

    const std::string range = headerValue(head, "Range");
    size_t asked = 0;
    if (range.starts_with("bytes="))
        asked = static_cast<size_t>(std::stoull(range.substr(6)));

    const std::string authorization = headerValue(head, "Authorization");

    Route route;
    bool found = false;
    bool failNow = false;
    bool refused = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        requested_.push_back(path);
        refused = !bearer_.empty() && authorization != "Bearer " + bearer_;
        if (refused)
            ++unauthorised_;
        else if (const auto it = routes_.find(path); it != routes_.end())
        {
            ranges_[path].push_back(asked);
            if (it->second.faults.failFirst > 0)
            {
                --it->second.faults.failFirst;
                failNow = true;
            }
            route = it->second;
            found = true;
        }
    }

    std::string response;
    std::string_view body;
    size_t from = 0;
    if (refused)
    {
        response = "HTTP/1.1 401 Unauthorized\r\nWWW-Authenticate: Bearer\r\n"
                   "Content-Length: 0\r\n";
    }
    else if (!found)
    {
        response = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n";
    }
    else if (!route.location.empty())
    {
        response = "HTTP/1.1 302 Found\r\nLocation: " + route.location +
                   "\r\nContent-Length: 0\r\n";
    }
    else if (failNow)
    {
        response = "HTTP/1.1 " + std::to_string(route.faults.failStatus) +
                   " Try Later\r\nContent-Length: 0\r\n";
    }
    else
    {
        const size_t size = route.body.size();
        from = std::min(asked, size);

        // The fault a resume must refuse: the whole file again, as if the
        // Range had not been sent.
        const bool whole = route.faults.rangeAs200AfterDrop && route.dropped && from > 0;
        if (whole)
            from = 0;

        body = std::string_view(reinterpret_cast<const char*>(route.body.data()) + from,
                                size - from);
        if (range.empty() || whole)
            response = "HTTP/1.1 200 OK\r\n";
        else
            response = "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes " +
                       std::to_string(from) + "-" + std::to_string(size == 0 ? 0 : size - 1) +
                       "/" + std::to_string(size) + "\r\n";
        response += "Accept-Ranges: bytes\r\nContent-Length: " + std::to_string(body.size()) +
                    "\r\n";
    }
    response += "Connection: close\r\n\r\n";

    if (!sendAll(stop, socket, response))
    {
        multiroom::net::closeSocket(socket);
        return;
    }

    // A break is spent only by a response that got as far as its byte.
    const size_t breakAt = route.faults.breakAt;
    if ((route.faults.drops > 0 || route.faults.stalls > 0) && !body.empty() &&
        from <= breakAt && breakAt < from + body.size())
    {
        if (!sendAll(stop, socket, body.substr(0, breakAt - from)))
        {
            multiroom::net::closeSocket(socket);
            return;
        }

        // Loopback absorbs a prefix whether or not anybody reads it. A
        // connection FFmpeg's probe has left closes at once, so one that is
        // still open a moment later is the one being read.
        const auto settleBy = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
        while (std::chrono::steady_clock::now() < settleBy)
        {
            if (!ready(socket, false))
                continue;
            char buffer[256];
            const int got = multiroom::net::receive(socket, buffer, sizeof(buffer));
            if (got == 0 || (got < 0 && !multiroom::net::wouldBlock(multiroom::net::lastError())))
            {
                multiroom::net::closeSocket(socket);
                return;
            }
        }

        bool drop = false;
        bool stall = false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            Route& live = routes_[path];
            if (live.faults.drops > 0)
            {
                --live.faults.drops;
                live.dropped = true;
                drop = true;

                // Nothing listening before this connection goes, so the
                // attempt that replaces it is refused.
                if (live.faults.shutAtDrop)
                {
                    shutRequested_ = true;
                    shutDone_.wait(lock, stop, [this] { return shut_; });
                }
            }
            else if (live.faults.stalls > 0)
            {
                --live.faults.stalls;
                stall = true;
            }
        }

        if (stall)
        {
            // Quiet, with the connection open, until the client gives up.
            const auto giveUpAt = std::chrono::steady_clock::now() + kIdleLimit;
            while (!stop.stop_requested() && std::chrono::steady_clock::now() < giveUpAt)
            {
                if (!ready(socket, false))
                    continue;
                char buffer[4096];
                const int got = multiroom::net::receive(socket, buffer, sizeof(buffer));
                if (got == 0 || (got < 0 && !multiroom::net::wouldBlock(multiroom::net::lastError())))
                    break;
            }
        }
        if (drop || stall)
        {
            multiroom::net::closeSocket(socket);
            return;
        }
        body = body.substr(breakAt - from);
    }

    if (sendAll(stop, socket, body))
    {
        // A graceful close: our half first, then wait for the client's, so
        // the end of the body is not overtaken by a reset.
#if defined(_WIN32)
        ::shutdown(socket, SD_SEND);
#else
        ::shutdown(socket, SHUT_WR);
#endif
        const auto closeBy = std::chrono::steady_clock::now() + kIdleLimit;
        while (!stop.stop_requested() && std::chrono::steady_clock::now() < closeBy)
        {
            if (!ready(socket, false))
                continue;
            char buffer[4096];
            const int got = multiroom::net::receive(socket, buffer, sizeof(buffer));
            if (got == 0 || (got < 0 && !multiroom::net::wouldBlock(multiroom::net::lastError())))
                break;
        }
    }

    multiroom::net::closeSocket(socket);
}

} // namespace multiroomtest
