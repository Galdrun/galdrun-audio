#include "matp/CoreBeacon.h"

#include "matp/Protocol.h"
#include "util/Json.h"

namespace multiroom {
namespace matp {

std::string makeCoreBeacon(const CoreBeacon& beacon)
{
    json::Value root = json::Value::makeObject();
    root["type"] = json::Value("coreBeacon");
    root["protocolVersion"] = json::Value(kProtocolVersion);
    root["coreId"] = json::Value(beacon.coreId);
    root["coreVersion"] = json::Value(beacon.coreVersion);
    root["controlPort"] = json::Value(static_cast<uint32_t>(beacon.controlPort));
    return json::serialize(root);
}

} // namespace matp
} // namespace multiroom
