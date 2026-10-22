#pragma once

#include "util/Json.h"
#include <string>

namespace multiroom::ssh_settings {

// Firmware opts in (board file "sshSettings"). SSH is off until the settings
// page turns it on, which needs a root password; once one is set, every change
// needs it. The board helper applies a change; this side checks its shape.
bool supported(); // the board file (config/BoardConfig.h), read once
bool supported(const std::string& boardPath);
bool load(json::Value& settings, std::string& error,
          const std::string& shadow = "/etc/shadow",
          const std::string& config = "/etc/" MULTIROOM_BRAND_SLUG "-ssh.conf",
          const std::string& pidFile = "/var/run/sshd.pid");
// {"enabled": bool, "currentPassword"?: string, "password"?: string}
bool validate(const json::Value& changes, std::string& error);
bool save(const json::Value& changes, json::Value& settings, std::string& error,
          const std::string& helper = "/usr/sbin/raumfeld-ssh",
          const std::string& shadow = "/etc/shadow",
          const std::string& config = "/etc/" MULTIROOM_BRAND_SLUG "-ssh.conf",
          const std::string& pidFile = "/var/run/sshd.pid");

} // namespace multiroom::ssh_settings
