#include "CaptiveDns.h"

#include "esp_log.h"
#include "lwip/sockets.h"

#include <atomic>
#include <cstring>
#include <thread>

namespace captive_dns {
namespace {

const char* const kTag = "captive_dns";
const uint16_t kDnsPort = 53;
const size_t kHeaderBytes = 12;
const uint16_t kTypeA = 1;

std::atomic<bool> g_running(false);
std::thread g_thread;
int g_socket = -1;

uint16_t readU16(const uint8_t* p)
{
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

void writeU16(uint8_t* p, uint16_t value)
{
    p[0] = static_cast<uint8_t>(value >> 8);
    p[1] = static_cast<uint8_t>(value);
}

// Answers the first question of a query in place. An A question gets the
// board's address; anything else (AAAA above all) gets an empty answer, so the
// client falls back to IPv4 instead of waiting for a timeout. Returns the
// reply length, or 0 for a packet that is not a usable query.
size_t answer(uint8_t* packet, size_t length, size_t capacity, uint32_t address)
{
    if (length < kHeaderBytes || (packet[2] & 0x80) != 0 || readU16(packet + 4) == 0)
        return 0;

    // Walk the question's name to find its type, class and end.
    size_t offset = kHeaderBytes;
    while (offset < length && packet[offset] != 0)
    {
        if ((packet[offset] & 0xC0) != 0)
            return 0; // no compression in a question we can trust
        offset += static_cast<size_t>(packet[offset]) + 1;
    }
    offset += 1; // the terminating zero
    if (offset + 4 > length)
        return 0;
    const uint16_t type = readU16(packet + offset);
    offset += 4; // type and class
    const bool answerIt = type == kTypeA;
    const size_t answerBytes = 16;
    if (answerIt && offset + answerBytes > capacity)
        return 0;

    packet[2] = static_cast<uint8_t>(0x80 | (packet[2] & 0x01)); // response, keep RD
    packet[3] = 0x80;                                             // RA, NOERROR
    writeU16(packet + 4, 1);                                      // one question
    writeU16(packet + 6, answerIt ? 1 : 0);
    writeU16(packet + 8, 0);
    writeU16(packet + 10, 0);
    if (!answerIt)
        return offset;

    uint8_t* record = packet + offset;
    writeU16(record, 0xC000 | kHeaderBytes); // name: pointer to the question
    writeU16(record + 2, kTypeA);
    writeU16(record + 4, 1);                 // class IN
    writeU16(record + 6, 0);
    writeU16(record + 8, 60);                // TTL 60 s
    writeU16(record + 10, 4);
    std::memcpy(record + 12, &address, 4);   // already network order
    return offset + answerBytes;
}

void serve(uint32_t address)
{
    uint8_t packet[512];
    while (g_running.load())
    {
        sockaddr_in from{};
        socklen_t fromLength = sizeof(from);
        const int received = recvfrom(g_socket, packet, sizeof(packet), 0,
                                      reinterpret_cast<sockaddr*>(&from), &fromLength);
        if (received <= 0)
            continue; // the receive timeout: look at g_running again
        const size_t reply =
            answer(packet, static_cast<size_t>(received), sizeof(packet), address);
        if (reply > 0)
            sendto(g_socket, packet, reply, 0, reinterpret_cast<sockaddr*>(&from), fromLength);
    }
}

} // namespace

bool start(uint32_t address)
{
    if (g_running.load())
        return true;

    g_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_socket < 0)
    {
        ESP_LOGE(kTag, "cannot create the socket");
        return false;
    }
    sockaddr_in bindAddress{};
    bindAddress.sin_family = AF_INET;
    bindAddress.sin_port = htons(kDnsPort);
    bindAddress.sin_addr.s_addr = htonl(INADDR_ANY);
    // Bounded, so stop() is noticed without a packet arriving.
    timeval timeout{};
    timeout.tv_sec = 1;
    setsockopt(g_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    if (bind(g_socket, reinterpret_cast<sockaddr*>(&bindAddress), sizeof(bindAddress)) != 0)
    {
        ESP_LOGE(kTag, "cannot bind port %u", static_cast<unsigned>(kDnsPort));
        close(g_socket);
        g_socket = -1;
        return false;
    }

    g_running.store(true);
    g_thread = std::thread(serve, address);
    return true;
}

void stop()
{
    if (!g_running.exchange(false))
        return;
    if (g_thread.joinable())
        g_thread.join();
    close(g_socket);
    g_socket = -1;
}

} // namespace captive_dns
