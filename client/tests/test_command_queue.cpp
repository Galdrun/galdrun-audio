#include "TestSupport.h"

#include "control/CommandQueue.h"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

using namespace multiroom;

namespace {

class EchoHandler : public IControlHandler
{
public:
    json::Value handle(const ControlRequest& request) override
    {
        handled.fetch_add(1);
        json::Value response = makeControlSuccess(request);
        response["echo"] = json::Value(request.name);
        return response;
    }

    std::atomic<int> handled{0};
};

ControlRequest makeRequest(const char* name, ControlCommand command)
{
    ControlRequest request;
    request.command = command;
    request.name = name;
    request.params = json::Value::makeObject();
    return request;
}

std::string fieldString(const json::Value& object, const char* key)
{
    const json::Value* value = object.find(key);
    return value != nullptr ? value->asString() : std::string();
}

bool fieldBool(const json::Value& object, const char* key, bool fallback)
{
    const json::Value* value = object.find(key);
    return value != nullptr ? value->asBool(fallback) : fallback;
}

} // namespace

MULTIROOM_TEST(command_queue_hands_a_request_to_the_owner_thread_and_back)
{
    CommandQueue queue;
    EchoHandler handler;

    json::Value response;
    std::thread submitter([&queue, &response] {
        response = queue.handle(makeRequest("status", ControlCommand::Status));
    });

    // Drain until the request has been served; the submitter blocks until then.
    while (queue.drain(handler, 200) == 0)
    {
    }
    submitter.join();

    CHECK_EQ(handler.handled.load(), 1);
    CHECK_EQ(fieldBool(response, "ok", false), true);
    CHECK_EQ(fieldString(response, "echo"), std::string("status"));
}

MULTIROOM_TEST(command_queue_drains_everything_that_is_queued)
{
    CommandQueue queue;
    EchoHandler handler;

    std::atomic<int> answered{0};
    std::thread submitters[4];
    for (int i = 0; i < 4; ++i)
    {
        submitters[i] = std::thread([&queue, &answered] {
            const json::Value response =
                queue.handle(makeRequest("getServer", ControlCommand::GetServer));
            if (response.isObject() && response.find("ok") != nullptr &&
                response.find("ok")->asBool(false))
            {
                answered.fetch_add(1);
            }
        });
    }

    while (answered.load() < 4)
        queue.drain(handler, 200);

    for (int i = 0; i < 4; ++i)
        submitters[i].join();

    CHECK_EQ(handler.handled.load(), 4);
    CHECK_EQ(queue.pending(), static_cast<size_t>(0));
}

MULTIROOM_TEST(command_queue_drain_returns_after_its_wait_when_idle)
{
    CommandQueue queue;
    EchoHandler handler;

    const auto started = std::chrono::steady_clock::now();
    CHECK_EQ(queue.drain(handler, 50), static_cast<size_t>(0));
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();

    // Only the lower bound is asserted: a loaded build machine can take much
    // longer, but returning early would mean the wait does not work.
    CHECK(elapsed >= 40);
    CHECK_EQ(handler.handled.load(), 0);
}

MULTIROOM_TEST(command_queue_shutdown_releases_a_waiting_caller)
{
    CommandQueue queue;

    std::atomic<bool> done{false};
    json::Value response;
    std::thread submitter([&queue, &response, &done] {
        response = queue.handle(makeRequest("status", ControlCommand::Status));
        done.store(true);
    });

    // Wait until the request is actually queued, so this exercises shutdown
    // against a caller that is already blocked.
    while (queue.pending() == 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    queue.shutdown();
    submitter.join();

    CHECK(done.load());
    CHECK_EQ(fieldBool(response, "ok", true), false);
    CHECK_EQ(fieldString(response, "error"), std::string(control_error::kUnavailable));
}

MULTIROOM_TEST(command_queue_refuses_requests_after_shutdown)
{
    CommandQueue queue;
    queue.shutdown();

    const json::Value response = queue.handle(makeRequest("status", ControlCommand::Status));
    CHECK_EQ(fieldBool(response, "ok", true), false);
    CHECK_EQ(fieldString(response, "error"), std::string(control_error::kUnavailable));
    CHECK_EQ(fieldString(response, "command"), std::string("status"));
}

MULTIROOM_TEST(command_queue_gives_up_when_the_owner_never_drains)
{
    // A socket thread must not hang forever behind an owner thread that is
    // stuck; the real timeout is minutes, this one is short enough to test.
    CommandQueue queue(60);

    const auto started = std::chrono::steady_clock::now();
    const json::Value response =
        queue.handle(makeRequest("rescanAudio", ControlCommand::RescanAudio));
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();

    CHECK(elapsed >= 50);
    CHECK_EQ(fieldBool(response, "ok", true), false);
    CHECK_EQ(fieldString(response, "error"), std::string(control_error::kUnavailable));
}

MULTIROOM_TEST(command_queue_never_runs_a_command_whose_caller_timed_out)
{
    CommandQueue queue(60);
    EchoHandler handler;

    const json::Value response =
        queue.handle(makeRequest("setServer", ControlCommand::SetServer));
    CHECK_EQ(fieldBool(response, "ok", true), false);

    // The caller was told the command did not answer and may well retry it.
    // Running the abandoned one as well would apply it twice.
    CHECK_EQ(queue.drain(handler, 0), static_cast<size_t>(0));
    CHECK_EQ(handler.handled.load(), 0);
}
