#pragma once

// The room's buttons and encoders as the server and the control API see them
// (phase 23, MATP section 57). Main thread only, like the rest of the client
// state.

#include "app/ClientCommandHandler.h"
#include "matp/Input.h"
#include "util/Json.h"

#include <string>
#include <vector>

namespace multiroom {

// Every configured input with its current bindings, for INPUT_CONTROLS and
// the control API's `inputs`.
void currentInputControls(const ClientRuntime& runtime, std::vector<matp::InputControl>& out);

enum class InputBindingChange
{
    Changed,
    Unchanged,
    UnknownInput, // no input with that id; nothing was changed
    Invalid,      // the bindings were refused; nothing was changed
    NotSaved      // client.json could not be written; nothing was changed
};

// The one path a binding change takes, from the server and from the control
// API alike: `bindings` is merged into the input's own (a gesture it leaves
// out keeps what it had), written to client.json, and a live session reports
// the new INPUT_CONTROLS. The next gesture uses it; nothing needs restarting.
InputBindingChange changeInputBindings(ClientRuntime& runtime, const std::string& inputId,
                                       const json::Value& bindings, std::string& error);

} // namespace multiroom
