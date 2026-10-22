#include "discovery/NodeId.h"

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#elif defined(ESP_PLATFORM)
#  include "esp_mac.h"
#else
#  include <unistd.h>
#endif

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <random>

namespace multiroom {
namespace discovery {

bool isValidNodeId(const std::string& id)
{
    if (id.empty() || id.size() > 64)
        return false;
    if (id.front() == ' ' || id.back() == ' ')
        return false;
    for (const char c : id)
    {
        const unsigned char byte = static_cast<unsigned char>(c);
        if (byte < 0x20 || byte == 0x7F)
            return false;
    }
    return true;
}

std::string generateNodeId(const std::string& prefix, const std::string& hostName)
{
    // 64 bytes minus the prefix, two '-' and the eight hex digits.
    const size_t reserved = prefix.size() + 2 + 8;
    const size_t maxHostLength = reserved < 64 ? 64 - reserved : 0;
    std::string host;
    for (const char c : hostName)
    {
        if (host.size() == maxHostLength)
            break;
        if (c >= 'A' && c <= 'Z')
            host += static_cast<char>(c - 'A' + 'a');
        else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
            host += c;
        else
            host += '-';
    }

    // The wall clock is mixed in because random_device may be a deterministic
    // generator on some standard libraries; two boards from the same image
    // still differ in the moment they first started.
    std::random_device device;
    const uint64_t now = static_cast<uint64_t>(
        std::chrono::system_clock::now().time_since_epoch().count());
    const uint32_t value =
        device() ^ static_cast<uint32_t>(now) ^ static_cast<uint32_t>(now >> 32);

    char random[16];
    std::snprintf(random, sizeof(random), "%08x", static_cast<unsigned>(value));
    return host.empty() ? prefix + "-" + random : prefix + "-" + host + "-" + random;
}

std::string generateRandomId()
{
    // Two draws, each mixed with the clock like generateNodeId: a board whose
    // random_device is deterministic still differs by the moment it started.
    std::random_device device;
    const uint64_t now = static_cast<uint64_t>(
        std::chrono::system_clock::now().time_since_epoch().count());
    const uint64_t value = ((static_cast<uint64_t>(device()) << 32) | device()) ^ now;

    char random[20];
    std::snprintf(random, sizeof(random), "%016llx", static_cast<unsigned long long>(value));
    return std::string(random);
}

std::string hostName()
{
#if defined(_WIN32)
    // GetComputerName rather than gethostname: it needs no Winsock, and the
    // id is wanted before any socket exists.
    char buffer[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD length = static_cast<DWORD>(sizeof(buffer));
    if (GetComputerNameA(buffer, &length) == 0)
        return std::string();
    return std::string(buffer, length);
#elif defined(ESP_PLATFORM)
    // lwIP has no gethostname, and every board would otherwise be called
    // "espressif". The station MAC's last three bytes tell rooms apart.
    uint8_t mac[6] = {};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK)
        return std::string();
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "esp32-%02x%02x%02x", mac[3], mac[4], mac[5]);
    return std::string(buffer);
#else
    char buffer[256];
    if (::gethostname(buffer, sizeof(buffer)) != 0)
        return std::string();
    buffer[sizeof(buffer) - 1] = '\0';
    return std::string(buffer);
#endif
}

} // namespace discovery
} // namespace multiroom
