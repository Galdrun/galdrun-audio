#include "discovery/BeaconSender.h"

#include "matp/ClientBeacon.h" // kBeaconIntervalNs

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <iphlpapi.h>
#else
#  include <arpa/inet.h>
#  include <cerrno>
#  include <cstring>
#  include <fcntl.h>
#  if defined(ESP_PLATFORM)
#    include "esp_netif.h"
#  else
#    include <ifaddrs.h>
#    include <net/if.h>
#  endif
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
#endif

#include <vector>

namespace multiroom {
namespace discovery {
namespace {

// The few socket calls this needs. Not the client's or the core's
// `net/Socket.h`: those are private to their own trees, and a datagram sender
// needs none of their connection handling.
#if defined(_WIN32)
using Socket = SOCKET;
const Socket kInvalidSocket = INVALID_SOCKET;
#else
using Socket = int;
const Socket kInvalidSocket = -1;
#endif

int socketError()
{
#if defined(_WIN32)
    return ::WSAGetLastError();
#else
    return errno;
#endif
}

std::string errorMessage(int error)
{
#if defined(_WIN32)
    char* buffer = nullptr;
    const DWORD length = ::FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, static_cast<DWORD>(error), 0, reinterpret_cast<char*>(&buffer), 0, nullptr);
    if (length == 0 || buffer == nullptr)
        return "socket error " + std::to_string(error);

    std::string message(buffer, length);
    ::LocalFree(buffer);
    while (!message.empty() && (message.back() == '\r' || message.back() == '\n' ||
                                message.back() == ' ' || message.back() == '.'))
    {
        message.pop_back();
    }
    return message;
#else
    return std::string(std::strerror(error));
#endif
}

void closeSocket(Socket socket)
{
    if (socket == kInvalidSocket)
        return;
#if defined(_WIN32)
    ::closesocket(socket);
#else
    ::close(socket);
#endif
}

bool setNonBlocking(Socket socket)
{
#if defined(_WIN32)
    u_long mode = 1;
    return ::ioctlsocket(socket, FIONBIO, &mode) == 0;
#else
    const int flags = ::fcntl(socket, F_GETFL, 0);
    return flags >= 0 && ::fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

std::string addressText(const in_addr& address)
{
    char text[INET_ADDRSTRLEN] = {};
    if (::inet_ntop(AF_INET, &address, text, sizeof(text)) == nullptr)
        return "?";
    return text;
}

void addUnique(std::vector<in_addr>& out, const in_addr& address)
{
    // Two addresses in one subnet share a broadcast address, and the LAN
    // would otherwise hear every beacon twice.
    for (const in_addr& existing : out)
    {
        if (existing.s_addr == address.s_addr)
            return;
    }
    out.push_back(address);
}

// The directed broadcast address of every IPv4 interface that is up, loopback
// excluded. Empty is not an error: at boot there may simply be no network yet.
bool listBroadcastAddresses(std::vector<in_addr>& out, std::string& error)
{
    out.clear();
#if defined(_WIN32)
    ULONG size = 16 * 1024;
    std::vector<unsigned char> buffer;
    ULONG result = ERROR_BUFFER_OVERFLOW;
    // The list can grow between the call that sizes it and the one that fills
    // it, which is the documented reason to retry.
    for (int attempt = 0; attempt < 3 && result == ERROR_BUFFER_OVERFLOW; ++attempt)
    {
        buffer.resize(size);
        result = ::GetAdaptersAddresses(
            AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
            nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    }
    if (result == ERROR_NO_DATA)
        return true;
    if (result != NO_ERROR)
    {
        error = "cannot list the network interfaces: " + errorMessage(static_cast<int>(result));
        return false;
    }

    for (const IP_ADAPTER_ADDRESSES* adapter =
             reinterpret_cast<const IP_ADAPTER_ADDRESSES*>(buffer.data());
         adapter != nullptr; adapter = adapter->Next)
    {
        if (adapter->OperStatus != IfOperStatusUp ||
            adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
            continue;

        for (const IP_ADAPTER_UNICAST_ADDRESS* unicast = adapter->FirstUnicastAddress;
             unicast != nullptr; unicast = unicast->Next)
        {
            if (unicast->Address.lpSockaddr->sa_family != AF_INET)
                continue;
            // A /31 or /32 has no broadcast address, and /0 is not a subnet.
            const ULONG prefix = unicast->OnLinkPrefixLength;
            if (prefix == 0 || prefix >= 31)
                continue;

            const uint32_t host = ntohl(
                reinterpret_cast<const sockaddr_in*>(unicast->Address.lpSockaddr)->sin_addr.s_addr);
            const uint32_t mask = 0xFFFFFFFFu << (32 - prefix);
            in_addr broadcast{};
            broadcast.s_addr = htonl(host | ~mask);
            addUnique(out, broadcast);
        }
    }
#elif defined(ESP_PLATFORM)
    // lwIP has no getifaddrs; esp_netif is its interface list. The walk is the
    // "unsafe" one, which is safe here: app_main creates the station and
    // access point interfaces before anything runs and never removes them.
    (void)error;
    for (esp_netif_t* netif = esp_netif_next_unsafe(nullptr); netif != nullptr;
         netif = esp_netif_next_unsafe(netif))
    {
        esp_netif_ip_info_t info{};
        if (!esp_netif_is_netif_up(netif) || esp_netif_get_ip_info(netif, &info) != ESP_OK ||
            info.ip.addr == 0 || info.netmask.addr == 0)
            continue;
        in_addr broadcast{};
        broadcast.s_addr = info.ip.addr | ~info.netmask.addr; // both in network order
        addUnique(out, broadcast);
    }
#else
    ifaddrs* list = nullptr;
    if (::getifaddrs(&list) != 0)
    {
        error = "cannot list the network interfaces: " + errorMessage(socketError());
        return false;
    }
    for (const ifaddrs* entry = list; entry != nullptr; entry = entry->ifa_next)
    {
        if (entry->ifa_addr == nullptr || entry->ifa_addr->sa_family != AF_INET)
            continue;
        if ((entry->ifa_flags & IFF_UP) == 0 || (entry->ifa_flags & IFF_LOOPBACK) != 0 ||
            (entry->ifa_flags & IFF_BROADCAST) == 0 || entry->ifa_broadaddr == nullptr)
            continue;
        addUnique(out, reinterpret_cast<const sockaddr_in*>(entry->ifa_broadaddr)->sin_addr);
    }
    ::freeifaddrs(list);
#endif
    return true;
}

} // namespace

struct BeaconSender::Impl
{
    bool libraryReady = false;
    Socket socket = kInvalidSocket;
    uint16_t port = 0;
    bool fixedDestination = false;
    in_addr destination{};
    Log log;

    bool hasSent = false;
    uint64_t lastSendNs = 0;
    uint32_t sent = 0;
    std::string lastError;
    std::string targets; // as last logged, so a change is logged once

    void info(const std::string& message)
    {
        if (log)
            log(false, message);
    }

    void report(const std::string& error)
    {
        // Once per change: at boot the network is routinely not up yet, and a
        // warning every two seconds would bury everything else in the log.
        if (error != lastError && log)
            log(true, error);
        lastError = error;
    }
};

BeaconSender::BeaconSender() : impl_(new Impl) {}

BeaconSender::~BeaconSender()
{
    stop();
#if defined(_WIN32)
    if (impl_->libraryReady)
        ::WSACleanup();
#endif
}

bool BeaconSender::start(uint16_t port, std::string& error, const std::string& destination,
                         Log log)
{
    if (impl_->socket != kInvalidSocket)
    {
        error = "discovery beacon is already running";
        return false;
    }

#if defined(_WIN32)
    // Every WSAStartup needs its own WSACleanup, which the destructor does.
    if (!impl_->libraryReady)
    {
        WSADATA data;
        const int result = ::WSAStartup(MAKEWORD(2, 2), &data);
        if (result != 0)
        {
            error = "WSAStartup failed: " + errorMessage(result);
            return false;
        }
        impl_->libraryReady = true;
    }
#endif

    in_addr fixed{};
    if (!destination.empty() && ::inet_pton(AF_INET, destination.c_str(), &fixed) != 1)
    {
        error = "invalid beacon destination \"" + destination + "\"";
        return false;
    }

    const Socket socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket == kInvalidSocket)
    {
        error = "cannot open the beacon socket: " + errorMessage(socketError());
        return false;
    }

    // Without it a send to a broadcast address is refused outright.
    int broadcast = 1;
    if (::setsockopt(socket, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&broadcast),
                     sizeof(broadcast)) != 0)
    {
        error = "cannot enable broadcast on the beacon socket: " + errorMessage(socketError());
        closeSocket(socket);
        return false;
    }

    // The beacon is sent from a loop that must never wait on the network: a
    // datagram that cannot go out now is simply the next one.
    if (!setNonBlocking(socket))
    {
        error = "cannot set the beacon socket non-blocking: " + errorMessage(socketError());
        closeSocket(socket);
        return false;
    }

    impl_->socket = socket;
    impl_->port = port;
    impl_->fixedDestination = !destination.empty();
    impl_->destination = fixed;
    impl_->log = std::move(log);
    impl_->hasSent = false;
    return true;
}

void BeaconSender::stop()
{
    closeSocket(impl_->socket);
    impl_->socket = kInvalidSocket;
}

void BeaconSender::poll(uint64_t nowNs, const std::function<std::string()>& payload)
{
    if (impl_->socket == kInvalidSocket)
        return;
    if (impl_->hasSent && nowNs - impl_->lastSendNs < matp::kBeaconIntervalNs)
        return;

    // Counted as an attempt whether or not it goes out, so a network that is
    // not up yet is retried at the interval rather than on every pass.
    impl_->hasSent = true;
    impl_->lastSendNs = nowNs;

    std::vector<in_addr> targets;
    if (impl_->fixedDestination)
    {
        targets.push_back(impl_->destination);
    }
    else
    {
        std::string error;
        if (!listBroadcastAddresses(targets, error))
        {
            impl_->report(error);
            return;
        }
    }
    if (targets.empty())
    {
        impl_->report("no network interface to send the discovery beacon on");
        return;
    }

    std::string targetText;
    for (const in_addr& target : targets)
        targetText += (targetText.empty() ? "" : ", ") + addressText(target);
    if (targetText != impl_->targets)
    {
        impl_->info("discovery beacon to " + targetText + ", port " +
                    std::to_string(impl_->port));
        impl_->targets = targetText;
    }

    const std::string text = payload();
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(impl_->port);

    uint32_t delivered = 0;
    std::string failure;
    for (const in_addr& target : targets)
    {
        address.sin_addr = target;
        const int result = static_cast<int>(
            ::sendto(impl_->socket, text.data(), static_cast<int>(text.size()), 0,
                     reinterpret_cast<const sockaddr*>(&address),
                     static_cast<socklen_t>(sizeof(address))));
        if (result < 0)
            failure = "beacon to " + addressText(target) + " failed: " + errorMessage(socketError());
        else
            ++delivered;
    }

    if (!failure.empty())
    {
        impl_->report(failure);
    }
    else if (!impl_->lastError.empty())
    {
        impl_->info("discovery beacon is being sent again");
        impl_->lastError.clear();
    }
    if (delivered > 0)
        ++impl_->sent;
}

uint32_t BeaconSender::sent() const
{
    return impl_->sent;
}

const std::string& BeaconSender::lastError() const
{
    return impl_->lastError;
}

} // namespace discovery
} // namespace multiroom
