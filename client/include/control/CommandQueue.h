#pragma once

// Hands control requests from the control-server thread to the thread that
// owns the client state, and hands the response back.
//
// The owner thread is the only one that ever touches the configuration, the
// device cache or the audio engine, so there is no lock around any of that --
// the one lock in the whole control path is the one in here. The audio thread
// is not involved at any point.
//
// A plain mutex plus a condition variable is the right tool: this queue sees a
// handful of messages a second from a diagnostic interface, not audio data.

#include "control/ControlProtocol.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <future>
#include <memory>
#include <mutex>

namespace multiroom {

class CommandQueue : public IControlHandler
{
public:
    // The timeout only exists so a socket thread cannot hang forever behind an
    // owner thread that is stuck; it is far longer than any legitimate command
    // needs, including a full device rescan.
    explicit CommandQueue(uint32_t timeoutMs = 60000);
    ~CommandQueue() override;

    CommandQueue(const CommandQueue&) = delete;
    CommandQueue& operator=(const CommandQueue&) = delete;

    // Control-server thread: enqueue and block until the owner thread answers.
    // Returns an "unavailable" error response after shutdown() or a timeout.
    json::Value handle(const ControlRequest& request) override;

    // Owner thread: wait up to waitMs for work, then run everything queued
    // through handler. Returns how many requests were handled.
    size_t drain(IControlHandler& handler, uint32_t waitMs);

    // Any thread: ends the current (or the next) drain()'s wait with nothing
    // to handle, so the owner loop runs a pass at once. How a button press
    // reaches the loop without waiting out its idle timeout (phase 23).
    void wake();

    // Answers everything pending, and everything submitted afterwards, with an
    // "unavailable" error, so no socket thread is left waiting for an owner
    // that has stopped draining.
    void shutdown();

    size_t pending() const;

private:
    // Queued until drain() claims it or a timed-out caller abandons it;
    // whichever gets there first wins, so a command whose caller was told it
    // did not answer is never run afterwards. Shared, because the entry moves
    // into drain()'s batch while the caller still holds on to it.
    enum EntryState : int { kQueued, kClaimed, kAbandoned };

    struct Entry
    {
        ControlRequest request;
        std::promise<json::Value> response;
        std::shared_ptr<std::atomic<int>> state;
    };

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<Entry> queue_;
    bool stopped_ = false;
    bool woken_ = false;
    uint32_t timeoutMs_;
};

} // namespace multiroom
