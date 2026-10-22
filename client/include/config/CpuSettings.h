#pragma once
#include "util/Json.h"
#include <string>

namespace multiroom::cpu_settings {
// Firmware opts in; ordinary Linux/Pi clients leave CPU policy to the OS.
bool supported(); // the board file (config/BoardConfig.h), read once
bool supported(const std::string& boardPath);
bool load(json::Value& settings, std::string& error,
          const std::string& policy = "/sys/devices/system/cpu/cpufreq/policy0",
          const std::string& path = "/etc/" MULTIROOM_BRAND_SLUG "-cpu.conf");
bool save(const json::Value& changes, json::Value& settings, std::string& error,
          const std::string& policy = "/sys/devices/system/cpu/cpufreq/policy0",
          const std::string& path = "/etc/" MULTIROOM_BRAND_SLUG "-cpu.conf");
bool restore(std::string& error,
             const std::string& policy = "/sys/devices/system/cpu/cpufreq/policy0",
             const std::string& path = "/etc/" MULTIROOM_BRAND_SLUG "-cpu.conf");
} // namespace multiroom::cpu_settings
