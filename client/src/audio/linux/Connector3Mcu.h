#pragma once

#include <string>

namespace multiroom {
// The root-only MCU service serializes audio requests with touch-button input.
bool connector3McuRequest(const std::string& command, long& mute, long& volume,
                          std::string& error);
}
