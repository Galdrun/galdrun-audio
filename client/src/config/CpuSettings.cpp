#include "config/CpuSettings.h"
#include "config/BoardConfig.h"
#include "util/FileUtil.h"
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <vector>

namespace multiroom::cpu_settings {
namespace {
bool read(const std::string& path, std::string& value, std::string& error)
{
    if (!fileutil::readTextFile(path, value, error)) return false;
    while (!value.empty() && (value.back() == '\n' || value.back() == ' ')) value.pop_back();
    return true;
}

bool write(const std::string& path, const std::string& value, std::string& error)
{
    std::ofstream file(path);
    file << value << '\n';
    file.close();
    if (!file.fail()) return true;
    error = "cannot write CPU policy: " + path;
    return false;
}

bool frequencies(const std::string& policy, std::vector<int>& values, std::string& error)
{
    std::string text;
    if (!read(policy + "/scaling_available_frequencies", text, error)) return false;
    std::istringstream stream(text);
    int khz = 0;
    while (stream >> khz)
    {
        if (khz <= 0 || khz % 1000 != 0)
        {
            error = "invalid supported CPU frequency";
            return false;
        }
        values.push_back(khz / 1000);
    }
    if (values.empty() || !stream.eof())
    {
        error = "CPU frequency list is unavailable or invalid";
        return false;
    }
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
    return true;
}

bool limits(const std::string& policy, const std::string& minimum,
            const std::string& maximum, std::string& error)
{
    std::string currentMax;
    if (!read(policy + "/scaling_max_freq", currentMax, error)) return false;
    // Raise max first when increasing; lower min first when decreasing.
    if (std::strtol(minimum.c_str(), nullptr, 10) > std::strtol(currentMax.c_str(), nullptr, 10))
        return write(policy + "/scaling_max_freq", maximum, error) &&
               write(policy + "/scaling_min_freq", minimum, error);
    return write(policy + "/scaling_min_freq", minimum, error) &&
           write(policy + "/scaling_max_freq", maximum, error);
}

bool apply(int mhz, const std::string& policy, const std::string* path, std::string& error)
{
    std::string minimum, maximum, governor;
    if (!read(policy + "/scaling_min_freq", minimum, error) ||
        !read(policy + "/scaling_max_freq", maximum, error) ||
        !read(policy + "/scaling_governor", governor, error)) return false;
    const auto khz = std::to_string(mhz * 1000);
    if (write(policy + "/scaling_governor", "performance", error) &&
        limits(policy, khz, khz, error) &&
        (path == nullptr || fileutil::writeTextFileAtomic(*path, "CPU_MHZ=" + std::to_string(mhz) + "\n", error)))
        return true;
    std::string rollbackError;
    if (!limits(policy, minimum, maximum, rollbackError) ||
        !write(policy + "/scaling_governor", governor, rollbackError))
        error += "; CPU policy rollback failed: " + rollbackError;
    return false;
}
} // namespace

bool supported()
{
    return board_config::enabled(board_config::current(), "cpuFrequency");
}

bool supported(const std::string& boardPath)
{
    return board_config::enabled(board_config::read(boardPath), "cpuFrequency");
}

bool load(json::Value& settings, std::string& error, const std::string& policy, const std::string& path)
{
    error.clear();
    std::vector<int> available;
    if (!frequencies(policy, available, error)) return false;
    int selected = available.front();
    std::string text;
    if (fileutil::exists(path))
    {
        if (!read(path, text, error)) return false;
        std::istringstream config(text);
        char remaining = 0;
        if (text.compare(0, 8, "CPU_MHZ=") != 0)
            error = "invalid saved CPU setting";
        else
        {
            config.seekg(8);
            if (!(config >> selected) || (config >> remaining)) error = "invalid saved CPU setting";
        }
        if (!error.empty()) return false;
    }
    if (std::find(available.begin(), available.end(), selected) == available.end())
    {
        error = "saved CPU frequency is unsupported";
        return false;
    }
    std::string current, governor;
    if (!read(policy + "/scaling_cur_freq", current, error) ||
        !read(policy + "/scaling_governor", governor, error)) return false;
    settings = json::Value::makeObject();
    settings["mhz"] = json::Value(selected);
    settings["defaultMhz"] = json::Value(available.front());
    settings["currentMhz"] = json::Value(static_cast<int32_t>(std::strtol(current.c_str(), nullptr, 10) / 1000));
    settings["governor"] = json::Value(governor);
    settings["frequenciesMhz"] = json::Value::makeArray();
    for (int value : available) settings["frequenciesMhz"].pushBack(json::Value(value));
    return true;
}

bool save(const json::Value& changes, json::Value& settings, std::string& error,
          const std::string& policy, const std::string& path)
{
    error.clear();
    const auto* mhz = changes.find("mhz");
    std::vector<int> available;
    if (!changes.isObject() || changes.members().size() != 1 || mhz == nullptr || !mhz->isNumber())
    {
        error = "CPU setting must contain only a supported mhz number";
        return false;
    }
    if (!frequencies(policy, available, error)) return false;
    const auto found = std::find_if(available.begin(), available.end(),
                                   [mhz](int value) { return mhz->asDouble() == value; });
    if (found == available.end())
    {
        error = "unsupported CPU frequency";
        return false;
    }
    return apply(*found, policy, &path, error) && load(settings, error, policy, path);
}

bool restore(std::string& error, const std::string& policy, const std::string& path)
{
    json::Value settings;
    if (!load(settings, error, policy, path)) return false;
    return apply(static_cast<int>(settings.find("mhz")->asDouble()), policy, nullptr, error);
}
} // namespace multiroom::cpu_settings
