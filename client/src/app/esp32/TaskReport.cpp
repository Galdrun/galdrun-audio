#include "app/TaskReport.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <vector>

namespace multiroom {
namespace {

// The run-time counters at the previous report, by task, so a report covers
// the time since the last one rather than since boot.
std::map<TaskHandle_t, configRUN_TIME_COUNTER_TYPE> g_lastCounters;
configRUN_TIME_COUNTER_TYPE g_lastTotal = 0;

} // namespace

json::Value esp32TaskReport()
{
    json::Value out = json::Value::makeObject();
    out["cores"] = json::Value(static_cast<uint32_t>(portNUM_PROCESSORS));

    std::vector<TaskStatus_t> tasks(uxTaskGetNumberOfTasks() + 4);
    configRUN_TIME_COUNTER_TYPE total = 0;
    const UBaseType_t count =
        uxTaskGetSystemState(tasks.data(), static_cast<UBaseType_t>(tasks.size()), &total);
    tasks.resize(count);

    const configRUN_TIME_COUNTER_TYPE window = total - g_lastTotal;
    out["windowMs"] = json::Value(static_cast<uint64_t>(window) / 1000); // the counter is in us

    struct Row
    {
        json::Value value;
        double cpu = 0.0;
    };
    std::vector<Row> rows;
    std::map<TaskHandle_t, configRUN_TIME_COUNTER_TYPE> counters;
    for (const TaskStatus_t& task : tasks)
    {
        const auto last = g_lastCounters.find(task.xHandle);
        const configRUN_TIME_COUNTER_TYPE ran =
            task.ulRunTimeCounter - (last != g_lastCounters.end() ? last->second : 0);
        counters[task.xHandle] = task.ulRunTimeCounter;

        Row row;
        row.cpu = window > 0 ? 100.0 * static_cast<double>(ran) / static_cast<double>(window) : 0.0;
        row.value = json::Value::makeObject();
        row.value["name"] = json::Value(task.pcTaskName);
        const BaseType_t core = xTaskGetCoreID(task.xHandle);
        row.value["core"] = core == tskNO_AFFINITY ? json::Value("any")
                                                   : json::Value(static_cast<int32_t>(core));
        row.value["priority"] = json::Value(static_cast<uint32_t>(task.uxCurrentPriority));
        row.value["cpuPercent"] = json::Value(std::round(row.cpu * 10.0) / 10.0);
        row.value["stackFreeBytes"] =
            json::Value(static_cast<uint32_t>(task.usStackHighWaterMark) * sizeof(StackType_t));
        rows.push_back(std::move(row));
    }
    g_lastCounters = std::move(counters);
    g_lastTotal = total;

    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.cpu > b.cpu; });
    json::Value list = json::Value::makeArray();
    for (Row& row : rows)
        list.pushBack(std::move(row.value));
    out["tasks"] = std::move(list);

    json::Value heap = json::Value::makeObject();
    heap["internalFree"] =
        json::Value(static_cast<uint64_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
    heap["internalMinFree"] =
        json::Value(static_cast<uint64_t>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)));
    heap["internalLargest"] =
        json::Value(static_cast<uint64_t>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
    heap["psramFree"] =
        json::Value(static_cast<uint64_t>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    heap["psramMinFree"] =
        json::Value(static_cast<uint64_t>(heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM)));
    out["heap"] = std::move(heap);
    return out;
}

} // namespace multiroom
