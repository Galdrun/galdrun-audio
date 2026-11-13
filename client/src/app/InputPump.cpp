#include "app/InputPump.h"

#include "audio/FeedbackSound.h"
#include "matp/Session.h"
#if defined(__linux__) && !defined(ESP_PLATFORM)
#include "config/BoardConfig.h"
#endif
#include "util/Logger.h"

namespace multiroom {
namespace {

const uint16_t kKeyPlayPause = 164; // Linux KEY_PLAYPAUSE, as the stock firmware's own sound

// A button that itself switches power keeps its own gestures in standby.
bool switchesPower(const InputDefinition& input)
{
    for (const matp::InputBinding& binding : input.control.bindings)
    {
        if (binding.action == matp::InputAction::StandbyOn ||
            binding.action == matp::InputAction::StandbyOff ||
            binding.action == matp::InputAction::ToggleStandby ||
            binding.action == matp::InputAction::DeepSleep)
            return true;
    }
    return false;
}

} // namespace

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
    // Touch fields give no feel of their own; the board asks for a click,
    // played once a gesture has been recognized and run (see poll()).
#if defined(__linux__) && !defined(ESP_PLATFORM)
    sounds_ = board_config::enabled(board_config::current(), "inputSounds");
#endif
    controller_.setButtonStateCallback([this, &control](size_t index, bool pressed) {
        control.publishInputState(inputs_[index].control.id, pressed);
    });
    // In standby a press only wakes the device: a tap in the dark must not
    // also change the volume or skip a track.
    controller_.setPressGate([this, &runtime](size_t index) {
        if (runtime.session == nullptr || std::string(runtime.session->powerState()) == "on" ||
            switchesPower(inputs_[index]))
            return false;
        wakeRequested_ = true;
        return true;
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
    if (wakeRequested_)
    {
        wakeRequested_ = false;
        matp::InputActionEvent wake;
        wake.binding.action = matp::InputAction::StandbyOff;
        std::string error;
        if (!handler.dispatchInputAction(wake, error))
            MULTIROOM_WARN("input: waking from standby: %s", error.c_str());
    }
    for (const matp::InputActionEvent& action : actions_)
    {
        std::string error;
        if (!handler.dispatchInputAction(action, error))
        {
            MULTIROOM_WARN("input: %s", error.c_str());
            continue;
        }
        if (!sounds_)
            continue;
        for (const InputDefinition& input : inputs_)
        {
            if (input.control.id == action.inputId && input.control.kind == matp::InputKind::Button)
                FeedbackSound::play(input.code == kKeyPlayPause ? FeedbackSound::Sound::PlayPause
                                                                : FeedbackSound::Sound::Touch);
        }
    }
    actions_.clear();
}

} // namespace multiroom
