#include "input/InputController.h"

#include <algorithm>
#include <utility>

namespace multiroom {
namespace {

size_t gestureIndex(matp::InputGesture gesture)
{
    return static_cast<size_t>(gesture);
}

// The main loop reads the clock before it takes the queue, so an event the
// backend stamped a moment later can be "in the future"; that is no time
// passed, not four hundred years.
uint64_t elapsed(uint64_t fromNs, uint64_t toNs)
{
    return toNs > fromNs ? toNs - fromNs : 0;
}

} // namespace

InputEventQueue::InputEventQueue()
{
    events_.reserve(kCapacity);
}

void InputEventQueue::setWake(std::function<void()> wake)
{
    std::lock_guard<std::mutex> lock(mutex_);
    wake_ = std::move(wake);
}

void InputEventQueue::push(const RawInputEvent& event)
{
    std::function<void()> wake;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (events_.size() >= kCapacity)
            mergeDetents();
        if (events_.size() >= kCapacity)
        {
            ++dropped_;
            return;
        }
        events_.push_back(event);
        wake = wake_;
    }
    if (wake)
        wake();
}

// A full queue is almost always an encoder turned fast while the main loop was
// busy. A run of its detents in one direction is one action however it is
// split, so each run is summed into its first event, which makes room for the
// button levels that cannot be summed. A reversal is not merged: feed() sends
// each direction as its own action, and up then down is not nothing.
void InputEventQueue::mergeDetents()
{
    size_t kept = 0;
    for (size_t i = 0; i < events_.size(); ++i)
    {
        const RawInputEvent& event = events_[i];
        if (event.type == RawInputEvent::Type::Detents)
        {
            // This encoder's previous event, wherever other inputs' are.
            size_t last = kept;
            while (last > 0 && events_[last - 1].index != event.index)
                --last;
            if (last > 0)
            {
                RawInputEvent& run = events_[last - 1];
                const int64_t sum = static_cast<int64_t>(run.detents) + event.detents;
                if (run.type == RawInputEvent::Type::Detents &&
                    (run.detents > 0) == (event.detents > 0) && sum >= INT32_MIN &&
                    sum <= INT32_MAX)
                {
                    run.detents = static_cast<int32_t>(sum);
                    run.atNs = event.atNs;
                    continue;
                }
            }
        }
        events_[kept++] = event;
    }
    events_.resize(kept);
}

void InputEventQueue::take(std::vector<RawInputEvent>& out)
{
    out.clear();
    std::lock_guard<std::mutex> lock(mutex_);
    out.swap(events_);
    // The backend thread then pushes into what the main thread just emptied,
    // and neither side allocates once both have grown to size.
    if (events_.capacity() < kCapacity)
        events_.reserve(kCapacity);
}

uint32_t InputEventQueue::dropped() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return dropped_;
}

InputController::InputController(const std::vector<InputDefinition>& inputs)
    : InputController(inputs, Timing())
{
}

InputController::InputController(const std::vector<InputDefinition>& inputs,
                                 const Timing& timing)
    : inputs_(inputs), timing_(timing)
{
}

InputController::State& InputController::state(size_t index)
{
    if (states_.size() < inputs_.size())
        states_.resize(inputs_.size());
    return states_[index];
}

bool InputController::pendingRaw(const State& state) const
{
    return state.rawPressed != state.stablePressed;
}

bool InputController::bound(size_t index, matp::InputGesture gesture) const
{
    return inputs_[index].control.bindings[gestureIndex(gesture)].action !=
           matp::InputAction::None;
}

void InputController::feed(const RawInputEvent& event, std::vector<matp::InputActionEvent>& out)
{
    if (event.index >= inputs_.size())
        return;
    const size_t index = event.index;
    State& s = state(index);
    const bool button = inputs_[index].control.kind == matp::InputKind::Button;

    if (event.type == RawInputEvent::Type::Level && button && event.resync)
    {
        // Whatever happened since the last level seen is unknown: a press
        // held since then may have been released and pressed again. Neither
        // a long press nor a click is decided from that; the level is simply
        // taken, and a held button does nothing until it is let go.
        s.phase = event.pressed ? Phase::IgnoreUntilRelease : Phase::Idle;
        s.phaseSinceNs = event.atNs;
        s.rawPressed = event.pressed;
        s.rawSinceNs = event.atNs;
        if (s.stablePressed != event.pressed)
        {
            s.stablePressed = event.pressed;
            if (buttonStateCallback_)
                buttonStateCallback_(index, event.pressed);
        }
        return;
    }

    if (event.type == RawInputEvent::Type::Level && button)
    {
        // Whatever the line settled on before this change is decided first,
        // at the time it settled.
        settle(index, event.atNs, out);
        if (event.initialState && event.pressed &&
            inputs_[index].control.bindings[gestureIndex(matp::InputGesture::LongPress)].action ==
                matp::InputAction::DeepSleep)
            s.phase = Phase::IgnoreUntilRelease;
        if (event.pressed == s.rawPressed)
            return;
        s.rawPressed = event.pressed;
        s.rawSinceNs = event.atNs;
        return;
    }

    if (event.type == RawInputEvent::Type::Detents && !button && event.detents != 0)
    {
        // A change of direction sends what was pending at once: summing the
        // two would report a knob turned back and forth as barely moved.
        if ((s.pendingDetents > 0) != (event.detents > 0) && s.pendingDetents != 0)
            flushDetents(index, event.atNs, out);
        s.pendingDetents += event.detents;
    }
}

void InputController::poll(uint64_t nowNs, std::vector<matp::InputActionEvent>& out)
{
    for (size_t index = 0; index < inputs_.size(); ++index)
    {
        State& s = state(index);
        if (inputs_[index].control.kind == matp::InputKind::Button)
        {
            settle(index, nowNs, out);
            // While a change is still bouncing, the level is only known up to
            // where it began: a release 5 ms before the long-press deadline
            // must not become a long press because its debounce ends after it.
            advance(index, pendingRaw(s) ? std::min(nowNs, s.rawSinceNs) : nowNs, out);
        }
        else if (s.pendingDetents != 0 &&
                 (!s.sentBefore || elapsed(s.lastSentNs, nowNs) >= timing_.encoderCoalesceNs))
        {
            flushDetents(index, nowNs, out);
        }
    }
}

uint32_t InputController::waitMs(uint64_t nowNs, uint32_t maxMs) const
{
    uint64_t waitNs = static_cast<uint64_t>(maxMs) * 1000000;
    const auto until = [&](uint64_t deadlineNs) {
        waitNs = std::min(waitNs, deadlineNs > nowNs ? deadlineNs - nowNs : 0);
    };

    for (size_t index = 0; index < inputs_.size() && index < states_.size(); ++index)
    {
        const State& s = states_[index];
        if (pendingRaw(s))
            until(s.rawSinceNs + timing_.debounceNs);
        if ((s.phase == Phase::Down || s.phase == Phase::SecondDown) &&
            bound(index, matp::InputGesture::LongPress))
            until(s.phaseSinceNs + timing_.longPressNs);
        if (s.phase == Phase::WaitSecond)
            until(s.phaseSinceNs + timing_.doubleClickNs);
        if (s.pendingDetents != 0)
            until(s.sentBefore ? s.lastSentNs + timing_.encoderCoalesceNs : nowNs);
    }
    // Rounded up, so the loop never wakes a fraction early and sleeps again.
    return static_cast<uint32_t>((waitNs + 999999) / 1000000);
}

void InputController::eventsLost()
{
    for (size_t index = 0; index < inputs_.size(); ++index)
    {
        if (inputs_[index].control.kind != matp::InputKind::Button)
            continue;
        // Held, as far as this knows: nothing until the next release. Up: the
        // click a second press may have followed is not sent.
        State& s = state(index);
        s.phase = s.rawPressed || s.stablePressed ? Phase::IgnoreUntilRelease : Phase::Idle;
    }
}

// A line that has held its new level for the debounce time is a real edge, at
// the moment it first changed.
void InputController::settle(size_t index, uint64_t nowNs, std::vector<matp::InputActionEvent>& out)
{
    State& s = state(index);
    if (pendingRaw(s) && elapsed(s.rawSinceNs, nowNs) >= timing_.debounceNs)
        edge(index, s.rawPressed, s.rawSinceNs, out);
}

// The gestures time alone decides: a hold that has become a long press, a
// single click no second press followed. A second press held that long is a
// long press too, not a double click, and its release ends it.
void InputController::advance(size_t index, uint64_t untilNs,
                              std::vector<matp::InputActionEvent>& out)
{
    State& s = state(index);
    if (untilNs < s.phaseSinceNs)
        return;
    const uint64_t elapsedNs = untilNs - s.phaseSinceNs;

    if ((s.phase == Phase::Down || s.phase == Phase::SecondDown) &&
        bound(index, matp::InputGesture::LongPress) && elapsedNs >= timing_.longPressNs)
    {
        emit(index, matp::InputGesture::LongPress, 1, out);
        s.phase = Phase::LongFired;
        s.phaseSinceNs = s.phaseSinceNs + timing_.longPressNs;
    }
    else if (s.phase == Phase::WaitSecond && elapsedNs >= timing_.doubleClickNs)
    {
        emit(index, matp::InputGesture::Click, 1, out);
        s.phase = Phase::Idle;
        s.phaseSinceNs = s.phaseSinceNs + timing_.doubleClickNs;
    }
}

void InputController::edge(size_t index, bool pressed, uint64_t atNs,
                           std::vector<matp::InputActionEvent>& out)
{
    advance(index, atNs, out);
    State& s = state(index);
    s.stablePressed = pressed;
    if (buttonStateCallback_)
        buttonStateCallback_(index, pressed);

    switch (s.phase)
    {
    case Phase::Idle:
        if (pressed)
        {
            s.phase = Phase::Down;
            s.phaseSinceNs = atNs;
        }
        break;

    case Phase::Down:
        if (pressed)
            break;
        // The wait for a second press is only taken when a double click
        // means something; otherwise it would delay every click for nothing.
        if (bound(index, matp::InputGesture::DoubleClick))
        {
            s.phase = Phase::WaitSecond;
        }
        else
        {
            emit(index, matp::InputGesture::Click, 1, out);
            s.phase = Phase::Idle;
        }
        s.phaseSinceNs = atNs;
        break;

    case Phase::WaitSecond:
        if (pressed)
        {
            s.phase = Phase::SecondDown;
            s.phaseSinceNs = atNs;
        }
        break;

    case Phase::SecondDown:
        if (!pressed)
        {
            emit(index, matp::InputGesture::DoubleClick, 1, out);
            s.phase = Phase::Idle;
            s.phaseSinceNs = atNs;
        }
        break;

    case Phase::IgnoreUntilRelease:
    case Phase::LongFired:
        if (!pressed)
        {
            s.phase = Phase::Idle;
            s.phaseSinceNs = atNs;
        }
        break;
    }
}

void InputController::flushDetents(size_t index, uint64_t nowNs,
                                   std::vector<matp::InputActionEvent>& out)
{
    State& s = state(index);
    if (s.pendingDetents == 0)
        return;
    const matp::InputGesture gesture = s.pendingDetents > 0
                                           ? matp::InputGesture::Clockwise
                                           : matp::InputGesture::CounterClockwise;
    const int64_t magnitude = s.pendingDetents > 0 ? s.pendingDetents : -s.pendingDetents;
    emit(index, gesture,
         static_cast<uint32_t>(std::min<int64_t>(magnitude, matp::kMaxInputActionCount)), out);
    s.pendingDetents = 0;
    s.sentBefore = true;
    s.lastSentNs = nowNs;
}

void InputController::emit(size_t index, matp::InputGesture gesture, uint32_t count,
                           std::vector<matp::InputActionEvent>& out) const
{
    const InputDefinition& input = inputs_[index];
    const matp::InputBinding& binding = input.control.bindings[gestureIndex(gesture)];
    if (binding.action == matp::InputAction::None)
        return;
    matp::InputActionEvent event;
    event.inputId = input.control.id;
    event.gesture = gesture;
    event.binding = binding;
    event.count = count;
    out.push_back(event);
}

bool resolveInputAction(const std::vector<InputDefinition>& inputs, const std::string& id,
                        matp::InputGesture gesture, uint32_t count,
                        matp::InputActionEvent& out, std::string& error)
{
    for (const InputDefinition& input : inputs)
    {
        if (input.control.id != id)
            continue;
        if (!matp::gestureAppliesTo(input.control.kind, gesture))
        {
            error = "an input of kind " + std::string(matp::inputKindName(input.control.kind)) +
                    " has no gesture " + matp::inputGestureName(gesture);
            return false;
        }
        const matp::InputBinding& binding = input.control.bindings[gestureIndex(gesture)];
        if (binding.action == matp::InputAction::None)
        {
            error = id + " " + matp::inputGestureName(gesture) + " is bound to none";
            return false;
        }
        if (count < 1 || count > matp::kMaxInputActionCount)
        {
            error = "count must be from 1 to " + std::to_string(matp::kMaxInputActionCount);
            return false;
        }
        out.inputId = id;
        out.gesture = gesture;
        out.binding = binding;
        out.count = count;
        return true;
    }
    error = "no input " + id;
    return false;
}

} // namespace multiroom
