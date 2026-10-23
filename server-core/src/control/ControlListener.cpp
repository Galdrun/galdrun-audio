#include "control/ControlListener.h"

#include "net/Socket.h"
#include "util/Logger.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <deque>
#include <list>
#include <map>
#include <mutex>
#include <thread>
#include <utility>

namespace multiroom {
namespace {

// One high-end server, and room for a person with netcat beside it. The cap
// exists so a misbehaving script cannot open sockets until the process runs
// out of descriptors.
constexpr size_t kMaxConnections = 8;

// Every command fits in a fraction of this; a caller past it is broken, not
// verbose.
constexpr size_t kMaxRequestBytes = 64 * 1024;

// A caller that stops reading while it keeps asking. `status` on a full house
// is the largest response by far and is well under this.
constexpr size_t kMaxPendingResponseBytes = 4 * 1024 * 1024;

constexpr size_t kReadChunkBytes = 4096;

// Only how often the loop notices a stop request; nothing here is a protocol
// timeout.
constexpr int kSelectTimeoutMs = 50;

} // namespace

const char* errorName(ControlListenerError error)
{
    switch (error)
    {
    case ControlListenerError::SocketLibrary:
        return "SocketLibrary";
    case ControlListenerError::BindFailed:
        return "BindFailed";
    }
    return "Unknown";
}

// ---------------------------------------------------------------------------

class ControlListener::Impl
{
public:
    explicit Impl(Handler handler) : handler_(std::move(handler)) {}

    ~Impl()
    {
        thread_.request_stop();
        if (thread_.joinable())
            thread_.join();
        for (Connection& connection : connections_)
            net::closeSocket(connection.socket);
        net::closeSocket(listener_);
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    // getaddrinfo rather than inet_pton, so an IPv6 address needs no second
    // path; AI_NUMERICHOST, because a bind address that triggered a DNS
    // lookup would be one nobody could reason about.
    [[nodiscard]] Status<ControlListenerError> bind(const ControlListenerSettings& settings,
                                                    uint16_t& portOut)
    {
        if (!library_.isReady())
            return fail(ControlListenerError::SocketLibrary, library_.error());

        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        hints.ai_flags = AI_PASSIVE | AI_NUMERICHOST;

        const std::string service = std::to_string(settings.port);
        addrinfo* resolved = nullptr;
        if (::getaddrinfo(settings.bindAddress.c_str(), service.c_str(), &hints, &resolved) != 0 ||
            resolved == nullptr)
        {
            return fail(ControlListenerError::BindFailed,
                        "\"" + settings.bindAddress + "\" is not a numeric address");
        }

        std::string lastError = "no usable address";
        for (addrinfo* candidate = resolved; candidate != nullptr; candidate = candidate->ai_next)
        {
            const net::Socket socket =
                ::socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
            if (socket == net::kInvalidSocket)
            {
                lastError = net::errorMessage(net::lastError());
                continue;
            }

#if !defined(_WIN32)
            // A restart inside TIME_WAIT would otherwise fail to bind. Not on
            // Windows, where the option lets another process take a port that
            // is still in use rather than only reuse a dead one.
            const int reuse = 1;
            ::setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif
            if (::bind(socket, candidate->ai_addr, static_cast<int>(candidate->ai_addrlen)) == 0 &&
                ::listen(socket, 4) == 0 && net::setNonBlocking(socket))
            {
                listener_ = socket;
                break;
            }
            lastError = net::errorMessage(net::lastError());
            net::closeSocket(socket);
        }
        ::freeaddrinfo(resolved);

        if (listener_ == net::kInvalidSocket)
        {
            return fail(ControlListenerError::BindFailed,
                        settings.bindAddress + ":" + service + ": " + lastError);
        }

        sockaddr_storage address{};
#if defined(_WIN32)
        int length = static_cast<int>(sizeof(address));
#else
        socklen_t length = static_cast<socklen_t>(sizeof(address));
#endif
        if (::getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length) != 0)
            return fail(ControlListenerError::BindFailed, "could not read back the bound port");
        portOut = address.ss_family == AF_INET6
                      ? ntohs(reinterpret_cast<const sockaddr_in6*>(&address)->sin6_port)
                      : ntohs(reinterpret_cast<const sockaddr_in*>(&address)->sin_port);
        return {};
    }

    void run()
    {
        thread_ = std::jthread([this](std::stop_token stop) { loop(stop); });
    }

    [[nodiscard]] bool listening() const noexcept { return listening_.load(); }
    [[nodiscard]] uint64_t accepted() const noexcept { return accepted_.load(); }

    void publish(const std::string& line)
    {
        std::lock_guard<std::mutex> lock(eventsMutex_);
        for (auto& entry : subscribers_)
        {
            EventQueue& queue = entry.second;
            queue.lines.push_back(line);
            if (queue.lines.size() > kMaxQueuedEvents)
            {
                queue.lines.pop_front(); // the oldest goes: the newest is nearer the truth
                ++queue.lost;
            }
        }
    }

private:
    struct Connection
    {
        uint64_t serial = 0; // names its event queue
        net::Socket socket = net::kInvalidSocket;
        std::string inbox;
        std::string outbox;
        bool discarding = false; // inside a line past the ceiling, until its newline
        bool subscribed = false;
    };

    // One subscribed connection's events, shared with whichever thread
    // publishes. Kept apart from Connection so that a publisher never touches
    // anything this thread owns.
    struct EventQueue
    {
        std::deque<std::string> lines;
        uint64_t lost = 0;
    };

    // The handler changed the flag: the queue is made or forgotten to match.
    void resubscribe(const Connection& connection)
    {
        std::lock_guard<std::mutex> lock(eventsMutex_);
        if (connection.subscribed)
            subscribers_.try_emplace(connection.serial);
        else
            subscribers_.erase(connection.serial);
    }

    // Events go out only behind everything a connection already owed, so a
    // caller that has stopped reading holds them in its bounded queue rather
    // than in its unbounded outbox -- and the first line it gets once it
    // reads again says how many it missed.
    void deliverEvents()
    {
        std::lock_guard<std::mutex> lock(eventsMutex_);
        for (Connection& connection : connections_)
        {
            if (!connection.subscribed || !connection.outbox.empty())
                continue;
            const auto found = subscribers_.find(connection.serial);
            if (found == subscribers_.end())
                continue;

            EventQueue& queue = found->second;
            if (queue.lost != 0)
            {
                connection.outbox += "{\"event\":\"events.lost\",\"count\":" +
                                     std::to_string(queue.lost) + "}\n";
                queue.lost = 0;
            }
            for (const std::string& line : queue.lines)
                connection.outbox += line;
            queue.lines.clear();
        }
    }

    void loop(std::stop_token stop)
    {
        while (!stop.stop_requested())
        {
            fd_set readable;
            fd_set writable;
            FD_ZERO(&readable);
            FD_ZERO(&writable);

            net::Socket highest = listener_;
            if (connections_.size() < kMaxConnections)
                FD_SET(listener_, &readable);
            for (const Connection& connection : connections_)
            {
                FD_SET(connection.socket, &readable);
                if (!connection.outbox.empty())
                    FD_SET(connection.socket, &writable);
                highest = std::max(highest, connection.socket);
            }

            timeval timeout{};
            timeout.tv_usec = kSelectTimeoutMs * 1000;

            const int ready = net::waitReady(highest, &readable, &writable, nullptr, timeout);
            if (ready < 0 && !net::wouldBlock(net::lastError()))
            {
                MULTIROOM_ERROR("control API listener: select failed: {}",
                          net::errorMessage(net::lastError()));
                listening_.store(false);
                break;
            }
            if (stop.stop_requested())
                break;

            // Every pass, a quiet one too: an event arrives from another
            // thread, and nothing on a socket says so.
            deliverEvents();
            if (ready <= 0)
            {
                FD_ZERO(&readable);
                FD_ZERO(&writable);
            }

            if (FD_ISSET(listener_, &readable))
                acceptPending();

            for (auto it = connections_.begin(); it != connections_.end();)
            {
                bool alive = true;
                if (FD_ISSET(it->socket, &writable))
                    alive = writeTo(*it);
                if (alive && FD_ISSET(it->socket, &readable))
                    alive = readFrom(*it);
                if (alive && !it->outbox.empty())
                    alive = writeTo(*it); // answer now, not on the next pass

                if (alive)
                {
                    ++it;
                    continue;
                }
                net::closeSocket(it->socket);
                if (it->subscribed)
                {
                    it->subscribed = false;
                    resubscribe(*it);
                }
                it = connections_.erase(it);
            }
        }
    }

    void acceptPending()
    {
        while (connections_.size() < kMaxConnections)
        {
            bool empty = false;
            std::string error;
            const net::Socket accepted = net::acceptOne(listener_, empty, error);
            if (empty)
                return;
            if (accepted == net::kInvalidSocket)
            {
                MULTIROOM_WARN("control API listener: {}", error);
                return;
            }
#if !defined(_WIN32)
            // FD_SET past FD_SETSIZE writes outside the set.
            if (accepted >= FD_SETSIZE)
            {
                MULTIROOM_WARN("control API listener: refusing a descriptor above FD_SETSIZE");
                net::closeSocket(accepted);
                continue;
            }
#endif
            Connection connection;
            connection.serial = accepted_.fetch_add(1);
            connection.socket = accepted;
            connections_.push_back(std::move(connection));
        }
    }

    bool readFrom(Connection& connection)
    {
        char buffer[kReadChunkBytes];
        const int received =
            net::receive(connection.socket, buffer, static_cast<int>(sizeof(buffer)));
        if (received == 0)
            return false; // the caller hung up
        if (received < 0)
            return net::wouldBlock(net::lastError());

        connection.inbox.append(buffer, static_cast<size_t>(received));

        // The rest of a line already answered as too long is dropped up to its
        // newline, and the caller carries on from the next one. Closing on it
        // instead would close with bytes unread, which is a reset -- and a
        // reset can take the answer that explains it down with it.
        if (connection.discarding)
        {
            const size_t newline = connection.inbox.find('\n');
            connection.inbox.erase(0, newline == std::string::npos ? std::string::npos
                                                                   : newline + 1);
            if (newline == std::string::npos)
                return true;
            connection.discarding = false;
        }

        size_t start = 0;
        for (size_t newline = connection.inbox.find('\n', start); newline != std::string::npos;
             newline = connection.inbox.find('\n', start))
        {
            std::string line = connection.inbox.substr(start, newline - start);
            start = newline + 1;

            // CRLF from a telnet session on Windows, and blank lines as
            // keepalive noise rather than malformed requests.
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line.find_first_not_of(" \t") == std::string::npos)
                continue;

            const bool wasSubscribed = connection.subscribed;
            connection.outbox += handler_(line, connection.subscribed);
            if (connection.subscribed != wasSubscribed)
                resubscribe(connection);
        }
        connection.inbox.erase(0, start);

        if (connection.inbox.size() > kMaxRequestBytes)
        {
            connection.outbox += "{\"ok\":false,\"error\":\"invalidRequest\","
                                 "\"message\":\"request line too long\"}\n";
            connection.inbox.clear();
            connection.discarding = true;
        }
        if (connection.outbox.size() > kMaxPendingResponseBytes)
        {
            MULTIROOM_WARN("control API listener: dropping a caller that does not read its answers");
            return false;
        }
        return true;
    }

    static bool writeTo(Connection& connection)
    {
        while (!connection.outbox.empty())
        {
            const int sent = net::transmit(connection.socket, connection.outbox.data(),
                                           static_cast<int>(connection.outbox.size()));
            if (sent > 0)
            {
                connection.outbox.erase(0, static_cast<size_t>(sent));
                continue;
            }
            return sent < 0 && net::wouldBlock(net::lastError());
        }
        return true;
    }

    Handler handler_;
    net::Library library_;
    net::Socket listener_ = net::kInvalidSocket;
    std::list<Connection> connections_;
    std::atomic<bool> listening_{true};
    std::atomic<uint64_t> accepted_{0};

    // The one thing here another thread touches: a queue per subscribed
    // connection, by serial. Bounded by kMaxConnections queues of
    // kMaxQueuedEvents lines.
    std::mutex eventsMutex_;
    std::map<uint64_t, EventQueue> subscribers_; // guarded by eventsMutex_

    // Last, so that it is stopped before anything it uses is destroyed.
    std::jthread thread_;
};

// ---------------------------------------------------------------------------

ControlListener::ControlListener(std::unique_ptr<Impl> impl, uint16_t port)
    : impl_(std::move(impl)), port_(port)
{
}

ControlListener::~ControlListener() = default;

Result<std::unique_ptr<ControlListener>, ControlListenerError>
ControlListener::start(ControlListenerSettings settings, Handler handler)
{
    auto impl = std::make_unique<Impl>(std::move(handler));

    uint16_t port = 0;
    if (const Status<ControlListenerError> bound = impl->bind(settings, port); !bound)
        return std::unexpected(bound.error());

    impl->run();

    MULTIROOM_INFO("control API on {}:{}", settings.bindAddress, port);
    return std::unique_ptr<ControlListener>(new ControlListener(std::move(impl), port));
}

bool ControlListener::listening() const noexcept
{
    return impl_->listening();
}

uint64_t ControlListener::acceptedCount() const noexcept
{
    return impl_->accepted();
}

void ControlListener::publish(const std::string& line)
{
    impl_->publish(line);
}

} // namespace multiroom
