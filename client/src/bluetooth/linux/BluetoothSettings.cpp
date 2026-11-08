#include "bluetooth/linux/BluetoothSettings.h"

#include "util/FileUtil.h"

#include <cstdlib>
#include <ctime>
#include <sstream>
#include <unistd.h>

namespace multiroom::bluetooth_settings {
namespace {

std::string firstLine(const std::string& path)
{
    std::string text, error;
    if (!fileutil::readTextFile(path, text, error))
        return std::string();
    const size_t end = text.find('\n');
    return end == std::string::npos ? text : text.substr(0, end);
}

bool exists(const char* path)
{
    return ::access(path, F_OK) == 0;
}

} // namespace

json::Value status()
{
    json::Value out = json::Value::makeObject();
    out["pairing"] = json::Value(exists("/run/bluetooth-pairing"));
    // The helper writes when its search ends (an epoch) while it searches.
    const char* search = "/run/bluetooth-headphones-search";
    out["scanning"] = json::Value(exists(search));
    const long until = std::atol(firstLine(search).c_str());
    const long now = static_cast<long>(std::time(nullptr));
    out["scanSeconds"] = json::Value(static_cast<int64_t>(until > now ? until - now : 0));
    // The headphones' SBC quality; the helper starts bluealsa with it.
    const std::string quality = firstLine(exists("/settings") ? "/settings/bluetooth-quality"
                                                             : "/run/bluetooth-quality");
    out["quality"] = json::Value(quality == "medium" || quality == "low" ? quality : "high");

    // The helper keeps the headphone room on the settings volume when there
    // is one (persistent image), else in /run.
    const char* directory = exists("/settings") ? "/settings/headphones" : "/run/headphones";
    const std::string address = firstLine(std::string(directory) + "/device");
    if (address.empty())
        out["headphones"] = json::Value();
    else
    {
        json::Value headphones = json::Value::makeObject();
        headphones["address"] = json::Value(address);
        headphones["name"] = json::Value(firstLine(std::string(directory) + "/name"));
        headphones["playing"] = json::Value(exists("/run/bluetooth-headphones"));
        out["headphones"] = headphones;
    }

    // "<rssi> <address> <icon> <name...>" per line.
    json::Value devices = json::Value::makeArray();
    std::string text, error;
    if (fileutil::readTextFile("/run/bluetooth-scan", text, error))
    {
        std::istringstream lines(text);
        std::string line;
        while (std::getline(lines, line))
        {
            std::istringstream fields(line);
            long rssi = 0;
            std::string device, icon, name;
            if (!(fields >> rssi >> device >> icon) || !validAddress(device))
                continue;
            std::getline(fields >> std::ws, name);
            json::Value entry = json::Value::makeObject();
            entry["address"] = json::Value(device);
            entry["name"] = json::Value(name.empty() ? device : name);
            entry["icon"] = json::Value(icon);
            if (rssi > -999)
                entry["rssi"] = json::Value(static_cast<int64_t>(rssi));
            devices.pushBack(entry);
        }
    }
    out["devices"] = devices;
    return out;
}

bool validAddress(const std::string& address)
{
    if (address.size() != 17)
        return false;
    for (size_t i = 0; i < address.size(); ++i)
    {
        const char c = address[i];
        const bool ok = i % 3 == 2 ? c == ':' : (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F');
        if (!ok)
            return false;
    }
    return true;
}

} // namespace multiroom::bluetooth_settings
