#include "net/Socket.h"

#if !defined(_WIN32)
#  include <cerrno>
#  include <cstring>
#  include <fcntl.h>
#endif

namespace multiroom::net {

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
    return ::fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

bool setNoDelay(Socket socket)
{
    const int on = 1;
#if defined(_WIN32)
    return ::setsockopt(socket, IPPROTO_TCP, TCP_NODELAY,
                        reinterpret_cast<const char*>(&on), sizeof(on)) == 0;
#else
    return ::setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on)) == 0;
#endif
}

int waitReady(Socket highest, fd_set* readable, fd_set* writable, fd_set* failed,
              timeval& timeout)
{
#if defined(_WIN32)
    static_cast<void>(highest);
    return ::select(0, readable, writable, failed, &timeout);
#else
    return ::select(highest + 1, readable, writable, failed, &timeout);
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

Socket listenOnPort(uint16_t port, int backlog, std::string& error)
{
    const Socket listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == kInvalidSocket)
    {
        error = "socket() failed: " + errorMessage(lastError());
        return kInvalidSocket;
    }

    // Without this a restart inside the TIME_WAIT window cannot rebind, which
    // turns every quick restart into a listener that is not there. Windows
    // rebinds past TIME_WAIT anyway, and its SO_REUSEADDR would also let a
    // second copy of this server bind a port that is still listening, so it
    // gets the opposite: no other socket may share the port.
    const int on = 1;
#if defined(_WIN32)
    ::setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&on),
                 sizeof(on));
#else
    ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
#endif

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);

    if (::bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0)
    {
        error = "bind() to port " + std::to_string(port) + " failed: " +
                errorMessage(lastError());
        closeSocket(listener);
        return kInvalidSocket;
    }

    if (::listen(listener, backlog) != 0)
    {
        error = "listen() failed: " + errorMessage(lastError());
        closeSocket(listener);
        return kInvalidSocket;
    }

    if (!setNonBlocking(listener))
    {
        error = "could not make the listening socket non-blocking: " +
                errorMessage(lastError());
        closeSocket(listener);
        return kInvalidSocket;
    }

    if (!selectable(listener))
    {
        error = "descriptor " + std::to_string(listener) + " is past what select() can watch";
        closeSocket(listener);
        return kInvalidSocket;
    }

    return listener;
}

bool boundPort(Socket socket, uint16_t& out)
{
    sockaddr_in address{};
#if defined(_WIN32)
    int length = static_cast<int>(sizeof(address));
#else
    socklen_t length = static_cast<socklen_t>(sizeof(address));
#endif
    if (::getsockname(socket, reinterpret_cast<sockaddr*>(&address), &length) != 0)
        return false;
    out = ntohs(address.sin_port);
    return true;
}

Socket acceptOne(Socket listener, bool& wouldBlockOut, std::string& error)
{
    wouldBlockOut = false;

    const Socket accepted = ::accept(listener, nullptr, nullptr);
    if (accepted == kInvalidSocket)
    {
        const int code = lastError();
        if (wouldBlock(code))
        {
            wouldBlockOut = true;
            return kInvalidSocket;
        }
        error = "accept() failed: " + errorMessage(code);
        return kInvalidSocket;
    }

    if (!selectable(accepted))
    {
        error = "refusing a connection: descriptor " + std::to_string(accepted) +
                " is past what select() can watch";
        closeSocket(accepted);
        return kInvalidSocket;
    }

    if (!setNonBlocking(accepted))
    {
        error = "could not make the accepted socket non-blocking: " +
                errorMessage(lastError());
        closeSocket(accepted);
        return kInvalidSocket;
    }
    // A failure here is not fatal: the connection works, it is only latency
    // that suffers, and refusing a room over it would be the worse trade.
    setNoDelay(accepted);

    return accepted;
}

std::string peerAddress(Socket socket)
{
    sockaddr_storage address{};
#if defined(_WIN32)
    int length = static_cast<int>(sizeof(address));
#else
    socklen_t length = static_cast<socklen_t>(sizeof(address));
#endif
    if (::getpeername(socket, reinterpret_cast<sockaddr*>(&address), &length) != 0)
        return {};
    char text[NI_MAXHOST] = {};
    if (::getnameinfo(reinterpret_cast<const sockaddr*>(&address), length, text, sizeof(text),
                      nullptr, 0, NI_NUMERICHOST) != 0)
        return {};
    return text;
}

} // namespace multiroom::net
