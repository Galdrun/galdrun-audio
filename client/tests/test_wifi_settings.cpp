#include "TestSupport.h"
#include "../esp32/main/WifiSettings.h"
#include "util/FileUtil.h"

using namespace multiroom;

MULTIROOM_TEST(esp32_wifi_country_preserves_legacy_settings_and_rejects_invalid_saves)
{
    multiroomtest::TempDirectory directory("wifi-country");
    const std::string path = directory.file("wifi.json");
    std::string error, before, after;
    CHECK(fileutil::writeTextFileAtomic(path, R"({"ssid":"test","password":"password123"})", error));
    WifiSettings settings;
    CHECK(wifi_settings::load(settings, error, path));
    CHECK(settings.country.empty());
    settings.country = "AT";
    CHECK(wifi_settings::save(settings, error, path));
    CHECK(wifi_settings::load(settings, error, path));
    CHECK_EQ(settings.country, std::string("AT"));
    CHECK_EQ(settings.ssid, std::string("test"));
    CHECK_EQ(settings.password, std::string("password123"));
    CHECK(fileutil::readTextFile(path, before, error));
    for (const char* invalid : {"at", "ZZ", "01", "AUT", "A\n"})
    {
        settings.country = invalid;
        CHECK(!wifi_settings::save(settings, error, path));
        CHECK(fileutil::readTextFile(path, after, error));
        CHECK_EQ(after, before);
    }
    settings.country.clear();
    CHECK(wifi_settings::save(settings, error, path));
    CHECK(wifi_settings::load(settings, error, path));
    CHECK(settings.country.empty());
    CHECK_EQ(settings.password, std::string("password123"));
    CHECK(fileutil::writeTextFileAtomic(path, R"({"country":"ZZ"})", error));
    CHECK(!wifi_settings::load(settings, error, path));
}
