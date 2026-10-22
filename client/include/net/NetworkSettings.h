#pragma once

#include "util/Json.h"
#include <string>

namespace multiroom::network_settings {

// Firmware opts in; ordinary Linux/Pi installations keep OS network management.
bool supported(); // the board file (config/BoardConfig.h), read once
bool supported(const std::string& boardPath);
bool load(json::Value& settings, std::string& error,
          const std::string& path = "/etc/" MULTIROOM_BRAND_SLUG "-network.conf");
bool validate(const json::Value& settings, std::string& error);
// Loads and merges first so an empty password preserves the current secret.
bool save(const json::Value& changes, json::Value& settings, std::string& error,
          const std::string& path = "/etc/" MULTIROOM_BRAND_SLUG "-network.conf");
json::Value publicSettings(json::Value settings);
json::Value parseScan(const std::string& text);
bool scan(bool start, json::Value& result, std::string& error,
          const std::string& state = "/run/raumfeld-network");

} // namespace multiroom::network_settings
