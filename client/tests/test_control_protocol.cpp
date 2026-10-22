#include "TestSupport.h"

#include "control/ControlProtocol.h"

#include <string>

using namespace multiroom;

namespace {

// Records what reached the handler, so the tests can assert that malformed
// input is answered before dispatch rather than passed through.
class RecordingHandler : public IControlHandler
{
public:
    json::Value handle(const ControlRequest& request) override
    {
        ++calls;
        lastCommand = request.command;
        lastName = request.name;
        lastParams = request.params;

        json::Value response = makeControlSuccess(request);
        response["echo"] = json::Value(request.name);
        return response;
    }

    int calls = 0;
    ControlCommand lastCommand = ControlCommand::Status;
    std::string lastName;
    json::Value lastParams;
};

// Returns a null value when the key is missing, which keeps the assertions
// below readable.
json::Value field(const json::Value& object, const char* key)
{
    const json::Value* value = object.find(key);
    return value != nullptr ? *value : json::Value();
}

json::Value parseOrFail(const std::string& text)
{
    json::Value value;
    std::string error;
    CHECK(json::parse(text, value, error));
    return value;
}

} // namespace

MULTIROOM_TEST(control_parses_a_minimal_request)
{
    ControlRequest request;
    json::Value errorResponse;
    CHECK(parseControlRequest("{\"command\":\"status\"}", request, errorResponse));
    CHECK(request.command == ControlCommand::Status);
    CHECK_EQ(request.name, std::string("status"));
    CHECK(request.params.isObject());
    CHECK_EQ(request.params.members().size(), static_cast<size_t>(0));
    CHECK(request.id.isNull());
}

MULTIROOM_TEST(control_parses_params_and_id)
{
    ControlRequest request;
    json::Value errorResponse;
    CHECK(parseControlRequest(
        "{\"command\":\"setServer\",\"params\":{\"server\":\"10.0.0.2:4711\"},\"id\":7}", request,
        errorResponse));
    CHECK(request.command == ControlCommand::SetServer);

    std::string server;
    CHECK(json::getString(request.params, "server", server));
    CHECK_EQ(server, std::string("10.0.0.2:4711"));
    CHECK_EQ(request.id.asInt(), static_cast<int64_t>(7));
}

MULTIROOM_TEST(control_recognizes_every_documented_command)
{
    // The five commands phase 6 defines and the two of phase 19. A name added
    // here without an implementation would fail to compile rather than
    // silently return unknownCommand at runtime.
    const ControlCommand expected[] = {ControlCommand::Status,      ControlCommand::AudioDevices,
                                       ControlCommand::RescanAudio, ControlCommand::GetServer,
                                       ControlCommand::SetServer,   ControlCommand::GetClientId,
                                       ControlCommand::SetClientId, ControlCommand::GetDac,
                                       ControlCommand::SetDac};
    for (const ControlCommand command : expected)
    {
        ControlCommand parsed = ControlCommand::Status;
        CHECK(parseControlCommand(controlCommandName(command), parsed));
        CHECK(parsed == command);
    }

    ControlCommand parsed = ControlCommand::Status;
    CHECK(!parseControlCommand("Status", parsed)); // the names are case sensitive
    CHECK(!parseControlCommand("", parsed));
    CHECK(!parseControlCommand("reboot", parsed));
}

MULTIROOM_TEST(control_rejects_input_that_is_not_a_json_object)
{
    const char* const lines[] = {"", "   ", "not json", "[1,2,3]", "\"status\"", "42",
                                 "{\"command\":\"status\"", "{}{}"};
    for (const char* line : lines)
    {
        ControlRequest request;
        json::Value errorResponse;
        CHECK(!parseControlRequest(line, request, errorResponse));
        CHECK_EQ(field(errorResponse, "ok").asBool(true), false);
        CHECK_EQ(field(errorResponse, "error").asString(), std::string(control_error::kInvalidJson));
        CHECK(!field(errorResponse, "message").asString().empty());
    }
}

MULTIROOM_TEST(control_rejects_a_request_without_a_command)
{
    ControlRequest request;
    json::Value errorResponse;
    CHECK(!parseControlRequest("{\"id\":3}", request, errorResponse));
    CHECK_EQ(field(errorResponse, "error").asString(),
             std::string(control_error::kInvalidRequest));
    // The id is echoed even though the request was rejected, so a caller
    // pipelining requests can still match the rejection to its cause.
    CHECK_EQ(field(errorResponse, "id").asInt(), static_cast<int64_t>(3));

    CHECK(!parseControlRequest("{\"command\":\"\"}", request, errorResponse));
    CHECK(!parseControlRequest("{\"command\":42}", request, errorResponse));
}

MULTIROOM_TEST(control_rejects_params_that_are_not_an_object)
{
    ControlRequest request;
    json::Value errorResponse;
    CHECK(!parseControlRequest("{\"command\":\"setServer\",\"params\":\"x\"}", request,
                               errorResponse));
    CHECK_EQ(field(errorResponse, "error").asString(),
             std::string(control_error::kInvalidRequest));
    CHECK_EQ(field(errorResponse, "command").asString(), std::string("setServer"));

    // A null params is the same as none at all.
    CHECK(parseControlRequest("{\"command\":\"status\",\"params\":null}", request,
                              errorResponse));
    CHECK(request.params.isObject());
}

MULTIROOM_TEST(control_answers_an_unknown_command_without_reaching_the_handler)
{
    RecordingHandler handler;
    const std::string line =
        dispatchControlLine("{\"command\":\"launchMissiles\",\"id\":\"a\"}", handler);

    CHECK_EQ(handler.calls, 0);

    const json::Value response = parseOrFail(line);
    CHECK_EQ(field(response, "ok").asBool(true), false);
    CHECK_EQ(field(response, "error").asString(), std::string(control_error::kUnknownCommand));
    CHECK_EQ(field(response, "command").asString(), std::string("launchMissiles"));
    CHECK_EQ(field(response, "id").asString(), std::string("a"));
}

MULTIROOM_TEST(control_dispatches_a_known_command_to_the_handler)
{
    RecordingHandler handler;
    const std::string line = dispatchControlLine(
        "{\"command\":\"rescanAudio\",\"params\":{\"deep\":true}}", handler);

    CHECK_EQ(handler.calls, 1);
    CHECK(handler.lastCommand == ControlCommand::RescanAudio);
    CHECK_EQ(handler.lastName, std::string("rescanAudio"));

    const json::Value response = parseOrFail(line);
    CHECK_EQ(field(response, "ok").asBool(false), true);
    CHECK_EQ(field(response, "echo").asString(), std::string("rescanAudio"));
}

MULTIROOM_TEST(control_every_response_is_exactly_one_line)
{
    // The framing is one object per line, so a payload containing a newline
    // must be escaped rather than split the response in two.
    ControlRequest request;
    request.name = "status";
    const json::Value response =
        makeControlError(request, control_error::kFailed, "first\nsecond\r\tthird");

    const std::string line = encodeControlResponse(response);
    CHECK_EQ(line.find('\n'), line.size() - 1);
    CHECK_EQ(line.find('\r'), std::string::npos);

    const json::Value parsed = parseOrFail(line);
    CHECK_EQ(field(parsed, "message").asString(), std::string("first\nsecond\r\tthird"));
}

MULTIROOM_TEST(control_garbage_still_produces_one_response_line)
{
    RecordingHandler handler;
    const char* const lines[] = {"", "{", "null", "{\"command\":\"nope\"}"};
    for (const char* input : lines)
    {
        const std::string line = dispatchControlLine(input, handler);
        CHECK(!line.empty());
        CHECK_EQ(line.find('\n'), line.size() - 1);
        const json::Value response = parseOrFail(line);
        CHECK_EQ(field(response, "ok").asBool(true), false);
    }
    CHECK_EQ(handler.calls, 0);
}

MULTIROOM_TEST(control_reports_a_handler_that_answers_with_nothing)
{
    class BrokenHandler : public IControlHandler
    {
    public:
        json::Value handle(const ControlRequest&) override { return json::Value(); }
    };

    BrokenHandler handler;
    const json::Value response =
        parseOrFail(dispatchControlLine("{\"command\":\"status\"}", handler));
    CHECK_EQ(field(response, "ok").asBool(true), false);
    CHECK_EQ(field(response, "error").asString(), std::string(control_error::kFailed));
}
