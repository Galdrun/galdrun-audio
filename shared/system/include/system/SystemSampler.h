#pragma once

// CPU, memory, threads, handles, uptime, temperature and the Wi-Fi link of this process and its machine, for a
// UI that wants to say how every box in the house is doing.
//
// Every figure is optional and **absent when the platform cannot say**, never
// zero: 0 % CPU is an idle process and 0 °C is a cold board, and neither is
// what "unknown" means. The two CPU figures are rates, so they are absent on
// the first sample, which has nothing to measure against.
//
// Not thread-safe and not for the audio thread: on Linux a sample reads files
// in /proc. One owner calls sample() on a thread that holds no lock anyone
// else waits for.

#include <cstdint>
#include <string>

namespace multiroom {

struct SystemMetrics
{
    // This process, as a share of the whole machine (all cores), since the
    // previous sample. 0 to 100.
    bool hasCpuPercent = false;
    double cpuPercent = 0.0;

    // The machine, over the same interval. 0 to 100.
    bool hasSystemCpuPercent = false;
    double systemCpuPercent = 0.0;

    // Resident memory of this process.
    bool hasMemoryBytes = false;
    uint64_t memoryBytes = 0;

    // Memory only this process holds: the commit charge on Windows
    // (PrivateUsage), resident anonymous memory on Linux (RssAnon).
    bool hasPrivateMemoryBytes = false;
    uint64_t privateMemoryBytes = 0;

    bool hasThreads = false;
    uint64_t threads = 0;

    // Open kernel handles on Windows, open file descriptors elsewhere.
    bool hasHandles = false;
    uint64_t handles = 0;

    bool hasSystemMemoryTotalBytes = false;
    uint64_t systemMemoryTotalBytes = 0;
    bool hasSystemMemoryAvailableBytes = false;
    uint64_t systemMemoryAvailableBytes = 0;

    // The ESP32 only: the system memory above split into the chip's own RAM,
    // which DMA, Wi-Fi and the stacks need and which runs out first, and the
    // external PSRAM. PSRAM is absent on a board without it.
    bool hasInternalMemoryTotalBytes = false;
    uint64_t internalMemoryTotalBytes = 0;
    bool hasInternalMemoryAvailableBytes = false;
    uint64_t internalMemoryAvailableBytes = 0;
    bool hasPsramTotalBytes = false;
    uint64_t psramTotalBytes = 0;
    bool hasPsramAvailableBytes = false;
    uint64_t psramAvailableBytes = 0;

    // Since the process started.
    bool hasUptimeMs = false;
    uint64_t uptimeMs = 0;

    // Since the machine booted.
    bool hasSystemUptimeMs = false;
    uint64_t systemUptimeMs = 0;

    // Linux only, from the first thermal zone. On Windows the only API for it
    // is WMI, which is not worth a COM apartment on the control path.
    bool hasTemperatureC = false;
    double temperatureC = 0.0;

    // What the machine is, read once: they do not change while it runs.
    // Empty when unknown. The architecture is the kernel's own name for it
    // ("armv6l", "aarch64", "x86_64"; "xtensa" on an ESP32), which is what
    // tells a 512 MB ARMv6 room from the others.
    std::string cpuArchitecture;
    std::string cpuModel;     // "ARMv6-compatible processor rev 7 (v6l)", "Cortex-A53"
    std::string machineModel; // "Raspberry Pi Model B Plus Rev 1.2", or the board
    bool hasCpuCores = false;
    uint64_t cpuCores = 0;

    // The clock of the first core, sampled each time: Linux reports the one
    // it runs at now (a Pi steps down when idle or hot), Windows its rated
    // clock, the ESP32 the one it was configured for.
    bool hasCpuMhz = false;
    uint64_t cpuMhz = 0;

    // The Wi-Fi link the machine is associated over, all absent on a wired
    // one. Signal strength in dBm, as the driver reports it (Windows, the
    // ESP32 and Linux's /proc/net/wireless).
    bool hasWifiRssiDbm = false;
    int32_t wifiRssiDbm = 0;

    // Windows and the ESP32 only: Linux needs nl80211 for them.
    bool hasWifiChannel = false;
    uint64_t wifiChannel = 0;
    std::string wifiBssid; // "aa:bb:cc:dd:ee:ff", empty when unknown

    // The PHY rate the machine last received at, the ceiling for audio sent
    // to it. Windows only: the ESP32 has no API for it.
    bool hasWifiRxRateKbps = false;
    uint64_t wifiRxRateKbps = 0;

    // The kind of interface the client's connection to the server leaves by:
    // "ethernet" or "wifi", empty when unknown. Not sampled here: the client
    // sets it from its server socket (ServerConnection::networkLink).
    std::string networkLink;
};

class SystemSampler
{
public:
    SystemSampler() = default;

    SystemMetrics sample();
    // CPU, memory and Wi-Fi link, for live charts without process/task scans.
    SystemMetrics sampleUsage();

private:
    // The previous sample's counters, which the two CPU rates are taken
    // against. Units are the platform's own; only their differences are used.
    bool hasPrevious_ = false;
    uint64_t previousWallNs_ = 0;
    uint64_t previousProcessCpuNs_ = 0;
    bool hasPreviousSystem_ = false;
    uint64_t previousSystemBusy_ = 0;
    uint64_t previousSystemTotal_ = 0;
};

} // namespace multiroom
