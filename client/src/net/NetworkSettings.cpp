#include "net/NetworkSettings.h"
#include "config/BoardConfig.h"
#include "system/TextUtil.h"
#include "util/FileUtil.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <fcntl.h>
#include <linux/wireless.h>
#include <sys/ioctl.h>
#include <vector>

namespace multiroom::network_settings {
namespace {
struct Field { const char* key; const char* group; const char* name; const char* initial; };
const Field fields[] = {
    {"DEVICE_NAME", "device", "name", ""},
    {"ETH_SPEED", "ethernet", "speedMbps", "auto"},
    {"ETH_IP_MODE", "ethernet", "ipMode", "dhcp"},
    {"ETH_IP", "ethernet", "ip", ""},
    {"ETH_NETMASK", "ethernet", "netmask", "255.255.255.0"},
    {"ETH_GATEWAY", "ethernet", "gateway", ""},
    {"ETH_DNS", "ethernet", "dns", ""},
    {"WIFI_SSID", "wifi", "ssid", ""},
    {"WIFI_BSSID", "wifi", "bssid", ""},
    {"WIFI_PASSWORD", "wifi", "password", ""},
    {"WIFI_COUNTRY", "wifi", "country", "AT"},
    {"WIFI_IP_MODE", "wifi", "ipMode", "dhcp"},
    {"WIFI_IP", "wifi", "ip", ""},
    {"WIFI_NETMASK", "wifi", "netmask", "255.255.255.0"},
    {"WIFI_GATEWAY", "wifi", "gateway", ""},
    {"WIFI_DNS", "wifi", "dns", ""}
};

std::string value(const json::Value& settings, const char* name)
{
    const auto* item = settings.find(name);
    return item == nullptr ? std::string() : item->asString();
}

bool macAddress(const std::string& text)
{
    unsigned char bytes[6];
    return text_util::parseMac(text, bytes);
}

bool addressValid(const json::Value& settings, std::string& error)
{
    const std::string mode = value(settings, "ipMode");
    if (mode != "dhcp" && mode != "static")
        error = "IP mode must be dhcp or static";
    else if (mode == "static")
        text_util::validateStaticIpv4(value(settings, "ip"), value(settings, "netmask"),
                                      value(settings, "gateway"), value(settings, "dns"), error);
    return error.empty();
}

bool merge(const json::Value& changes, json::Value& settings, std::string& error)
{
    if (!changes.isObject())
    {
        error = "network settings must be an object";
        return false;
    }
    for (const auto& group : changes.members())
    {
        if ((group.first != "ethernet" && group.first != "wifi" && group.first != "device") || !group.second.isObject())
        {
            error = "network settings must contain device/ethernet/wifi objects";
            return false;
        }
        for (const auto& entry : group.second.members())
        {
            if (group.first == "wifi" && entry.first == "clearPassword" && entry.second.isBool())
                continue;
            bool known = false;
            for (const auto& field : fields)
                known |= group.first == field.group && entry.first == field.name;
            if (!known || !entry.second.isString())
            {
                error = "unknown network setting or non-string value: " + entry.first;
                return false;
            }
            const std::string text = entry.second.asString();
            if (text.size() > 128 || text.find_first_of("\r\n") != std::string::npos ||
                text.find('\0') != std::string::npos)
            {
                error = "network values must be single-line strings up to 128 bytes";
                return false;
            }
            settings[group.first][entry.first] = entry.second;
        }
    }
    return true;
}
} // namespace

bool supported()
{
    return board_config::enabled(board_config::current(), "networkSettings");
}

bool supported(const std::string& boardPath)
{
    return board_config::enabled(board_config::read(boardPath), "networkSettings");
}

bool load(json::Value& settings, std::string& error, const std::string& path)
{
    settings = json::Value::makeObject();
    settings["device"] = json::Value::makeObject();
    settings["ethernet"] = json::Value::makeObject();
    settings["wifi"] = json::Value::makeObject();
    for (const auto& field : fields)
        settings[field.group][field.name] = json::Value(field.initial);
    if (!fileutil::exists(path))
        return true;
    std::string text;
    if (!fileutil::readTextFile(path, text, error))
        return false;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line))
    {
        if (line.empty() || line.front() == '#')
            continue;
        const size_t equal = line.find('=');
        bool known = false;
        for (const auto& field : fields)
            if (line.substr(0, equal) == field.key && equal != std::string::npos)
            {
                settings[field.group][field.name] = json::Value(line.substr(equal + 1));
                known = true;
                break;
            }
        if (!known)
        {
            error = "invalid network configuration line";
            return false;
        }
    }
    return validate(settings, error);
}

bool validate(const json::Value& settings, std::string& error)
{
    error.clear();
    const auto* device = settings.find("device");
    const auto name = device == nullptr ? std::string() : value(*device, "name");
    if (!name.empty() && !text_util::isValidHostName(name))
    {
        error = "device name must be 1 to 31 letters, digits and hyphens, starting and ending with a letter or digit";
        return false;
    }
    for (const char* group : {"ethernet", "wifi"})
    {
        const auto* section = settings.find(group);
        if (section == nullptr || !section->isObject() || !addressValid(*section, error))
        {
            error = std::string(group) + ": " + (error.empty() ? "missing settings" : error);
            return false;
        }
    }
    const auto& wifi = *settings.find("wifi");
    const auto speed = value(*settings.find("ethernet"), "speedMbps");
    if (speed != "auto" && speed != "10" && speed != "100")
    {
        error = "Ethernet speed must be auto, 10 or 100 Mbps";
        return false;
    }
    const auto ssid = value(wifi, "ssid"), password = value(wifi, "password");
    const auto country = value(wifi, "country");
    if (ssid.size() > 32)
        error = "Wi-Fi network name must be at most 32 bytes";
    else if (!value(wifi, "bssid").empty() && !macAddress(value(wifi, "bssid")))
        error = "access point must be a MAC address, or empty for automatic";
    else if (!password.empty() && (password.size() < 8 || password.size() > 63))
        error = "Wi-Fi password must be 8 to 63 bytes, or empty for an open network";
    else if (country.size() != 2 || country.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ") != std::string::npos)
        error = "Wi-Fi country must be a two-letter uppercase code";
    return error.empty();
}

bool save(const json::Value& changes, json::Value& settings, std::string& error,
          const std::string& path)
{
    if (!load(settings, error, path))
        return false;
    const std::string oldSsid = value(settings["wifi"], "ssid");
    const std::string oldPassword = value(settings["wifi"], "password");
    if (!merge(changes, settings, error))
        return false;
    const auto* wifi = changes.find("wifi");
    if (wifi != nullptr)
    {
        bool clear = false;
        json::getBool(*wifi, "clearPassword", clear);
        const auto* password = wifi->find("password");
        if (clear)
            settings["wifi"]["password"] = json::Value("");
        else if (password == nullptr || password->asString().empty())
            settings["wifi"]["password"] = json::Value(
                value(settings["wifi"], "ssid") == oldSsid ? oldPassword : std::string());
    }
    if (!validate(settings, error))
        return false;
    std::string text = "# Data only; never source or eval this file.\n";
    for (const auto& field : fields)
        text += std::string(field.key) + "=" + value(settings[field.group], field.name) + "\n";
    // mkstemp creates mode 0600 before any password bytes reach disk.
    std::string temporary = path + ".XXXXXX";
    const int fd = mkstemp(&temporary[0]);
    if (fd < 0)
    {
        error = "cannot create private network settings: " + std::string(std::strerror(errno));
        return false;
    }
    size_t written = 0;
    while (written < text.size())
    {
        const ssize_t count = ::write(fd, text.data() + written, text.size() - written);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            break;
        written += static_cast<size_t>(count);
    }
    bool ok = written == text.size() && fsync(fd) == 0;
    const int closeResult = ::close(fd);
    ok = ok && closeResult == 0;
    if (ok && std::rename(temporary.c_str(), path.c_str()) == 0)
        return true;
    error = "cannot write network settings: " + std::string(std::strerror(errno));
    ::unlink(temporary.c_str());
    return false;
}

json::Value publicSettings(json::Value settings)
{
    std::string defaultName, error;
    if (fileutil::readTextFile("/etc/hostname", defaultName, error))
    {
        const auto end = defaultName.find_first_of("\r\n");
        settings["device"]["defaultName"] = json::Value(defaultName.substr(0, end));
    }
    settings["wifi"]["passwordSet"] = json::Value(!value(settings["wifi"], "password").empty());
    json::Value wifi = json::Value::makeObject();
    for (const auto& entry : settings["wifi"].members())
        if (entry.first != "password")
            wifi[entry.first] = entry.second;
    settings["wifi"] = wifi;
    ifaddrs* interfaces = nullptr;
    if (getifaddrs(&interfaces) == 0)
    {
        for (auto* current = interfaces; current != nullptr; current = current->ifa_next)
        {
            const std::string name = current->ifa_name;
            const bool wireless = name != MULTIROOM_BRAND_SLUG "scan0" && fileutil::exists("/sys/class/net/" + name + "/phy80211");
            if (wireless && current->ifa_addr != nullptr && current->ifa_addr->sa_family == AF_PACKET)
            {
                const int fd = socket(AF_INET, SOCK_DGRAM, 0);
                iwreq request{};
                std::strncpy(request.ifr_name, name.c_str(), IFNAMSIZ - 1);
                if (fd >= 0 && ioctl(fd, SIOCGIWMODE, &request) == 0 && request.u.mode == IW_MODE_INFRA &&
                    ioctl(fd, SIOCGIWAP, &request) == 0)
                {
                    const auto* mac = reinterpret_cast<const unsigned char*>(request.u.ap_addr.sa_data);
                    if (std::any_of(mac, mac + 6, [](unsigned char b) { return b != 0; }))
                    {
                        settings["wifi"]["connectedBssid"] = json::Value(text_util::formatMac(mac));
                    }
                }
                if (fd >= 0) ::close(fd);
            }
            if (current->ifa_addr == nullptr || current->ifa_addr->sa_family != AF_INET) continue;
            const char* group = name == "eth0" ? "ethernet" : wireless ? "wifi" : nullptr;
            char text[INET_ADDRSTRLEN]{};
            const auto* address = reinterpret_cast<const sockaddr_in*>(current->ifa_addr);
            if (group != nullptr && inet_ntop(AF_INET, &address->sin_addr, text, sizeof(text)) != nullptr)
                settings[group]["currentIp"] = json::Value(text);
        }
        freeifaddrs(interfaces);
    }
    return settings;
}

json::Value parseScan(const std::string& text)
{
    std::vector<json::Value> networks;
    json::Value entry;
    auto finish = [&] {
        if (entry.isObject() && !value(entry, "ssid").empty()) networks.push_back(entry);
    };
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line))
    {
        if (line.compare(0, 4, "BSS ") == 0)
        {
            finish();
            entry = json::Value();
            const auto mac = line.substr(4, 17);
            if (!macAddress(mac)) continue;
            entry = json::Value::makeObject();
            entry["bssid"] = json::Value(mac);
            entry["ssid"] = json::Value("");
            entry["rssi"] = json::Value(-100.0);
            entry["channel"] = json::Value(0);
            entry["secure"] = json::Value(false);
        }
        else if (entry.isObject())
        {
            const auto first = line.find_first_not_of("\t ");
            if (first == std::string::npos) continue;
            line.erase(0, first);
            if (line.compare(0, 6, "SSID: ") == 0)
            {
                const auto raw = line.substr(6);
                std::string ssid;
                for (size_t i = 0; i < raw.size(); ++i)
                {
                    if (raw[i] == '\\' && i + 3 < raw.size() && raw[i + 1] == 'x' &&
                        std::isxdigit(static_cast<unsigned char>(raw[i + 2])) && std::isxdigit(static_cast<unsigned char>(raw[i + 3])))
                    { ssid += static_cast<char>(std::strtoul(raw.substr(i + 2, 2).c_str(), nullptr, 16)); i += 3; }
                    else ssid += raw[i];
                }
                if (ssid.size() <= 32) entry["ssid"] = json::Value(ssid);
            }
            else if (line.compare(0, 8, "signal: ") == 0)
                entry["rssi"] = json::Value(std::strtod(line.c_str() + 8, nullptr));
            else if (line.compare(0, 6, "freq: ") == 0)
            {
                const int frequency = std::atoi(line.c_str() + 6);
                entry["channel"] = json::Value(frequency == 2484 ? 14 : frequency >= 2412 && frequency <= 2472 ? (frequency - 2407) / 5 : frequency >= 5000 && frequency <= 5900 ? (frequency - 5000) / 5 : 0);
            }
            else if ((line.compare(0, 11, "capability:") == 0 && line.find("Privacy") != std::string::npos) ||
                     line.compare(0, 4, "RSN:") == 0 || line.compare(0, 4, "WPA:") == 0)
                entry["secure"] = json::Value(true);
        }
    }
    finish();
    std::stable_sort(networks.begin(), networks.end(), [](const json::Value& a, const json::Value& b) {
        return a.find("rssi")->asDouble() > b.find("rssi")->asDouble();
    });
    auto result = json::Value::makeArray();
    if (networks.size() > 128) networks.resize(128);
    for (const auto& network : networks) result.pushBack(network);
    return result;
}

bool scan(bool start, json::Value& result, std::string& error, const std::string& state)
{
    result = json::Value::makeObject();
    bool pending = fileutil::exists(state + "/scan.request") || fileutil::exists(state + "/scan.pending");
    if (start && !pending)
    {
        const int fd = ::open((state + "/scan.request").c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd < 0) { error = "cannot request Wi-Fi scan: " + std::string(std::strerror(errno)); return false; }
        ::close(fd);
        pending = true;
    }
    result["pending"] = json::Value(pending);
    if (!pending)
    {
        std::string text;
        if (fileutil::exists(state + "/scan.error"))
        {
            if (!fileutil::readTextFile(state + "/scan.error", text, error)) return false;
            result["error"] = json::Value(text.substr(0, 1024));
        }
        else if (fileutil::exists(state + "/scan.txt"))
        {
            if (!fileutil::readTextFile(state + "/scan.txt", text, error)) return false;
            result["networks"] = parseScan(text);
        }
        else result["networks"] = json::Value::makeArray();
    }
    return true;
}
} // namespace multiroom::network_settings
