#pragma once

// The settings page's view of an appliance's Bluetooth, read from the files
// its image helper (/usr/sbin/galdrun-bluetooth) keeps. The helper does the
// work -- pairing, scanning, the headphone room -- and the client only starts
// it (ClientCommandHandler) and reads what it left here.

#include "util/Json.h"

namespace multiroom::bluetooth_settings {

constexpr const char* kHelper = "/usr/sbin/galdrun-bluetooth";

// {"pairing":bool, "scanning":bool, "scanSeconds": left of it,
//  "headphones":{address,name,playing}|null,
//  "devices":[{address,name,icon,rssi}]} -- the last scan, nearest first.
json::Value status();

// A Bluetooth address as the helper takes it: six upper-case hex pairs.
bool validAddress(const std::string& address);

} // namespace multiroom::bluetooth_settings
