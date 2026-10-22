#include "WifiSettings.h"

#include "system/TextUtil.h"
#include "util/FileUtil.h"
#include "util/Json.h"

using namespace multiroom;

namespace wifi_settings {

const char* const kPath = "/data/wifi.json";

bool load(WifiSettings& out, std::string& error)
{
    out = WifiSettings();
    if (!fileutil::exists(kPath))
        return true;

    std::string text;
    if (!fileutil::readTextFile(kPath, text, error))
        return false;
    json::Value root;
    if (!json::parse(text, root, error))
        return false;
    if (!root.isObject())
    {
        error = "root value is not an object";
        return false;
    }
    if ((root.find("ssid") != nullptr && !json::getString(root, "ssid", out.ssid)) ||
        (root.find("password") != nullptr && !json::getString(root, "password", out.password)) ||
        (root.find("deviceName") != nullptr && !json::getString(root, "deviceName", out.deviceName)) ||
        (root.find("bssid") != nullptr && !json::getString(root, "bssid", out.bssid)))
    {
        error = "ssid, password, deviceName and bssid must be strings";
        return false;
    }
    std::string ipMode = "dhcp";
    if ((root.find("ipMode") != nullptr && !json::getString(root, "ipMode", ipMode)) ||
        (root.find("ip") != nullptr && !json::getString(root, "ip", out.ip)) ||
        (root.find("netmask") != nullptr && !json::getString(root, "netmask", out.netmask)) ||
        (root.find("gateway") != nullptr && !json::getString(root, "gateway", out.gateway)) ||
        (root.find("dns") != nullptr && !json::getString(root, "dns", out.dns)))
    {
        error = "ipMode, ip, netmask, gateway and dns must be strings";
        return false;
    }
    if (ipMode != "dhcp" && ipMode != "static")
    {
        error = "ipMode must be dhcp or static";
        return false;
    }
    out.staticIp = ipMode == "static";
    return true;
}

bool validateStaticIp(const WifiSettings& settings, std::string& error)
{
    return text_util::validateStaticIpv4(settings.ip, settings.netmask, settings.gateway, settings.dns,
                                         error);
}

bool save(const WifiSettings& settings, std::string& error)
{
    json::Value root = json::Value::makeObject();
    root["ssid"] = json::Value(settings.ssid);
    root["password"] = json::Value(settings.password);
    root["deviceName"] = json::Value(settings.deviceName);
    root["bssid"] = json::Value(settings.bssid);
    root["ipMode"] = json::Value(settings.staticIp ? "static" : "dhcp");
    root["ip"] = json::Value(settings.ip);
    root["netmask"] = json::Value(settings.netmask);
    root["gateway"] = json::Value(settings.gateway);
    root["dns"] = json::Value(settings.dns);
    return fileutil::writeTextFileAtomic(kPath, json::serialize(root, true), error);
}

} // namespace wifi_settings
