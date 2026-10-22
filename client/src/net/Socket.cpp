#include "net/Socket.h"

#if !defined(_WIN32)
#  include <cerrno>
#  include <cstring>
#  include <fcntl.h>
#endif

namespace multiroom {
namespace net {

Library::Library()
{
#if defined(_WIN32)
    WSADATA data;
    const int result = WSAStartup(MAKEWORD(2, 2), &data);
    if (result != 0)
    {
        error_ = "WSAStartup failed: " + errorMessage(result);
        return;
    }
#endif
    ready_ = true;
}

Library::~Library()
{
#if defined(_WIN32)
    if (ready_)
        WSACleanup();
#endif
}

void closeSocket(Socket socket)
{
    if (socket == kInvalidSocket)
        return;
#if defined(_WIN32)
    ::closesocket(socket);
#else
    ::close(socket);
#endif
}

void shutdownSocket(Socket socket)
{
    if (socket == kInvalidSocket)
        return;
#if defined(_WIN32)
    ::shutdown(socket, SD_BOTH);
#else
    ::shutdown(socket, SHUT_RDWR);
#endif
}

bool setNonBlocking(Socket socket)
{
#if defined(_WIN32)
    u_long mode = 1;
    return ::ioctlsocket(socket, FIONBIO, &mode) == 0;
#else
    const int flags = ::fcntl(socket, F_GETFL, 0);
    if (flags < 0)
        return false;
#if !defined(ESP_PLATFORM)
    // A board helper must not keep control/audio sockets open after exec.
    // lwIP has neither exec nor F_GETFD/F_SETFD (ENOSYS), so not on an ESP32.
    const int descriptorFlags = ::fcntl(socket, F_GETFD, 0);
    if (descriptorFlags < 0 || ::fcntl(socket, F_SETFD, descriptorFlags | FD_CLOEXEC) != 0)
        return false;
#endif
    return ::fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

int lastError()
{
#if defined(_WIN32)
    return ::WSAGetLastError();
#else
    return errno;
#endif
}

bool wouldBlock(int error)
{
#if defined(_WIN32)
    return error == WSAEWOULDBLOCK;
#else
    // EAGAIN and EWOULDBLOCK are the same value on Linux but not guaranteed to
    // be everywhere; EINTR is a signal, not a failure.
    return error == EAGAIN || error == EWOULDBLOCK || error == EINTR;
#endif
}

std::string errorMessage(int error)
{
#if defined(_WIN32)
    char* buffer = nullptr;
    const DWORD length = ::FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, static_cast<DWORD>(error), 0, reinterpret_cast<char*>(&buffer), 0, nullptr);
    if (length == 0 || buffer == nullptr)
        return "socket error " + std::to_string(error);

    std::string message(buffer, length);
    ::LocalFree(buffer);
    while (!message.empty() && (message.back() == '\r' || message.back() == '\n' ||
                                message.back() == ' ' || message.back() == '.'))
    {
        message.pop_back();
    }
    return message;
#else
    return std::string(std::strerror(error));
#endif
}

int receive(Socket socket, char* buffer, int length)
{
#if defined(_WIN32)
    return ::recv(socket, buffer, length, 0);
#else
    return static_cast<int>(::recv(socket, buffer, static_cast<size_t>(length), 0));
#endif
}

int transmit(Socket socket, const char* buffer, int length)
{
#if defined(_WIN32)
    return ::send(socket, buffer, length, 0);
#else
    // MSG_NOSIGNAL: a client that vanishes mid-write must not raise SIGPIPE
    // and kill the process.
    return static_cast<int>(::send(socket, buffer, static_cast<size_t>(length), MSG_NOSIGNAL));
#endif
}

} // namespace net
} // namespace multiroom
