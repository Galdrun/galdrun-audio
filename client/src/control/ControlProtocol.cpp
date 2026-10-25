#include "control/ControlProtocol.h"

#include <cstring>

namespace multiroom {

namespace control_error {

const char* const kInvalidJson = "invalidJson";
const char* const kInvalidRequest = "invalidRequest";
const char* const kUnknownCommand = "unknownCommand";
const char* const kInvalidArgument = "invalidArgument";
const char* const kFailed = "failed";
const char* const kUnavailable = "unavailable";

} // namespace control_error

namespace {

struct CommandEntry
{
    ControlCommand command;
    const char* name;
};

const CommandEntry kCommands[] = {
    {ControlCommand::Status, "status"},
    {ControlCommand::AudioDevices, "audioDevices"},
    {ControlCommand::RescanAudio, "rescanAudio"},
    {ControlCommand::GetServer, "getServer"},
    {ControlCommand::SetServer, "setServer"},
    {ControlCommand::GetActive, "getActive"},
    {ControlCommand::SetActive, "setActive"},
    {ControlCommand::GetClientId, "getClientId"},
    {ControlCommand::SetClientId, "setClientId"},
    {ControlCommand::SetName, "setName"},
    {ControlCommand::SetDriftCorrection, "setDriftCorrection"},
    {ControlCommand::SetAudioDevice, "setAudioDevice"},
    {ControlCommand::Inputs, "inputs"},
    {ControlCommand::SetInputBindings, "setInputBindings"},
    {ControlCommand::SimulateInput, "simulateInput"},
    {ControlCommand::Diagnostics, "diagnostics"},
    {ControlCommand::GetNetwork, "getNetwork"},
    {ControlCommand::SetNetwork, "setNetwork"},
    {ControlCommand::ScanNetworks, "scanNetworks"},
    {ControlCommand::GetNetworks, "getNetworks"},
    {ControlCommand::GetCpu, "getCpu"},
    {ControlCommand::SetCpu, "setCpu"},
    {ControlCommand::GetSsh, "getSsh"},
    {ControlCommand::SetSsh, "setSsh"},
    {ControlCommand::GetDac, "getDac"},
    {ControlCommand::SetDac, "setDac"},
    {ControlCommand::ResetDac, "resetDac"},
    {ControlCommand::GetAudioProfile, "getAudioProfile"},
    {ControlCommand::SetAudioProfile, "setAudioProfile"},
    {ControlCommand::Restart, "restart"},
    {ControlCommand::GetLeds, "getLeds"},
    {ControlCommand::SetLeds, "setLeds"},
    {ControlCommand::GetPower, "getPower"},
    {ControlCommand::SetPower, "setPower"},
    {ControlCommand::StandbyOff, "standbyOff"},
    {ControlCommand::StandbyOn, "standbyOn"},
    {ControlCommand::ToggleStandby, "toggleStandby"},
    {ControlCommand::DeepSleep, "deepSleep"},
};

// Builds the common head of every response. Field order is fixed so a human
// reading the socket sees "ok" first.
json::Value makeResponseHead(bool ok, const std::string& name, const json::Value& id)
{
    json::Value response = json::Value::makeObject();
    response["ok"] = json::Value(ok);
    if (!name.empty())
        response["command"] = json::Value(name);
    if (!id.isNull())
        response["id"] = id;
    return response;
}

} // namespace

const char* controlCommandName(ControlCommand command)
{
    for (const CommandEntry& entry : kCommands)
    {
        if (entry.command == command)
            return entry.name;
    }
    return "unknown";
}

bool parseControlCommand(const std::string& name, ControlCommand& out)
{
    for (const CommandEntry& entry : kCommands)
    {
        if (name == entry.name)
        {
            out = entry.command;
            return true;
        }
    }
    return false;
}

bool parseControlRequest(const std::string& line, ControlRequest& out,
                         json::Value& errorResponse)
{
    ControlRequest request;

    json::Value root;
    std::string parseError;
    if (!json::parse(line, root, parseError))
    {
        errorResponse = makeResponseHead(false, std::string(), json::Value());
        errorResponse["error"] = json::Value(control_error::kInvalidJson);
        errorResponse["message"] = json::Value(parseError);
        return false;
    }
    if (!root.isObject())
    {
        errorResponse = makeResponseHead(false, std::string(), json::Value());
        errorResponse["error"] = json::Value(control_error::kInvalidJson);
        errorResponse["message"] = json::Value("request must be a JSON object");
        return false;
    }

    // Read the echo fields first, so even a rejected request can be matched to
    // its response.
    const json::Value* id = root.find("id");
    if (id != nullptr)
        request.id = *id;

    if (!json::getString(root, "command", request.name) || request.name.empty())
    {
        errorResponse = makeResponseHead(false, std::string(), request.id);
        errorResponse["error"] = json::Value(control_error::kInvalidRequest);
        errorResponse["message"] = json::Value("missing or empty \"command\" string");
        return false;
    }

    const json::Value* params = root.find("params");
    if (params != nullptr && !params->isNull())
    {
        if (!params->isObject())
        {
            errorResponse = makeResponseHead(false, request.name, request.id);
            errorResponse["error"] = json::Value(control_error::kInvalidRequest);
            errorResponse["message"] = json::Value("\"params\" must be an object");
            return false;
        }
        request.params = *params;
    }
    else
    {
        request.params = json::Value::makeObject();
    }

    if (!parseControlCommand(request.name, request.command))
    {
        errorResponse = makeResponseHead(false, request.name, request.id);
        errorResponse["error"] = json::Value(control_error::kUnknownCommand);
        errorResponse["message"] = json::Value("unknown command \"" + request.name + "\"");
        return false;
    }

    out = request;
    return true;
}

json::Value makeControlSuccess(const ControlRequest& request)
{
    return makeResponseHead(true, request.name, request.id);
}

json::Value makeControlError(const ControlRequest& request, const char* errorCode,
                             const std::string& message)
{
    json::Value response = makeResponseHead(false, request.name, request.id);
    response["error"] = json::Value(errorCode != nullptr ? errorCode : control_error::kFailed);
    response["message"] = json::Value(message);
    return response;
}

std::string encodeControlResponse(const json::Value& response)
{
    std::string line = json::serialize(response, false);
    line.push_back('\n');
    return line;
}

IControlHandler::~IControlHandler() = default;

std::string dispatchControlLine(const std::string& line, IControlHandler& handler)
{
    ControlRequest request;
    json::Value errorResponse;
    if (!parseControlRequest(line, request, errorResponse))
        return encodeControlResponse(errorResponse);

    json::Value response = handler.handle(request);
    if (!response.isObject())
    {
        // A handler that returns nothing usable is a bug on our side, not the
        // caller's; say so instead of writing a bare "null" to the socket.
        response = makeControlError(request, control_error::kFailed,
                                    "the command handler produced no response");
    }
    return encodeControlResponse(response);
}

} // namespace multiroom
