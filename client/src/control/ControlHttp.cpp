#include "control/ControlHttp.h"

#include "discovery/NodeId.h"
#include "system/TextUtil.h"

#include <cstdlib>
#if defined(ESP_PLATFORM)
#include "esp_netif.h"
#endif

namespace multiroom {
namespace {

// Far above what a browser sends; a request beyond either is not one of ours.
// Header and body together stay under the control server's 64 KiB line cap,
// so that cap never answers an HTTP request with a JSON line.
const size_t kMaxHeaderBytes = 8 * 1024;
const size_t kMaxBodyBytes = 32 * 1024;

std::string trim(const std::string& text)
{
    const size_t first = text.find_first_not_of(" \t");
    if (first == std::string::npos)
        return std::string();
    return text.substr(first, text.find_last_not_of(" \t") + 1 - first);
}

std::string makeResponse(const char* status, const char* contentType, const std::string& body)
{
    std::string response = "HTTP/1.1 ";
    response += status;
    response += "\r\nContent-Type: ";
    response += contentType;
    response += "\r\nContent-Length: " + std::to_string(body.size());
    response += "\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff"
                "\r\nConnection: close\r\n\r\n";
    return response + body;
}

std::string textResponse(const char* status, const std::string& message)
{
    return makeResponse(status, "text/plain; charset=utf-8", message);
}

} // namespace

const char kClientSettingsPage[] =
#include "MultiroomClientSettings.inc"
    ;
const size_t kClientSettingsPageBytes = sizeof(kClientSettingsPage) - 1;

bool isTrustedHost(const std::string& hostHeader)
{
    std::string host = text_util::toLower(hostHeader);
    if (!host.empty() && host[0] == '[')
        return true; // an IPv6 literal; a name cannot start with '['
    host = host.substr(0, host.find(':'));
    if (!host.empty() && host.back() == '.')
        host.pop_back(); // the fully qualified spelling of the same name
    if (host.empty())
        return false;
    if (host.find_first_not_of("0123456789.") == std::string::npos || host == "localhost")
        return true;
    const auto isOwn = [&host](const std::string& own) {
        return !own.empty() && (host == own || host == own + ".local");
    };
#if defined(ESP_PLATFORM)
    const char* stationName = nullptr;
    esp_netif_t* station = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (station != nullptr && esp_netif_get_hostname(station, &stationName) == ESP_OK &&
        stationName != nullptr && isOwn(text_util::toLower(stationName)))
        return true;
#endif
    return isOwn(text_util::toLower(discovery::hostName()));
}

bool looksLikeHttpRequest(const std::string& inbox)
{
    for (const char* method : {"GET ", "POST ", "HEAD ", "OPTIONS "})
        if (inbox.compare(0, std::char_traits<char>::length(method), method) == 0)
            return true;
    return false;
}

bool serveHttpRequest(std::string& inbox, IControlHandler& handler, std::string& response,
                      bool* inputEvents)
{
    if (inputEvents != nullptr)
        *inputEvents = false;
    const size_t headerEnd = inbox.find("\r\n\r\n");
    if (headerEnd == std::string::npos)
    {
        if (inbox.size() <= kMaxHeaderBytes)
            return false;
        inbox.clear();
        response = textResponse("431 Request Header Fields Too Large", "headers too large");
        return true;
    }

    // "METHOD target HTTP/1.1", then one "Name: value" per line.
    const size_t lineEnd = inbox.find("\r\n");
    const std::string requestLine = inbox.substr(0, lineEnd);
    const size_t space = requestLine.find(' ');
    const std::string method = requestLine.substr(0, space);
    std::string target = requestLine.substr(space + 1, requestLine.find(' ', space + 1) - space - 1);
    target = target.substr(0, target.find('?'));

    std::string host;
    std::string contentType;
    std::string origin;
    size_t contentLength = 0;
    for (size_t start = lineEnd + 2; start < headerEnd;)
    {
        const size_t end = inbox.find("\r\n", start);
        const std::string line = inbox.substr(start, end - start);
        start = end + 2;
        const size_t colon = line.find(':');
        if (colon == std::string::npos)
            continue;
        const std::string name = text_util::toLower(trim(line.substr(0, colon)));
        const std::string value = trim(line.substr(colon + 1));
        if (name == "host")
            host = value;
        else if (name == "content-type")
            contentType = text_util::toLower(value);
        else if (name == "origin")
            origin = value;
        else if (name == "content-length")
        {
            char* rest = nullptr;
            const unsigned long long parsed = std::strtoull(value.c_str(), &rest, 10);
            if (value.empty() || *rest != '\0' || parsed > kMaxBodyBytes)
            {
                inbox.clear();
                response = textResponse("413 Payload Too Large", "invalid or too large body");
                return true;
            }
            contentLength = static_cast<size_t>(parsed);
        }
    }

    const size_t bodyStart = headerEnd + 4;
    if (inbox.size() < bodyStart + contentLength)
        return false;
    const std::string body = inbox.substr(bodyStart, contentLength);
    inbox.clear(); // one request per connection; anything after it is dropped

    if (target == "/" || target == "/index.html")
    {
        if (method != "GET")
            response = textResponse("405 Method Not Allowed", "use GET");
#if defined(ESP_PLATFORM)
        // The board's settings page on port 80 is its one page, with this
        // one's status in it.
        else if (!host.empty())
        {
            const size_t bracket = host.find(']');
            const size_t end = host[0] != '[' ? host.find(':')
                               : bracket == std::string::npos ? bracket : bracket + 1;
            response = "HTTP/1.1 302 Found\r\nLocation: http://" + host.substr(0, end) +
                       "/\r\nContent-Length: 0\r\nCache-Control: no-store"
                       "\r\nConnection: close\r\n\r\n";
        }
#endif
        else
            response = makeResponse("200 OK", "text/html; charset=utf-8",
                                    std::string(kClientSettingsPage, kClientSettingsPageBytes));
        return true;
    }
    if (target == "/api/input-events")
    {
        // ESP serves its settings on port 80 and this feed on the control
        // port. Allow that same host across ports, never an unrelated site.
        const auto hostname = [](std::string address) {
            address = text_util::toLower(address);
            const size_t end = !address.empty() && address[0] == '['
                                   ? (address.find(']') == std::string::npos ? address.size() : address.find(']') + 1)
                                   : address.find(':');
            return address.substr(0, end);
        };
        std::string originHost;
        if (origin.compare(0, 7, "http://") == 0)
            originHost = origin.substr(7);
        else if (origin.compare(0, 8, "https://") == 0)
            originHost = origin.substr(8);
        if (method != "GET")
            response = textResponse("405 Method Not Allowed", "use GET");
        else if (!isTrustedHost(host) || (!origin.empty() &&
                 (originHost.empty() || originHost.find_first_of("/?#") != std::string::npos ||
                  hostname(originHost) != hostname(host))))
            response = textResponse("403 Forbidden", "open this page by the client's address");
        else
        {
            response = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                       "Cache-Control: no-store\r\nTransfer-Encoding: chunked\r\n"
                       "Connection: keep-alive\r\n";
            if (!origin.empty())
                response += "Access-Control-Allow-Origin: " + origin + "\r\nVary: Origin\r\n";
            response += "\r\n";
            if (inputEvents != nullptr)
                *inputEvents = true;
        }
        return true;
    }
    if (target != "/api")
    {
        response = textResponse("404 Not Found", "not found");
        return true;
    }
    if (method != "POST")
        response = textResponse("405 Method Not Allowed", "use POST");
    else if (contentType.compare(0, 16, "application/json") != 0)
        response = textResponse("415 Unsupported Media Type", "the body must be application/json");
    else if (!isTrustedHost(host))
        response = textResponse("403 Forbidden", "open this page by the client's address");
    else
        response = makeResponse("200 OK", "application/json",
                                dispatchControlLine(body, handler));
    return true;
}

} // namespace multiroom
