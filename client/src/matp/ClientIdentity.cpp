#include "matp/ClientIdentity.h"

#include "audio/AudioDeviceCache.h" // currentPlatformName
#include "discovery/NodeId.h"         // hostName

#if defined(ESP_PLATFORM)
#include "esp_netif.h"
#endif

namespace multiroom {
namespace matp {
namespace {

#define MULTIROOM_STRINGIFY_INNER(x) #x
#define MULTIROOM_STRINGIFY(x) MULTIROOM_STRINGIFY_INNER(x)

const char* architectureName()
{
#if defined(_M_ARM64) || defined(__aarch64__)
    return "arm64";
#elif defined(__ARM_ARCH)
    // The compiler knows which ARM revision it is generating for, and the
    // difference between v6 and v7 is exactly what the server cares about.
    return "armv" MULTIROOM_STRINGIFY(__ARM_ARCH);
#elif defined(_M_ARM) || defined(__arm__)
    return "arm";
#elif defined(_M_X64) || defined(__x86_64__)
    return "x86_64";
#elif defined(_M_IX86) || defined(__i386__)
    return "x86";
#elif defined(__XTENSA__)
    return "xtensa";
#elif defined(__riscv)
    return "riscv";
#else
    return "unknown";
#endif
}

} // namespace

std::string platformName()
{
    return std::string(device_cache::currentPlatformName()) + "-" + architectureName();
}

std::string deviceName()
{
#if defined(ESP_PLATFORM)
    // The name the board gave its station interface (app_main: wifi.json's
    // deviceName), which discovery::hostName() does not know.
    const char* name = nullptr;
    esp_netif_t* station = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (station != nullptr && esp_netif_get_hostname(station, &name) == ESP_OK && name != nullptr &&
        name[0] != '\0')
        return std::string(name);
#endif
    return discovery::hostName();
}

} // namespace matp
} // namespace multiroom
