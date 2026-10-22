#pragma once

// Every FreeRTOS task of an ESP32 client, for the `diagnostics` command: which
// core it may run on, its priority, its share of a core since the last report
// and its free stack. Which task competes with which for a core is the first
// question when the client answers slowly.

#if defined(ESP_PLATFORM)

#include "util/Json.h"

namespace multiroom {

// {"cores":2,"windowMs":..,"tasks":[{"name":..,"core":0|1|"any","priority":..,
// "cpuPercent":..,"stackFreeBytes":..}, ...],"heap":{..}}, busiest first.
// cpuPercent is of one core, over the time since the previous call (since boot
// on the first). heap is internal RAM and PSRAM: free now, the lowest it has
// been, and internal RAM's largest free block.
json::Value esp32TaskReport();

} // namespace multiroom

#endif
