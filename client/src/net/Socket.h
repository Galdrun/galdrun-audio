#pragma once

// Thin wrapper over Winsock2 and POSIX sockets.
//
// It lives in src/ rather than in include/, for the same reason the audio
// backends do: nothing outside the socket users -- ControlServer and
// ServerConnection -- may pull <winsock2.h> or
// <netinet/in.h> into its translation unit. The wrapper covers exactly what
// those need, no abstraction beyond that.

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <arpa/inet.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <sys/select.h>
#  include <sys/socket.h>
#  include <sys/types.h>
#  include <unistd.h>
#endif

#include <string>

namespace multiroom {
namespace net {

#if defined(_WIN32)
using Socket = SOCKET;
const Socket kInvalidSocket = INVALID_SOCKET;
#else
using Socket = int;
const Socket kInvalidSocket = -1;
#endif

// Winsock has to be initialized and torn down per process, and every
// WSAStartup needs its own WSACleanup. POSIX needs neither, so this is empty
// there and the caller does not have to care which platform it is on.
class Library
{
public:
    Library();
    ~Library();

    Library(const Library&) = delete;
    Library& operator=(const Library&) = delete;

    bool isReady() const { return ready_; }
    const std::string& error() const { return error_; }

private:
    bool ready_ = false;
    std::string error_;
};

void closeSocket(Socket socket);

// Breaks a connection that another thread may be sitting in select() on: the
// socket stays valid, but the blocked call returns at once. Closing it there
// instead would free a descriptor the other thread is still holding.
void shutdownSocket(Socket socket);

bool setNonBlocking(Socket socket);

// Errno / WSAGetLastError, captured immediately after a failing call.
int lastError();
bool wouldBlock(int error);
std::string errorMessage(int error);

// recv/send differ only in their length and buffer types between the two
// platforms; both return the byte count, 0 on orderly shutdown, and -1 on
// error with lastError() set.
int receive(Socket socket, char* buffer, int length);
int transmit(Socket socket, const char* buffer, int length);

} // namespace net
} // namespace multiroom
