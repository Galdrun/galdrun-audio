#include "Connector3Mcu.h"

#include <algorithm>
#include <iterator>
#include <sstream>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace multiroom {
bool connector3McuRequest(const std::string& command, long& mute, long& volume, std::string& error)
{
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) { error = "Cannot open MCU audio socket"; return false; }
    timeval timeout{4, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const char path[] = "/run/galdrun-connector3-mcu.sock";
    std::copy(std::begin(path), std::end(path), address.sun_path);
    const std::string request = command + "\n";
    bool ok = connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 &&
              send(fd, request.data(), request.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(request.size());
    std::string response;
    char byte = 0;
    while (ok && response.size() < 80 && recv(fd, &byte, 1, 0) == 1) {
        if (byte == '\n') break;
        response += byte;
    }
    close(fd);
    std::istringstream reply(response);
    std::string status, extra;
    ok = ok && byte == '\n' && bool(reply >> status >> mute >> volume) && status == "ok" &&
         !(reply >> extra) && mute >= 0 && mute <= 1 && volume >= 0 && volume <= 100;
    if (!ok) error = "MCU audio control failed or readback was unavailable";
    return ok;
}

} // namespace multiroom
