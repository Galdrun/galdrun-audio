#include "discovery/DiscoveryListener.h"

#include "net/Socket.h"
#include "util/Logger.h"

#include <atomic>
#include <thread>

namespace multiroom {
namespace {

// A beacon is a couple of hundred bytes. Anything that does not fit is not
// one, and recvfrom truncates it into a parse failure.
constexpr int kMaxDatagramBytes = 2048;

// Only how often the loop notices a stop request.
constexpr int kSelectTimeoutMs = 200;

} // namespace

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
    Impl(DiscoveredClients& clients, const IClock& clock, std::function<void()> onTick)
        : clients_(clients), clock_(clock), onTick_(std::move(onTick))
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
            if (clients_.onBeacon(beacon, address, clock_.monotonicNowNs()))
                MULTIROOM_INFO("discovered client {} at {}, control port {}", beacon.clientId, address,
                         beacon.controlPort);
        }
    }

    DiscoveredClients& clients_;
    const IClock& clock_;
    std::function<void()> onTick_;
    net::Library library_;
    net::Socket socket_ = net::kInvalidSocket;
    std::atomic<bool> listening_{true};
    std::atomic<uint64_t> received_{0};
    std::atomic<uint64_t> rejected_{0};
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
    auto impl = std::make_unique<Impl>(clients, clock, std::move(settings.onTick));

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

} // namespace multiroom
