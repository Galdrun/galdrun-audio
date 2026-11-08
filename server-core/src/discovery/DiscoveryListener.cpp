#include "discovery/DiscoveryListener.h"

#include "net/Socket.h"
#include "util/Logger.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

namespace multiroom {
namespace {

// A beacon is a couple of hundred bytes. Anything that does not fit is not
// one, and recvfrom truncates it into a parse failure.
constexpr int kMaxDatagramBytes = 2048;

// Only how often the loop notices a stop request.
constexpr int kSelectTimeoutMs = 200;

constexpr size_t kNtpPacketBytes = 48;
constexpr uint64_t kNtpEraOffsetSeconds = 2208988800ULL; // 1900 to 1970

// 2026-01-01 UTC. A host clock before it has not been set (a Pi server
// before its own sync): no answer then, so a device keeps its time rather
// than going back to 1970.
constexpr int64_t kPlausibleSinceUnixSeconds = 1767225600;

bool clockIsSet(std::chrono::system_clock::time_point now)
{
    return std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count() >=
           kPlausibleSinceUnixSeconds;
}

// An SNTP client request (RFC 4330): mode 3, version 1-4. A JSON beacon
// starts with '{', which reads as version 7, so the two never mix.
bool isTimeRequest(const char* data, int bytes)
{
    if (bytes < static_cast<int>(kNtpPacketBytes))
        return false;
    const auto first = static_cast<uint8_t>(data[0]);
    const unsigned version = (first >> 3) & 7U;
    return (first & 7U) == 3 && version >= 1 && version <= 4;
}

void putTimestamp(uint8_t* out, std::chrono::system_clock::time_point time)
{
    const auto sinceEpoch = std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch());
    const uint64_t ns = static_cast<uint64_t>(sinceEpoch.count());
    const uint64_t seconds = ns / 1000000000ULL + kNtpEraOffsetSeconds;
    const uint64_t fraction = ((ns % 1000000000ULL) << 32) / 1000000000ULL;
    for (int i = 0; i < 4; ++i)
    {
        out[i] = static_cast<uint8_t>(seconds >> (24 - 8 * i));
        out[4 + i] = static_cast<uint8_t>(fraction >> (24 - 8 * i));
    }
}

} // namespace

std::array<uint8_t, 48> makeTimeReply(const uint8_t* request,
                                      std::chrono::system_clock::time_point received,
                                      std::chrono::system_clock::time_point transmitted)
{
    std::array<uint8_t, kNtpPacketBytes> reply{};
    // No leap warning, the request's version, server mode. Stratum 3: this
    // host's own clock, which it keeps from wherever its OS does.
    reply[0] = static_cast<uint8_t>(request[0] & 0x38U) | 4U;
    reply[1] = 3;
    reply[2] = request[2];                 // poll interval, as asked
    reply[3] = static_cast<uint8_t>(-20); // precision about 1 us
    reply[9] = 0x01;                       // root dispersion ~ 15.6 ms
    std::memcpy(&reply[12], "GLDR", 4);    // reference id
    putTimestamp(&reply[16], received);    // reference
    std::memcpy(&reply[24], request + 40, 8); // originate: the request's transmit
    putTimestamp(&reply[32], received);
    putTimestamp(&reply[40], transmitted);
    return reply;
}

const char* errorName(DiscoveryListenerError error)
{
    switch (error)
    {
    case DiscoveryListenerError::SocketLibrary:
        return "SocketLibrary";
    case DiscoveryListenerError::BindFailed:
        return "BindFailed";
    }
    return "Unknown";
}

class DiscoveryListener::Impl
{
public:
    Impl(DiscoveredClients& clients, const IClock& clock, std::function<void()> onTick,
         std::function<void(const std::string&, bool)> onActiveChanged)
        : clients_(clients), clock_(clock), onTick_(std::move(onTick)),
          onActiveChanged_(std::move(onActiveChanged))
    {
    }

    ~Impl()
    {
        thread_.request_stop();
        if (thread_.joinable())
            thread_.join();
        net::closeSocket(socket_);
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    [[nodiscard]] Status<DiscoveryListenerError> bind(const DiscoveryListenerSettings& settings,
                                                      uint16_t& portOut)
    {
        if (!library_.isReady())
            return fail(DiscoveryListenerError::SocketLibrary, library_.error());

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(settings.port);
        if (::inet_pton(AF_INET, settings.bindAddress.c_str(), &address.sin_addr) != 1)
        {
            return fail(DiscoveryListenerError::BindFailed,
                        "\"" + settings.bindAddress + "\" is not a numeric IPv4 address");
        }

        const net::Socket socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket == net::kInvalidSocket)
            return fail(DiscoveryListenerError::BindFailed, net::errorMessage(net::lastError()));
        if (!net::selectable(socket))
        {
            net::closeSocket(socket);
            return fail(DiscoveryListenerError::BindFailed,
                        "descriptor is past what select() can watch");
        }

        if (::bind(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
            !net::setNonBlocking(socket))
        {
            const std::string error = net::errorMessage(net::lastError());
            net::closeSocket(socket);
            return fail(DiscoveryListenerError::BindFailed,
                        settings.bindAddress + ":" + std::to_string(settings.port) + ": " + error);
        }
        socket_ = socket;

        sockaddr_in bound{};
#if defined(_WIN32)
        int length = static_cast<int>(sizeof(bound));
#else
        socklen_t length = static_cast<socklen_t>(sizeof(bound));
#endif
        if (::getsockname(socket_, reinterpret_cast<sockaddr*>(&bound), &length) != 0)
            return fail(DiscoveryListenerError::BindFailed, "could not read back the bound port");
        portOut = ntohs(bound.sin_port);
        return {};
    }

    void run()
    {
        thread_ = std::jthread([this](std::stop_token stop) { loop(stop); });
    }

    [[nodiscard]] bool listening() const noexcept { return listening_.load(); }
    [[nodiscard]] uint64_t received() const noexcept { return received_.load(); }
    [[nodiscard]] uint64_t rejected() const noexcept { return rejected_.load(); }
    [[nodiscard]] uint64_t timeRequests() const noexcept { return timeRequests_.load(); }

private:
    void loop(const std::stop_token& stop)
    {
        while (!stop.stop_requested())
        {
            fd_set readable;
            FD_ZERO(&readable);
            FD_SET(socket_, &readable);
            timeval timeout{};
            timeout.tv_usec = kSelectTimeoutMs * 1000;
            const int ready = net::waitReady(socket_, &readable, nullptr, nullptr, timeout);
            if (ready < 0 && !net::wouldBlock(net::lastError()))
            {
                // It would fail again at once, every pass: a loop that went on
                // would spin, and the tick with it.
                MULTIROOM_ERROR("discovery listener: select failed: {}",
                          net::errorMessage(net::lastError()));
                listening_.store(false);
                break;
            }
            if (ready > 0)
                drain();
            if (onTick_)
                onTick_();
        }
    }

    // Everything that has arrived, so a burst from a full house is one pass.
    void drain()
    {
        char buffer[kMaxDatagramBytes];
        for (;;)
        {
            sockaddr_in from{};
#if defined(_WIN32)
            int fromLength = static_cast<int>(sizeof(from));
#else
            socklen_t fromLength = static_cast<socklen_t>(sizeof(from));
#endif
            const int bytes = static_cast<int>(::recvfrom(socket_, buffer, kMaxDatagramBytes, 0,
                                                          reinterpret_cast<sockaddr*>(&from),
                                                          &fromLength));
            // Would-block is the normal end of a pass. Anything else concerns
            // one datagram, and the next select() hands over the one after it
            // -- a retry here instead would spin on an error that persists.
            if (bytes < 0)
                return;

            // Time for the Galdrun appliances when their router serves none
            // (galdrun-time-sources on the devices): on this port because it
            // is already open to the network, firewall included.
            if (isTimeRequest(buffer, bytes))
            {
                const auto received = std::chrono::system_clock::now();
                if (!clockIsSet(received))
                    continue;
                const auto reply = makeTimeReply(reinterpret_cast<const uint8_t*>(buffer), received,
                                                 std::chrono::system_clock::now());
                ::sendto(socket_, reinterpret_cast<const char*>(reply.data()),
                         static_cast<int>(reply.size()), 0, reinterpret_cast<const sockaddr*>(&from),
                         fromLength);
                ++timeRequests_;
                continue;
            }

            matp::ClientBeacon beacon;
            std::string error;
            if (!matp::parseClientBeacon(std::string(buffer, static_cast<size_t>(bytes)), beacon,
                                         error))
            {
                ++rejected_;
                MULTIROOM_DEBUG("discovery: dropped a datagram that is not a beacon: {}", error);
                continue;
            }

            char text[INET_ADDRSTRLEN] = {};
            const std::string address =
                ::inet_ntop(AF_INET, &from.sin_addr, text, sizeof(text)) != nullptr ? text : "";
            ++received_;
            const auto previous = clients_.find(ClientId(beacon.clientId));
            if (clients_.onBeacon(beacon, address, clock_.monotonicNowNs()))
                MULTIROOM_INFO("discovered client {} at {}, control port {}", beacon.clientId, address,
                         beacon.controlPort);
            if (onActiveChanged_ && ((!previous && !beacon.active) ||
                                    (previous && previous->active != beacon.active)))
                onActiveChanged_(beacon.clientId, beacon.active);
        }
    }

    DiscoveredClients& clients_;
    const IClock& clock_;
    std::function<void()> onTick_;
    std::function<void(const std::string&, bool)> onActiveChanged_;
    net::Library library_;
    net::Socket socket_ = net::kInvalidSocket;
    std::atomic<bool> listening_{true};
    std::atomic<uint64_t> received_{0};
    std::atomic<uint64_t> rejected_{0};
    std::atomic<uint64_t> timeRequests_{0};
    std::jthread thread_; // last: stopped before anything it reads
};

DiscoveryListener::DiscoveryListener(std::unique_ptr<Impl> impl, uint16_t port)
    : impl_(std::move(impl)), port_(port)
{
}

DiscoveryListener::~DiscoveryListener() = default;

Result<std::unique_ptr<DiscoveryListener>, DiscoveryListenerError>
DiscoveryListener::start(DiscoveryListenerSettings settings, DiscoveredClients& clients,
                         const IClock& clock)
{
    auto impl = std::make_unique<Impl>(clients, clock, std::move(settings.onTick),
                                       std::move(settings.onActiveChanged));

    uint16_t port = 0;
    if (const Status<DiscoveryListenerError> bound = impl->bind(settings, port); !bound)
        return std::unexpected(bound.error());

    impl->run();

    MULTIROOM_INFO("discovery beacons on {}:{} (udp)", settings.bindAddress, port);
    return std::unique_ptr<DiscoveryListener>(new DiscoveryListener(std::move(impl), port));
}

bool DiscoveryListener::listening() const noexcept
{
    return impl_->listening();
}

uint64_t DiscoveryListener::received() const noexcept
{
    return impl_->received();
}

uint64_t DiscoveryListener::rejected() const noexcept
{
    return impl_->rejected();
}

uint64_t DiscoveryListener::timeRequests() const noexcept
{
    return impl_->timeRequests();
}

} // namespace multiroom
