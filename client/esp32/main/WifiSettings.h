#pragma once

#include <cstdint>
#include <string>

// The network this board joins, in /data/wifi.json. Kept apart from
// client.json because it belongs to the board rather than to the client: the
// desktop builds have no such file, and the client never reads it.
//
// The password is stored in clear text, as the Wi-Fi driver itself would store
// it in NVS; flash encryption is what protects either.
struct WifiSettings
{
    std::string ssid;
    std::string password;
    // The board's own network name; empty means the esp32-<mac> default.
    std::string deviceName;
    // One access point of the network, as aa:bb:cc:dd:ee:ff; empty means
    // automatic, the strongest one with this name.
    std::string bssid;
    // Empty uses the SDK's automatic/world-safe country policy.
    std::string country;
    // The station's address. False is DHCP; true takes ip and netmask, and
    // gateway and dns when set (dotted IPv4, empty dns: the gateway).
    bool staticIp = false;
    std::string ip;
    std::string netmask;
    std::string gateway;
    std::string dns;
};

namespace wifi_settings {

extern const char* const kPath;

// Missing is not an error: `out` stays empty and the board starts the setup
// access point.
bool load(WifiSettings& out, std::string& error, const std::string& path = kPath);
bool save(const WifiSettings& settings, std::string& error, const std::string& path = kPath);
bool validateCountry(const std::string& country, std::string& error);

// The static address is usable (text_util::validateStaticIpv4). A typo here
// leaves the board on the network but unreachable, so this is checked before
// saving and again at boot.
bool validateStaticIp(const WifiSettings& settings, std::string& error);

} // namespace wifi_settings
