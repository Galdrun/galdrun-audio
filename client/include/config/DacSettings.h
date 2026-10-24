#pragma once
#include "util/Json.h"
#include <string>

namespace multiroom::dac_settings {
// Firmware opts in. ALSA owns the codec's I2C registers and cache.
// Select/restore the concrete card on the owner thread; Connector 2 stays fixed.
bool selectDevice(const std::string& deviceId, const std::string& volumeElement, std::string& error);
bool supported(); // the board file (config/BoardConfig.h), read once
bool supported(const std::string& boardPath);
bool load(json::Value& settings, std::string& error);
bool save(const json::Value& changes, json::Value& settings, std::string& error,
          const std::string& path = "/etc/" MULTIROOM_BRAND_SLUG "-dac.json");
bool restore(std::string& error, const std::string& path = "/etc/" MULTIROOM_BRAND_SLUG "-dac.json");
// The master level and switch, which the room's volume owns while it plays
// on this card: written by StreamPlayback, never by these settings.
bool isRoomVolumeControl(const std::string& name);
// keepRoomVolume leaves those two where the room's volume put them.
bool reset(json::Value& settings, std::string& error, bool keepRoomVolume,
           const std::string& path = "/etc/" MULTIROOM_BRAND_SLUG "-dac.json");
} // namespace multiroom::dac_settings
