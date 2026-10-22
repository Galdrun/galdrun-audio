// ESP32 entry point: what an operating system would have done before main()
// on the other platforms -- flash storage, a file system, a network -- and
// then the same clientMain the desktop build runs as main().
//
// The network comes from /data/wifi.json. Without one, with the setup button
// held at reset, or when the saved network cannot be joined for a minute, the
// board opens its own access point with the settings page (web_ui) behind a
// captive portal. That page is also served on the LAN once the board is on it.

#include "CaptiveDns.h"
#include "WebUi.h"
#include "WifiSettings.h"

#include "config/ClientConfig.h"
#include "discovery/NodeId.h"
#include "system/TextUtil.h"
#include "util/FileUtil.h"

#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_pthread.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <pthread.h>
#include <string>
#include <sys/stat.h>

int clientMain(int argc, char** argv);

// boards/<board>.inputs.json, or "[]" for a board without one (CMakeLists.txt).
extern const char kBoardInputs[] asm("_binary_board_inputs_json_start");

namespace {

const char* const kTag = MULTIROOM_BRAND_SLUG;
const char* const kDataPath = "/data";
const char* const kDataPartition = "storage";

const TickType_t kConnectTimeout = pdMS_TO_TICKS(60000);
// While the setup access point is up, the station retries this far apart:
// every attempt scans across channels, and the access point follows the
// station's channel, so constant retries would keep dropping the phone that
// is trying to reach the setup page.
const uint64_t kSetupRetryUs = 15000000;

const EventBits_t kConnectedBit = BIT0;
EventGroupHandle_t g_wifiEvents = nullptr;

esp_netif_t* g_station = nullptr;
esp_netif_t* g_accessPoint = nullptr;
std::atomic<bool> g_stationEnabled(false);
std::atomic<bool> g_setupActive(false);
esp_timer_handle_t g_retryTimer = nullptr;

void connectStation()
{
    if (g_stationEnabled.load())
        esp_wifi_connect();
}

void onWifiEvent(void*, esp_event_base_t base, int32_t id, void*)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START)
    {
        connectStation();
    }
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED)
    {
        // Forever: a room that gave up on its access point is useless. With
        // no setup access point up, at once -- the driver's scan paces it.
        xEventGroupClearBits(g_wifiEvents, kConnectedBit);
        if (g_setupActive.load())
            esp_timer_start_once(g_retryTimer, kSetupRetryUs);
        else
            connectStation();
    }
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
    {
        xEventGroupSetBits(g_wifiEvents, kConnectedBit);
    }
}

bool initStorage()
{
    esp_err_t result = nvs_flash_init();
    if (result == ESP_ERR_NVS_NO_FREE_PAGES || result == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        result = nvs_flash_init();
    }
    ESP_ERROR_CHECK(result);

    esp_vfs_littlefs_conf_t config = {};
    config.base_path = kDataPath;
    config.partition_label = kDataPartition;
    // No silent formatting: a mount that fails every boot would wipe the setup
    // page's saves on every boot. Say why it failed, then format as the
    // recovery it is meant to be.
    config.format_if_mount_failed = false;
    result = esp_vfs_littlefs_register(&config);
    if (result != ESP_OK)
    {
        ESP_LOGE(kTag, "mounting %s failed: %s", kDataPath, esp_err_to_name(result));
        ESP_LOGW(kTag, "formatting the storage partition to recover");
        config.format_if_mount_failed = true;
        result = esp_vfs_littlefs_register(&config);
        if (result != ESP_OK)
        {
            ESP_LOGE(kTag, "cannot mount %s even after formatting: %s", kDataPath,
                     esp_err_to_name(result));
            return false;
        }
    }
    // What the mount sees, versus what a raw flash dump holds: this list is
    // the difference the lost-saves investigation turns on.
    if (DIR* directory = opendir(kDataPath))
    {
        while (const dirent* entry = readdir(directory))
        {
            struct stat info = {};
            const std::string path = std::string(kDataPath) + "/" + entry->d_name;
            if (stat(path.c_str(), &info) != 0)
                info.st_size = -1;
            ESP_LOGI(kTag, "%s: %lld bytes", path.c_str(),
                     static_cast<long long>(info.st_size));
        }
        closedir(directory);
    }
    else
        ESP_LOGE(kTag, "cannot list %s", kDataPath);
    return true;
}

// The first start writes client.json with the board's own buttons, before the
// settings page or the client could write one without them. From then on the
// file is what counts: the server changes the bindings there, and a button
// taken out of it stays out.
void seedClientConfig()
{
    if (multiroom::fileutil::exists(web_ui::kClientConfigPath))
        return;
    multiroom::ClientConfig config;
    std::string error;
    if (!multiroom::client_config::deserialize(std::string("{\"inputs\":") + kBoardInputs + "}",
                                            config, error) ||
        !multiroom::client_config::save(web_ui::kClientConfigPath, config, error))
    {
        ESP_LOGE(kTag, "board inputs for %s: %s", MULTIROOM_BOARD, error.c_str());
        return;
    }
    ESP_LOGI(kTag, "%s written with %u input(s) of %s", web_ui::kClientConfigPath,
             static_cast<unsigned>(config.inputs.size()), MULTIROOM_BOARD);
}

// Active low, sampled for 50 ms so a bounce at power-up is not a press.
bool setupButtonHeld()
{
#if CONFIG_MULTIROOM_SETUP_BUTTON_GPIO >= 0
    const gpio_num_t pin = static_cast<gpio_num_t>(CONFIG_MULTIROOM_SETUP_BUTTON_GPIO);
    gpio_config_t config = {};
    config.pin_bit_mask = 1ULL << CONFIG_MULTIROOM_SETUP_BUTTON_GPIO;
    config.mode = GPIO_MODE_INPUT;
    config.pull_up_en = GPIO_PULLUP_ENABLE; // GPIO 34-39 have none; the board's own holds
    gpio_config(&config);
    for (int i = 0; i < 5; ++i)
    {
        if (gpio_get_level(pin) != 0)
            return false;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return true;
#else
    return false;
#endif
}

// Before the station starts. With its DHCP client stopped, esp_netif reports
// the static address as IP_EVENT_STA_GOT_IP on every association, so the
// rest of the start-up waits for the network exactly as with DHCP. An invalid
// stored address (an edited file) falls back to DHCP rather than to a board
// nobody can reach.
void applyStaticIp(const WifiSettings& settings)
{
    std::string error;
    if (!wifi_settings::validateStaticIp(settings, error))
    {
        ESP_LOGW(kTag, "ignoring the static address: %s; using DHCP", error.c_str());
        return;
    }
    uint32_t ip = 0;
    uint32_t netmask = 0;
    uint32_t gateway = 0;
    uint32_t dns = 0;
    multiroom::text_util::parseIpv4(settings.ip, ip);
    multiroom::text_util::parseIpv4(settings.netmask, netmask);
    multiroom::text_util::parseIpv4(settings.gateway, gateway); // stays 0 when empty
    if (!multiroom::text_util::parseIpv4(settings.dns, dns))
        dns = gateway;

    esp_netif_ip_info_t info = {};
    info.ip.addr = esp_netif_htonl(ip);
    info.netmask.addr = esp_netif_htonl(netmask);
    info.gw.addr = esp_netif_htonl(gateway);
    esp_netif_dhcpc_stop(g_station);
    if (esp_netif_set_ip_info(g_station, &info) != ESP_OK)
    {
        ESP_LOGW(kTag, "cannot set the static address; using DHCP");
        esp_netif_dhcpc_start(g_station);
        return;
    }
    if (dns != 0)
    {
        esp_netif_dns_info_t server = {};
        server.ip.type = ESP_IPADDR_TYPE_V4;
        server.ip.u_addr.ip4.addr = esp_netif_htonl(dns);
        esp_netif_set_dns_info(g_station, ESP_NETIF_DNS_MAIN, &server);
    }
    ESP_LOGI(kTag, "static address %s/%s, gateway %s", settings.ip.c_str(),
             settings.netmask.c_str(), settings.gateway.empty() ? "none" : settings.gateway.c_str());
}

void startWifi(const WifiSettings& settings)
{
    g_wifiEvents = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    // Both interfaces exist from here on and are never destroyed; the beacon's
    // interface walk relies on that.
    g_station = esp_netif_create_default_wifi_sta();
    g_accessPoint = esp_netif_create_default_wifi_ap();
    // The name a router shows and, with its DNS, the one to type in a
    // browser: wifi.json's deviceName, or the esp32-<mac> default a
    // generated client id is also built from. An invalid stored name (an
    // edited file) falls back to the default rather than failing the boot.
    std::string host = settings.deviceName;
    if (!multiroom::text_util::isValidHostName(host))
        host = multiroom::discovery::hostName();
    if (esp_netif_set_hostname(g_station, host.c_str()) != ESP_OK)
        ESP_LOGW(kTag, "cannot set the host name \"%s\"; the driver keeps its own",
                 host.c_str());
    if (settings.staticIp)
        applyStaticIp(settings);

    esp_timer_create_args_t retry = {};
    retry.callback = [](void*) { connectStation(); };
    retry.name = "wifi_retry";
    ESP_ERROR_CHECK(esp_timer_create(&retry, &g_retryTimer));

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    // wifi.json is the one copy of the credentials; the driver keeps none.
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(
        esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &onWifiEvent, nullptr));
    ESP_ERROR_CHECK(
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &onWifiEvent, nullptr));

    wifi_config_t config = {};
    std::strncpy(reinterpret_cast<char*>(config.sta.ssid), settings.ssid.c_str(),
                 sizeof(config.sta.ssid));
    std::strncpy(reinterpret_cast<char*>(config.sta.password), settings.password.c_str(),
                 sizeof(config.sta.password));
    // The strongest access point with this name, not the first one a fast scan
    // finds: with several, that was one at -86 dBm beside one at -66 dBm.
    config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    // Or the one access point the settings page fixed. While that one is off,
    // the board cannot join and brings up its setup page, as for any network
    // it cannot reach.
    if (!settings.bssid.empty())
    {
        if (multiroom::text_util::parseMac(settings.bssid, config.sta.bssid))
        {
            config.sta.bssid_set = true;
            ESP_LOGI(kTag, "access point fixed to %s", settings.bssid.c_str());
        }
        else
            ESP_LOGW(kTag, "ignoring the invalid access point \"%s\"", settings.bssid.c_str());
    }
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &config));
    g_stationEnabled.store(!settings.ssid.empty());
    ESP_ERROR_CHECK(esp_wifi_start());
    // Modem sleep holds packets back for up to a beacon interval, which is
    // exactly the jitter the clock exchange measures and the audio stream
    // has to buffer against.
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
}

// Station and access point together: the station keeps looking for the saved
// network (when there is one) and serves the page's network scan.
void startSetupAccessPoint()
{
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    wifi_config_t config = {};
    const int length = std::snprintf(reinterpret_cast<char*>(config.ap.ssid),
                                     sizeof(config.ap.ssid), MULTIROOM_BRAND_NAME "-Setup-%02X%02X%02X",
                                     mac[3], mac[4], mac[5]);
    config.ap.ssid_len = static_cast<uint8_t>(length);
    config.ap.channel = 1;
    config.ap.max_connection = 2;
    // Open, so a phone joins without a password to type; the page is only
    // reachable while setup mode lasts.
    config.ap.authmode = WIFI_AUTH_OPEN;

    g_setupActive.store(true);
    web_ui::setSetupMode(true);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &config));

    esp_netif_ip_info_t ip = {};
    esp_netif_get_ip_info(g_accessPoint, &ip);
    captive_dns::start(ip.ip.addr);
    ESP_LOGW(kTag, "setup mode: join \"%s\" and open http://" IPSTR "/",
             reinterpret_cast<const char*>(config.ap.ssid), IP2STR(&ip.ip));
}

void stopSetupAccessPoint()
{
    captive_dns::stop();
    esp_wifi_set_mode(WIFI_MODE_STA);
    g_setupActive.store(false);
    web_ui::setSetupMode(false);
}

bool waitForNetwork(TickType_t timeout)
{
    return (xEventGroupWaitBits(g_wifiEvents, kConnectedBit, pdFALSE, pdTRUE, timeout) &
            kConnectedBit) != 0;
}

} // namespace

extern "C" void app_main()
{
    if (!initStorage())
        return;
    ESP_LOGI(kTag, "board %s", MULTIROOM_BOARD);
    seedClientConfig();

    WifiSettings wifi;
    std::string error;
    if (!wifi_settings::load(wifi, error))
    {
        ESP_LOGE(kTag, "%s: %s", wifi_settings::kPath, error.c_str());
        wifi = WifiSettings();
    }
    const bool forced = setupButtonHeld();

    startWifi(forced ? WifiSettings() : wifi);
    web_ui::start();

    if (forced || wifi.ssid.empty())
    {
        // Nothing to play on without a network; the page's save restarts the
        // board into the configured one.
        ESP_LOGW(kTag, "%s", forced ? "setup button held" : "no Wi-Fi configured");
        startSetupAccessPoint();
        return;
    }

    ESP_LOGI(kTag, "joining Wi-Fi \"%s\"", wifi.ssid.c_str());
    if (!waitForNetwork(kConnectTimeout))
    {
        ESP_LOGW(kTag, "cannot join \"%s\"; still trying, with the setup page up",
                 wifi.ssid.c_str());
        startSetupAccessPoint();
        waitForNetwork(portMAX_DELAY);
        stopSetupAccessPoint();
    }

    // A fresh scan on every start: the formats follow from i2sClockMode and
    // i2sSlaveSampleRate in client.json, and a cache written under the other
    // mode would offer the server rates the bus cannot run at. The scan costs
    // one I2C probe.
    std::string configOption = std::string("--config=") + web_ui::kClientConfigPath;
    char program[] = MULTIROOM_BRAND_SLUG "-client";
    char cacheOption[] = "--audio-cache=/data/audio_devices.json";
    char rescanOption[] = "--rescan-audio";
    char* argv[] = {program, &configOption[0], cacheOption, rescanOption, nullptr};
    // libstdc++'s std::mutex goes through pthread_self(), which asserts on a
    // task that pthread_create() did not make -- and the main task is one.
    // The client therefore runs on its own pthread, with the stack size the
    // main task would have given it.
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, CONFIG_ESP_MAIN_TASK_STACK_SIZE);
    const auto run = [](void* argument) -> void* {
        const int exitCode = clientMain(4, static_cast<char**>(argument));
        ESP_LOGE(kTag, "client exited with %d", exitCode);
        return nullptr;
    };
    // Named, so the `diagnostics` task report can tell it from the others.
    // Pinned to core 1, away from Wi-Fi and lwIP on core 0: the I2S driver
    // allocates its DMA interrupt on the core that opens the channel, which is
    // this thread, and unpinned it would land wherever the thread first used
    // the FPU (IDF then pins it there), a different core from boot to boot.
    esp_pthread_cfg_t named = esp_pthread_get_default_config();
    named.thread_name = "client_main";
    named.pin_to_core = 1;
    esp_pthread_set_cfg(&named);
    pthread_t thread = 0;
    const int created = pthread_create(&thread, &attributes, run, argv);
    const esp_pthread_cfg_t defaults = esp_pthread_get_default_config();
    esp_pthread_set_cfg(&defaults);
    if (created != 0)
    {
        ESP_LOGE(kTag, "cannot start the client's thread");
        return;
    }
    pthread_join(thread, nullptr);
}
