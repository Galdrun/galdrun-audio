#include "system/TextUtil.h"

#include <cstdio>

namespace multiroom {
namespace text_util {
namespace {

int hexValue(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

bool letterOrDigit(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

bool unicast(uint32_t address)
{
    const uint32_t first = address >> 24;
    return first != 0 && first != 127 && first < 224;
}

} // namespace

std::string toLower(std::string text)
{
    for (char& c : text)
    {
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    }
    return text;
}

std::string formatMac(const unsigned char* bytes)
{
    char text[18];
    std::snprintf(text, sizeof(text), "%02x:%02x:%02x:%02x:%02x:%02x", bytes[0], bytes[1],
                  bytes[2], bytes[3], bytes[4], bytes[5]);
    return text;
}

bool parseMac(const std::string& text, unsigned char (&out)[6])
{
    if (text.size() != 17)
        return false;
    for (size_t i = 0; i < 6; ++i)
    {
        const size_t at = i * 3;
        const int high = hexValue(text[at]);
        const int low = hexValue(text[at + 1]);
        if (high < 0 || low < 0 || (i < 5 && text[at + 2] != ':'))
            return false;
        out[i] = static_cast<unsigned char>(high * 16 + low);
    }
    return true;
}

bool parseIpv4(const std::string& text, uint32_t& out)
{
    uint32_t address = 0;
    size_t at = 0;
    for (int part = 0; part < 4; ++part)
    {
        if (part > 0)
        {
            if (at >= text.size() || text[at] != '.')
                return false;
            ++at;
        }
        const size_t first = at;
        uint32_t value = 0;
        while (at < text.size() && text[at] >= '0' && text[at] <= '9' && at - first < 3)
        {
            value = value * 10 + static_cast<uint32_t>(text[at] - '0');
            ++at;
        }
        const size_t digits = at - first;
        if (digits == 0 || value > 255 || (digits > 1 && text[first] == '0'))
            return false;
        address = (address << 8) | value;
    }
    if (at != text.size())
        return false;
    out = address;
    return true;
}

bool validateStaticIpv4(const std::string& ip, const std::string& netmask,
                        const std::string& gateway, const std::string& dns, std::string& error)
{
    uint32_t address = 0;
    uint32_t mask = 0;
    uint32_t router = 0;
    uint32_t server = 0;
    if (!parseIpv4(ip, address) || !unicast(address))
        error = "the IP address must be a unicast IPv4 address, like 192.168.1.50";
    // Contiguous ones from the top: inverted plus one is a power of two.
    else if (!parseIpv4(netmask, mask) || mask < 0xFF000000u || mask > 0xFFFFFFFCu ||
             ((~mask + 1) & ~mask) != 0)
        error = "the subnet mask must be contiguous, /8 to /30, like 255.255.255.0";
    else if ((address & ~mask) == 0 || (address & ~mask) == ~mask)
        error = "the IP address is the subnet's network or broadcast address";
    else if (!gateway.empty() &&
             (!parseIpv4(gateway, router) || !unicast(router) || (router & mask) != (address & mask) ||
              router == address || (router & ~mask) == 0 || (router & ~mask) == ~mask))
        error = "the gateway must be another host in the same subnet, or empty";
    else if (!dns.empty() && (!parseIpv4(dns, server) || !unicast(server)))
        error = "the DNS server must be a unicast IPv4 address, or empty";
    else
        return true;
    return false;
}

bool isValidHostName(const std::string& name)
{
    if (name.empty() || name.size() > 31 || !letterOrDigit(name.front()) ||
        !letterOrDigit(name.back()))
        return false;
    for (const char c : name)
    {
        if (!letterOrDigit(c) && c != '-')
            return false;
    }
    return true;
}

} // namespace text_util
} // namespace multiroom
