#pragma once

// Buttons and rotary encoders (phase 23, MATP section 57): from the raw line
// changes a GPIO backend reads to the actions the server is sent.
//
// Split across two threads on purpose, and the split is the reason this
// header has two classes:
//
//   * **The backend thread** only reads lines and posts what it saw, time
//     stamped, into an InputEventQueue. It decides nothing, so it holds
//     nothing a binding change would have to reach.
//   * **The main thread** does everything else in InputController: debounce,
//     click / double click / long press, coalescing encoder detents, and
//     looking up what a gesture is bound to. It owns the configuration and the
//     session already, so the bindings need no lock and a SET_INPUT_BINDINGS
//     takes effect on the next gesture.
//
// The audio thread is never involved.
//
// Every decision is made on the time stamps the backend took, never on when
// the main loop got round to looking. A loop busy with a rescan for two
// seconds delays a gesture; it does not turn a click into a long press.

#include "config/ClientConfig.h"
#include "matp/Input.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace multiroom {

struct RawInputEvent
{
    enum class Type : uint8_t
    {
        Level,  // a button line changed; `pressed` is its new state
        Detents // an encoder turned; `detents` is signed, clockwise positive
    };

    Type type = Type::Level;
    uint16_t index = 0; // into ClientConfig::inputs
    bool pressed = false;
    bool initialState = false; // a level already held when the backend opened
    // A level read back after the backend lost events: what the line is now,
    // with no edge known in between. It ends any gesture in progress rather
    // than being timed as one.
    bool resync = false;
    int32_t detents = 0;
    uint64_t atNs = 0; // monotonicNowNs() when the backend saw it
};

// The one thing the backend thread and the main thread share.
class InputEventQueue
{
public:
    // Bounces included, a press is a few dozen edges, and the main loop takes
    // them every pass; this holds several seconds of a busy loop.
    static const size_t kCapacity = 1024;

    InputEventQueue();

    // Called after every push, on the backend thread: how the main loop is
    // woken instead of finding the event at its next idle timeout. Set before
    // the backend starts.
    void setWake(std::function<void()> wake);

    // Backend thread. A full queue sums each encoder's queued detents to make
    // room; one that is still full drops the event and counts it.
    void push(const RawInputEvent& event);

    // Main thread: everything posted so far, oldest first.
    void take(std::vector<RawInputEvent>& out);

    uint32_t dropped() const;

private:
    void mergeDetents();

    mutable std::mutex mutex_;
    std::vector<RawInputEvent> events_;
    uint32_t dropped_ = 0;
    std::function<void()> wake_;
};

class InputController
{
public:
    struct Timing
    {
        uint64_t debounceNs = 20000000;      // a line must hold this long to count
        uint64_t longPressNs = 2000000000;   // held two seconds is a long press
        uint64_t doubleClickNs = 300000000;  // a second press within this is a double click
        uint64_t encoderCoalesceNs = 50000000; // detents are sent at most this often
    };

    // `inputs` is the configuration's own list and must outlive this object.
    // Its bindings are read at every gesture; its length and order must not
    // change, because the backend's indices point into it.
    explicit InputController(const std::vector<InputDefinition>& inputs);
    InputController(const std::vector<InputDefinition>& inputs, const Timing& timing);

    // Main thread: one event from the backend, in the order they were posted.
    void feed(const RawInputEvent& event, std::vector<matp::InputActionEvent>& out);

    // Main thread: the gestures that have become certain by `nowNs`, resolved
    // to what they are bound to. A gesture bound to none is dropped here.
    void poll(uint64_t nowNs, std::vector<matp::InputActionEvent>& out);

    // How long the main loop may sleep before poll() has something to decide,
    // capped at `maxMs`.
    uint32_t waitMs(uint64_t nowNs, uint32_t maxMs) const;

    // Main thread: the queue dropped events, so a button's level may be one
    // it no longer has. Its pending gesture is dropped rather than decided:
    // a release that never arrived must not become a long press.
    void eventsLost();

    // Debounced level changes, including buttons with no assigned action.
    void setButtonStateCallback(std::function<void(size_t, bool)> callback)
    {
        buttonStateCallback_ = callback;
    }

private:
    enum class Phase
    {
        Idle,
        Down,       // pressed once
        WaitSecond, // released, a double click still possible
        SecondDown, // pressed the second time
        LongFired,   // the long press has been sent; the release ends it
        IgnoreUntilRelease // consume a deep-sleep button held during restart/wake
    };

    struct State
    {
        // Debounce: the level last seen on the line and since when, against
        // the level last accepted.
        bool rawPressed = false;
        uint64_t rawSinceNs = 0;
        bool stablePressed = false;

        Phase phase = Phase::Idle;
        uint64_t phaseSinceNs = 0;

        // Encoder: detents not sent yet, and when the last batch went out.
        int64_t pendingDetents = 0;
        bool sentBefore = false;
        uint64_t lastSentNs = 0;
    };

    bool pendingRaw(const State& state) const;
    bool bound(size_t index, matp::InputGesture gesture) const;
    void settle(size_t index, uint64_t nowNs, std::vector<matp::InputActionEvent>& out);
    void advance(size_t index, uint64_t untilNs, std::vector<matp::InputActionEvent>& out);
    void edge(size_t index, bool pressed, uint64_t atNs, std::vector<matp::InputActionEvent>& out);
    void flushDetents(size_t index, uint64_t nowNs, std::vector<matp::InputActionEvent>& out);
    void emit(size_t index, matp::InputGesture gesture, uint32_t count,
              std::vector<matp::InputActionEvent>& out) const;
    State& state(size_t index);

    const std::vector<InputDefinition>& inputs_;
    Timing timing_;
    std::vector<State> states_;
    std::function<void(size_t, bool)> buttonStateCallback_;
};

// What `gesture` on the input named `id` is bound to, as the action a real
// gesture would send: the control API's simulateInput, which is how the
// whole path is driven on a machine without GPIOs. False with a reason for
// an unknown input, a gesture its kind does not have, or a gesture bound to
// none.
bool resolveInputAction(const std::vector<InputDefinition>& inputs, const std::string& id,
                        matp::InputGesture gesture, uint32_t count,
                        matp::InputActionEvent& out, std::string& error);

} // namespace multiroom
