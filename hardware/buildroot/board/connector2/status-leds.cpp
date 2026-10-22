// Board-only status monitor. LED work never runs in the audio process/thread.
#include "util/Json.h"

#include <cstdint>
#include <string>
#include <utility>

namespace {
using multiroom::json::Value;
enum class State { Boot, Connecting, Setup, Ready, Error, Off };

State statusState(const Value& response)
{
    const auto* status = response.find("status");
    if (!status) return State::Error;
    if (const auto* power = status->find("power"))
    {
        std::string state;
        if (multiroom::json::getString(*power, "state", state) &&
            (state == "standby" || state == "standbyManual")) return State::Off;
    }
    const auto* server = status->find("server");
    const auto* session = status->find("session");
    const auto* audio = status->find("audio");
    std::string connection, handshake, playback;
    if (!server || !session || !audio ||
        !multiroom::json::getString(*server, "state", connection) ||
        !multiroom::json::getString(*session, "state", handshake)) return State::Error;
    if (connection == "notConfigured") return State::Setup;
    bool selected = false, playable = true;
    multiroom::json::getBool(*audio, "deviceSelected", selected);
    multiroom::json::getBool(*session, "formatPlayable", playable);
    multiroom::json::getString(*audio, "playbackState", playback);
    if (!selected || !playable || handshake == "failed" ||
        playback == "failed" || playback == "recovering")
        return State::Error;
    return connection == "connected" && handshake == "established"
               ? State::Ready : State::Connecting;
}

std::pair<bool, bool> brightness(State state, uint64_t elapsedMs)
{
    switch (state)
    {
    case State::Boot: return {elapsedMs % 1000 < 500, false};
    case State::Connecting: return {true, elapsedMs % 1000 < 500};
    case State::Setup:
        return {true, elapsedMs % 2000 < 200 ||
                      (elapsedMs % 2000 >= 400 && elapsedMs % 2000 < 600)};
    case State::Ready: return {true, true};
    case State::Error: return {true, elapsedMs % 400 < 200};
    case State::Off: return {false, false};
    }
    return {false, false};
}

#ifndef RAUMFELD_LEDS_TEST
} // namespace

#include <arpa/inet.h>
#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
volatile std::sig_atomic_t stopped = 0;
extern "C" void stop(int) { stopped = 1; }
using Clock = std::chrono::steady_clock;
uint64_t milliseconds()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now().time_since_epoch()).count());
}

bool readFile(const char* path, std::string& text)
{
    text.clear();
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    char buffer[1024];
    ssize_t count;
    while ((count = read(fd, buffer, sizeof(buffer))) > 0 && text.size() < 65536)
        text.append(buffer, static_cast<size_t>(count));
    close(fd);
    return count == 0 && text.size() <= 65536;
}

bool writeFile(const std::string& path, const char* text, size_t length)
{
    const int fd = open(path.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd < 0) return false;
    const bool ok = write(fd, text, length) == static_cast<ssize_t>(length);
    close(fd);
    return ok;
}

bool query(uint16_t port, const std::string& host, Value& response)
{
    const int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1) { close(fd); return false; }
    const uint64_t deadline = milliseconds() + 300;
    connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    pollfd event{fd, POLLOUT, 0};
    int error = 0;
    socklen_t length = sizeof(error);
    bool ok = poll(&event, 1, 300) > 0 &&
              getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) == 0 && error == 0;
    const char request[] = "{\"command\":\"status\"}\n";
    if (ok) ok = send(fd, request, sizeof(request) - 1, MSG_NOSIGNAL) ==
                 static_cast<ssize_t>(sizeof(request) - 1);
    std::string text;
    char buffer[1024];
    while (ok && text.size() < 65536 && milliseconds() < deadline)
    {
        event.events = POLLIN;
        const uint64_t current = milliseconds();
        if (current >= deadline || poll(&event, 1, static_cast<int>(deadline - current)) <= 0) break;
        const ssize_t count = recv(fd, buffer, sizeof(buffer), 0);
        if (count <= 0) break;
        text.append(buffer, static_cast<size_t>(count));
        const size_t newline = text.find('\n');
        if (newline != std::string::npos)
        {
            text.resize(newline);
            close(fd);
            std::string parseError;
            return multiroom::json::parse(text, response, parseError);
        }
    }
    close(fd);
    return false;
}
} // namespace

int main()
{
    std::string text, error;
    Value board;
    bool enabled = false;
    if (!readFile("/etc/galdrun-board.json", text) ||
        !multiroom::json::parse(text, board, error) ||
        !multiroom::json::getBool(board, "statusLeds", enabled) || !enabled) return 0;
    const std::string ledA = "/sys/class/leds/raumfeld:1/";
    const std::string ledB = "/sys/class/leds/raumfeld:2/";
    if (!writeFile(ledA + "trigger", "none\n", 5) ||
        !writeFile(ledB + "trigger", "none\n", 5)) return 1;
    std::signal(SIGTERM, stop);
    std::signal(SIGINT, stop);
    const uint64_t started = milliseconds();
    uint64_t nextPoll = 0, failedSince = 0, stateSince = started;
    State state = State::Boot;
    int previousA = -1, previousB = -1;
    while (!stopped)
    {
        const uint64_t now = milliseconds();
        if (now >= nextPoll)
        {
            nextPoll = now + 1000;
            State desired = State::Error;
            Value config, response;
            int64_t port = 4720;
            std::string host = "127.0.0.1";
            if (readFile("/etc/galdrun/client.json", text) &&
                multiroom::json::parse(text, config, error))
            {
                multiroom::json::getInt(config, "controlPort", port);
                std::string bind;
                if (multiroom::json::getString(config, "controlBindAddress", bind) &&
                    !bind.empty() && bind != "0.0.0.0") host = bind;
                bool off = false;
                multiroom::json::getBool(config, "ledsOff", off);
                if (off) desired = State::Off;
                else if (port > 0 && port <= 65535 && query(static_cast<uint16_t>(port), host, response))
                    desired = statusState(response);
            }
            if (desired == State::Error)
            {
                if (!failedSince) failedSince = now;
                if (now - started < 30000 && state == State::Boot) desired = State::Boot;
                else if (now - failedSince < 5000) desired = state;
            }
            else failedSince = 0;
            if (desired != state) { state = desired; stateSince = now; }
        }
        const auto levels = brightness(state, milliseconds() - stateSince);
        if (previousA != levels.first)
        {
            if (!writeFile(ledA + "brightness", levels.first ? "1\n" : "0\n", 2)) return 1;
            previousA = levels.first;
        }
        if (previousB != levels.second)
        {
            if (!writeFile(ledB + "brightness", levels.second ? "1\n" : "0\n", 2)) return 1;
            previousB = levels.second;
        }
        usleep(100000);
    }
    writeFile(ledA + "brightness", "0\n", 2);
    writeFile(ledB + "brightness", "0\n", 2);
    return 0;
}
#else
} // namespace
#endif
