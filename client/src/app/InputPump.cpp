#include "app/InputPump.h"

#include "util/Logger.h"

namespace multiroom {

InputPump::InputPump(const std::vector<InputDefinition>& inputs)
    : inputs_(inputs), controller_(inputs)
{
}

void InputPump::start(ClientRuntime& runtime, ControlServer& control, CommandQueue& queue,
                      const std::string& gpioChip)
{
    std::vector<std::string> buttonIds;
    for (const InputDefinition& input : inputs_)
        if (input.control.kind == matp::InputKind::Button)
            buttonIds.push_back(input.control.id);
    control.setInputButtons(buttonIds);
    controller_.setButtonStateCallback([this, &control](size_t index, bool pressed) {
        control.publishInputState(inputs_[index].control.id, pressed);
    });

    if (inputs_.empty())
        return;

    events_.setWake([&queue]() { queue.wake(); });
    backend_ = createPlatformInputBackend();
    runtime.inputBackend = platformInputBackendName();
    std::string inputError;
    if (!backend_)
    {
        runtime.inputBackendError = "this platform has no GPIO access; the inputs are "
                                    "reachable only through simulateInput";
        MULTIROOM_WARN("%s", runtime.inputBackendError.c_str());
    }
    else if (!backend_->start(inputs_, gpioChip, events_, inputError))
    {
        runtime.inputBackendError = inputError;
        MULTIROOM_ERROR("inputs are not read: %s", inputError.c_str());
    }
}

void InputPump::poll(uint64_t nowNs, ClientCommandHandler& handler)
{
    events_.take(rawInputs_);
    for (const RawInputEvent& event : rawInputs_)
        controller_.feed(event, actions_);
    const uint32_t dropped = events_.dropped();
    if (dropped != droppedSeen_)
    {
        MULTIROOM_WARN("input: %u event(s) dropped by a full queue; pending button "
                       "gestures were cancelled",
                       static_cast<unsigned>(dropped - droppedSeen_));
        droppedSeen_ = dropped;
        controller_.eventsLost();
    }
    controller_.poll(nowNs, actions_);
    for (const matp::InputActionEvent& action : actions_)
    {
        std::string error;
        if (!handler.dispatchInputAction(action, error))
            MULTIROOM_WARN("input: %s", error.c_str());
    }
    actions_.clear();
}

} // namespace multiroom
