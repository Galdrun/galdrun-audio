#pragma once

// Network names and addresses as text, one rule each for every build: the
// client's Linux network settings, its ESP32 Wi-Fi settings, its HTTP pages
// and the system sampler all read and write the same MAC, IPv4 and host-name
// spellings. Plain C++, no OS calls.

#include <cstdint>
#include <string>

namespace multiroom {
namespace text_util {

// ASCII lower case, for names and header values; other bytes are kept.
std::string toLower(std::string text);

// "aa:bb:cc:dd:ee:ff", lower case, from six bytes.
std::string formatMac(const unsigned char* bytes);

// Six hex pairs separated by ':', either case.
bool parseMac(const std::string& text, unsigned char (&out)[6]);

// Dotted-quad IPv4 as inet_pton reads it: four decimal parts 0 to 255, no
// leading zeros. `out` is in host byte order.
bool parseIpv4(const std::string& text, uint32_t& out);

// A static IPv4 setting that leaves the device reachable: a unicast host
// address that is neither its subnet's network nor broadcast address, a
// contiguous /8 to /30 mask, a gateway that is another host of the same subnet
// and a unicast DNS server. An empty gateway or dns means none.
bool validateStaticIpv4(const std::string& ip, const std::string& netmask,
                        const std::string& gateway, const std::string& dns, std::string& error);

// 1 to 31 letters, digits and '-', starting and ending with a letter or digit:
// a DHCP host name, within lwIP's limit.
bool isValidHostName(const std::string& name);

} // namespace text_util
} // namespace multiroom
