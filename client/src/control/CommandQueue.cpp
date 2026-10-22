#include "control/CommandQueue.h"

#include <chrono>
#include <utility>
#include <vector>

namespace multiroom {

CommandQueue::CommandQueue(uint32_t timeoutMs) : timeoutMs_(timeoutMs) {}

CommandQueue::~CommandQueue()
{
    shutdown();
}

json::Value CommandQueue::handle(const ControlRequest& request)
{
    std::future<json::Value> future;
    const auto state = std::make_shared<std::atomic<int>>(kQueued);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopped_)
        {
            return makeControlError(request, control_error::kUnavailable,
                                    "the client is shutting down");
        }

        // The request is copied rather than referenced: on timeout this call
        // returns while the entry is still queued, and a pointer into the
        // caller's frame would then dangle.
        Entry entry;
        entry.request = request;
        entry.state = state;
        future = entry.response.get_future();
        queue_.push_back(std::move(entry));
    }
    condition_.notify_one();

    if (future.wait_for(std::chrono::milliseconds(timeoutMs_)) != std::future_status::ready)
    {
        // Abandoned so a later drain() skips it: a caller that retries after
        // this answer would otherwise have its setServer or rescan applied
        // twice. One the owner has already started is running and cannot be
        // taken back, and the answer says so.
        int expected = kQueued;
        if (state->compare_exchange_strong(expected, kAbandoned))
        {
            return makeControlError(request, control_error::kUnavailable,
                                    "the client did not answer in time; the command was not run");
        }
        return makeControlError(request, control_error::kUnavailable,
                                "the client did not answer in time; the command is still running");
    }
    return future.get();
}

size_t CommandQueue::drain(IControlHandler& handler, uint32_t waitMs)
{
    std::vector<Entry> batch;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (queue_.empty() && !stopped_ && !woken_ && waitMs > 0)
        {
            condition_.wait_for(lock, std::chrono::milliseconds(waitMs),
                                [this] { return !queue_.empty() || stopped_ || woken_; });
        }
        woken_ = false;
        // Taken out under the lock and handled outside it, so a slow command
        // never blocks a control thread that only wants to enqueue.
        while (!queue_.empty())
        {
            batch.push_back(std::move(queue_.front()));
            queue_.pop_front();
        }
    }

    size_t handled = 0;
    for (size_t i = 0; i < batch.size(); ++i)
    {
        int expected = kQueued;
        if (!batch[i].state->compare_exchange_strong(expected, kClaimed))
            continue; // its caller has already been told it timed out
        ++handled;

        json::Value response = handler.handle(batch[i].request);
        if (!response.isObject())
        {
            response = makeControlError(batch[i].request, control_error::kFailed,
                                        "the command handler produced no response");
        }
        batch[i].response.set_value(response);
    }
    return handled;
}

void CommandQueue::wake()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        woken_ = true;
    }
    condition_.notify_one();
}

void CommandQueue::shutdown()
{
    std::deque<Entry> pending;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopped_ && queue_.empty())
            return;
        stopped_ = true;
        pending.swap(queue_);
    }
    condition_.notify_all();

    // Answered explicitly instead of dropped: destroying the promise would
    // make the waiting future throw broken_promise rather than return an error
    // the caller can put on the socket.
    while (!pending.empty())
    {
        pending.front().response.set_value(makeControlError(
            pending.front().request, control_error::kUnavailable,
            "the client is shutting down"));
        pending.pop_front();
    }
}

size_t CommandQueue::pending() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

} // namespace multiroom
