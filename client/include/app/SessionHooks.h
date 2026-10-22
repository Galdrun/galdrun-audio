#pragma once

// The MATP session's hooks into the rest of the client. Each hook only
// forwards: what the server asks to change takes the control API's path,
// persistence included, and the stream's commands go to StreamPlayback.

#include "app/ClientCommandHandler.h"
#include "app/StreamPlayback.h"
#include "matp/Session.h"

namespace multiroom {

// Everything but the stream. `runtime` must outlive `session`.
void connectSessionHooks(matp::Session& session, ClientRuntime& runtime);

// PREPARE_STREAM and what follows it. `playback` must outlive the session's
// use of the hooks.
void connectStreamHooks(matp::Session& session, StreamPlayback& playback);

} // namespace multiroom
