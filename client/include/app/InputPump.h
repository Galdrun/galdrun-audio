#pragma once

// Buttons and encoders (phase 23) as one step of the main loop. A backend
// thread reads the lines and wakes the loop; everything after that -- the
// gestures, the actions they are bound to -- happens on the main thread.

#include "app/ClientCommandHandler.h"
#include "control/CommandQueue.h"
#include "control/ControlServer.h"
#include "input/InputBackend.h"
#include "input/InputController.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace multiroom {

class InputPump
{
public:
    // `inputs` is the configuration's own list and must outlive this object;
    // see InputController.
    explicit InputPump(const std::vector<InputDefinition>& inputs);

    InputPump(const InputPump&) = delete;
    InputPump& operator=(const InputPump&) = delete;

    // Publishes the buttons on the control API and starts the backend. Not
    // fatal when the lines cannot be read: the reason goes to the runtime and
    // the log, and a room whose knob is miswired still plays.
    void start(ClientRuntime& runtime, ControlServer& control, CommandQueue& queue,
               const std::string& gpioChip);

    // Main thread: turns what the backend posted and what has become certain
    // by `nowNs` into actions, and runs them.
    void poll(uint64_t nowNs, ClientCommandHandler& handler);

    // How long the main loop may sleep before poll() has something to decide.
    uint32_t waitMs(uint64_t nowNs, uint32_t maxMs) const { return controller_.waitMs(nowNs, maxMs); }

private:
    const std::vector<InputDefinition>& inputs_;
    InputEventQueue events_;
    InputController controller_;
    std::vector<RawInputEvent> rawInputs_;
    std::vector<matp::InputActionEvent> actions_;
    uint32_t droppedSeen_ = 0;
    // Last, so its thread is stopped before the queue it posts to is gone.
    std::unique_ptr<IInputBackend> backend_;
};

} // namespace multiroom
