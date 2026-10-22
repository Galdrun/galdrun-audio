#pragma once

// Thin wrapper over Winsock2 and POSIX sockets.
//
// It lives in src/ rather than in include/, for the same reason the FFmpeg and
// resampler headers do: nothing outside `matp/` and `control/`'s listener may
// pull <winsock2.h> or <netinet/in.h> into its translation unit. The wrapper
// covers exactly what the listeners need and abstracts nothing beyond that.
//
// It is the client's `Client/src/net/Socket.h` with a listener's three calls
// added, and it is a copy on purpose -- the same arrangement as
// `cmake/MultiroomWarnings.cmake`. The client connects and this server accepts,
// so the two halves share no code here beyond a platform shim that would gain
// nothing from being a third shared library, and unlike the wire format there
// is no way for the two copies to silently disagree: a socket that is set up
// wrongly fails on the first call rather than in a room six hours later.

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

#include <cstdint>
#include <string>

namespace multiroom::net {

#if defined(_WIN32)
using Socket = SOCKET;
inline const Socket kInvalidSocket = INVALID_SOCKET;
#else
using Socket = int;
inline const Socket kInvalidSocket = -1;
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

// Breaks a connection another thread may be sitting in select() on: the socket
// stays valid, but the blocked call returns at once. Closing it there instead
// would free a descriptor the other thread is still holding.
void shutdownSocket(Socket socket);

bool setNonBlocking(Socket socket);

// Whether select() can watch `socket`. A POSIX fd_set is a bitmap of
// FD_SETSIZE bits, and FD_SET on a descriptor past it writes outside the set;
// Winsock's is a list of handles and takes any of them.
inline bool selectable(Socket socket)
{
#if defined(_WIN32)
    return socket != kInvalidSocket;
#else
    return socket >= 0 && socket < FD_SETSIZE;
#endif
}

// Nagle off. Every control message this server sends is a whole message that
// the peer is waiting for -- a HELLO_ACK, a SESSION_CONFIG, a clock sync reply
// whose value *is* the instant it was sent -- so coalescing them into the next
// write would add up to 40 ms to a measurement taken to the nanosecond.
bool setNoDelay(Socket socket);

// Errno / WSAGetLastError, captured immediately after a failing call.
int lastError();
bool wouldBlock(int error);
std::string errorMessage(int error);

// recv/send differ only in their length and buffer types between the two
// platforms; both return the byte count, 0 on orderly shutdown, and -1 on
// error with lastError() set.
int receive(Socket socket, char* buffer, int length);
int transmit(Socket socket, const char* buffer, int length);

// A listening socket on `port`, non-blocking, with SO_REUSEADDR set.
//
// Port 0 means "whichever port the system has free", which is what the tests
// use: a fixed port is a test that fails when something else on the machine
// happens to hold it, and the roadmap forbids one. boundPort() reads back what
// the system chose.
Socket listenOnPort(uint16_t port, int backlog, std::string& error);

// Which port a listening socket actually bound to.
bool boundPort(Socket socket, uint16_t& out);

// A connection, or kInvalidSocket with `wouldBlockOut` true when there is
// simply none waiting. The accepted socket is non-blocking and Nagle-free like
// the listener that produced it.
Socket acceptOne(Socket listener, bool& wouldBlockOut, std::string& error);

// The numeric address of a connected socket's peer, or empty when the system
// cannot say.
std::string peerAddress(Socket socket);

} // namespace multiroom::net
