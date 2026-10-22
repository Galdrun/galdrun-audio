#include "control/ControlServer.h"

#include "control/ControlHttp.h"
#include "net/Socket.h"
#include "util/Logger.h"
#include "system/SystemSampler.h"

#if defined(ESP_PLATFORM)
#include "esp_pthread.h"
#endif

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

namespace multiroom {
namespace {

// A diagnostic interface does not need more. The cap exists so a misbehaving
// script cannot open sockets until the process runs out of descriptors.
const size_t kMaxConnections = 8;

// With every slot taken, the connection that has been silent longest is closed
// once it has been silent this long, so sockets that connect and never send
// cannot lock everyone else out. Below the cap nothing is closed for idling:
// a line client may hold its connection open between commands.
const auto kIdleEvictAfter = std::chrono::seconds(30);

// Longest request line accepted. Every documented command fits in a fraction
// of this; a client that exceeds it is malfunctioning, not verbose.
const size_t kMaxRequestBytes = 64 * 1024;

// A client that stops reading while we keep answering. audioDevices on a
// machine with many devices is the largest response by far, and it is well
// under this.
const size_t kMaxPendingResponseBytes = 4 * 1024 * 1024;

const size_t kReadChunkBytes = 4096;

const size_t kWriteChunkBytes = 64 * 1024;

// Bounds how long stop() waits: the loop checks the stop flag once per select
// timeout.
const int kSelectTimeoutMs = 200;
const size_t kMaxInputEvents = 128;
const size_t kMaxInputOutboxBytes = 16 * 1024;
#if defined(ESP_PLATFORM)
const size_t kMaxInputStreams = 2;
#else
const size_t kMaxInputStreams = 4;
#endif

std::string httpChunk(const std::string& text)
{
    char length[24];
    std::snprintf(length, sizeof(length), "%x\r\n", static_cast<unsigned>(text.size()));
    return std::string(length) + text + "\r\n";
}

std::string inputStateEvent(const std::string& id, bool pressed)
{
    json::Value state = json::Value::makeObject();
    state["id"] = json::Value(id);
    state["pressed"] = json::Value(pressed);
    return httpChunk("event: input-state\ndata: " + json::serialize(state) + "\n\n");
}

std::string inputSnapshotEvent(const std::map<std::string, bool>& buttons)
{
    json::Value states = json::Value::makeArray();
    for (const auto& button : buttons)
    {
        json::Value state = json::Value::makeObject();
        state["id"] = json::Value(button.first);
        state["pressed"] = json::Value(button.second);
        states.pushBack(state);
    }
    return httpChunk("event: input-snapshot\ndata: " + json::serialize(states) + "\n\n");
}

struct Connection
{
    net::Socket socket = net::kInvalidSocket;
    std::string inbox;
    std::string outbox;
    bool closing = false; // flush the outbox, then drop the connection
    bool inputEvents = false;
    uint64_t inputSequence = 0;
    std::chrono::steady_clock::time_point lastActivity = std::chrono::steady_clock::now();
};

// getaddrinfo rather than inet_pton, so an IPv6 bind address needs no second
// code path. AI_NUMERICHOST: the control address is a literal, and a DNS
// lookup is not something a bind address should ever trigger.
bool resolveBindAddress(const std::string& address, uint16_t port, addrinfo*& out,
                        std::string& error)
{
    addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_PASSIVE | AI_NUMERICHOST;

    char service[8];
    std::snprintf(service, sizeof(service), "%u", static_cast<unsigned>(port));

    addrinfo* result = nullptr;
    const int status = ::getaddrinfo(address.c_str(), service, &hints, &result);
    if (status != 0 || result == nullptr)
    {
        error = "cannot resolve bind address \"" + address + "\"";
        return false;
    }
    out = result;
    return true;
}

uint16_t readBoundPort(net::Socket socket)
{
    sockaddr_storage address;
    std::memset(&address, 0, sizeof(address));
#if defined(_WIN32)
    int length = static_cast<int>(sizeof(address));
#else
    socklen_t length = static_cast<socklen_t>(sizeof(address));
#endif
    if (::getsockname(socket, reinterpret_cast<sockaddr*>(&address), &length) != 0)
        return 0;

    if (address.ss_family == AF_INET)
        return ntohs(reinterpret_cast<const sockaddr_in*>(&address)->sin_port);
    if (address.ss_family == AF_INET6)
        return ntohs(reinterpret_cast<const sockaddr_in6*>(&address)->sin6_port);
    return 0;
}

} // namespace

struct ControlServer::Impl
{
    struct InputStateEvent { uint64_t sequence; std::string id; bool pressed; };
    std::mutex inputMutex;
    std::map<std::string, bool> inputButtons;
    std::deque<InputStateEvent> inputEvents;
    std::deque<InputStateEvent> inputPending; // reused by the socket thread
    uint64_t inputSequence = 0;
    bool inputResync = false;
    std::chrono::steady_clock::time_point lastInputHeartbeat = std::chrono::steady_clock::now();
    SystemSampler usageSampler;
    std::chrono::steady_clock::time_point lastUsageSample{};
    net::Library library;
    net::Socket listener = net::kInvalidSocket;
    uint16_t boundPort = 0;
    IControlHandler* handler = nullptr;

    std::thread thread;
    std::atomic<bool> stop{false};
    std::atomic<bool> running{false};
    std::atomic<uint64_t> accepted{0};

    std::vector<Connection> connections;
    // Impl lives on the heap. Keep the read buffer off the control task stack.
    char readBuffer[kReadChunkBytes];

    void run();
    void acceptPending();
    void evictIdle();
    bool serviceRead(Connection& connection);
    bool serviceWrite(Connection& connection);
    void processLines(Connection& connection);
    void broadcastInputEvents();
    void closeAll();
};

ControlServer::ControlServer() : impl_(new Impl()) {}

ControlServer::~ControlServer()
{
    stop();
}

bool ControlServer::start(const std::string& bindAddress, uint16_t port,
                          IControlHandler& handler, std::string& error)
{
    if (impl_->running.load())
    {
        error = "control server is already running";
        return false;
    }
    if (!impl_->library.isReady())
    {
        error = impl_->library.error();
        return false;
    }

    addrinfo* resolved = nullptr;
    if (!resolveBindAddress(bindAddress, port, resolved, error))
        return false;

    net::Socket listener = net::kInvalidSocket;
    std::string bindError = "no usable address";
    for (addrinfo* candidate = resolved; candidate != nullptr; candidate = candidate->ai_next)
    {
        listener = ::socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
        if (listener == net::kInvalidSocket)
        {
            bindError = net::errorMessage(net::lastError());
            continue;
        }

#if !defined(_WIN32)
        // Without this a restart within TIME_WAIT fails to bind. Deliberately
        // not set on Windows, where SO_REUSEADDR lets another process steal a
        // port that is still in use rather than only reusing a dead one.
        int reuse = 1;
        ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
                     sizeof(reuse));
#endif

        if (::bind(listener, candidate->ai_addr, static_cast<int>(candidate->ai_addrlen)) == 0 &&
            ::listen(listener, static_cast<int>(kMaxConnections)) == 0)
        {
            break;
        }

        bindError = net::errorMessage(net::lastError());
        net::closeSocket(listener);
        listener = net::kInvalidSocket;
    }
    ::freeaddrinfo(resolved);

    if (listener == net::kInvalidSocket)
    {
        char where[16];
        std::snprintf(where, sizeof(where), ":%u", static_cast<unsigned>(port));
        error = "cannot listen on " + bindAddress + where + ": " + bindError;
        return false;
    }

#if !defined(_WIN32)
    // The same limit the accepted connections are held to: select() cannot
    // watch it, and FD_SET would write past the fd_set.
    if (static_cast<int>(listener) >= FD_SETSIZE)
    {
        error = "the listening socket's descriptor is above FD_SETSIZE";
        net::closeSocket(listener);
        return false;
    }
#endif

    if (!net::setNonBlocking(listener))
    {
        error = "cannot set the listening socket non-blocking: " +
                net::errorMessage(net::lastError());
        net::closeSocket(listener);
        return false;
    }

    impl_->listener = listener;
    impl_->boundPort = readBoundPort(listener);
    impl_->handler = &handler;
    impl_->stop.store(false);
    impl_->accepted.store(0);
    impl_->running.store(true);
#if defined(ESP_PLATFORM)
    // Named for the `diagnostics` task report.
    esp_pthread_cfg_t named = esp_pthread_get_default_config();
    named.thread_name = "client_ctrl";
    esp_pthread_set_cfg(&named);
#endif
    impl_->thread = std::thread([this] { impl_->run(); });
#if defined(ESP_PLATFORM)
    const esp_pthread_cfg_t defaults = esp_pthread_get_default_config();
    esp_pthread_set_cfg(&defaults);
#endif

    MULTIROOM_INFO("control server listening on %s:%u", bindAddress.c_str(),
             static_cast<unsigned>(impl_->boundPort));
    return true;
}

void ControlServer::stop()
{
    if (!impl_->running.load())
        return;

    impl_->stop.store(true);
    if (impl_->thread.joinable())
        impl_->thread.join();

    impl_->closeAll();
    net::closeSocket(impl_->listener);
    impl_->listener = net::kInvalidSocket;
    impl_->handler = nullptr;
    impl_->running.store(false);

    MULTIROOM_INFO("control server stopped");
}

bool ControlServer::isRunning() const
{
    return impl_->running.load();
}

uint16_t ControlServer::boundPort() const
{
    return impl_->boundPort;
}

uint64_t ControlServer::acceptedConnections() const
{
    return impl_->accepted.load();
}

void ControlServer::setInputButtons(const std::vector<std::string>& ids)
{
    std::lock_guard<std::mutex> lock(impl_->inputMutex);
    impl_->inputButtons.clear();
    for (const std::string& id : ids)
        impl_->inputButtons[id] = false;
    impl_->inputEvents.clear();
    ++impl_->inputSequence;
    impl_->inputResync = true;
}

void ControlServer::publishInputState(const std::string& id, bool pressed)
{
    std::lock_guard<std::mutex> lock(impl_->inputMutex);
    const auto button = impl_->inputButtons.find(id);
    if (button == impl_->inputButtons.end() || button->second == pressed)
        return;
    button->second = pressed;
    ++impl_->inputSequence;
    if (impl_->inputEvents.size() >= kMaxInputEvents)
    {
        // Recover current levels rather than leaving a badge stuck on after
        // a burst or a stalled control loop. Memory stays bounded.
        impl_->inputEvents.clear();
        impl_->inputResync = true;
    }
    impl_->inputEvents.push_back({impl_->inputSequence, id, pressed});
}

void ControlServer::Impl::broadcastInputEvents()
{
    std::deque<InputStateEvent>& pending = inputPending;
    pending.clear();
    std::map<std::string, bool> snapshot;
    uint64_t sequence = 0;
    bool resync;
    {
        std::lock_guard<std::mutex> lock(inputMutex);
        pending.swap(inputEvents);
        resync = inputResync;
        inputResync = false;
        if (resync)
        {
            snapshot = inputButtons;
            sequence = inputSequence;
        }
    }
    const auto now = std::chrono::steady_clock::now();
    std::string usageEvent;
    const bool hasViewers = std::any_of(connections.begin(), connections.end(),
        [](const Connection& c) { return c.inputEvents && !c.closing; });
    if (hasViewers && now - lastUsageSample >= std::chrono::milliseconds(500))
    {
        lastUsageSample = now;
        const SystemMetrics usage = usageSampler.sampleUsage();
        json::Value data = json::Value::makeObject();
        data["cpuPercent"] = usage.hasSystemCpuPercent ? json::Value(usage.systemCpuPercent) : json::Value();
        data["cpuModel"] = usage.cpuModel.empty() ? json::Value(usage.cpuArchitecture) : json::Value(usage.cpuModel);
        data["cpuMhz"] = usage.hasCpuMhz ? json::Value(usage.cpuMhz) : json::Value();
        data["ramTotalBytes"] = usage.hasSystemMemoryTotalBytes ? json::Value(usage.systemMemoryTotalBytes) : json::Value();
        data["ramUsedBytes"] = usage.hasSystemMemoryTotalBytes && usage.hasSystemMemoryAvailableBytes ?
            json::Value(usage.systemMemoryTotalBytes - std::min(usage.systemMemoryAvailableBytes,
                usage.systemMemoryTotalBytes)) : json::Value();
        data["wifiRssiDbm"] = usage.hasWifiRssiDbm ? json::Value(usage.wifiRssiDbm) : json::Value();
        data["ramPercent"] = usage.hasSystemMemoryTotalBytes && usage.hasSystemMemoryAvailableBytes &&
            usage.systemMemoryTotalBytes > 0 ? json::Value(100.0 * (1.0 -
                static_cast<double>(std::min(usage.systemMemoryAvailableBytes, usage.systemMemoryTotalBytes)) /
                static_cast<double>(usage.systemMemoryTotalBytes))) : json::Value();
        usageEvent = httpChunk("event: system-usage\ndata: " + json::serialize(data) + "\n\n");
    }
    const bool heartbeat = now - lastInputHeartbeat >= std::chrono::seconds(10);
    if (heartbeat)
        lastInputHeartbeat = now;
    for (Connection& connection : connections)
    {
        if (!connection.inputEvents || connection.closing)
            continue;
        if (resync && sequence > connection.inputSequence)
        {
            connection.outbox += inputSnapshotEvent(snapshot);
            connection.inputSequence = sequence;
        }
        if (!resync)
            for (const InputStateEvent& event : pending)
                if (event.sequence > connection.inputSequence)
                {
                    connection.outbox += inputStateEvent(event.id, event.pressed);
                    connection.inputSequence = event.sequence;
                }
        if (heartbeat)
            connection.outbox += httpChunk(": keepalive\n\n");
        connection.outbox += usageEvent;
        if (connection.outbox.size() > kMaxInputOutboxBytes)
        {
            connection.outbox.clear();
            connection.closing = true;
        }
    }
}

void ControlServer::Impl::run()
{
    bool selectFailing = false; // logged once per run of failures
    while (!stop.load())
    {
        broadcastInputEvents();
        for (size_t i = 0; i < connections.size();)
        {
            if (connections[i].closing && connections[i].outbox.empty())
            {
                net::closeSocket(connections[i].socket);
                connections.erase(connections.begin() + static_cast<std::ptrdiff_t>(i));
            }
            else
                ++i;
        }
        if (connections.size() >= kMaxConnections)
            evictIdle();

        fd_set readable;
        fd_set writable;
        FD_ZERO(&readable);
        FD_ZERO(&writable);

        int highest = 0;
        bool hasInputStreams = false;
        if (connections.size() < kMaxConnections)
        {
            FD_SET(listener, &readable);
#if !defined(_WIN32)
            highest = static_cast<int>(listener);
#endif
        }
        for (size_t i = 0; i < connections.size(); ++i)
        {
            const Connection& connection = connections[i];
            hasInputStreams = hasInputStreams || connection.inputEvents;
            if (!connection.closing)
                FD_SET(connection.socket, &readable);
            if (!connection.outbox.empty())
                FD_SET(connection.socket, &writable);
#if !defined(_WIN32)
            if (static_cast<int>(connection.socket) > highest)
                highest = static_cast<int>(connection.socket);
#endif
        }

        timeval timeout;
        // An active viewer gets button edges within one debounce interval.
        // This is the existing socket loop; no requests poll the owner thread.
        const int timeoutMs = hasInputStreams ? 20 : kSelectTimeoutMs;
        timeout.tv_sec = timeoutMs / 1000;
        timeout.tv_usec = (timeoutMs % 1000) * 1000;

        // The stop flag is polled once per timeout rather than woken through a
        // self-connected socket pair: 200 ms of shutdown latency on a
        // diagnostic interface does not justify the extra machinery, which on
        // Windows would mean a real loopback connection.
        const int ready = ::select(highest + 1, &readable, &writable, nullptr, &timeout);
        if (ready < 0)
        {
            const int error = net::lastError();
            if (net::wouldBlock(error))
                continue;
            // Retried, not given up: what fails here is usually passing (the
            // network stack going down under a network change, lwIP out of
            // memory for its select semaphore), and leaving the loop would
            // leave the client playing with no control API until a restart.
            if (!selectFailing)
                MULTIROOM_ERROR("control server select() failed: %s; retrying",
                          net::errorMessage(error).c_str());
            selectFailing = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(kSelectTimeoutMs));
            continue;
        }
        if (selectFailing)
        {
            MULTIROOM_INFO("control server select() works again");
            selectFailing = false;
        }
        if (stop.load())
            break;
        if (ready == 0)
            continue;

        if (FD_ISSET(listener, &readable))
            acceptPending();

        for (size_t i = 0; i < connections.size();)
        {
            Connection& connection = connections[i];
            bool alive = true;

            if (FD_ISSET(connection.socket, &writable))
                alive = serviceWrite(connection);
            if (alive && !connection.closing && FD_ISSET(connection.socket, &readable))
                alive = serviceRead(connection);
            if (alive && connection.closing && connection.outbox.empty())
                alive = false;

            if (alive)
            {
                ++i;
                continue;
            }
            net::closeSocket(connection.socket);
            connections.erase(connections.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }
}

void ControlServer::Impl::acceptPending()
{
    while (connections.size() < kMaxConnections)
    {
        const net::Socket incoming = ::accept(listener, nullptr, nullptr);
        if (incoming == net::kInvalidSocket)
            return; // nothing pending, or a client that gave up before we got here

#if !defined(_WIN32)
        // select() cannot watch a descriptor at or above FD_SETSIZE, and
        // FD_SET would write past the fd_set. Refusing is the only safe answer.
        if (static_cast<int>(incoming) >= FD_SETSIZE)
        {
            MULTIROOM_WARN("control connection refused: descriptor above FD_SETSIZE");
            net::closeSocket(incoming);
            continue;
        }
#endif

        if (!net::setNonBlocking(incoming))
        {
            MULTIROOM_WARN("control connection refused: %s",
                     net::errorMessage(net::lastError()).c_str());
            net::closeSocket(incoming);
            continue;
        }

        // Responses are single small writes; waiting 40 ms for a companion
        // packet that never comes would show up directly as command latency.
        int noDelay = 1;
        ::setsockopt(incoming, IPPROTO_TCP, TCP_NODELAY,
                     reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));

        Connection connection;
        connection.socket = incoming;
        connections.push_back(std::move(connection));
        accepted.fetch_add(1);
    }
}

void ControlServer::Impl::evictIdle()
{
    size_t oldest = connections.size();
    for (size_t i = 0; i < connections.size(); ++i)
    {
        if (oldest == connections.size() ||
            connections[i].lastActivity < connections[oldest].lastActivity)
            oldest = i;
    }
    if (oldest == connections.size() ||
        std::chrono::steady_clock::now() - connections[oldest].lastActivity < kIdleEvictAfter)
        return;

    MULTIROOM_WARN("control connection closed: idle while every connection slot is taken");
    net::closeSocket(connections[oldest].socket);
    connections.erase(connections.begin() + static_cast<std::ptrdiff_t>(oldest));
}

bool ControlServer::Impl::serviceRead(Connection& connection)
{
    const int received =
        net::receive(connection.socket, readBuffer, static_cast<int>(sizeof(readBuffer)));
    if (received == 0)
        return false; // orderly shutdown by the peer
    if (received < 0)
    {
        const int error = net::lastError();
        if (net::wouldBlock(error))
            return true;
        // A client that disappears mid-request -- typically Ctrl-C in a shell
        // pipeline -- is routine, not a server failure. Drop that connection
        // and keep serving the others.
        MULTIROOM_DEBUG("control connection dropped: %s", net::errorMessage(error).c_str());
        return false;
    }

    connection.lastActivity = std::chrono::steady_clock::now();
    connection.inbox.append(readBuffer, static_cast<size_t>(received));
    processLines(connection);

    if (connection.inbox.size() > kMaxRequestBytes)
    {
        json::Value response = json::Value::makeObject();
        response["ok"] = json::Value(false);
        response["error"] = json::Value(control_error::kInvalidRequest);
        response["message"] = json::Value("request line too long");
        connection.outbox += encodeControlResponse(response);
        connection.inbox.clear();
        connection.closing = true;
        return true;
    }

    if (connection.outbox.size() > kMaxPendingResponseBytes)
    {
        MULTIROOM_WARN("control connection dropped: the client is not reading its responses");
        return false;
    }
    return true;
}

bool ControlServer::Impl::serviceWrite(Connection& connection)
{
    while (!connection.outbox.empty())
    {
        const size_t chunk =
            connection.outbox.size() > kWriteChunkBytes ? kWriteChunkBytes : connection.outbox.size();
        const int sent =
            net::transmit(connection.socket, connection.outbox.data(), static_cast<int>(chunk));
        if (sent > 0)
        {
            connection.lastActivity = std::chrono::steady_clock::now();
            connection.outbox.erase(0, static_cast<size_t>(sent));
            continue;
        }
        const int error = net::lastError();
        if (sent < 0 && net::wouldBlock(error))
            return true;
        MULTIROOM_DEBUG("control connection dropped while writing: %s",
                  net::errorMessage(error).c_str());
        return false;
    }
    return true;
}

void ControlServer::Impl::processLines(Connection& connection)
{
    if (connection.inputEvents)
    {
        // Streaming connections are read-only and never dispatch commands.
        connection.inbox.clear();
        connection.closing = true;
        return;
    }
    // A browser instead of a line client: one request, then the connection
    // closes once the response is out.
    if (looksLikeHttpRequest(connection.inbox))
    {
        bool inputStream = false;
        if (serveHttpRequest(connection.inbox, *handler, connection.outbox, &inputStream))
        {
            connection.closing = !inputStream;
            if (inputStream)
            {
                size_t viewers = 0;
                for (const Connection& viewer : connections)
                    if (viewer.inputEvents)
                        ++viewers;
                if (viewers >= kMaxInputStreams)
                {
                    connection.outbox = "HTTP/1.1 503 Service Unavailable\r\n"
                                        "Content-Length: 0\r\nConnection: close\r\n\r\n";
                    connection.closing = true;
                    return;
                }
                connection.inputEvents = true;
                std::map<std::string, bool> snapshot;
                {
                    std::lock_guard<std::mutex> lock(inputMutex);
                    snapshot = inputButtons;
                    connection.inputSequence = inputSequence;
                }
                connection.outbox += httpChunk("retry: 1500\n\n") + inputSnapshotEvent(snapshot);
            }
        }
        return;
    }

    size_t start = 0;
    for (;;)
    {
        const size_t newline = connection.inbox.find('\n', start);
        if (newline == std::string::npos)
            break;

        std::string line = connection.inbox.substr(start, newline - start);
        start = newline + 1;

        // Tolerate CRLF, so the interface can be driven from a plain telnet
        // session on Windows.
        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        // A blank line is keepalive noise, not a malformed request.
        bool blank = true;
        for (size_t i = 0; i < line.size(); ++i)
        {
            if (line[i] != ' ' && line[i] != '\t')
            {
                blank = false;
                break;
            }
        }
        if (blank)
            continue;

        connection.outbox += dispatchControlLine(line, *handler);
    }
    connection.inbox.erase(0, start);
}

void ControlServer::Impl::closeAll()
{
    for (size_t i = 0; i < connections.size(); ++i)
        net::closeSocket(connections[i].socket);
    connections.clear();
}

} // namespace multiroom
