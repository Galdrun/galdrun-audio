#pragma once

// A loopback HTTP/1.1 server for the tests that need the far end of an
// HttpMediaSource -- the high-end server's media endpoint, reduced to what
// phase 20 asks of it.
//
// Port 0, so no test takes a fixed port, and no dependency: it is the
// server's own socket shim and a hand-written request parser, which is all a
// GET with an optional `Range: bytes=N-` needs. It answers a served path with
// 200 (or 206 for a range, because FFmpeg probes a container by seeking and a
// server that ignored Range would be tested as a live stream), a redirect
// with 302, and anything else with 404. Every connection is `Connection:
// close` and has its own thread, because FFmpeg opens the connection a seek
// needs before it closes the one it is leaving.
//
// It records every path it was asked for, which is how a test proves a
// redirect was *not* followed: the target is never requested.
//
// Phase 22 adds faults to a served path, the ones the media endpoint contract
// names: an error status for the first requests, a connection that closes or
// goes quiet part-way through the body, and a ranged request answered with
// 200. A break is counted when a response actually *reaches* its byte, not
// when a request asks for a range covering it: FFmpeg's probe opens
// connections it abandons after a few kilobytes, and a fault spent on one of
// those would leave the stream that matters unbroken and the test passing for
// the wrong reason.

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace multiroomtest {

class HttpTestServer
{
public:
    HttpTestServer();
    ~HttpTestServer();

    HttpTestServer(const HttpTestServer&) = delete;
    HttpTestServer& operator=(const HttpTestServer&) = delete;

    [[nodiscard]] uint16_t port() const { return port_; }

    // `http://127.0.0.1:<port>` followed by `path`, which starts with '/'.
    [[nodiscard]] std::string url(std::string_view path) const;

    struct Faults
    {
        uint32_t failFirst = 0; // the first requests, answered with failStatus
        int failStatus = 503;

        size_t breakAt = 0;  // the body offset where a response breaks
        uint32_t drops = 0;  // responses that reach it and are closed there
        uint32_t stalls = 0; // responses that reach it and go quiet there

        // Once a response has been dropped, a request for a range that does
        // not start at zero is answered 200 with the whole body.
        bool rangeAs200AfterDrop = false;

        // Phase 28: the drop is the whole server going away. The listener is
        // closed before the dropped connection is, so every attempt after it
        // is refused until reopen().
        bool shutAtDrop = false;
    };

    // Called before the first request for that path.
    void serve(const std::string& path, std::vector<std::byte> body);
    void serve(const std::string& path, std::vector<std::byte> body, Faults faults);
    void redirect(const std::string& path, const std::string& location);

    // Phase 23: from now on every request without exactly
    // `Authorization: Bearer <token>` is answered 401, whatever its path.
    void requireBearer(std::string token);

    // How many requests were answered 401 for want of the token.
    [[nodiscard]] size_t unauthorised() const;

    // Whether a `shutAtDrop` has closed the listener, and listening again on
    // the same port after one -- a restart, with every route as it was.
    [[nodiscard]] bool isShut() const;
    void reopen();

    // Every request line's path, in arrival order.
    [[nodiscard]] std::vector<std::string> requested() const;

    // Where each request for `path` asked its body to start, in arrival
    // order: zero for one without a Range.
    [[nodiscard]] std::vector<size_t> rangesAsked(const std::string& path) const;

private:
    struct Route
    {
        std::vector<std::byte> body;
        std::string location; // non-empty: a redirect
        Faults faults;
        bool dropped = false;
    };

    void accept(std::stop_token stop);
    void answer(std::stop_token stop, uintptr_t socket);

    struct Library;
    std::unique_ptr<Library> library_;
    uint16_t port_ = 0;

    mutable std::mutex mutex_;
    uintptr_t listener_ = 0; // written under mutex_, and closed only by the acceptor
    bool shutRequested_ = false;
    bool shut_ = false;
    std::condition_variable_any shutDone_;
    std::map<std::string, Route> routes_;
    std::vector<std::string> requested_;
    std::map<std::string, std::vector<size_t>> ranges_;
    std::string bearer_; // empty: no token asked for
    size_t unauthorised_ = 0;
    std::vector<std::jthread> connections_;

    std::jthread acceptor_; // last: stopped and joined before what it reads
};

} // namespace multiroomtest
