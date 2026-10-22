#pragma once

// Reading buttons and encoders off GPIO lines (phase 23).
//
// A backend runs one thread of its own, reads its lines and posts what it saw
// into the InputEventQueue -- a level for a button, signed detents for an
// encoder, each time stamped with monotonicNowNs(). Debounce, gestures and
// bindings are InputController's, on the main thread, so a backend is the
// small platform-specific part and nothing else.
//
// Never on or near the audio thread: the thread runs at default priority (on
// the ESP32 below the network, on the core the render task does not use), and
// neither backend installs a GPIO interrupt.

#include "config/ClientConfig.h"
#include "input/InputController.h"

#include <memory>
#include <string>
#include <vector>

namespace multiroom {

class IInputBackend
{
public:
    virtual ~IInputBackend();

    // Claims the lines and starts the thread. False with a reason when a line
    // cannot be had; nothing is left claimed then. `inputs` and `queue` must
    // outlive the backend; only the wiring is read, and only here.
    virtual bool start(const std::vector<InputDefinition>& inputs, const std::string& gpioChip,
                       InputEventQueue& queue, std::string& error) = 0;

    // Idempotent; also called by the destructor.
    virtual void stop() = 0;
};

// Null on a platform with no GPIO access (Windows): inputs configured there
// are reachable only through the control API's simulateInput.
std::unique_ptr<IInputBackend> createPlatformInputBackend();

// Linux: adds the kernel's own button and knob devices (gpio-keys,
// rotary-encoder overlays) that `inputs` does not name yet, so they are
// reported and bindable without a line in client.json. In memory only; they
// reach the file with its next save. Nothing elsewhere.
void appendDiscoveredInputs(std::vector<InputDefinition>& inputs);

// "linux" | "esp32-gpio" | "none"
const char* platformInputBackendName();

} // namespace multiroom
