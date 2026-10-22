// Phase 34: the shared system sampler, on whatever machine builds this.
//
// The figures are the build machine's own, so only their ranges are asserted:
// a percentage between 0 and 100, memory above zero, available not above
// total. What a platform cannot say is asserted absent rather than zero.

#include "TestSupport.h"

#include "system/SystemSampler.h"

#include <chrono>
#include <thread>

using namespace multiroom;

MULTIROOM_TEST(system_sampler_answers_its_figures_in_range)
{
    SystemSampler sampler;

    // A rate needs two samples: the first has nothing to measure against.
    const SystemMetrics first = sampler.sample();
    CHECK(!first.hasCpuPercent);
    CHECK(!first.hasSystemCpuPercent);

    // Something to measure, so the interval is not all rounding.
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
    volatile uint64_t spin = 0;
    while (std::chrono::steady_clock::now() < until)
        spin = spin + 1;

    const SystemMetrics second = sampler.sample();
    CHECK(second.hasCpuPercent);
    CHECK(second.cpuPercent >= 0.0 && second.cpuPercent <= 100.0);
    CHECK(second.hasSystemCpuPercent);
    CHECK(second.systemCpuPercent >= 0.0 && second.systemCpuPercent <= 100.0);

    CHECK(second.hasMemoryBytes);
    CHECK(second.memoryBytes > 0);
    CHECK(second.hasSystemMemoryTotalBytes);
    CHECK(second.hasSystemMemoryAvailableBytes);
    CHECK(second.systemMemoryTotalBytes > 0);
    CHECK(second.systemMemoryAvailableBytes <= second.systemMemoryTotalBytes);
    CHECK(second.memoryBytes <= second.systemMemoryTotalBytes);

    CHECK(second.hasPrivateMemoryBytes);
    CHECK(second.privateMemoryBytes > 0);
    CHECK(second.hasThreads);
    CHECK(second.threads >= 1);
    CHECK(second.hasHandles);
    CHECK(second.handles > 0);

    CHECK(second.hasUptimeMs);
    CHECK(second.uptimeMs >= first.uptimeMs);
    CHECK(second.hasSystemUptimeMs);
    CHECK(second.systemUptimeMs >= second.uptimeMs);

    // Every build machine knows what it is.
    CHECK(!second.cpuArchitecture.empty());
    CHECK(!second.cpuModel.empty());
    CHECK(second.hasCpuCores);
    CHECK_EQ(second.cpuCores, uint64_t(std::thread::hardware_concurrency()));

#if defined(_WIN32)
    // The only API for it is WMI, which the sampler does not use.
    CHECK(!second.hasTemperatureC);
#endif
}
