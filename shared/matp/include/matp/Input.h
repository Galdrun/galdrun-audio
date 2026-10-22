#pragma once

// Hardware controls (MATP section 57): the buttons and rotary encoders wired
// to a room's machine, what each gesture on them is bound to, and the three
// messages that carry them.
//
// Shared for the reason Protocol.h is: the client validates a binding before
// it persists it and the core validates one before it forwards it, and two
// copies of "which actions take a step" would disagree the first time one of
// them changed.
//
// The model is deliberately closed. Gestures and actions are fixed sets of
// stable strings, not free text, because the server switches on them; a
// binding carries only the parameter its action takes, and a parameter an
// action does not take is an error rather than something ignored.

#include "util/Json.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace multiroom {
namespace matp {

enum class InputKind
{
    Button,
    Encoder
};

// A button has the first three, an encoder the last two. An encoder's push
// switch is a separate input of kind Button.
enum class InputGesture
{
    Click,
    DoubleClick,
    LongPress,
    Clockwise,
    CounterClockwise
};

const size_t kInputGestureCount = 5;

enum class InputAction
{
    None,
    Play,
    Pause,
    TogglePause,
    Stop,
    Next,
    Previous,
    VolumeUp,
    VolumeDown,
    SetVolume,
    ToggleMute,
    StandbyOff,
    StandbyOn,
    ToggleStandby,
    // User actions: the server does nothing with them but announce them as
    // room.input, for whatever a tinkerer attaches to that event.
    Action1,
    Action2,
    Action3,
    Action4,
    Action5,
    Action6,
    Action7,
    Action8,
    Action9,
    Action10,
    DeepSleep
};

// Which volume a volume action or a mute changes: the room's own, or its
// zone's factor over every member (the server's desired state has both).
enum class VolumeScope
{
    Room,
    Zone
};

const char* inputKindName(InputKind kind);
bool parseInputKind(const std::string& name, InputKind& out);
const char* inputGestureName(InputGesture gesture);
bool parseInputGesture(const std::string& name, InputGesture& out);
const char* inputActionName(InputAction action);
bool parseInputAction(const std::string& name, InputAction& out);
const char* volumeScopeName(VolumeScope scope);
bool parseVolumeScope(const std::string& name, VolumeScope& out);

bool gestureAppliesTo(InputKind kind, InputGesture gesture);

// What an action takes: VolumeUp and VolumeDown a step, SetVolume a value,
// those three and ToggleMute a scope.
bool actionTakesStep(InputAction action);
bool actionTakesValue(InputAction action);
bool actionTakesScope(InputAction action);

const double kDefaultVolumeStep = 0.05;

struct InputBinding
{
    InputAction action = InputAction::None;
    double step = kDefaultVolumeStep; // VolumeUp / VolumeDown, (0, 1]
    double value = 0.0;               // SetVolume, [0, 1]
    VolumeScope scope = VolumeScope::Room;

    bool operator==(const InputBinding& other) const;
    bool operator!=(const InputBinding& other) const { return !(*this == other); }
};

// Indexed by InputGesture. The entries a kind does not have stay None.
using InputBindings = std::array<InputBinding, kInputGestureCount>;

// What an input does when nothing configured it: a button clicks
// TogglePause, an encoder turns the room's volume by 0.02 a detent.
InputBindings defaultInputBindings(InputKind kind);

// Every gesture the kind has, each as {"action":...} plus the parameters its
// action takes. None is written out rather than left out, so a UI sees every
// gesture there is to bind.
json::Value inputBindingsToJson(InputKind kind, const InputBindings& bindings);

// Merges `object` into `inOut`: each key is a gesture of this kind, each value
// a binding. A gesture the object leaves out keeps what it had. An unknown
// gesture, a gesture of the other kind, an unknown action, a parameter the
// action does not take and a value out of range are errors, and leave `inOut`
// untouched.
bool inputBindingsFromJson(const json::Value& object, InputKind kind, InputBindings& inOut,
                           std::string& error);

// 1 to 64 bytes, no control characters, no surrounding blanks: it is a key in
// client.json, in the server's UI and in an event.
bool isValidInputId(const std::string& id);

// A room with more than this has not wired buttons, it has wired a keyboard.
const size_t kMaxInputs = 32;

struct InputControl
{
    std::string id;
    InputKind kind = InputKind::Button;
    InputBindings bindings{};
};

// INPUT_CONTROLS (type 27), client -> server: every input the room has and
// what each gesture on it is bound to. Sent once the session is established
// and again whenever a binding changes; a room with no inputs sends none.
// `deepSleep` says whether the client can run that action; absent is false.
json::Value makeInputControls(const std::string& sessionId, const std::vector<InputControl>& inputs,
                              bool deepSleep = false);
bool parseInputControls(const json::Value& payload, std::string& sessionId,
                        std::vector<InputControl>& out, bool& deepSleep, std::string& error);

// INPUT_ACTION (type 28), client -> server: a gesture happened and this is
// what it is bound to. `count` is how many times -- detents of an encoder,
// coalesced; 1 for a button -- so a volume step is `step * count`. Only the
// parameters the action takes are on the wire.
struct InputActionEvent
{
    std::string inputId;
    InputGesture gesture = InputGesture::Click;
    InputBinding binding;
    uint32_t count = 1;
};

// A burst of encoder detents is coalesced into one message; this is far more
// than a hand turns in the 50 ms between two.
const uint32_t kMaxInputActionCount = 1000;

json::Value makeInputAction(const std::string& sessionId, const InputActionEvent& event);
bool parseInputAction(const json::Value& payload, std::string& sessionId, InputActionEvent& out,
                      std::string& error);

// SET_INPUT_BINDINGS (type 29), server -> client: rebind some gestures of one
// input. `bindings` is the object inputBindingsFromJson reads, kept as JSON
// here because only the receiver knows the input's kind. The client persists
// the change and answers with INPUT_CONTROLS, or with ERROR (code
// `setInputBindings`) when it refuses; the session stays up either way.
struct SetInputBindings
{
    std::string sessionId; // empty when the server did not say
    std::string inputId;
    json::Value bindings; // an object
};

json::Value makeSetInputBindings(const SetInputBindings& request);
bool parseSetInputBindings(const json::Value& payload, SetInputBindings& out, std::string& error);

} // namespace matp
} // namespace multiroom
