#include "TestSupport.h"
#include "net/NetworkSettings.h"
#include "util/FileUtil.h"
#include <sys/stat.h>
#include <unistd.h>

using namespace multiroom;

MULTIROOM_TEST(network_device_name_persists_and_rejects_invalid_names)
{
    multiroomtest::TempDirectory directory("device-name");
    const auto path = directory.file("network.conf");
    std::string error, before, after;
    json::Value settings, changes;
    CHECK(network_settings::load(settings, error, path));
    CHECK(settings["device"]["name"].asString().empty());
    for (const char* name : {"kitchen-connector", "A", "", "Raumfeld-123"})
    {
        changes = json::Value::makeObject();
        changes["device"]["name"] = json::Value(name);
        CHECK(network_settings::save(changes, settings, error, path));
        CHECK(network_settings::load(settings, error, path));
        CHECK_EQ(settings["device"]["name"].asString(), std::string(name));
    }
    CHECK(fileutil::readTextFile(path, before, error));
    for (const char* name : {"-room", "room-", "two rooms", "room.local", "room\nETH_IP=bad",
                            "room;reboot", "abcdefghijklmnopqrstuvwxyz123456"})
    {
        changes["device"]["name"] = json::Value(name);
        CHECK(!network_settings::save(changes, settings, error, path));
        CHECK(fileutil::readTextFile(path, after, error));
        CHECK_EQ(before, after);
    }
    changes["device"]["name"] = json::Value(42);
    CHECK(!network_settings::save(changes, settings, error, path));
}

MULTIROOM_TEST(network_ethernet_speed_merges_validates_and_persists)
{
    multiroomtest::TempDirectory directory("ethernet-speed");
    const auto path = directory.file("network.conf");
    std::string error, before, after;
    json::Value settings, changes;
    CHECK(network_settings::load(settings, error, path));
    CHECK_EQ(settings["ethernet"]["speedMbps"].asString(), std::string("auto"));
    for (const char* speed : {"10", "100", "auto"})
    {
        changes = json::Value::makeObject();
        changes["ethernet"] = json::Value::makeObject();
        changes["ethernet"]["speedMbps"] = json::Value(speed);
        CHECK(network_settings::save(changes, settings, error, path));
        CHECK(network_settings::load(settings, error, path));
        CHECK_EQ(settings["ethernet"]["speedMbps"].asString(), std::string(speed));
    }
    CHECK(fileutil::readTextFile(path, before, error));
    for (const char* invalid : {R"({"ethernet":{"speedMbps":"1000"}})",
                              R"({"ethernet":{"speedMbps":10}})",
                              R"({"ethernet":{"speedMbps":"10;reboot"}})"})
    {
        CHECK(json::parse(invalid, changes, error));
        CHECK(!network_settings::save(changes, settings, error, path));
        CHECK(fileutil::readTextFile(path, after, error));
        CHECK_EQ(before, after);
    }
}

MULTIROOM_TEST(network_settings_are_firmware_opt_in)
{
    multiroomtest::TempDirectory directory("network-board");
    const auto path = directory.file("board.json");
    std::string error;
    CHECK(!network_settings::supported(path));
    CHECK(fileutil::writeTextFileAtomic(path, "{\"networkSettings\":false}", error));
    CHECK(!network_settings::supported(path));
    CHECK(fileutil::writeTextFileAtomic(path, "{\"networkSettings\":true}", error));
    CHECK(network_settings::supported(path));
}

MULTIROOM_TEST(network_settings_keep_secrets_private_and_treat_shell_text_as_data)
{
    multiroomtest::TempDirectory directory("network-secrets");
    const auto path = directory.file("network.conf");
    json::Value settings, changes;
    std::string error;
    CHECK(json::parse(R"({"wifi":{"ssid":"test'$(touch BAD)`id`=wifi","password":"safe'$(id)=pass"}})", changes, error));
    CHECK(network_settings::save(changes, settings, error, path));
    CHECK(network_settings::load(settings, error, path));
    CHECK_EQ(settings["wifi"]["ssid"].asString(), std::string("test'$(touch BAD)`id`=wifi"));
    CHECK_EQ(settings["wifi"]["password"].asString(), std::string("safe'$(id)=pass"));
    struct stat status{};
    CHECK_EQ(stat(path.c_str(), &status), 0);
    CHECK_EQ(status.st_mode & 0777, 0600u);
    const auto publicSettings = network_settings::publicSettings(settings);
    CHECK(publicSettings.find("wifi")->find("password") == nullptr);
    CHECK(publicSettings.find("wifi")->find("passwordSet")->asBool());

    CHECK(json::parse(R"({"wifi":{"password":""}})", changes, error));
    CHECK(network_settings::save(changes, settings, error, path));
    CHECK_EQ(settings["wifi"]["password"].asString(), std::string("safe'$(id)=pass"));
    CHECK(json::parse(R"({"wifi":{"clearPassword":true}})", changes, error));
    CHECK(network_settings::save(changes, settings, error, path));
    CHECK(settings["wifi"]["password"].asString().empty());
    CHECK(json::parse(R"({"wifi":{"password":"old-password"}})", changes, error));
    CHECK(network_settings::save(changes, settings, error, path));
    CHECK(json::parse(R"({"wifi":{"ssid":"different-network"}})", changes, error));
    CHECK(network_settings::save(changes, settings, error, path));
    CHECK(settings["wifi"]["password"].asString().empty());
}

MULTIROOM_TEST(network_settings_reject_invalid_changes_without_overwriting_saved_settings)
{
    multiroomtest::TempDirectory directory("network-validation");
    const auto path = directory.file("network.conf");
    json::Value settings, changes;
    std::string error, before, after;
    CHECK(json::parse(R"({"ethernet":{"ipMode":"static","ip":"10.0.0.38","netmask":"255.255.255.0","gateway":"10.0.0.1","dns":"1.1.1.1"}})", changes, error));
    CHECK(network_settings::save(changes, settings, error, path));
    CHECK(fileutil::readTextFile(path, before, error));
    for (const char* invalid : {
        R"({"ethernet":{"ip":"10.0.0.0"}})",
        R"({"ethernet":{"ip":"10.0.0.255"}})",
        R"({"ethernet":{"ip":"127.0.0.1"}})",
        R"({"ethernet":{"ip":"224.0.0.1"}})",
        R"({"ethernet":{"netmask":"255.0.255.0"}})",
        R"({"ethernet":{"gateway":"10.0.1.1"}})",
        R"({"ethernet":{"gateway":"10.0.0.255"}})",
        R"({"ethernet":{"dns":"not-an-ip"}})",
        R"({"ethernet":{"ipMode":"other"}})",
        R"({"wifi":{"ssid":"name\nETH_IP_MODE=static"}})",
        R"({"wifi":{"password":"short"}})",
        R"({"wifi":{"country":"at"}})",
        R"({"wifi":{"bssid":"aa:bb:cc:dd:ee:gg"}})",
        R"({"wifi":{"bssid":"aa:bb:cc"}})",
        R"({"wifi":{"password":123}})",
        R"({"wifi":{"clearPassword":"true"}})",
        R"({"shell":"id"})"})
    {
        CHECK(json::parse(invalid, changes, error));
        CHECK(!network_settings::save(changes, settings, error, path));
        CHECK(fileutil::readTextFile(path, after, error));
        CHECK_EQ(before, after);
    }
    CHECK(json::parse(R"({"ethernet":{"ipMode":"dhcp"}})", changes, error));
    CHECK(network_settings::save(changes, settings, error, path));
}

MULTIROOM_TEST(network_scan_preserves_access_points_and_runs_asynchronously)
{
    const auto networks = network_settings::parseScan(R"(BSS aa:bb:cc:dd:ee:01(on galdrunscan0)
    freq: 2412
    signal: -70.50 dBm
    capability: ESS Privacy (0x0011)
    SSID: Test
BSS aa:bb:cc:dd:ee:02(on galdrunscan0)
    freq: 5180
    signal: -45.00 dBm
    SSID: Test
    RSN:
BSS aa:bb:cc:dd:ee:03(on galdrunscan0)
    freq: 2484
    signal: -60.00 dBm
    SSID: Caf\xc3\xa9
BSS aa:bb:cc:dd:ee:04(on galdrunscan0)
    SSID:
)");
    CHECK_EQ(networks.size(), size_t(3));
    CHECK_EQ(networks.at(0).find("bssid")->asString(), std::string("aa:bb:cc:dd:ee:02"));
    CHECK_EQ(networks.at(0).find("channel")->asInt(), int64_t(36));
    CHECK(networks.at(0).find("secure")->asBool());
    CHECK_EQ(networks.at(1).find("ssid")->asString(), std::string("Caf\xc3\xa9"));
    CHECK_EQ(networks.at(1).find("channel")->asInt(), int64_t(14));
    CHECK(!networks.at(1).find("secure")->asBool());
    CHECK(networks.at(2).find("secure")->asBool());

    multiroomtest::TempDirectory directory("network-scan");
    const auto state = directory.file(".");
    json::Value result;
    std::string error;
    CHECK(network_settings::scan(true, result, error, state));
    CHECK(result["pending"].asBool());
    CHECK(network_settings::scan(true, result, error, state));
    CHECK(result["pending"].asBool());
    CHECK_EQ(::unlink(directory.file("scan.request").c_str()), 0);
    CHECK(fileutil::writeTextFileAtomic(directory.file("scan.txt"), "BSS aa:bb:cc:dd:ee:01\n\tSSID: Found\n", error));
    CHECK(network_settings::scan(false, result, error, state));
    CHECK(!result["pending"].asBool());
    CHECK_EQ(result["networks"].size(), size_t(1));
    CHECK(fileutil::writeTextFileAtomic(directory.file("scan.error"), "driver unavailable", error));
    CHECK(network_settings::scan(false, result, error, state));
    CHECK_EQ(result["error"].asString(), std::string("driver unavailable"));
    CHECK(fileutil::writeTextFileAtomic(directory.file("scan.error"), std::string(65536, 'x'), error));
    CHECK(network_settings::scan(false, result, error, state));
    CHECK_EQ(result["error"].asString().size(), size_t(1024));
}
