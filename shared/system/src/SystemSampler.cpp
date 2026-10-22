#include "system/SystemSampler.h"
#include "system/TextUtil.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
// After windows.h. K32GetProcessMemoryInfo is exported by kernel32 itself, so
// no psapi library is linked.
#  include <psapi.h>
#  include <tlhelp32.h>
#  include <wlanapi.h>
#elif defined(ESP_PLATFORM)
#  include "esp_chip_info.h"
#  include "esp_heap_caps.h"
#  include "esp_timer.h"
#  include "esp_wifi.h"
#  include "freertos/FreeRTOS.h"
#  include "freertos/task.h"
#  include "sdkconfig.h"
#else
#  include <cmath>
#  include <cstring>
#  include <ctime>
#  include <dirent.h>
#  include <sys/utsname.h>
#  include <unistd.h>
#endif

namespace multiroom {
namespace {

// Taken during static initialization, which is before main(): the closest a
// portable program gets to "when the process started" without asking the OS
// for a wall-clock creation time that moves with the clock.
const std::chrono::steady_clock::time_point kProcessStart = std::chrono::steady_clock::now();

uint64_t steadyNowNs()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

double percentOf(uint64_t part, uint64_t whole)
{
    return std::clamp(100.0 * static_cast<double>(part) / static_cast<double>(whole), 0.0, 100.0);
}

#if defined(_WIN32)

uint64_t fileTimeTicks(const FILETIME& time)
{
    return (static_cast<uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}

bool processCpuNs(uint64_t& out)
{
    FILETIME created, exited, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
        return false;
    out = (fileTimeTicks(kernel) + fileTimeTicks(user)) * 100ULL; // 100 ns ticks
    return true;
}

bool systemCpuTicks(uint64_t& busy, uint64_t& total)
{
    FILETIME idle, kernel, user;
    if (!GetSystemTimes(&idle, &kernel, &user))
        return false;
    // Kernel time includes idle time.
    total = fileTimeTicks(kernel) + fileTimeTicks(user);
    busy = total - std::min(total, fileTimeTicks(idle));
    return true;
}

void sampleMemory(SystemMetrics& out)
{
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (K32GetProcessMemoryInfo(GetCurrentProcess(),
                                reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                                sizeof(counters)))
    {
        out.hasMemoryBytes = true;
        out.memoryBytes = static_cast<uint64_t>(counters.WorkingSetSize);
        out.hasPrivateMemoryBytes = true;
        out.privateMemoryBytes = static_cast<uint64_t>(counters.PrivateUsage);
    }

    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status))
    {
        out.hasSystemMemoryTotalBytes = true;
        out.systemMemoryTotalBytes = status.ullTotalPhys;
        out.hasSystemMemoryAvailableBytes = true;
        out.systemMemoryAvailableBytes = status.ullAvailPhys;
    }
}

void sampleProcess(SystemMetrics& out)
{
    DWORD handles = 0;
    if (GetProcessHandleCount(GetCurrentProcess(), &handles))
    {
        out.hasHandles = true;
        out.handles = handles;
    }

    // Windows has no per-process thread count short of this: the process
    // list, not the far longer thread list, carries it.
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return;
    const DWORD self = GetCurrentProcessId();
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry))
    {
        if (entry.th32ProcessID == self)
        {
            out.hasThreads = true;
            out.threads = entry.cntThreads;
            break;
        }
    }
    CloseHandle(snapshot);
}

void sampleSystemUptime(SystemMetrics& out)
{
    out.hasSystemUptimeMs = true;
    out.systemUptimeMs = GetTickCount64();
}

void sampleTemperature(SystemMetrics&)
{
}

// wlanapi.dll is loaded rather than linked: a Windows Server without the
// Wireless LAN Service feature does not have it, and a missing import would
// stop the process from starting at all over a figure it can do without.
struct WlanApi
{
    decltype(&::WlanOpenHandle) open = nullptr;
    decltype(&::WlanCloseHandle) close = nullptr;
    decltype(&::WlanEnumInterfaces) enumerate = nullptr;
    decltype(&::WlanQueryInterface) query = nullptr;
    decltype(&::WlanFreeMemory) free = nullptr;
};

const WlanApi* wlanApi()
{
    static const WlanApi api = [] {
        WlanApi loaded;
        const HMODULE module =
            LoadLibraryExW(L"wlanapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (module == nullptr)
            return loaded;
        loaded.open = reinterpret_cast<decltype(&::WlanOpenHandle)>(
            reinterpret_cast<void*>(GetProcAddress(module, "WlanOpenHandle")));
        loaded.close = reinterpret_cast<decltype(&::WlanCloseHandle)>(
            reinterpret_cast<void*>(GetProcAddress(module, "WlanCloseHandle")));
        loaded.enumerate = reinterpret_cast<decltype(&::WlanEnumInterfaces)>(
            reinterpret_cast<void*>(GetProcAddress(module, "WlanEnumInterfaces")));
        loaded.query = reinterpret_cast<decltype(&::WlanQueryInterface)>(
            reinterpret_cast<void*>(GetProcAddress(module, "WlanQueryInterface")));
        loaded.free = reinterpret_cast<decltype(&::WlanFreeMemory)>(
            reinterpret_cast<void*>(GetProcAddress(module, "WlanFreeMemory")));
        return loaded;
    }();
    const bool complete =
        api.open && api.close && api.enumerate && api.query && api.free;
    return complete ? &api : nullptr;
}

// The first connected interface. Opened per sample: every 2 s on the control
// path, and a held handle would be one more thing to close at exit.
void sampleWifi(SystemMetrics& out)
{
    const WlanApi* api = wlanApi();
    if (api == nullptr)
        return;
    DWORD version = 0;
    HANDLE client = nullptr;
    if (api->open(2, nullptr, &version, &client) != ERROR_SUCCESS)
        return;

    PWLAN_INTERFACE_INFO_LIST interfaces = nullptr;
    if (api->enumerate(client, nullptr, &interfaces) == ERROR_SUCCESS)
    {
        for (DWORD i = 0; i < interfaces->dwNumberOfItems; ++i)
        {
            const WLAN_INTERFACE_INFO& info = interfaces->InterfaceInfo[i];
            if (info.isState != wlan_interface_state_connected)
                continue;

            DWORD size = 0;
            PWLAN_CONNECTION_ATTRIBUTES connection = nullptr;
            if (api->query(client, &info.InterfaceGuid, wlan_intf_opcode_current_connection,
                           nullptr, &size, reinterpret_cast<PVOID*>(&connection),
                           nullptr) == ERROR_SUCCESS)
            {
                const WLAN_ASSOCIATION_ATTRIBUTES& association =
                    connection->wlanAssociationAttributes;
                out.wifiBssid = text_util::formatMac(association.dot11Bssid);
                out.hasWifiRxRateKbps = true;
                out.wifiRxRateKbps = association.ulRxRate;
                api->free(connection);
            }

            PLONG rssi = nullptr;
            if (api->query(client, &info.InterfaceGuid, wlan_intf_opcode_rssi, nullptr, &size,
                           reinterpret_cast<PVOID*>(&rssi), nullptr) == ERROR_SUCCESS)
            {
                out.hasWifiRssiDbm = true;
                out.wifiRssiDbm = static_cast<int32_t>(*rssi);
                api->free(rssi);
            }

            PULONG channel = nullptr;
            if (api->query(client, &info.InterfaceGuid, wlan_intf_opcode_channel_number,
                           nullptr, &size, reinterpret_cast<PVOID*>(&channel),
                           nullptr) == ERROR_SUCCESS)
            {
                out.hasWifiChannel = true;
                out.wifiChannel = *channel;
                api->free(channel);
            }
            break;
        }
        api->free(interfaces);
    }
    api->close(client, nullptr);
}

#elif defined(ESP_PLATFORM)

// The firmware is the whole machine, so the process and the machine figures
// are the same heap seen from two sides, and the same CPU (sample() copies
// it). No temperature: the ESP32 has no sensor worth reading.
bool processCpuNs(uint64_t&)
{
    return false;
}

// Busy is every core's time minus what its idle task got, both in the
// microseconds of esp_timer that FreeRTOS's run-time counters are kept in.
// Interrupts are charged to whatever task they cut into, so time spent in
// one while a core idles counts as idle: an estimate, slightly low. Absent
// without run-time stats (sdkconfig.defaults turns them on).
bool systemCpuTicks(uint64_t& busy, uint64_t& total)
{
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS && CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER && \
    !CONFIG_FREERTOS_SMP
    total = static_cast<uint64_t>(esp_timer_get_time()) * portNUM_PROCESSORS;
    uint64_t idle = 0;
    for (BaseType_t core = 0; core < portNUM_PROCESSORS; ++core)
        idle += ulTaskGetIdleRunTimeCounterForCore(core);
    busy = total > idle ? total - idle : 0;
    return true;
#else
    (void)busy;
    (void)total;
    return false;
#endif
}

void sampleMemory(SystemMetrics& out)
{
    const size_t total = heap_caps_get_total_size(MALLOC_CAP_DEFAULT);
    const size_t available = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    out.hasSystemMemoryTotalBytes = true;
    out.systemMemoryTotalBytes = total;
    out.hasSystemMemoryAvailableBytes = true;
    out.systemMemoryAvailableBytes = available;
    out.hasMemoryBytes = true;
    out.memoryBytes = total > available ? total - available : 0;

    out.hasInternalMemoryTotalBytes = true;
    out.internalMemoryTotalBytes = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    out.hasInternalMemoryAvailableBytes = true;
    out.internalMemoryAvailableBytes = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    if (psram > 0)
    {
        out.hasPsramTotalBytes = true;
        out.psramTotalBytes = psram;
        out.hasPsramAvailableBytes = true;
        out.psramAvailableBytes = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    }
}

void sampleProcess(SystemMetrics& out)
{
    out.hasThreads = true;
    out.threads = uxTaskGetNumberOfTasks();
}

void sampleSystemUptime(SystemMetrics& out)
{
    out.hasSystemUptimeMs = true;
    out.systemUptimeMs = static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL;
}

void sampleTemperature(SystemMetrics&)
{
}

// Not associated (setup mode, or between two attempts) is no link at all,
// which is every field absent.
void sampleWifi(SystemMetrics& out)
{
    wifi_ap_record_t access = {};
    if (esp_wifi_sta_get_ap_info(&access) != ESP_OK)
        return;
    out.hasWifiRssiDbm = true;
    out.wifiRssiDbm = access.rssi;
    out.hasWifiChannel = true;
    out.wifiChannel = access.primary;
    out.wifiBssid = text_util::formatMac(access.bssid);
}

#else

bool processCpuNs(uint64_t& out)
{
    timespec time{};
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &time) != 0)
        return false;
    out = static_cast<uint64_t>(time.tv_sec) * 1000000000ULL + static_cast<uint64_t>(time.tv_nsec);
    return true;
}

// The aggregate "cpu" line of /proc/stat, in clock ticks. Guest time is
// already counted in user time, so it is not added again.
bool systemCpuTicks(uint64_t& busy, uint64_t& total)
{
    FILE* file = std::fopen("/proc/stat", "r");
    if (file == nullptr)
        return false;
    unsigned long long user = 0, nice = 0, system = 0, idle = 0, iowait = 0, irq = 0,
                       softirq = 0, steal = 0;
    const int read = std::fscanf(file, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &user,
                                 &nice, &system, &idle, &iowait, &irq, &softirq, &steal);
    std::fclose(file);
    if (read < 4)
        return false;
    total = user + nice + system + idle + iowait + irq + softirq + steal;
    busy = total - (idle + iowait);
    return true;
}

void sampleMemory(SystemMetrics& out)
{
    const long pageBytes = sysconf(_SC_PAGESIZE);
    if (FILE* file = std::fopen("/proc/self/statm", "r"))
    {
        unsigned long long size = 0, resident = 0;
        if (std::fscanf(file, "%llu %llu", &size, &resident) == 2 && pageBytes > 0)
        {
            out.hasMemoryBytes = true;
            out.memoryBytes = resident * static_cast<uint64_t>(pageBytes);
        }
        std::fclose(file);
    }

    if (FILE* file = std::fopen("/proc/meminfo", "r"))
    {
        char line[256];
        while (std::fgets(line, sizeof(line), file) != nullptr)
        {
            unsigned long long kib = 0;
            if (std::sscanf(line, "MemTotal: %llu kB", &kib) == 1)
            {
                out.hasSystemMemoryTotalBytes = true;
                out.systemMemoryTotalBytes = kib * 1024ULL;
            }
            else if (std::sscanf(line, "MemAvailable: %llu kB", &kib) == 1)
            {
                out.hasSystemMemoryAvailableBytes = true;
                out.systemMemoryAvailableBytes = kib * 1024ULL;
            }
        }
        std::fclose(file);
    }
}

void sampleProcess(SystemMetrics& out)
{
    if (FILE* file = std::fopen("/proc/self/status", "r"))
    {
        char line[256];
        while (std::fgets(line, sizeof(line), file) != nullptr)
        {
            unsigned long long value = 0;
            if (std::sscanf(line, "Threads: %llu", &value) == 1)
            {
                out.hasThreads = true;
                out.threads = value;
            }
            else if (std::sscanf(line, "RssAnon: %llu kB", &value) == 1)
            {
                out.hasPrivateMemoryBytes = true;
                out.privateMemoryBytes = value * 1024ULL;
            }
        }
        std::fclose(file);
    }

    if (DIR* dir = opendir("/proc/self/fd"))
    {
        uint64_t count = 0;
        while (const dirent* entry = readdir(dir))
        {
            if (entry->d_name[0] != '.')
                ++count;
        }
        closedir(dir);
        // Less the descriptor the listing itself held open.
        out.hasHandles = count > 0;
        out.handles = count > 0 ? count - 1 : 0;
    }
}

void sampleSystemUptime(SystemMetrics& out)
{
    FILE* file = std::fopen("/proc/uptime", "r");
    if (file == nullptr)
        return;
    double seconds = 0.0;
    if (std::fscanf(file, "%lf", &seconds) == 1 && seconds >= 0.0)
    {
        out.hasSystemUptimeMs = true;
        out.systemUptimeMs = static_cast<uint64_t>(seconds * 1000.0);
    }
    std::fclose(file);
}

void sampleTemperature(SystemMetrics& out)
{
    FILE* file = std::fopen("/sys/class/thermal/thermal_zone0/temp", "r");
    if (file == nullptr)
        return;
    long milliCelsius = 0;
    if (std::fscanf(file, "%ld", &milliCelsius) == 1)
    {
        out.hasTemperatureC = true;
        out.temperatureC = static_cast<double>(milliCelsius) / 1000.0;
    }
    std::fclose(file);
}

// /proc/net/wireless: two header lines, then one per interface with its
// signal level in the fourth column. Only a negative level is taken as dBm;
// some drivers put a relative 0-100 figure there instead, which is not one.
void sampleWifi(SystemMetrics& out)
{
    FILE* file = std::fopen("/proc/net/wireless", "r");
    if (file == nullptr)
        return;
    char line[256];
    for (int number = 0; std::fgets(line, sizeof(line), file) != nullptr; ++number)
    {
        if (number < 2)
            continue;
        const char* fields = std::strchr(line, ':');
        unsigned status = 0;
        double link = 0.0;
        double level = 0.0;
        if (fields != nullptr && std::sscanf(fields + 1, "%x %lf %lf", &status, &link, &level) == 3 &&
            level < 0.0)
        {
            out.hasWifiRssiDbm = true;
            out.wifiRssiDbm = static_cast<int32_t>(std::lround(level));
            break;
        }
    }
    std::fclose(file);
}

#endif

// --- what the machine is: read once ----------------------------------------

struct Machine
{
    std::string architecture;
    std::string cpuModel;
    std::string model;
    uint64_t cores = 0;
};

#if defined(_WIN32)

std::string utf8(const wchar_t* text)
{
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1)
        return std::string();
    std::string out(static_cast<size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, &out[0], size, nullptr, nullptr);
    return out;
}

std::string registryString(const wchar_t* key, const wchar_t* name)
{
    wchar_t value[256];
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, key, name, RRF_RT_REG_SZ, nullptr, value, &size) !=
        ERROR_SUCCESS)
        return std::string();
    std::string text = utf8(value);
    // Intel pads the processor name with spaces on both sides.
    const size_t first = text.find_first_not_of(' ');
    const size_t last = text.find_last_not_of(' ');
    return first == std::string::npos ? std::string() : text.substr(first, last - first + 1);
}

Machine readMachine()
{
    Machine machine;
    SYSTEM_INFO info = {};
    GetNativeSystemInfo(&info);
    switch (info.wProcessorArchitecture)
    {
    case PROCESSOR_ARCHITECTURE_AMD64:
        machine.architecture = "x86_64";
        break;
    case PROCESSOR_ARCHITECTURE_ARM64:
        machine.architecture = "aarch64";
        break;
    case PROCESSOR_ARCHITECTURE_INTEL:
        machine.architecture = "x86";
        break;
    case PROCESSOR_ARCHITECTURE_ARM:
        machine.architecture = "arm";
        break;
    default:
        break;
    }
    machine.cores = info.dwNumberOfProcessors;
    machine.cpuModel = registryString(L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                                      L"ProcessorNameString");
    machine.model =
        registryString(L"HARDWARE\\DESCRIPTION\\System\\BIOS", L"SystemProductName");
    return machine;
}

#elif defined(ESP_PLATFORM)

Machine readMachine()
{
    Machine machine;
#  if defined(__XTENSA__)
    machine.architecture = "xtensa";
#  else
    machine.architecture = "riscv32";
#  endif
    esp_chip_info_t chip = {};
    esp_chip_info(&chip);
    machine.cores = chip.cores;

    char model[48];
    std::string target = CONFIG_IDF_TARGET;
    std::transform(target.begin(), target.end(), target.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    std::snprintf(model, sizeof(model), "%s rev v%u.%u", target.c_str(),
                  static_cast<unsigned>(chip.revision / 100),
                  static_cast<unsigned>(chip.revision % 100));
    machine.cpuModel = model;
#  if defined(MULTIROOM_BOARD)
    machine.model = MULTIROOM_BOARD;
#  endif
    return machine;
}

#else

std::string trimmed(const std::string& text)
{
    const size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return std::string();
    const size_t last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

// 64-bit ARM kernels list no "model name", only the core's part number. The
// cores a Raspberry Pi or a comparable board has.
std::string armCoreName(const std::string& part)
{
    static const struct
    {
        const char* part;
        const char* name;
    } kCores[] = {{"0xb76", "ARM1176"},    {"0xc07", "Cortex-A7"},  {"0xd03", "Cortex-A53"},
                  {"0xd04", "Cortex-A35"}, {"0xd05", "Cortex-A55"}, {"0xd07", "Cortex-A57"},
                  {"0xd08", "Cortex-A72"}, {"0xd0b", "Cortex-A76"}};
    for (const auto& core : kCores)
    {
        if (part == core.part)
            return core.name;
    }
    return std::string();
}

Machine readMachine()
{
    Machine machine;
    utsname name = {};
    if (uname(&name) == 0)
        machine.architecture = name.machine;
    machine.cores = std::thread::hardware_concurrency();

    std::string part;
    if (FILE* file = std::fopen("/proc/cpuinfo", "r"))
    {
        char line[512];
        while (std::fgets(line, sizeof(line), file) != nullptr)
        {
            const char* colon = std::strchr(line, ':');
            if (colon == nullptr)
                continue;
            const std::string key =
                trimmed(std::string(line, static_cast<size_t>(colon - line)));
            const std::string value = trimmed(colon + 1);
            if (key == "model name" && machine.cpuModel.empty())
                machine.cpuModel = value;
            else if (key == "CPU part" && part.empty())
                part = value;
        }
        std::fclose(file);
    }
    if (machine.cpuModel.empty())
        machine.cpuModel = armCoreName(part);

    // Device tree boards (every Raspberry Pi) name themselves here. The file
    // ends in a NUL, and the buffer keeps one past what is read either way.
    if (FILE* file = std::fopen("/proc/device-tree/model", "r"))
    {
        char model[128] = {};
        if (std::fread(model, 1, sizeof(model) - 1, file) > 0)
            machine.model = trimmed(model);
        std::fclose(file);
    }
    return machine;
}

#endif

void sampleMachine(SystemMetrics& out)
{
    static const Machine machine = readMachine();
    out.cpuArchitecture = machine.architecture;
    out.cpuModel = machine.cpuModel;
    out.machineModel = machine.model;
    out.hasCpuCores = machine.cores > 0;
    out.cpuCores = machine.cores;
}

#if defined(_WIN32)

void sampleCpuClock(SystemMetrics& out)
{
    DWORD mhz = 0;
    DWORD size = sizeof(mhz);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                     L"~MHz", RRF_RT_REG_DWORD, nullptr, &mhz, &size) == ERROR_SUCCESS &&
        mhz > 0)
    {
        out.hasCpuMhz = true;
        out.cpuMhz = mhz;
    }
}

#elif defined(ESP_PLATFORM)

void sampleCpuClock(SystemMetrics& out)
{
    out.hasCpuMhz = true;
    out.cpuMhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
}

#else

// cpufreq reports kHz. Absent where the kernel has no cpufreq driver.
void sampleCpuClock(SystemMetrics& out)
{
    FILE* file = std::fopen("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", "r");
    if (file == nullptr)
        return;
    unsigned long long khz = 0;
    if (std::fscanf(file, "%llu", &khz) == 1 && khz > 0)
    {
        out.hasCpuMhz = true;
        out.cpuMhz = (khz + 500) / 1000;
    }
    std::fclose(file);
}

#endif

} // namespace

SystemMetrics SystemSampler::sampleUsage()
{
    SystemMetrics out;

    const uint64_t wallNs = steadyNowNs();
    uint64_t processNs = 0;
    const bool hasProcess = processCpuNs(processNs);
    const unsigned cores = std::thread::hardware_concurrency();
    if (hasProcess && hasPrevious_ && cores > 0 && wallNs > previousWallNs_ &&
        processNs >= previousProcessCpuNs_)
    {
        out.hasCpuPercent = true;
        out.cpuPercent = percentOf(processNs - previousProcessCpuNs_,
                                   (wallNs - previousWallNs_) * cores);
    }
    hasPrevious_ = hasProcess;
    previousWallNs_ = wallNs;
    previousProcessCpuNs_ = processNs;

    uint64_t busy = 0;
    uint64_t total = 0;
    const bool hasSystem = systemCpuTicks(busy, total);
    if (hasSystem && hasPreviousSystem_ && total > previousSystemTotal_ &&
        busy >= previousSystemBusy_)
    {
        out.hasSystemCpuPercent = true;
        out.systemCpuPercent = percentOf(busy - previousSystemBusy_, total - previousSystemTotal_);
    }
    hasPreviousSystem_ = hasSystem;
    previousSystemBusy_ = busy;
    previousSystemTotal_ = total;
#if defined(ESP_PLATFORM)
    out.hasCpuPercent = out.hasSystemCpuPercent;
    out.cpuPercent = out.systemCpuPercent;
#endif

    sampleMemory(out);
    sampleWifi(out);
    sampleMachine(out);
    sampleCpuClock(out);
    return out;
}

SystemMetrics SystemSampler::sample()
{
    SystemMetrics out = sampleUsage();
    sampleProcess(out);
    sampleSystemUptime(out);
    sampleTemperature(out);

    out.hasUptimeMs = true;
    out.uptimeMs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                             std::chrono::steady_clock::now() - kProcessStart)
                                             .count());
    return out;
}

} // namespace multiroom
