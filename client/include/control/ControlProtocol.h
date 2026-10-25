#pragma once

// Wire format of the local control interface.
//
// One JSON object per line in each direction, request/response, no streaming
// and no server-initiated messages. Everything in this header is pure: it
// parses text and builds text, opens no socket and touches no client state,
// which is what makes the whole protocol testable without a listening port.
//
// Request:
//   {"command":"status"}
//   {"command":"setServer","params":{"server":"192.168.1.10:4711"},"id":7}
//
// Response:
//   {"ok":true,"command":"status","status":{...}}
//   {"ok":false,"command":"setServer","error":"invalidArgument",
//    "message":"invalid server address"}
//
// "id" is optional, of any JSON type, and echoed back unchanged so a caller
// that pipelines requests can match responses without counting lines.

#include "util/Json.h"

#include <string>

namespace multiroom {

// Error codes are part of the interface: a caller may switch on them, so they
// are stable strings and the human-readable "message" carries the detail.
namespace control_error {

extern const char* const kInvalidJson;     // the line is not a JSON object
extern const char* const kInvalidRequest;  // object, but not a valid request
extern const char* const kUnknownCommand;  // well-formed, no such command
extern const char* const kInvalidArgument; // known command, bad params
extern const char* const kFailed;          // understood, but it did not work
extern const char* const kUnavailable;     // cannot be served right now

} // namespace control_error

enum class ControlCommand
{
    Status,
    AudioDevices,
    RescanAudio,
    GetServer,
    SetServer,
    GetActive,
    SetActive,
    GetClientId,
    SetClientId,
    SetName,
    SetDriftCorrection,
    SetAudioDevice,
    Inputs,
    SetInputBindings,
    SimulateInput,
    Diagnostics,
    GetNetwork,
    SetNetwork,
    ScanNetworks,
    GetNetworks,
    GetCpu,
    SetCpu,
    GetSsh,
    SetSsh,
    GetDac,
    SetDac,
    ResetDac,
    GetAudioProfile,
    SetAudioProfile,
    GetLeds,
    SetLeds,
    GetPower,
    SetPower,
    StandbyOff,
    StandbyOn,
    ToggleStandby,
    DeepSleep,
    Restart
};

const char* controlCommandName(ControlCommand command);
bool parseControlCommand(const std::string& name, ControlCommand& out);

struct ControlRequest
{
    ControlCommand command = ControlCommand::Status;
    std::string name; // as sent; echoed back even when it is not a known command
    json::Value params; // always an object, empty when the request carried none
    json::Value id;     // null when the request carried none
};

// Parses one line. On failure the request is not usable, but errorResponse is
// filled with the object to send back -- it echoes "command" and "id" whenever
// those could still be read, so a caller pipelining requests can match the
// rejection to the request that caused it.
bool parseControlRequest(const std::string& line, ControlRequest& out,
                         json::Value& errorResponse);

// {"ok":true,"command":...,"id":...}. The caller adds the payload fields.
json::Value makeControlSuccess(const ControlRequest& request);

json::Value makeControlError(const ControlRequest& request, const char* errorCode,
                             const std::string& message);

// Compact JSON plus the terminating newline. Strings are escaped, so a payload
// can never contain a raw newline and break the framing.
std::string encodeControlResponse(const json::Value& response);

// Implemented by whoever owns the client state. Called on that owner's thread,
// never on a socket thread and never on the audio thread.
class IControlHandler
{
public:
    virtual ~IControlHandler();

    // Must return a response object and must not throw. Only known commands
    // reach it; malformed lines are answered before dispatch.
    virtual json::Value handle(const ControlRequest& request) = 0;
};

// One line in, exactly one response line out, for every input including
// garbage. This is the only entry point the socket layer needs.
std::string dispatchControlLine(const std::string& line, IControlHandler& handler);

} // namespace multiroom
