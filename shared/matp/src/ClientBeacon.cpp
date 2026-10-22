#include "matp/ClientBeacon.h"

#include "matp/Protocol.h"
#include "util/Json.h"

namespace multiroom {
namespace matp {
namespace {

const char* const kBeaconType = "clientBeacon";

bool optionalString(const json::Value& root, const char* key, std::string& out,
                    std::string& error)
{
    const json::Value* value = root.find(key);
    if (value == nullptr)
        return true;
    if (!value->isString())
    {
        error = std::string(key) + " must be a string";
        return false;
    }
    out = value->asString();
    return true;
}

} // namespace

std::string makeClientBeacon(const ClientBeacon& beacon)
{
    json::Value root = json::Value::makeObject();
    root["type"] = json::Value(kBeaconType);
    root["protocolVersion"] = json::Value(kProtocolVersion);
    root["clientId"] = json::Value(beacon.clientId);
    if (!beacon.name.empty())
        root["name"] = json::Value(beacon.name);
    root["clientVersion"] = json::Value(beacon.clientVersion);
    root["platform"] = json::Value(beacon.platform);
    if (!beacon.host.empty())
        root["host"] = json::Value(beacon.host);
    root["controlPort"] = json::Value(static_cast<uint32_t>(beacon.controlPort));
    root["server"] = json::Value(beacon.server.value_or(std::string()));
    return json::serialize(root);
}

bool parseClientBeacon(const std::string& text, ClientBeacon& out, std::string& error)
{
    json::Value root;
    if (!json::parse(text, root, error))
        return false;
    if (!root.isObject())
    {
        error = "not a JSON object";
        return false;
    }

    std::string type;
    if (!json::getString(root, "type", type) || type != kBeaconType)
    {
        error = "type must be \"clientBeacon\"";
        return false;
    }

    ClientBeacon beacon;
    const json::Value* version = root.find("protocolVersion");
    if (version == nullptr || !version->isNumber() || !version->isIntegral() ||
        version->asInt() < 0 || version->asInt() > 0xFFFF)
    {
        error = "protocolVersion must be a protocol version number";
        return false;
    }
    beacon.protocolVersion = static_cast<uint32_t>(version->asInt());

    if (!json::getString(root, "clientId", beacon.clientId) || beacon.clientId.empty())
    {
        error = "clientId must be a non-empty string";
        return false;
    }

    const json::Value* port = root.find("controlPort");
    if (port == nullptr || !port->isNumber() || !port->isIntegral() || port->asInt() < 1 ||
        port->asInt() > 65535)
    {
        error = "controlPort must be a port number";
        return false;
    }
    beacon.controlPort = static_cast<uint16_t>(port->asInt());

    if (!optionalString(root, "name", beacon.name, error) ||
        !optionalString(root, "clientVersion", beacon.clientVersion, error) ||
        !optionalString(root, "platform", beacon.platform, error) ||
        !optionalString(root, "host", beacon.host, error))
        return false;
    if (root.find("server") != nullptr)
    {
        std::string server;
        if (!optionalString(root, "server", server, error))
            return false;
        beacon.server = server;
    }

    out = beacon;
    return true;
}

} // namespace matp
} // namespace multiroom
