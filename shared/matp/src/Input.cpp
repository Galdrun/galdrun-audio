#include "matp/Input.h"

#include "matp/Protocol.h"

namespace multiroom {
namespace matp {
namespace {

const InputKind kKinds[] = {InputKind::Button, InputKind::Encoder};
const InputGesture kGestures[] = {InputGesture::Click, InputGesture::DoubleClick,
                                  InputGesture::LongPress, InputGesture::Clockwise,
                                  InputGesture::CounterClockwise};
const InputAction kActions[] = {InputAction::None,       InputAction::Play,
                                InputAction::Pause,      InputAction::TogglePause,
                                InputAction::Stop,       InputAction::Next,
                                InputAction::Previous,   InputAction::VolumeUp,
                                InputAction::VolumeDown, InputAction::SetVolume,
                                InputAction::ToggleMute, InputAction::StandbyOn,
                                InputAction::StandbyOff, InputAction::ToggleStandby,
                                InputAction::DeepSleep,
                                InputAction::Action1,
                                InputAction::Action2,    InputAction::Action3,
                                InputAction::Action4,    InputAction::Action5,
                                InputAction::Action6,    InputAction::Action7,
                                InputAction::Action8,    InputAction::Action9,
                                InputAction::Action10};
const VolumeScope kScopes[] = {VolumeScope::Room, VolumeScope::Zone};

template <typename T, size_t N>
bool parseName(const T (&known)[N], const char* (*nameOf)(T), const std::string& name, T& out)
{
    for (size_t i = 0; i < N; ++i)
    {
        if (name == nameOf(known[i]))
        {
            out = known[i];
            return true;
        }
    }
    return false;
}

size_t indexOf(InputGesture gesture)
{
    return static_cast<size_t>(gesture);
}

// A number in [low, high], or in (low, high] when the low end is open.
bool numberFromJson(const json::Value& object, const char* key, double low, bool lowOpen,
                    double high, double& out, std::string& error)
{
    const json::Value* value = object.find(key);
    if (value == nullptr)
        return true;
    const double number = value->isNumber() ? value->asDouble() : -1.0;
    if (!value->isNumber() || number > high || (lowOpen ? number <= low : number < low))
    {
        error = std::string(key) + " must be a number " + (lowOpen ? "above " : "from ") +
                json::serialize(json::Value(low)) + " to " + json::serialize(json::Value(high));
        return false;
    }
    out = number;
    return true;
}

// One binding object. `context` prefixes every error, so a UI can name the
// gesture that was refused.
bool bindingFromJson(const json::Value& object, InputBinding& out, const std::string& context,
                     std::string& error)
{
    if (!object.isObject())
    {
        error = context + " must be an object";
        return false;
    }

    InputBinding binding;
    std::string actionName;
    if (!json::getString(object, "action", actionName) ||
        !parseInputAction(actionName, binding.action))
    {
        error = context + ": action must be one of none, play, pause, togglePause, stop, next, "
                          "previous, volumeUp, volumeDown, setVolume, toggleMute, standbyOn, "
                          "standbyOff, toggleStandby, deepSleep, action1 to "
                          "action10";
        return false;
    }

    for (const json::Value::Member& member : object.members())
    {
        const std::string& key = member.first;
        const bool allowed = key == "action" ||
                             (key == "step" && actionTakesStep(binding.action)) ||
                             (key == "value" && actionTakesValue(binding.action)) ||
                             (key == "scope" && actionTakesScope(binding.action));
        if (!allowed)
        {
            error = context + ": " + actionName + " takes no " + key;
            return false;
        }
    }

    std::string numberError;
    if (!numberFromJson(object, "step", 0.0, true, 1.0, binding.step, numberError) ||
        !numberFromJson(object, "value", 0.0, false, 1.0, binding.value, numberError))
    {
        error = context + ": " + numberError;
        return false;
    }
    if (actionTakesValue(binding.action) && object.find("value") == nullptr)
    {
        error = context + ": setVolume needs a value";
        return false;
    }
    if (const json::Value* scope = object.find("scope"))
    {
        if (!scope->isString() || !parseVolumeScope(scope->asString(), binding.scope))
        {
            error = context + ": scope must be room or zone";
            return false;
        }
    }

    out = binding;
    return true;
}

void bindingToJson(const InputBinding& binding, json::Value& out)
{
    out["action"] = json::Value(inputActionName(binding.action));
    if (actionTakesStep(binding.action))
        out["step"] = json::Value(binding.step);
    if (actionTakesValue(binding.action))
        out["value"] = json::Value(binding.value);
    if (actionTakesScope(binding.action))
        out["scope"] = json::Value(volumeScopeName(binding.scope));
}

bool sessionIdFromJson(const json::Value& payload, bool required, std::string& out,
                       std::string& error)
{
    const json::Value* value = payload.find("sessionId");
    if (value == nullptr && !required)
        return true;
    if (value == nullptr || !value->isString() || value->asString().empty())
    {
        error = "sessionId must be a non-empty string";
        return false;
    }
    out = value->asString();
    return true;
}

bool inputIdFromJson(const json::Value& object, const char* key, std::string& out,
                     std::string& error)
{
    if (!json::getString(object, key, out) || !isValidInputId(out))
    {
        error = std::string(key) + " must be 1 to 64 bytes without control characters or "
                                   "surrounding blanks";
        return false;
    }
    return true;
}

} // namespace

const char* inputKindName(InputKind kind)
{
    switch (kind)
    {
    case InputKind::Button:
        return "button";
    case InputKind::Encoder:
        return "encoder";
    }
    return "unknown";
}

bool parseInputKind(const std::string& name, InputKind& out)
{
    return parseName(kKinds, inputKindName, name, out);
}

const char* inputGestureName(InputGesture gesture)
{
    switch (gesture)
    {
    case InputGesture::Click:
        return "click";
    case InputGesture::DoubleClick:
        return "doubleClick";
    case InputGesture::LongPress:
        return "longPress";
    case InputGesture::Clockwise:
        return "clockwise";
    case InputGesture::CounterClockwise:
        return "counterClockwise";
    }
    return "unknown";
}

bool parseInputGesture(const std::string& name, InputGesture& out)
{
    return parseName(kGestures, inputGestureName, name, out);
}

const char* inputActionName(InputAction action)
{
    switch (action)
    {
    case InputAction::None:
        return "none";
    case InputAction::Play:
        return "play";
    case InputAction::Pause:
        return "pause";
    case InputAction::TogglePause:
        return "togglePause";
    case InputAction::Stop:
        return "stop";
    case InputAction::Next:
        return "next";
    case InputAction::Previous:
        return "previous";
    case InputAction::VolumeUp:
        return "volumeUp";
    case InputAction::VolumeDown:
        return "volumeDown";
    case InputAction::SetVolume:
        return "setVolume";
    case InputAction::ToggleMute:
        return "toggleMute";
    case InputAction::StandbyOff:
        return "standbyOff";
    case InputAction::StandbyOn:
        return "standbyOn";
    case InputAction::ToggleStandby:
        return "toggleStandby";
    case InputAction::DeepSleep:
        return "deepSleep";
    case InputAction::Action1:
        return "action1";
    case InputAction::Action2:
        return "action2";
    case InputAction::Action3:
        return "action3";
    case InputAction::Action4:
        return "action4";
    case InputAction::Action5:
        return "action5";
    case InputAction::Action6:
        return "action6";
    case InputAction::Action7:
        return "action7";
    case InputAction::Action8:
        return "action8";
    case InputAction::Action9:
        return "action9";
    case InputAction::Action10:
        return "action10";
    }
    return "unknown";
}

bool parseInputAction(const std::string& name, InputAction& out)
{
    // Saved bindings from before standby got its own name; the factory POWER
    // click was togglePower.
    if (name == "togglePower")
    {
        out = InputAction::ToggleStandby;
        return true;
    }
    return parseName(kActions, inputActionName, name, out);
}

const char* volumeScopeName(VolumeScope scope)
{
    switch (scope)
    {
    case VolumeScope::Room:
        return "room";
    case VolumeScope::Zone:
        return "zone";
    }
    return "unknown";
}

bool parseVolumeScope(const std::string& name, VolumeScope& out)
{
    return parseName(kScopes, volumeScopeName, name, out);
}

bool gestureAppliesTo(InputKind kind, InputGesture gesture)
{
    const bool encoderGesture =
        gesture == InputGesture::Clockwise || gesture == InputGesture::CounterClockwise;
    return (kind == InputKind::Encoder) == encoderGesture;
}

bool actionTakesStep(InputAction action)
{
    return action == InputAction::VolumeUp || action == InputAction::VolumeDown;
}

bool actionTakesValue(InputAction action)
{
    return action == InputAction::SetVolume;
}

bool actionTakesScope(InputAction action)
{
    return actionTakesStep(action) || actionTakesValue(action) ||
           action == InputAction::ToggleMute;
}

// Parameters an action does not take are not compared: they are not on the
// wire and not in the file, so two bindings differing only there are the same
// binding to everyone who can see them.
bool InputBinding::operator==(const InputBinding& other) const
{
    return action == other.action && (!actionTakesStep(action) || step == other.step) &&
           (!actionTakesValue(action) || value == other.value) &&
           (!actionTakesScope(action) || scope == other.scope);
}

InputBindings defaultInputBindings(InputKind kind)
{
    InputBindings bindings{};
    if (kind == InputKind::Button)
    {
        bindings[indexOf(InputGesture::Click)].action = InputAction::TogglePause;
    }
    else
    {
        InputBinding& up = bindings[indexOf(InputGesture::Clockwise)];
        up.action = InputAction::VolumeUp;
        up.step = 0.02;
        InputBinding& down = bindings[indexOf(InputGesture::CounterClockwise)];
        down.action = InputAction::VolumeDown;
        down.step = 0.02;
    }
    return bindings;
}

json::Value inputBindingsToJson(InputKind kind, const InputBindings& bindings)
{
    json::Value object = json::Value::makeObject();
    for (InputGesture gesture : kGestures)
    {
        if (!gestureAppliesTo(kind, gesture))
            continue;
        json::Value binding = json::Value::makeObject();
        bindingToJson(bindings[indexOf(gesture)], binding);
        object[inputGestureName(gesture)] = binding;
    }
    return object;
}

bool inputBindingsFromJson(const json::Value& object, InputKind kind, InputBindings& inOut,
                           std::string& error)
{
    if (!object.isObject())
    {
        error = "bindings must be an object";
        return false;
    }

    InputBindings merged = inOut;
    for (const json::Value::Member& member : object.members())
    {
        InputGesture gesture = InputGesture::Click;
        if (!parseInputGesture(member.first, gesture) || !gestureAppliesTo(kind, gesture))
        {
            error = "an input of kind " + std::string(inputKindName(kind)) + " has no gesture " +
                    member.first;
            return false;
        }
        if (!bindingFromJson(member.second, merged[indexOf(gesture)], member.first, error))
            return false;
    }

    inOut = merged;
    return true;
}

bool isValidInputId(const std::string& id)
{
    if (id.empty() || id.size() > 64 || id.front() == ' ' || id.back() == ' ')
        return false;
    for (char c : id)
    {
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f)
            return false;
    }
    return true;
}

json::Value makeInputControls(const std::string& sessionId, const std::vector<InputControl>& inputs,
                              bool deepSleep)
{
    json::Value root = json::Value::makeObject();
    root["type"] = json::Value(messageTypeName(MessageType::InputControls));
    root["sessionId"] = json::Value(sessionId);
    json::Value list = json::Value::makeArray();
    for (const InputControl& input : inputs)
    {
        json::Value entry = json::Value::makeObject();
        entry["id"] = json::Value(input.id);
        entry["kind"] = json::Value(inputKindName(input.kind));
        entry["bindings"] = inputBindingsToJson(input.kind, input.bindings);
        list.pushBack(entry);
    }
    root["inputs"] = list;
    if (deepSleep)
        root["deepSleep"] = json::Value(true);
    return root;
}

bool parseInputControls(const json::Value& payload, std::string& sessionId,
                        std::vector<InputControl>& out, bool& deepSleep, std::string& error)
{
    std::string session;
    if (!sessionIdFromJson(payload, true, session, error))
        return false;

    const json::Value* list = payload.find("inputs");
    if (list == nullptr || !list->isArray())
    {
        error = "inputs must be an array";
        return false;
    }
    if (list->size() > kMaxInputs)
    {
        error = "inputs lists " + std::to_string(list->size()) + " entries, more than " +
                std::to_string(kMaxInputs);
        return false;
    }

    std::vector<InputControl> inputs;
    inputs.reserve(list->size());
    for (size_t i = 0; i < list->size(); ++i)
    {
        const json::Value& entry = list->at(i);
        const std::string context = "inputs[" + std::to_string(i) + "]";
        InputControl input;
        std::string kind;
        if (!entry.isObject() || !inputIdFromJson(entry, "id", input.id, error))
        {
            error = context + ": " + (entry.isObject() ? error : "must be an object");
            return false;
        }
        if (!json::getString(entry, "kind", kind) || !parseInputKind(kind, input.kind))
        {
            error = context + ": kind must be button or encoder";
            return false;
        }
        const json::Value* bindings = entry.find("bindings");
        if (bindings == nullptr ||
            !inputBindingsFromJson(*bindings, input.kind, input.bindings, error))
        {
            error = context + ": " + (bindings == nullptr ? "bindings is missing" : error);
            return false;
        }
        inputs.push_back(input);
    }

    bool canDeepSleep = false;
    if (payload.find("deepSleep") != nullptr && !json::getBool(payload, "deepSleep", canDeepSleep))
    {
        error = "deepSleep must be a boolean";
        return false;
    }

    sessionId = session;
    out = inputs;
    deepSleep = canDeepSleep;
    return true;
}

json::Value makeInputAction(const std::string& sessionId, const InputActionEvent& event)
{
    json::Value root = json::Value::makeObject();
    root["type"] = json::Value(messageTypeName(MessageType::InputAction));
    root["sessionId"] = json::Value(sessionId);
    root["inputId"] = json::Value(event.inputId);
    root["gesture"] = json::Value(inputGestureName(event.gesture));
    bindingToJson(event.binding, root);
    root["count"] = json::Value(event.count);
    return root;
}

bool parseInputAction(const json::Value& payload, std::string& sessionId, InputActionEvent& out,
                      std::string& error)
{
    std::string session;
    if (!sessionIdFromJson(payload, true, session, error))
        return false;

    InputActionEvent event;
    if (!inputIdFromJson(payload, "inputId", event.inputId, error))
        return false;
    std::string gesture;
    if (!json::getString(payload, "gesture", gesture) || !parseInputGesture(gesture, event.gesture))
    {
        error = "gesture must be one of click, doubleClick, longPress, clockwise, "
                "counterClockwise";
        return false;
    }

    int64_t count = 0;
    if (!json::getInt(payload, "count", count) || count < 1 ||
        count > static_cast<int64_t>(kMaxInputActionCount))
    {
        error = "count must be a whole number from 1 to " + std::to_string(kMaxInputActionCount);
        return false;
    }
    event.count = static_cast<uint32_t>(count);

    // The binding's own fields sit beside the message's; the checker sees
    // only them, so a misspelled parameter is still refused.
    json::Value binding = json::Value::makeObject();
    for (const json::Value::Member& member : payload.members())
    {
        if (member.first != "type" && member.first != "sessionId" && member.first != "inputId" &&
            member.first != "gesture" && member.first != "count")
            binding[member.first] = member.second;
    }
    if (!bindingFromJson(binding, event.binding, "inputAction", error))
        return false;
    if (event.binding.action == InputAction::None)
    {
        error = "inputAction: none is not an action anyone sends";
        return false;
    }

    sessionId = session;
    out = event;
    return true;
}

json::Value makeSetInputBindings(const SetInputBindings& request)
{
    json::Value root = json::Value::makeObject();
    root["type"] = json::Value(messageTypeName(MessageType::SetInputBindings));
    if (!request.sessionId.empty())
        root["sessionId"] = json::Value(request.sessionId);
    root["inputId"] = json::Value(request.inputId);
    root["bindings"] = request.bindings;
    return root;
}

bool parseSetInputBindings(const json::Value& payload, SetInputBindings& out, std::string& error)
{
    SetInputBindings request;
    if (!sessionIdFromJson(payload, false, request.sessionId, error))
        return false;
    if (!inputIdFromJson(payload, "inputId", request.inputId, error))
        return false;
    const json::Value* bindings = payload.find("bindings");
    if (bindings == nullptr || !bindings->isObject())
    {
        error = "bindings must be an object";
        return false;
    }
    request.bindings = *bindings;

    out = request;
    return true;
}

} // namespace matp
} // namespace multiroom
