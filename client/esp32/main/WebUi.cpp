#include "WebUi.h"

#include "WifiSettings.h"

#include "audio/esp32/I2sBackend.h"
#include "app/InputSettings.h"
#include "config/ClientConfig.h"
#include "control/ControlHttp.h"
#include "discovery/NodeId.h"
#include "system/TextUtil.h"
#include "util/FileUtil.h"
#include "util/Json.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>
#include <utility>

using namespace multiroom;

namespace web_ui {

const char* const kClientConfigPath = "/data/client.json";

namespace {

const char* const kTag = "web_ui";
// The external DAC's formats are the largest part: every rate in both layouts
// is about 1.3 kB of JSON.
const size_t kMaxBodyBytes = 4096;
// A body that size arrives in well under a second; this is a stalled sender.
const int64_t kBodyDeadlineUs = 10000000;
const uint16_t kMaxScanResults = 20;
const uint64_t kRestartDelayUs = 750000; // lets the response reach the browser
// The client's status line is a few kilobytes; far above that is not one.
const size_t kMaxStatusBytes = 32 * 1024;
const int kControlTimeoutSeconds = 3;

std::atomic<bool> g_setupMode(false);
// The running control listener keeps this endpoint until the next restart.
std::string g_controlAddress;
uint16_t g_controlPort = 0;

esp_err_t sendJson(httpd_req_t* request, const char* status, const json::Value& body)
{
    httpd_resp_set_status(request, status);
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    const std::string text = json::serialize(body);
    return httpd_resp_send(request, text.data(), static_cast<ssize_t>(text.size()));
}

esp_err_t sendError(httpd_req_t* request, const char* status, const std::string& message)
{
    json::Value body = json::Value::makeObject();
    body["error"] = json::Value(message);
    return sendJson(request, status, body);
}

// client.json as the client would read it. A missing file is the defaults; an
// invalid one is the defaults too, because the client refuses to start on it
// and saving from this page is then the repair.
ClientConfig loadClientConfig()
{
    ClientConfig config;
    std::string error;
    const client_config::LoadResult result =
        client_config::load(kClientConfigPath, config, error);
    if (result == client_config::LoadResult::Invalid)
    {
        ESP_LOGW(kTag, "%s is invalid (%s); the page starts from the defaults",
                 kClientConfigPath, error.c_str());
        config = ClientConfig();
    }
    return config;
}

// Rate and sample type, as client.json spells them; two channels always.
// Empty when the request has no such header.
std::string header(httpd_req_t* request, const char* name)
{
    const size_t length = httpd_req_get_hdr_value_len(request, name);
    if (length == 0)
        return std::string();
    std::string value(length + 1, '\0');
    if (httpd_req_get_hdr_value_str(request, name, &value[0], value.size()) != ESP_OK)
        return std::string();
    value.resize(length);
    return value;
}

// The address the page is on: the access point's in setup mode, else the
// station's. Empty while the interface has none.
std::string ownAddress()
{
    esp_netif_t* netif =
        esp_netif_get_handle_from_ifkey(g_setupMode.load() ? "WIFI_AP_DEF" : "WIFI_STA_DEF");
    esp_netif_ip_info_t info = {};
    if (netif == nullptr || esp_netif_get_ip_info(netif, &info) != ESP_OK || info.ip.addr == 0)
        return std::string();
    char text[16];
    std::snprintf(text, sizeof(text), IPSTR, IP2STR(&info.ip));
    return text;
}

// To the page by the board's address. Absolute, so a captive portal probe
// lands on that address and not on the probe's own name, which the API
// refuses (isTrustedHost).
esp_err_t redirectToPage(httpd_req_t* request)
{
    const std::string address = ownAddress();
    const std::string location = address.empty() ? "/" : "http://" + address + "/";
    httpd_resp_set_status(request, "302 Found");
    httpd_resp_set_hdr(request, "Location", location.c_str());
    return httpd_resp_send(request, nullptr, 0);
}

// Every /api request needs the board's own Host; see isTrustedHost: a DNS rebinding
// attack brings a foreign name, and so does a captive portal probe.
bool refuseForeignHost(httpd_req_t* request, esp_err_t& result)
{
    if (isTrustedHost(header(request, "Host")))
        return false;
    result = sendError(request, "403 Forbidden", "open this page by the board's address");
    return true;
}

// One control request to the running client, over its own control port: the
// protocol a server uses, so this page needs nothing of the client's
// internals. False with `error` set when there is no answer, as in setup
// mode, where the client is not started.
bool askClient(const char* requestLine, std::string& answer, std::string& error)
{
    std::string address = g_controlAddress;
    if (address.empty() || address == "0.0.0.0")
        address = "127.0.0.1";
    sockaddr_in target = {};
    target.sin_family = AF_INET;
    target.sin_port = htons(g_controlPort);
    if (inet_pton(AF_INET, address.c_str(), &target.sin_addr) != 1)
    {
        error = "the control API listens on " + address + ", which this page cannot reach";
        return false;
    }

    const int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0)
    {
        error = "no socket left to reach the client";
        return false;
    }
    timeval timeout = {};
    timeout.tv_sec = kControlTimeoutSeconds;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    bool ok = connect(sock, reinterpret_cast<sockaddr*>(&target), sizeof(target)) == 0;
    if (!ok)
        error = "not running";
    const std::string line = std::string(requestLine) + "\n";
    if (ok && send(sock, line.data(), line.size(), 0) != static_cast<ssize_t>(line.size()))
    {
        ok = false;
        error = "cannot send to the client";
    }
    answer.clear();
    char buffer[512];
    while (ok && answer.find('\n') == std::string::npos)
    {
        const int received = recv(sock, buffer, sizeof(buffer), 0);
        if (received <= 0 || answer.size() > kMaxStatusBytes)
        {
            ok = false;
            error = "no answer from the client";
            break;
        }
        answer.append(buffer, static_cast<size_t>(received));
    }
    close(sock);
    if (ok)
        answer.resize(answer.find('\n'));
    return ok;
}

esp_err_t handlePage(httpd_req_t* request)
{
    // A page opened by a foreign name (a captive portal, a typed site in setup
    // mode) could not use the API; move it to the address first.
    if (!isTrustedHost(header(request, "Host")) && !ownAddress().empty())
        return redirectToPage(request);
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, kClientSettingsPage, static_cast<ssize_t>(kClientSettingsPageBytes));
}

esp_err_t handleGetSettings(httpd_req_t* request)
{
    esp_err_t refused = ESP_OK;
    if (refuseForeignHost(request, refused))
        return refused;
    WifiSettings wifi;
    std::string error;
    if (!wifi_settings::load(wifi, error))
        ESP_LOGW(kTag, "%s: %s", wifi_settings::kPath, error.c_str());
    const ClientConfig config = loadClientConfig();

    json::Value body = json::Value::makeObject();
    body["setupMode"] = json::Value(g_setupMode.load());
    body["inputEventsPort"] = json::Value(static_cast<int32_t>(g_setupMode.load() ? 0 : g_controlPort));
    body["board"] = json::Value(MULTIROOM_BOARD); // the build's boards/ file
    body["ssid"] = json::Value(wifi.ssid);
    // Never the password itself: the page is reachable from the whole LAN.
    body["passwordSet"] = json::Value(!wifi.password.empty());
    body["deviceName"] = json::Value(wifi.deviceName);
    body["bssid"] = json::Value(wifi.bssid);
    // The access point the board is on now, so the page can mark it.
    wifi_ap_record_t current = {};
    if (esp_wifi_sta_get_ap_info(&current) == ESP_OK)
        body["connectedBssid"] = json::Value(text_util::formatMac(current.bssid));
    body["defaultDeviceName"] = json::Value(discovery::hostName());
    body["ipMode"] = json::Value(wifi.staticIp ? "static" : "dhcp");
    body["ip"] = json::Value(wifi.ip);
    body["netmask"] = json::Value(wifi.netmask);
    body["gateway"] = json::Value(wifi.gateway);
    body["dns"] = json::Value(wifi.dns);
    // The address in use now, whichever way it came.
    esp_netif_ip_info_t now = {};
    esp_netif_t* station = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (station != nullptr && esp_netif_get_ip_info(station, &now) == ESP_OK && now.ip.addr != 0)
    {
        char text[16];
        std::snprintf(text, sizeof(text), IPSTR, IP2STR(&now.ip));
        body["currentIp"] = json::Value(text);
    }
    body["name"] = json::Value(config.name);
    body["clientId"] = json::Value(config.clientId);
    body["server"] = json::Value(config.server);
    body["audioDeviceId"] = json::Value(config.audioDeviceId);
    body["i2sClockMode"] = json::Value(i2sClockModeName(config.i2s.clockMode));
    body["i2sSlaveSampleRate"] = json::Value(config.i2s.slaveSampleRate);
    body["bufferMs"] = json::Value(config.bufferMs);
    body["i2sExternalFormats"] = client_config::i2sFormatsToJson(config.i2s.externalFormats);
    json::Value rates = json::Value::makeArray();
    for (uint32_t rate : i2sSampleRates())
        rates.pushBack(json::Value(rate));
    body["i2sRates"] = rates;
    // As I2S master; the page says what slave mode leaves of it.
    I2sSettings master = config.i2s;
    master.clockMode = I2sClockMode::Master;
    body["es8388Formats"] = client_config::i2sFormatsToJson(es8388Formats(master));
    return sendJson(request, "200 OK", body);
}

void restartSoon()
{
    esp_timer_create_args_t args = {};
    args.callback = [](void*) { esp_restart(); };
    args.name = "restart";
    esp_timer_handle_t timer = nullptr;
    if (esp_timer_create(&args, &timer) != ESP_OK || esp_timer_start_once(timer, kRestartDelayUs) != ESP_OK)
        esp_restart();
}

bool readJsonBody(httpd_req_t* request, json::Value& root, esp_err_t& result)
{
    esp_err_t refused = ESP_OK;
    if (refuseForeignHost(request, refused))
    {
        result = refused;
        return false;
    }
    // A cross-site request with this content type needs a preflight, and this
    // server answers none; without it any web page could post a form here.
    if (text_util::toLower(header(request, "Content-Type")).compare(0, 16, "application/json") != 0)
    {
        result = sendError(request, "415 Unsupported Media Type", "the body must be application/json");
        return false;
    }
    if (request->content_len == 0 || request->content_len > kMaxBodyBytes)
    {
        result = sendError(request, "400 Bad Request", "the request body is missing or too large");
        return false;
    }

    // A whole-body deadline: the server runs one handler at a time, and a
    // client that sends its headers and withholds the body would otherwise
    // keep every other page waiting for as long as it likes -- or trickle it a
    // byte at a time, so every read succeeds. Checked after every read,
    // whatever it did: one receive timeout (recv_wait_timeout) past it at
    // most, and a body completed late is refused like one that never came.
    const int64_t deadlineUs = esp_timer_get_time() + kBodyDeadlineUs;
    std::string text(request->content_len, '\0');
    size_t received = 0;
    while (received < text.size())
    {
        const int chunk = httpd_req_recv(request, &text[received], text.size() - received);
        if (chunk <= 0 && chunk != HTTPD_SOCK_ERR_TIMEOUT)
        {
            result = ESP_FAIL;
            return false;
        }
        if (chunk > 0)
            received += static_cast<size_t>(chunk);
        if (esp_timer_get_time() >= deadlineUs)
        {
            result = sendError(request, "408 Request Timeout", "the request body did not arrive");
            return false;
        }
    }

    std::string error;
    if (!json::parse(text, root, error) || !root.isObject())
    {
        result = sendError(request, "400 Bad Request", "the body is not a JSON object");
        return false;
    }

    return true;
}


esp_err_t handlePostSettings(httpd_req_t* request)
{
    json::Value root;
    esp_err_t readResult = ESP_OK;
    if (!readJsonBody(request, root, readResult))
        return readResult;
    std::string error;

    // Load, change and save as one: the running client saves client.json from
    // a snapshot of its own (a rename, a buffer change), and one of those
    // landing in between would be lost, or would undo this.
    const auto saveLock = client_config::lockSaves();

    WifiSettings wifi;
    if (!wifi_settings::load(wifi, error))
        wifi = WifiSettings(); // replaced below; a broken file is not worth keeping
    ClientConfig config = loadClientConfig();

    std::string ssid;
    std::string password;
    std::string clockMode;
    int64_t slaveRate = 0;
    if (!json::getString(root, "ssid", ssid) || ssid.empty() || ssid.size() > 32)
        return sendError(request, "400 Bad Request", "the network name must be 1 to 32 bytes");
    // Absent keeps the stored password, which the page never sees.
    if (root.find("password") != nullptr)
    {
        if (!json::getString(root, "password", password) ||
            (!password.empty() && (password.size() < 8 || password.size() > 63)))
            return sendError(request, "400 Bad Request",
                             "a WPA password is 8 to 63 characters (empty for an open network)");
        wifi.password = password;
    }
    wifi.ssid = ssid;

    // An empty device name keeps the esp32-<mac> default.
    std::string deviceName;
    if (root.find("deviceName") != nullptr)
    {
        if (!json::getString(root, "deviceName", deviceName))
            return sendError(request, "400 Bad Request", "deviceName must be a string");
        if (!deviceName.empty() && !text_util::isValidHostName(deviceName))
            return sendError(request, "400 Bad Request",
                             "the device name is 1 to 31 letters, digits and hyphens");
        wifi.deviceName = deviceName;
    }
    // Empty is automatic: the strongest access point with this name.
    if (root.find("bssid") != nullptr)
    {
        std::string bssid;
        uint8_t parsed[6];
        if (!json::getString(root, "bssid", bssid) ||
            (!bssid.empty() && !text_util::parseMac(bssid, parsed)))
            return sendError(request, "400 Bad Request",
                             "the access point must be empty or aa:bb:cc:dd:ee:ff");
        wifi.bssid = bssid.empty() ? std::string() : text_util::formatMac(parsed);
    }
    // DHCP, or a static address; the addresses are kept either way, so
    // switching back to static finds them again.
    if (root.find("ipMode") != nullptr)
    {
        std::string ipMode;
        if (!json::getString(root, "ipMode", ipMode) || (ipMode != "dhcp" && ipMode != "static"))
            return sendError(request, "400 Bad Request", "ipMode must be dhcp or static");
        wifi.staticIp = ipMode == "static";
    }
    if ((root.find("ip") != nullptr && !json::getString(root, "ip", wifi.ip)) ||
        (root.find("netmask") != nullptr && !json::getString(root, "netmask", wifi.netmask)) ||
        (root.find("gateway") != nullptr && !json::getString(root, "gateway", wifi.gateway)) ||
        (root.find("dns") != nullptr && !json::getString(root, "dns", wifi.dns)))
        return sendError(request, "400 Bad Request", "ip, netmask, gateway and dns must be strings");
    if (wifi.staticIp && !wifi_settings::validateStaticIp(wifi, error))
        return sendError(request, "400 Bad Request", error);

    // The room's name (MATP section 60) and its identity; validate() checks
    // both like any hand-edited file. Empty keeps what is stored: an empty
    // name would read as a file from before the name, and an empty id as a
    // fresh board, and either gives the room a new id on the next start.
    std::string name;
    std::string clientId;
    if ((root.find("name") != nullptr && !json::getString(root, "name", name)) ||
        (root.find("clientId") != nullptr && !json::getString(root, "clientId", clientId)))
        return sendError(request, "400 Bad Request", "name and clientId must be strings");
    if (!name.empty())
        config.name = name;
    if (!clientId.empty())
        config.clientId = clientId;

    if ((root.find("server") != nullptr && !json::getString(root, "server", config.server)) ||
        (root.find("audioDeviceId") != nullptr &&
         !json::getString(root, "audioDeviceId", config.audioDeviceId)))
        return sendError(request, "400 Bad Request", "server and audioDeviceId must be strings");
    if (root.find("i2sClockMode") != nullptr &&
        (!json::getString(root, "i2sClockMode", clockMode) ||
         !parseI2sClockMode(clockMode, config.i2s.clockMode)))
        return sendError(request, "400 Bad Request", "i2sClockMode must be master or slave");
    if (root.find("i2sSlaveSampleRate") != nullptr)
    {
        if (!json::getInt(root, "i2sSlaveSampleRate", slaveRate) || slaveRate <= 0 ||
            slaveRate > 384000)
            return sendError(request, "400 Bad Request", "i2sSlaveSampleRate is out of range");
        config.i2s.slaveSampleRate = static_cast<uint32_t>(slaveRate);
    }
    if (root.find("bufferMs") != nullptr)
    {
        int64_t bufferMs = 0;
        if (!json::getInt(root, "bufferMs", bufferMs) || bufferMs <= 0 || bufferMs > UINT32_MAX)
            return sendError(request, "400 Bad Request", "bufferMs must be a number");
        config.bufferMs = static_cast<uint32_t>(bufferMs);
    }
    // Range, layouts and duplicates are validate()'s, below.
    if (const json::Value* formats = root.find("i2sExternalFormats"))
    {
        if (!client_config::i2sFormatsFromJson(*formats, config.i2s.externalFormats, error))
            return sendError(request, "400 Bad Request", error);
    }

    // Everything is checked before anything is written, so a refused request
    // leaves both files as they were.
    if (!client_config::validate(config, error))
        return sendError(request, "400 Bad Request", error);

    // The two files are one change: a client.json that cannot be written
    // puts wifi.json back as it was, so a later restart does not join a
    // network whose save was reported as failed.
    std::string previousWifi;
    std::string readError;
    const bool hadWifi = fileutil::exists(wifi_settings::kPath);
    if (hadWifi && !fileutil::readTextFile(wifi_settings::kPath, previousWifi, readError))
        return sendError(request, "500 Internal Server Error", "cannot read wifi.json: " + readError);
    if (!wifi_settings::save(wifi, error))
        return sendError(request, "500 Internal Server Error", "cannot write wifi.json: " + error);
    if (!client_config::save(kClientConfigPath, config, error))
    {
        std::string restoreError;
        const bool restored =
            hadWifi ? fileutil::writeTextFileAtomic(wifi_settings::kPath, previousWifi, restoreError)
                    : fileutil::removeFile(wifi_settings::kPath);
        if (!restored)
        {
            // Said so, rather than reported as a failure that changed nothing.
            ESP_LOGE(kTag, "cannot write client.json (%s) nor restore wifi.json (%s)", error.c_str(),
                     restoreError.c_str());
            return sendError(request, "500 Internal Server Error",
                             "cannot write client.json: " + error +
                                 "; wifi.json keeps the new network and could not be restored");
        }
        return sendError(request, "500 Internal Server Error", "cannot write client.json: " + error);
    }
    client_config::sealSaves();

    ESP_LOGI(kTag, "settings saved; restarting");
    json::Value body = json::Value::makeObject();
    body["ok"] = json::Value(true);
    const esp_err_t result = sendJson(request, "200 OK", body);
    restartSoon();
    return result;
}

// The client's own status response, passed through unchanged.
esp_err_t handleStatus(httpd_req_t* request)
{
    esp_err_t refused = ESP_OK;
    if (refuseForeignHost(request, refused))
        return refused;
    std::string answer;
    std::string error;
    if (!askClient("{\"command\":\"status\"}", answer, error))
        return sendError(request, "503 Service Unavailable", error);
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, answer.data(), static_cast<ssize_t>(answer.size()));
}

esp_err_t handleInputs(httpd_req_t* request)
{
    esp_err_t refused = ESP_OK;
    if (refuseForeignHost(request, refused))
        return refused;
    json::Value body;
    std::string error;
    if (g_setupMode.load())
    {
        // The client does not run in setup mode; show the saved wiring instead.
        json::Value config;
        if (!json::parse(client_config::serialize(loadClientConfig()), config, error))
            return sendError(request, "500 Internal Server Error", error);
        body = json::Value::makeObject();
        body["ok"] = json::Value(true);
        body["inputs"] = *config.find("inputs");
        body["backend"] = json::Value("not running (setup mode)");
    }
    else
    {
        std::string answer;
        if (!askClient("{\"command\":\"inputs\"}", answer, error))
            return sendError(request, "503 Service Unavailable", error);
        if (!json::parse(answer, body, error))
            return sendError(request, "502 Bad Gateway", error);
    }
    body["setupButtonPin"] = json::Value(static_cast<int32_t>(CONFIG_MULTIROOM_SETUP_BUTTON_GPIO));
    return sendJson(request, "200 OK", body);
}

esp_err_t handlePostInputs(httpd_req_t* request)
{
    json::Value params;
    esp_err_t result = ESP_OK;
    if (!readJsonBody(request, params, result))
        return result;
    std::string error;
    if (!g_setupMode.load())
    {
        json::Value command = json::Value::makeObject();
        command["command"] = json::Value("setInputBindings");
        command["params"] = std::move(params);
        std::string answer;
        if (!askClient(json::serialize(command).c_str(), answer, error))
            return sendError(request, "503 Service Unavailable", error);
        json::Value body;
        if (!json::parse(answer, body, error))
            return sendError(request, "502 Bad Gateway", error);
        return sendJson(request, "200 OK", body);
    }

    // Setup mode has no running client. Use its same validated persistence path.
    std::string id;
    const json::Value* bindings = params.find("bindings");
    if (!json::getString(params, "id", id) || bindings == nullptr)
        return sendError(request, "400 Bad Request", "params needs id (a string) and bindings (an object)");
    ClientConfig config = loadClientConfig();
    // Setup-only state must not consume the normal HTTP request stack.
    std::unique_ptr<ClientRuntime> runtime(new ClientRuntime);
    runtime->config = &config;
    runtime->configPath = kClientConfigPath;
    const InputBindingChange change = changeInputBindings(*runtime, id, *bindings, error);
    if (change == InputBindingChange::UnknownInput || change == InputBindingChange::Invalid)
        return sendError(request, "400 Bad Request", error);
    if (change == InputBindingChange::NotSaved)
        return sendError(request, "500 Internal Server Error", error);
    for (const InputDefinition& input : config.inputs)
    {
        if (input.control.id != id) continue;
        json::Value body = json::Value::makeObject();
        body["ok"] = json::Value(true);
        body["id"] = json::Value(id);
        body["changed"] = json::Value(change == InputBindingChange::Changed);
        body["bindings"] = matp::inputBindingsToJson(input.control.kind, input.control.bindings);
        return sendJson(request, "200 OK", body);
    }
    return sendError(request, "500 Internal Server Error", "the input disappeared");
}

esp_err_t handleNetworks(httpd_req_t* request)
{
    esp_err_t refused = ESP_OK;
    if (refuseForeignHost(request, refused))
        return refused;
    // Blocking, a few seconds. In normal operation it takes the radio off the
    // channel for that long, which a playing room hears as an underrun.
    esp_err_t result = esp_wifi_scan_start(nullptr, true);
    if (result != ESP_OK)
        return sendError(request, "503 Service Unavailable",
                         std::string("scan failed: ") + esp_err_to_name(result));

    uint16_t count = kMaxScanResults;
    std::vector<wifi_ap_record_t> records(count);
    result = esp_wifi_scan_get_ap_records(&count, records.data());
    if (result != ESP_OK)
        return sendError(request, "503 Service Unavailable",
                         std::string("scan failed: ") + esp_err_to_name(result));
    records.resize(count);
    std::sort(records.begin(), records.end(),
              [](const wifi_ap_record_t& a, const wifi_ap_record_t& b) { return a.rssi > b.rssi; });

    // Every access point, strongest first: the page lists each network once
    // and offers the access points of the one picked.
    json::Value list = json::Value::makeArray();
    for (const wifi_ap_record_t& record : records)
    {
        const std::string ssid(reinterpret_cast<const char*>(record.ssid));
        // Hidden networks have no name to pick.
        if (ssid.empty())
            continue;
        json::Value entry = json::Value::makeObject();
        entry["ssid"] = json::Value(ssid);
        entry["bssid"] = json::Value(text_util::formatMac(record.bssid));
        entry["rssi"] = json::Value(static_cast<int64_t>(record.rssi));
        entry["secure"] = json::Value(record.authmode != WIFI_AUTH_OPEN);
        // Channels above 14 are 5 GHz; the page shows the band (an ESP32 sees
        // 2.4 GHz only, later chips see both).
        entry["channel"] = json::Value(static_cast<int64_t>(record.primary));
        list.pushBack(entry);
    }
    return sendJson(request, "200 OK", list);
}

// Every unknown path goes to the page. That is the captive portal's other half:
// a phone's connectivity check (/generate_204, /hotspot-detect.html, ...) gets
// a redirect instead of the answer it expects and shows the page.
esp_err_t handleNotFound(httpd_req_t* request, httpd_err_code_t)
{
    return redirectToPage(request);
}

} // namespace

void setSetupMode(bool active)
{
    g_setupMode.store(active);
}

bool start()
{
    const ClientConfig clientSettings = loadClientConfig();
    g_controlAddress = clientSettings.controlBindAddress;
    g_controlPort = clientSettings.controlPort;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192; // JSON and std::string on the handler's stack
    config.lru_purge_enable = true;
    // One browser needs two or three; the rest of the socket budget belongs
    // to the client (sdkconfig.defaults, CONFIG_LWIP_MAX_SOCKETS).
    config.max_open_sockets = 3;

    httpd_handle_t server = nullptr;
    const esp_err_t result = httpd_start(&server, &config);
    if (result != ESP_OK)
    {
        ESP_LOGE(kTag, "cannot start the web server: %s", esp_err_to_name(result));
        return false;
    }

    const struct
    {
        const char* uri;
        httpd_method_t method;
        esp_err_t (*handler)(httpd_req_t*);
    } routes[] = {
        {"/", HTTP_GET, handlePage},
        {"/api/settings", HTTP_GET, handleGetSettings},
        {"/api/settings", HTTP_POST, handlePostSettings},
        {"/api/networks", HTTP_GET, handleNetworks},
        {"/api/status", HTTP_GET, handleStatus},
        {"/api/inputs", HTTP_GET, handleInputs},
        {"/api/inputs", HTTP_POST, handlePostInputs},
    };
    for (const auto& route : routes)
    {
        httpd_uri_t uri = {};
        uri.uri = route.uri;
        uri.method = route.method;
        uri.handler = route.handler;
        httpd_register_uri_handler(server, &uri);
    }
    httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, handleNotFound);
    return true;
}

} // namespace web_ui
