#include "TestSupport.h"

#include "system/TextUtil.h"

using namespace multiroom;

MULTIROOM_TEST(text_util_lowers_ascii_only)
{
    CHECK_EQ(text_util::toLower("Host.LOCAL:80"), std::string("host.local:80"));
    CHECK_EQ(text_util::toLower("\xC3\x84"), std::string("\xC3\x84"));
}

MULTIROOM_TEST(text_util_formats_and_parses_mac_addresses)
{
    const unsigned char bytes[6] = {0x00, 0x1a, 0xFF, 0x09, 0xab, 0xcd};
    CHECK_EQ(text_util::formatMac(bytes), std::string("00:1a:ff:09:ab:cd"));

    unsigned char parsed[6] = {};
    CHECK(text_util::parseMac("00:1A:ff:09:AB:cd", parsed));
    for (int i = 0; i < 6; ++i)
        CHECK_EQ(parsed[i], bytes[i]);

    for (const char* bad : {"", "00:1a:ff:09:ab", "00:1a:ff:09:ab:cd:", "00-1a-ff-09-ab-cd",
                            "00:1a:ff:09:ab:cg", "001a:ff:09:ab:cd0"})
        CHECK(!text_util::parseMac(bad, parsed));
}

MULTIROOM_TEST(text_util_parses_dotted_ipv4_like_inet_pton)
{
    uint32_t address = 0;
    CHECK(text_util::parseIpv4("192.168.1.50", address));
    CHECK_EQ(address, static_cast<uint32_t>(0xC0A80132u));
    CHECK(text_util::parseIpv4("0.0.0.0", address));
    CHECK_EQ(address, static_cast<uint32_t>(0));
    CHECK(text_util::parseIpv4("255.255.255.255", address));
    CHECK_EQ(address, static_cast<uint32_t>(0xFFFFFFFFu));

    for (const char* bad : {"", "1.2.3", "1.2.3.4.", "1.2.3.4.5", "256.1.1.1", "1.2.3.04",
                            "01.2.3.4", "1..3.4", " 1.2.3.4", "1.2.3.4 ", "a.b.c.d", "1.2.3.1000"})
        CHECK(!text_util::parseIpv4(bad, address));
}

MULTIROOM_TEST(text_util_validates_a_static_ipv4_setting)
{
    std::string error;
    CHECK(text_util::validateStaticIpv4("10.0.0.38", "255.255.255.0", "10.0.0.1", "1.1.1.1", error));
    CHECK(text_util::validateStaticIpv4("10.0.0.38", "255.255.255.0", "", "", error));

    const struct
    {
        const char* ip;
        const char* netmask;
        const char* gateway;
        const char* dns;
    } refused[] = {
        {"10.0.0", "255.255.255.0", "", ""},          // not an address
        {"127.0.0.5", "255.0.0.0", "", ""},           // loopback
        {"224.0.0.5", "255.255.255.0", "", ""},       // multicast
        {"10.0.0.38", "255.0.255.0", "", ""},         // mask not contiguous
        {"10.0.0.38", "255.255.255.254", "", ""},     // /31
        {"10.0.0.38", "254.0.0.0", "", ""},           // /7
        {"10.0.0.0", "255.255.255.0", "", ""},        // network address
        {"10.0.0.255", "255.255.255.0", "", ""},      // broadcast address
        {"10.0.0.38", "255.255.255.0", "10.0.1.1", ""},   // gateway elsewhere
        {"10.0.0.38", "255.255.255.0", "10.0.0.38", ""},  // gateway is itself
        {"10.0.0.38", "255.255.255.0", "10.0.0.255", ""}, // gateway is broadcast
        {"10.0.0.38", "255.255.255.0", "", "239.1.1.1"},  // multicast DNS
        {"10.0.0.38", "255.255.255.0", "", "dns"},        // not an address
    };
    for (const auto& setting : refused)
    {
        error.clear();
        CHECK(!text_util::validateStaticIpv4(setting.ip, setting.netmask, setting.gateway,
                                             setting.dns, error));
        CHECK(!error.empty());
    }
}

MULTIROOM_TEST(text_util_checks_host_names)
{
    for (const char* good : {"a", "esp32-1a2b3c", "Living-Room-2", "abcdefghijklmnopqrstuvwxyz01234"})
        CHECK(text_util::isValidHostName(good));
    for (const char* bad : {"", "-room", "room-", "two words", "dots.too", "r\xC3\xA4um",
                            "abcdefghijklmnopqrstuvwxyz012345"})
        CHECK(!text_util::isValidHostName(bad));
}
