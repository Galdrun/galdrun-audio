#include "TestSupport.h"
#include "config/CpuSettings.h"
#include "util/FileUtil.h"
#include <string>

using namespace multiroom;

MULTIROOM_TEST(cpu_settings_opt_in_validate_persist_and_restore)
{
    multiroomtest::TempDirectory directory("cpu-settings");
    const auto policy = directory.path();
    const auto config = directory.file("cpu.conf");
    const auto board = directory.file("board.json");
    std::string error;
    const auto put = [&](const char* name, const std::string& value) {
        CHECK(fileutil::writeTextFileAtomic(directory.file(name), value, error));
    };
    const auto get = [&](const char* name) {
        std::string value;
        CHECK(fileutil::readTextFile(directory.file(name), value, error));
        return value;
    };
    CHECK(!cpu_settings::supported(board));
    CHECK(fileutil::writeTextFileAtomic(board, "{\"cpuFrequency\":false}", error));
    CHECK(!cpu_settings::supported(board));
    CHECK(fileutil::writeTextFileAtomic(board, "{\"cpuFrequency\":true}", error));
    CHECK(cpu_settings::supported(board));
    put("scaling_available_frequencies", "720000 300000 600000\n");
    put("scaling_cur_freq", "720000\n");
    put("scaling_min_freq", "300000\n");
    put("scaling_max_freq", "720000\n");
    put("scaling_governor", "performance\n");
    json::Value settings, change = json::Value::makeObject();
    CHECK(cpu_settings::load(settings, error, policy, config));
    CHECK(settings.find("mhz")->asDouble() == 300);
    CHECK(settings.find("defaultMhz")->asDouble() == 300);
    CHECK(settings.find("frequenciesMhz")->size() == 3);
    CHECK(!settings.find("configured")->asBool());
    CHECK(cpu_settings::restore(error, policy, config));
    CHECK(get("scaling_min_freq") == "300000\n");
    CHECK(get("scaling_max_freq") == "720000\n");
    for (double invalid : {0., 600.5, 721., 1e20})
    {
        change["mhz"] = json::Value(invalid);
        CHECK(!cpu_settings::save(change, settings, error, policy, config));
        CHECK(!fileutil::exists(config));
        CHECK(get("scaling_min_freq") == "300000\n");
    }
    change["mhz"] = json::Value(600);
    CHECK(cpu_settings::save(change, settings, error, policy, config));
    CHECK(settings.find("mhz")->asDouble() == 600);
    CHECK(settings.find("configured")->asBool());
    CHECK(get("scaling_min_freq") == "600000\n");
    CHECK(get("scaling_max_freq") == "600000\n");
    put("scaling_min_freq", "300000\n");
    put("scaling_max_freq", "720000\n");
    CHECK(cpu_settings::restore(error, policy, config));
    CHECK(get("scaling_min_freq") == "600000\n");
    change["mhz"] = json::Value(720);
    // A directory at the atomic-write temporary path forces persistence failure.
    CHECK(fileutil::createDirectories(config + ".tmp"));
    CHECK(!cpu_settings::save(change, settings, error, policy, config));
    CHECK(get("scaling_min_freq") == "600000\n");
    CHECK(get("scaling_max_freq") == "600000\n");
    CHECK(cpu_settings::load(settings, error, policy, config));
    CHECK(settings.find("mhz")->asDouble() == 600);
    const auto invalidConfig = directory.file("invalid.conf");
    CHECK(fileutil::writeTextFileAtomic(invalidConfig, "CPU_MHZ=900\n", error));
    CHECK(!cpu_settings::restore(error, policy, invalidConfig));
    CHECK(get("scaling_min_freq") == "600000\n");
}
