#include <arpa/inet.h>
#include <sys/socket.h>
#include <poll.h>
#include <unistd.h>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

int main(int argc, char** argv)
{
    if (argc != 3) return 2;
    char* end = nullptr;
    const long port = std::strtol(argv[2], &end, 10);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    if (*end || port < 1 || port > 65535 || inet_pton(AF_INET, argv[1], &address.sin_addr) != 1)
        return 2;
    address.sin_port = htons(static_cast<uint16_t>(port));
    const int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) return 1;
    const int reuse = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    // Binding to the hotspot address keeps redirects off Ethernet and home Wi-Fi.
    if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || listen(listener, 8) != 0)
    {
        std::perror("captive portal listen");
        close(listener);
        return 1;
    }
    for (;;)
    {
        const int client = accept(listener, nullptr, nullptr);
        if (client < 0)
        {
            if (errno == EINTR) continue;
            std::perror("captive portal accept");
            close(listener);
            return 1;
        }
        const timeval timeout{2, 0};
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        char request[8192]{};
        size_t used = 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (used < sizeof(request) - 1 && std::strstr(request, "\r\n\r\n") == nullptr)
        {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()).count();
            pollfd ready{client, POLLIN, 0};
            if (remaining <= 0 || poll(&ready, 1, static_cast<int>(remaining)) <= 0) break;
            const ssize_t count = recv(client, request + used, sizeof(request) - 1 - used, 0);
            if (count <= 0) break;
            used += static_cast<size_t>(count);
        }
        if (std::strstr(request, "\r\n\r\n") != nullptr)
        {
            char response[512];
            const bool get = std::strncmp(request, "GET ", 4) == 0 || std::strncmp(request, "HEAD ", 5) == 0;
            const int length = get
                ? std::snprintf(response, sizeof(response), "HTTP/1.1 302 Found\r\nLocation: http://%s:4720/\r\n"
                    "Content-Length: 0\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n", argv[1])
                : std::snprintf(response, sizeof(response), "HTTP/1.1 405 Method Not Allowed\r\nAllow: GET, HEAD\r\n"
                    "Content-Length: 0\r\nConnection: close\r\n\r\n");
            size_t sent = 0;
            while (sent < static_cast<size_t>(length))
            {
                const ssize_t count = send(client, response + sent, static_cast<size_t>(length) - sent, MSG_NOSIGNAL);
                if (count < 0 && errno == EINTR) continue;
                if (count <= 0) break;
                sent += static_cast<size_t>(count);
            }
        }
        close(client);
    }
}
