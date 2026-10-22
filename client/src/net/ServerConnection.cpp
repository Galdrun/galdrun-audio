#include "net/ServerConnection.h"

#include "config/ClientConfig.h" // parseServerAddress
#include "net/Socket.h"
#include "util/Logger.h"

#if defined(ESP_PLATFORM)
#include "esp_pthread.h"
#include "sdkconfig.h"
#elif defined(_WIN32)
#include <iphlpapi.h>
#elif defined(__linux__)
#include <ifaddrs.h>
#endif

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace multiroom {
namespace {

// Backoff between connection attempts. Short enough that a server which comes
// back is picked up quickly, long enough that a client left pointing at
// nothing does not hammer the network for days.
const uint32_t kBackoffMs[] = {1000, 2000, 5000, 10000, 30000};
const size_t kBackoffCount = sizeof(kBackoffMs) / sizeof(kBackoffMs[0]);

// A connect that has not completed by then is treated as failed. The OS
// default is tens of seconds on both platforms, which would also be how long a
// shutdown could take.
const uint32_t kConnectTimeoutMs = 5000;

// How long select() waits before the loop re-checks the stop flag and the
// configured address. It does not bound send latency: send() writes from the
// calling thread.
const int kPollMs = 200;

#if !defined(ESP_PLATFORM) && (defined(_WIN32) || defined(__linux__))
bool sameAddress(const sockaddr* address, const sockaddr_storage& local)
{
    if (address == nullptr || address->sa_family != local.ss_family)
        return false;
    if (address->sa_family == AF_INET)
        return std::memcmp(&reinterpret_cast<const sockaddr_in*>(address)->sin_addr,
                           &reinterpret_cast<const sockaddr_in*>(&local)->sin_addr, sizeof(in_addr)) == 0;
    if (address->sa_family == AF_INET6)
        return std::memcmp(&reinterpret_cast<const sockaddr_in6*>(address)->sin6_addr,
                           &reinterpret_cast<const sockaddr_in6*>(&local)->sin6_addr, sizeof(in6_addr)) == 0;
    return false;
}
#endif

// The kind of interface a connected socket's local address belongs to:
// "ethernet", "wifi", or empty when that cannot be told (loopback, a VPN, a
// platform without a lookup). Looked up once per connection.
std::string networkLinkOf(net::Socket socket)
{
#if defined(ESP_PLATFORM)
    // The ESP32 build brings up Wi-Fi only.
    (void)socket;
    return "wifi";
#elif defined(_WIN32) || defined(__linux__)
    sockaddr_storage local{};
#  if defined(_WIN32)
    int length = static_cast<int>(sizeof(local));
#  else
    socklen_t length = static_cast<socklen_t>(sizeof(local));
#  endif
    if (::getsockname(socket, reinterpret_cast<sockaddr*>(&local), &length) != 0)
        return {};
#  if defined(_WIN32)
    ULONG size = 16 * 1024;
    std::vector<unsigned char> buffer;
    ULONG result = ERROR_BUFFER_OVERFLOW;
    while (result == ERROR_BUFFER_OVERFLOW)
    {
        buffer.resize(size);
        result = ::GetAdaptersAddresses(local.ss_family,
                                        GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                        nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    }
    if (result != NO_ERROR)
        return {};
    for (auto* adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); adapter != nullptr;
         adapter = adapter->Next)
    {
        for (auto* unicast = adapter->FirstUnicastAddress; unicast != nullptr; unicast = unicast->Next)
        {
            if (sameAddress(unicast->Address.lpSockaddr, local))
                return adapter->IfType == IF_TYPE_IEEE80211       ? "wifi"
                       : adapter->IfType == IF_TYPE_ETHERNET_CSMACD ? "ethernet"
                                                                    : "";
        }
    }
    return {};
#  else
    ifaddrs* interfaces = nullptr;
    if (::getifaddrs(&interfaces) != 0)
        return {};
    std::string name;
    for (auto* current = interfaces; current != nullptr; current = current->ifa_next)
    {
        if (sameAddress(current->ifa_addr, local))
        {
            name = current->ifa_name;
            break;
        }
    }
    ::freeifaddrs(interfaces);
    if (name.empty())
        return {};
    // nl80211 drivers have phy80211, the older wireless-extensions ones wireless.
    const std::string sys = "/sys/class/net/" + name;
    if (::access((sys + "/phy80211").c_str(), F_OK) == 0 || ::access((sys + "/wireless").c_str(), F_OK) == 0)
        return "wifi";
    // Anything else of ARPHRD_ETHER (1) is a wired port; a bridge or VLAN over
    // one counts as one too.
    int type = 0;
    if (FILE* file = std::fopen((sys + "/type").c_str(), "r"))
    {
        if (std::fscanf(file, "%d", &type) != 1)
            type = 0;
        std::fclose(file);
    }
    return type == 1 ? "ethernet" : "";
#  endif
#else
    (void)socket;
    return {};
#endif
}

// Per connection, in internal RAM. The ESP32 and ESP32-S3 have little of it to
// spare, so they read in smaller pieces and pay for that with more reads
// (about 90 a second instead of 60 for a 44.1 kHz int32 stream).
#if defined(CONFIG_IDF_TARGET_ESP32) || defined(CONFIG_IDF_TARGET_ESP32S3)
const size_t kReadChunkBytes = 4096;
#else
const size_t kReadChunkBytes = 8192;
#endif

uint64_t elapsedNs(std::chrono::steady_clock::time_point from,
                   std::chrono::steady_clock::time_point to)
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(to - from).count());
}

// A server that has stopped reading. Control messages are small, so anything
// near this means the peer is gone and the connection should be dropped rather
// than buffered.
const size_t kMaxOutboxBytes = 1024 * 1024;

bool connectInProgress(int error)
{
#if defined(_WIN32)
    return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS;
#else
    return error == EINPROGRESS || error == EINTR;
#endif
}

// Waits for a non-blocking connect to finish, in slices so a stop request is
// noticed long before the timeout runs out.
bool awaitConnect(net::Socket socket, const std::atomic<bool>& cancelled, std::string& error)
{
    uint32_t waitedMs = 0;
    while (waitedMs < kConnectTimeoutMs)
    {
        if (cancelled.load())
        {
            error = "cancelled";
            return false;
        }

        fd_set writable;
        fd_set failed;
        FD_ZERO(&writable);
        FD_ZERO(&failed);
        FD_SET(socket, &writable);
        FD_SET(socket, &failed);

        const uint32_t sliceMs =
            kConnectTimeoutMs - waitedMs < 200 ? kConnectTimeoutMs - waitedMs : 200;
        timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = static_cast<long>(sliceMs) * 1000;

        int highest = 0;
#if !defined(_WIN32)
        highest = static_cast<int>(socket);
#endif
        // A refused connect shows up in the exception set on Windows and in
        // the write set on POSIX, so both are watched and SO_ERROR decides.
        const int ready = ::select(highest + 1, nullptr, &writable, &failed, &timeout);
        if (ready < 0)
        {
            const int selectError = net::lastError();
            if (net::wouldBlock(selectError))
                continue;
            error = net::errorMessage(selectError);
            return false;
        }
        if (ready == 0)
        {
            waitedMs += sliceMs;
            continue;
        }

        int socketError = 0;
#if defined(_WIN32)
        int length = static_cast<int>(sizeof(socketError));
#else
        socklen_t length = static_cast<socklen_t>(sizeof(socketError));
#endif
        if (::getsockopt(socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&socketError),
                         &length) != 0)
        {
            error = net::errorMessage(net::lastError());
            return false;
        }
        if (socketError != 0)
        {
            error = net::errorMessage(socketError);
            return false;
        }
        return true;
    }

    error = "timed out after " + std::to_string(kConnectTimeoutMs / 1000) + " s";
    return false;
}

// Unlike the control server's bind address, this one is a user-supplied host
// name, so a DNS lookup is expected here rather than forbidden.
net::Socket connectTo(const std::string& host, uint16_t port, const std::atomic<bool>& cancelled,
                      std::string& error)
{
    addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    char service[8];
    std::snprintf(service, sizeof(service), "%u", static_cast<unsigned>(port));

    addrinfo* resolved = nullptr;
    if (::getaddrinfo(host.c_str(), service, &hints, &resolved) != 0 || resolved == nullptr)
    {
        error = "cannot resolve " + host;
        return net::kInvalidSocket;
    }

    net::Socket connected = net::kInvalidSocket;
    error = "no usable address";

    for (addrinfo* candidate = resolved; candidate != nullptr; candidate = candidate->ai_next)
    {
        net::Socket attempt =
            ::socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
        if (attempt == net::kInvalidSocket)
        {
            error = net::errorMessage(net::lastError());
            continue;
        }

#if !defined(_WIN32)
        // select() cannot watch a descriptor at or above FD_SETSIZE, and
        // FD_SET would write past the fd_set.
        if (static_cast<int>(attempt) >= FD_SETSIZE)
        {
            net::closeSocket(attempt);
            error = "descriptor above FD_SETSIZE";
            continue;
        }
#endif

        if (!net::setNonBlocking(attempt))
        {
            error = net::errorMessage(net::lastError());
            net::closeSocket(attempt);
            continue;
        }

        if (::connect(attempt, candidate->ai_addr, static_cast<int>(candidate->ai_addrlen)) == 0)
        {
            connected = attempt;
            break;
        }

        const int connectError = net::lastError();
        if (!connectInProgress(connectError))
        {
            error = net::errorMessage(connectError);
            net::closeSocket(attempt);
            continue;
        }

        if (awaitConnect(attempt, cancelled, error))
        {
            connected = attempt;
            break;
        }
        net::closeSocket(attempt);
        if (cancelled.load())
            break;
    }

    ::freeaddrinfo(resolved);

    if (connected != net::kInvalidSocket)
    {
        // Protocol messages are small and their round trip is measured (MATP
        // sections 8 and 9), so waiting 40 ms for a companion packet would
        // show up directly as clock-sync error.
        int noDelay = 1;
        ::setsockopt(connected, IPPROTO_TCP, TCP_NODELAY,
                     reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
        error.clear();
    }
    return connected;
}

} // namespace

struct ServerConnection::Impl
{
    net::Library library;
    DataHandler onData;

    // Guards everything below it, and is held across a write so two threads
    // cannot interleave bytes on the same socket.
    mutable std::mutex mutex;
    std::condition_variable wake;

    std::string address;
    std::string host;
    uint16_t port = 0;
    // Bumped by every address change. The connection thread carries the value
    // it started an attempt with, so it can tell "still current" from "the
    // address changed under me" without a second flag.
    uint32_t generation = 0;

    net::Socket socket = net::kInvalidSocket;
    std::string outbox;
    std::string lastError;
    std::string networkLink; // networkLinkOf(socket) while connected

    // Set by dropConnection and cleared by every new connection. A socket that
    // was only shut down does not reliably wake the select() the connection
    // thread is sitting in -- stop() and setServer() have the same problem and
    // solve it the same way, with a flag the loop re-reads on every pass.
    bool dropRequested = false;

    std::thread thread;
    std::atomic<bool> stopFlag;
    std::atomic<bool> running;
    std::atomic<ServerConnectionState> state;
    std::atomic<uint64_t> attempts;
    std::atomic<uint64_t> connections;
    std::atomic<uint64_t> receiveCalls;
    std::atomic<uint64_t> receivedBytes;
    std::atomic<uint64_t> receiveNs;
    std::atomic<uint64_t> handlerNs;

    Impl()
        : stopFlag(false), running(false), state(ServerConnectionState::NotConfigured),
          attempts(0), connections(0), receiveCalls(0), receivedBytes(0), receiveNs(0), handlerNs(0)
    {
    }

    void run();
    void serve(uint32_t myGeneration);
    bool flushLocked();
    void shutdownLocked();
    void closeConnection();
    void waitFor(uint32_t milliseconds, uint32_t myGeneration);
};

void ServerConnection::Impl::shutdownLocked()
{
    // Shut down, not closed: the connection thread may be inside select() on
    // this descriptor, and closing it out from under that call is undefined.
    net::shutdownSocket(socket);
}

void ServerConnection::Impl::closeConnection()
{
    std::lock_guard<std::mutex> lock(mutex);
    net::closeSocket(socket);
    socket = net::kInvalidSocket;
    outbox.clear();
    networkLink.clear();
}

bool ServerConnection::Impl::flushLocked()
{
    while (!outbox.empty())
    {
        const int sent = net::transmit(socket, outbox.data(), static_cast<int>(outbox.size()));
        if (sent > 0)
        {
            outbox.erase(0, static_cast<size_t>(sent));
            continue;
        }
        const int error = net::lastError();
        if (sent < 0 && net::wouldBlock(error))
            return true; // the rest goes out when the socket is writable again

        lastError = net::errorMessage(error);
        shutdownLocked();
        return false;
    }
    return true;
}

void ServerConnection::Impl::waitFor(uint32_t milliseconds, uint32_t myGeneration)
{
    std::unique_lock<std::mutex> lock(mutex);
    wake.wait_for(lock, std::chrono::milliseconds(milliseconds), [this, myGeneration] {
        return stopFlag.load() || generation != myGeneration;
    });
}

void ServerConnection::Impl::run()
{
    size_t backoffStep = 0;
    uint32_t lastGeneration = 0;
    std::string loggedError; // so a server that is simply off does not fill the log

    while (!stopFlag.load())
    {
        std::string currentHost;
        uint16_t currentPort = 0;
        uint32_t currentGeneration = 0;
        {
            std::lock_guard<std::mutex> lock(mutex);
            currentHost = host;
            currentPort = port;
            currentGeneration = generation;
        }

        if (currentGeneration != lastGeneration)
        {
            // A new address deserves an immediate attempt, not the backoff the
            // previous one had worked its way up to.
            backoffStep = 0;
            loggedError.clear();
            lastGeneration = currentGeneration;
        }

        if (currentHost.empty())
        {
            state.store(ServerConnectionState::NotConfigured);
            std::unique_lock<std::mutex> lock(mutex);
            wake.wait(lock, [this, currentGeneration] {
                return stopFlag.load() || generation != currentGeneration;
            });
            continue;
        }

        state.store(ServerConnectionState::Connecting);
        attempts.fetch_add(1);

        std::string error;
        const net::Socket connected = connectTo(currentHost, currentPort, stopFlag, error);
        if (connected == net::kInvalidSocket)
        {
            {
                std::lock_guard<std::mutex> lock(mutex);
                lastError = error;
            }
            state.store(ServerConnectionState::Disconnected);
            if (stopFlag.load())
                continue;

            const uint32_t delayMs = kBackoffMs[backoffStep];
            if (error != loggedError)
            {
                MULTIROOM_WARN("cannot connect to %s:%u: %s; retrying in %lu s", currentHost.c_str(),
                         static_cast<unsigned>(currentPort), error.c_str(),
                         static_cast<unsigned long>(delayMs / 1000));
                loggedError = error;
            }
            else
            {
                MULTIROOM_DEBUG("still cannot connect to %s:%u: %s", currentHost.c_str(),
                          static_cast<unsigned>(currentPort), error.c_str());
            }
            waitFor(delayMs, currentGeneration);
            if (backoffStep + 1 < kBackoffCount)
                ++backoffStep;
            continue;
        }

        // Outside the lock: the interface lookup asks the OS.
        const std::string link = networkLinkOf(connected);
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (generation != currentGeneration || stopFlag.load())
            {
                // The address changed while we were connecting, so this socket
                // leads somewhere nobody asked for any more.
                net::closeSocket(connected);
                continue;
            }
            socket = connected;
            outbox.clear();
            lastError.clear();
            dropRequested = false;
            networkLink = link;
        }

        backoffStep = 0;
        loggedError.clear();
        // Counted before it is Connected, in this order: a caller that reads
        // the state first and the count second gets this socket's count with
        // Connected (Session::poll, AudioStream::poll).
        connections.fetch_add(1);
        state.store(ServerConnectionState::Connected);
        MULTIROOM_INFO("connected to %s:%u over %s", currentHost.c_str(), static_cast<unsigned>(currentPort),
                       link.empty() ? "an unknown link" : link.c_str());

        serve(currentGeneration);

        std::string reason;
        {
            std::lock_guard<std::mutex> lock(mutex);
            reason = lastError;
        }
        closeConnection();
        state.store(ServerConnectionState::Disconnected);

        if (stopFlag.load())
            continue;

        MULTIROOM_WARN("connection to %s:%u lost: %s", currentHost.c_str(),
                 static_cast<unsigned>(currentPort), reason.empty() ? "closed" : reason.c_str());
        // A server that accepts and immediately closes is throttled to one
        // attempt per backoff step this way, rather than spinning.
        waitFor(kBackoffMs[0], currentGeneration);
    }

    std::lock_guard<std::mutex> lock(mutex);
    state.store(host.empty() ? ServerConnectionState::NotConfigured
                             : ServerConnectionState::Disconnected);
}

void ServerConnection::Impl::serve(uint32_t myGeneration)
{
    std::vector<char> buffer(kReadChunkBytes);

    for (;;)
    {
        if (stopFlag.load())
            return;

        net::Socket current = net::kInvalidSocket;
        bool wantWrite = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (generation != myGeneration || dropRequested)
                return;
            current = socket;
            wantWrite = !outbox.empty();
        }
        if (current == net::kInvalidSocket)
            return;

        fd_set readable;
        fd_set writable;
        FD_ZERO(&readable);
        FD_ZERO(&writable);
        FD_SET(current, &readable);
        if (wantWrite)
            FD_SET(current, &writable);

        int highest = 0;
#if !defined(_WIN32)
        highest = static_cast<int>(current);
#endif
        timeval timeout;
        timeout.tv_sec = kPollMs / 1000;
        timeout.tv_usec = (kPollMs % 1000) * 1000;

        const int ready =
            ::select(highest + 1, &readable, wantWrite ? &writable : nullptr, nullptr, &timeout);
        if (ready < 0)
        {
            const int error = net::lastError();
            if (net::wouldBlock(error))
                continue;
            std::lock_guard<std::mutex> lock(mutex);
            lastError = net::errorMessage(error);
            return;
        }
        if (ready == 0)
            continue;

        if (wantWrite && FD_ISSET(current, &writable))
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!flushLocked())
                return;
        }

        if (FD_ISSET(current, &readable))
        {
            const auto receiveStart = std::chrono::steady_clock::now();
            const int received =
                net::receive(current, buffer.data(), static_cast<int>(buffer.size()));
            const auto receiveEnd = std::chrono::steady_clock::now();
            if (received == 0)
            {
                std::lock_guard<std::mutex> lock(mutex);
                // Only when nobody has already said why: a connection this
                // side dropped on purpose reads as closed by the peer from
                // here, and reporting that would be a lie.
                if (lastError.empty())
                    lastError = "closed by the server";
                return;
            }
            if (received < 0)
            {
                const int error = net::lastError();
                if (net::wouldBlock(error))
                    continue;
                std::lock_guard<std::mutex> lock(mutex);
                lastError = net::errorMessage(error);
                return;
            }

            receiveCalls.fetch_add(1, std::memory_order_relaxed);
            receivedBytes.fetch_add(static_cast<uint64_t>(received), std::memory_order_relaxed);
            receiveNs.fetch_add(elapsedNs(receiveStart, receiveEnd), std::memory_order_relaxed);

            // Raw bytes, on this thread, framed by nothing: one read is not
            // one message, and putting them in order is the handler's job.
            if (onData)
            {
                onData(buffer.data(), static_cast<size_t>(received));
                handlerNs.fetch_add(elapsedNs(receiveEnd, std::chrono::steady_clock::now()),
                                    std::memory_order_relaxed);
            }
        }
    }
}

const char* serverConnectionStateName(ServerConnectionState state)
{
    switch (state)
    {
    case ServerConnectionState::NotConfigured:
        return "notConfigured";
    case ServerConnectionState::Disconnected:
        return "disconnected";
    case ServerConnectionState::Connecting:
        return "connecting";
    case ServerConnectionState::Connected:
        return "connected";
    }
    return "unknown";
}

ServerConnection::ServerConnection() : impl_(new Impl()) {}

ServerConnection::~ServerConnection()
{
    stop();
}

bool ServerConnection::start(DataHandler onData, std::string& error)
{
    if (impl_->running.load())
    {
        error = "server connection is already running";
        return false;
    }
    if (!impl_->library.isReady())
    {
        error = impl_->library.error();
        return false;
    }

    impl_->onData = std::move(onData);
    impl_->stopFlag.store(false);
    impl_->running.store(true);
#if defined(ESP_PLATFORM)
    // Named for the `diagnostics` task report; both MATP connections are one.
    esp_pthread_cfg_t named = esp_pthread_get_default_config();
    named.thread_name = "client_conn";
    esp_pthread_set_cfg(&named);
#endif
    impl_->thread = std::thread([this] { impl_->run(); });
#if defined(ESP_PLATFORM)
    const esp_pthread_cfg_t defaults = esp_pthread_get_default_config();
    esp_pthread_set_cfg(&defaults);
#endif
    return true;
}

void ServerConnection::stop()
{
    if (!impl_->running.load())
        return;

    impl_->stopFlag.store(true);
    {
        // Breaks the select() the thread may be sitting in; the connect slices
        // and the backoff wait check the flag themselves.
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->shutdownLocked();
    }
    impl_->wake.notify_all();

    if (impl_->thread.joinable())
        impl_->thread.join();

    impl_->closeConnection();
    impl_->onData = nullptr;
    impl_->running.store(false);
}

bool ServerConnection::setServer(const std::string& address, std::string& error)
{
    std::string newHost;
    uint16_t newPort = 0;
    if (!address.empty() && !parseServerAddress(address, newHost, newPort))
    {
        // Refused before anything is touched: a typo must not take down a
        // working connection.
        error = "invalid server address \"" + address + "\"";
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (address == impl_->address)
            return true;

        impl_->address = address;
        impl_->host = newHost;
        impl_->port = newPort;
        ++impl_->generation;
        impl_->lastError.clear();
        impl_->shutdownLocked();
    }
    impl_->wake.notify_all();
    return true;
}

std::string ServerConnection::server() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->address;
}

ServerConnectionState ServerConnection::state() const
{
    return impl_->state.load();
}

bool ServerConnection::send(const void* data, size_t size)
{
    if (data == nullptr || size == 0)
        return false;

    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->socket == net::kInvalidSocket)
        return false;

    if (impl_->outbox.size() + size > kMaxOutboxBytes)
    {
        // A peer that has stopped reading. Growing the buffer would trade
        // memory for a stall that never resolves; dropping the connection
        // hands the problem to the reconnect path, which can solve it.
        impl_->lastError = "the server is not reading";
        impl_->shutdownLocked();
        return false;
    }

    impl_->outbox.append(static_cast<const char*>(data), size);
    return impl_->flushLocked();
}

void ServerConnection::dropConnection(const std::string& reason)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->socket == net::kInvalidSocket)
        return;

    impl_->lastError = reason;
    impl_->dropRequested = true;
    impl_->shutdownLocked();
}

uint64_t ServerConnection::connectAttempts() const
{
    return impl_->attempts.load();
}

uint64_t ServerConnection::connectionCount() const
{
    return impl_->connections.load();
}

uint64_t ServerConnection::receiveCalls() const
{
    return impl_->receiveCalls.load(std::memory_order_relaxed);
}

uint64_t ServerConnection::receivedBytes() const
{
    return impl_->receivedBytes.load(std::memory_order_relaxed);
}

uint64_t ServerConnection::receiveNs() const
{
    return impl_->receiveNs.load(std::memory_order_relaxed);
}

uint64_t ServerConnection::handlerNs() const
{
    return impl_->handlerNs.load(std::memory_order_relaxed);
}

std::string ServerConnection::lastError() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->lastError;
}

std::string ServerConnection::networkLink() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->networkLink;
}

} // namespace multiroom
