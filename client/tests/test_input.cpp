#include "TestSupport.h"

#include "config/ClientConfig.h"
#include "input/InputController.h"
#include "matp/Input.h"
#include "matp/Protocol.h"

#include <string>
#include <vector>

using namespace multiroom;

MULTIROOM_TEST(power_input_actions_roundtrip_and_take_no_volume_parameters)
{
    for (const char* name : {"standbyOff", "standbyOn", "toggleStandby", "deepSleep"})
    {
        matp::InputAction action;
        CHECK(matp::parseInputAction(name, action));
        CHECK_EQ(std::string(matp::inputActionName(action)), std::string(name));
        CHECK(!matp::actionTakesScope(action));
        CHECK(!matp::actionTakesValue(action));
        CHECK(!matp::actionTakesStep(action));
        json::Value value;
        std::string error;
        CHECK(json::parse(std::string("{\"click\":{\"action\":\"") + name + "\"}}", value, error));
        matp::InputBindings bindings;
        CHECK(matp::inputBindingsFromJson(value, matp::InputKind::Button, bindings, error));
        CHECK(bindings[static_cast<size_t>(matp::InputGesture::Click)].action == action);
    }
}

namespace {

const uint64_t kMs = 1000000;

json::Value parsed(const std::string& text)
{
    json::Value value;
    std::string error;
    CHECK(json::parse(text, value, error));
    return value;
}

InputDefinition button(const std::string& id, int pin)
{
    InputDefinition input;
    input.control.id = id;
    input.control.kind = matp::InputKind::Button;
    input.control.bindings = matp::defaultInputBindings(matp::InputKind::Button);
    input.pin = pin;
    return input;
}

InputDefinition encoder(const std::string& id)
{
    InputDefinition input;
    input.control.id = id;
    input.control.kind = matp::InputKind::Encoder;
    input.control.bindings = matp::defaultInputBindings(matp::InputKind::Encoder);
    input.pinA = 5;
    input.pinB = 6;
    return input;
}

void bind(InputDefinition& input, matp::InputGesture gesture, matp::InputAction action)
{
    input.control.bindings[static_cast<size_t>(gesture)].action = action;
}

RawInputEvent level(uint16_t index, bool pressed, uint64_t atMs)
{
    RawInputEvent event;
    event.type = RawInputEvent::Type::Level;
    event.index = index;
    event.pressed = pressed;
    event.atNs = atMs * kMs;
    return event;
}

RawInputEvent detents(uint16_t index, int32_t count, uint64_t atMs)
{
    RawInputEvent event;
    event.type = RawInputEvent::Type::Detents;
    event.index = index;
    event.detents = count;
    event.atNs = atMs * kMs;
    return event;
}

// Feeds the events and polls at every millisecond up to `untilMs`, the way
// the main loop would if it woke that often.
std::vector<matp::InputActionEvent> run(InputController& controller,
                                        const std::vector<RawInputEvent>& events,
                                        uint64_t untilMs)
{
    std::vector<matp::InputActionEvent> out;
    size_t next = 0;
    for (uint64_t ms = 0; ms <= untilMs; ++ms)
    {
        while (next < events.size() && events[next].atNs <= ms * kMs)
            controller.feed(events[next++], out);
        controller.poll(ms * kMs, out);
    }
    return out;
}

} // namespace

MULTIROOM_TEST(deep_sleep_fires_after_two_seconds_while_held_and_consumes_a_held_wake_button)
{
    std::vector<InputDefinition> inputs{button("POWER", 6)};
    bind(inputs[0], matp::InputGesture::Click, matp::InputAction::ToggleStandby);
    bind(inputs[0], matp::InputGesture::LongPress, matp::InputAction::DeepSleep);
    InputController controller(inputs);
    std::vector<matp::InputActionEvent> actions;
    controller.feed(level(0, true, 0), actions);
    controller.poll(1999 * kMs, actions);
    CHECK(actions.empty());
    CHECK_EQ(controller.waitMs(1999 * kMs, 100), static_cast<uint32_t>(1));
    controller.poll(2000 * kMs, actions); // Trigger while still held, without release.
    CHECK_EQ(actions.size(), static_cast<size_t>(1));
    CHECK(actions[0].gesture == matp::InputGesture::LongPress);
    CHECK(actions[0].binding.action == matp::InputAction::DeepSleep);
    controller.poll(3000 * kMs, actions);
    CHECK_EQ(actions.size(), static_cast<size_t>(1));
    controller.feed(level(0, false, 3100), actions);
    controller.poll(3120 * kMs, actions);
    CHECK_EQ(actions.size(), static_cast<size_t>(1)); // Release does not toggle power.

    InputController resumed(inputs);
    actions.clear();
    auto wake = level(0, true, 0);
    wake.initialState = true;
    resumed.feed(wake, actions);
    resumed.poll(2000 * kMs, actions);
    resumed.feed(level(0, false, 2100), actions);
    resumed.poll(2120 * kMs, actions);
    CHECK(actions.empty()); // Neither another sleep nor the normal click toggle.
    resumed.feed(level(0, true, 2200), actions);
    resumed.poll(2220 * kMs, actions);
    resumed.feed(level(0, false, 2300), actions);
    resumed.poll(2320 * kMs, actions);
    CHECK_EQ(actions.size(), static_cast<size_t>(1));
    CHECK(actions[0].binding.action == matp::InputAction::ToggleStandby);
}

MULTIROOM_TEST(input_bindings_round_trip_and_default_per_kind)
{
    const matp::InputBindings buttons = matp::defaultInputBindings(matp::InputKind::Button);
    const json::Value written = matp::inputBindingsToJson(matp::InputKind::Button, buttons);
    CHECK_EQ(json::serialize(written),
             std::string("{\"click\":{\"action\":\"togglePause\"},\"doubleClick\":{\"action\":"
                         "\"none\"},\"longPress\":{\"action\":\"none\"}}"));

    matp::InputBindings read{};
    std::string error;
    CHECK(matp::inputBindingsFromJson(written, matp::InputKind::Button, read, error));
    CHECK(read == buttons);

    const json::Value knob = matp::inputBindingsToJson(
        matp::InputKind::Encoder, matp::defaultInputBindings(matp::InputKind::Encoder));
    CHECK_EQ(json::serialize(knob),
             std::string("{\"clockwise\":{\"action\":\"volumeUp\",\"step\":0.02,\"scope\":"
                         "\"room\"},\"counterClockwise\":{\"action\":\"volumeDown\",\"step\":"
                         "0.02,\"scope\":\"room\"}}"));
}

MULTIROOM_TEST(input_bindings_merge_and_take_parameters_per_action)
{
    matp::InputBindings bindings = matp::defaultInputBindings(matp::InputKind::Button);
    std::string error;
    CHECK(matp::inputBindingsFromJson(
        parsed("{\"longPress\":{\"action\":\"setVolume\",\"value\":0.3,\"scope\":\"zone\"},"
               "\"doubleClick\":{\"action\":\"next\"}}"),
        matp::InputKind::Button, bindings, error));

    const matp::InputBinding& click = bindings[static_cast<size_t>(matp::InputGesture::Click)];
    const matp::InputBinding& hold = bindings[static_cast<size_t>(matp::InputGesture::LongPress)];
    CHECK(click.action == matp::InputAction::TogglePause); // left out, kept
    CHECK(hold.action == matp::InputAction::SetVolume);
    CHECK_NEAR(hold.value, 0.3, 1e-9);
    CHECK(hold.scope == matp::VolumeScope::Zone);
    CHECK(bindings[static_cast<size_t>(matp::InputGesture::DoubleClick)].action ==
          matp::InputAction::Next);
}

MULTIROOM_TEST(input_user_actions_take_nothing_and_travel_as_input_actions)
{
    matp::InputBindings bindings = matp::defaultInputBindings(matp::InputKind::Button);
    std::string error;
    CHECK(matp::inputBindingsFromJson(parsed("{\"click\":{\"action\":\"action10\"}}"),
                                      matp::InputKind::Button, bindings, error));
    CHECK(bindings[static_cast<size_t>(matp::InputGesture::Click)].action ==
          matp::InputAction::Action10);
    CHECK(!matp::inputBindingsFromJson(
        parsed("{\"click\":{\"action\":\"action1\",\"scope\":\"room\"}}"),
        matp::InputKind::Button, bindings, error));
    CHECK(!matp::inputBindingsFromJson(parsed("{\"click\":{\"action\":\"action11\"}}"),
                                       matp::InputKind::Button, bindings, error));

    matp::InputActionEvent sent;
    sent.inputId = "play";
    sent.binding.action = matp::InputAction::Action3;
    std::string session;
    matp::InputActionEvent received;
    CHECK(matp::parseInputAction(matp::makeInputAction("s-1", sent), session, received, error));
    CHECK(received.binding.action == matp::InputAction::Action3);
}

MULTIROOM_TEST(input_bindings_refuse_what_the_kind_or_action_does_not_have)
{
    const char* refused[] = {
        "{\"clockwise\":{\"action\":\"next\"}}",                   // an encoder's gesture
        "{\"tripleClick\":{\"action\":\"next\"}}",                 // no such gesture
        "{\"click\":{\"action\":\"shuffle\"}}",                    // no such action
        "{\"click\":{\"action\":\"next\",\"step\":0.1}}",          // next takes no step
        "{\"click\":{\"action\":\"setVolume\"}}",                  // setVolume needs a value
        "{\"click\":{\"action\":\"setVolume\",\"value\":1.5}}",    // out of range
        "{\"click\":{\"action\":\"volumeUp\",\"step\":0}}",        // a step of nothing
        "{\"click\":{\"action\":\"toggleMute\",\"scope\":\"house\"}}",
        "{\"click\":\"next\"}",
    };
    for (const char* text : refused)
    {
        matp::InputBindings bindings = matp::defaultInputBindings(matp::InputKind::Button);
        const matp::InputBindings before = bindings;
        std::string error;
        CHECK(!matp::inputBindingsFromJson(parsed(text), matp::InputKind::Button, bindings, error));
        CHECK(!error.empty());
        CHECK(bindings == before);
    }
}

MULTIROOM_TEST(input_messages_round_trip)
{
    std::vector<matp::InputControl> inputs(2);
    inputs[0].id = "play";
    inputs[0].bindings = matp::defaultInputBindings(matp::InputKind::Button);
    inputs[1].id = "knob";
    inputs[1].kind = matp::InputKind::Encoder;
    inputs[1].bindings = matp::defaultInputBindings(matp::InputKind::Encoder);

    std::string sessionId;
    std::vector<matp::InputControl> readInputs;
    std::string error;
    bool deepSleep = true;
    const json::Value controls = matp::makeInputControls("s-1", inputs);
    CHECK_EQ(json::serialize(*controls.find("type")), std::string("\"inputControls\""));
    CHECK(controls.find("deepSleep") == nullptr);
    CHECK(matp::parseInputControls(controls, sessionId, readInputs, deepSleep, error));
    CHECK(!deepSleep);
    CHECK(matp::parseInputControls(matp::makeInputControls("s-1", inputs, true), sessionId,
                                   readInputs, deepSleep, error));
    CHECK(deepSleep);
    CHECK_EQ(sessionId, std::string("s-1"));
    CHECK_EQ(readInputs.size(), static_cast<size_t>(2));
    CHECK(readInputs[1].kind == matp::InputKind::Encoder);
    CHECK(readInputs[1].bindings == inputs[1].bindings);

    matp::InputActionEvent event;
    event.inputId = "knob";
    event.gesture = matp::InputGesture::CounterClockwise;
    event.binding = inputs[1].bindings[static_cast<size_t>(event.gesture)];
    event.count = 3;
    const json::Value action = matp::makeInputAction("s-1", event);
    CHECK_EQ(json::serialize(action),
             std::string("{\"type\":\"inputAction\",\"sessionId\":\"s-1\",\"inputId\":\"knob\","
                         "\"gesture\":\"counterClockwise\",\"action\":\"volumeDown\",\"step\":0.02,"
                         "\"scope\":\"room\",\"count\":3}"));
    matp::InputActionEvent readEvent;
    CHECK(matp::parseInputAction(action, sessionId, readEvent, error));
    CHECK_EQ(readEvent.count, static_cast<uint32_t>(3));
    CHECK(readEvent.binding == event.binding);

    // Nobody sends "none", and a count of zero is no gesture.
    CHECK(!matp::parseInputAction(parsed("{\"sessionId\":\"s-1\",\"inputId\":\"a\",\"gesture\":"
                                         "\"click\",\"action\":\"none\",\"count\":1}"),
                                  sessionId, readEvent, error));
    CHECK(!matp::parseInputAction(parsed("{\"sessionId\":\"s-1\",\"inputId\":\"a\",\"gesture\":"
                                         "\"click\",\"action\":\"next\",\"count\":0}"),
                                  sessionId, readEvent, error));

    matp::SetInputBindings request;
    request.inputId = "play";
    request.bindings = parsed("{\"click\":{\"action\":\"stop\"}}");
    matp::SetInputBindings readRequest;
    CHECK(matp::parseSetInputBindings(matp::makeSetInputBindings(request), readRequest, error));
    CHECK_EQ(readRequest.sessionId, std::string());
    CHECK_EQ(readRequest.inputId, std::string("play"));
    CHECK_EQ(json::serialize(readRequest.bindings), json::serialize(request.bindings));

    matp::MessageType type = matp::MessageType::Unknown;
    CHECK(matp::parseMessageType("setInputBindings", type));
    CHECK_EQ(static_cast<int>(type), 29);
}

MULTIROOM_TEST(config_reads_and_writes_inputs)
{
    ClientConfig config;
    std::string error;
    CHECK(client_config::deserialize(
        "{\"inputs\":[{\"id\":\"play\",\"kind\":\"button\",\"pin\":17},"
        "{\"id\":\"knob\",\"kind\":\"encoder\",\"pinA\":5,\"pinB\":6,\"pull\":\"none\","
        "\"stepsPerDetent\":2,\"bindings\":{\"clockwise\":{\"action\":\"next\"}}}]}",
        config, error));
    CHECK_EQ(config.inputs.size(), static_cast<size_t>(2));
    const InputDefinition& play = config.inputs[0];
    CHECK_EQ(play.pin, 17);
    CHECK(play.activeLow);
    CHECK(play.pull == InputPull::Up);
    CHECK(play.control.bindings[0].action == matp::InputAction::TogglePause); // the default
    const InputDefinition& knob = config.inputs[1];
    CHECK_EQ(knob.stepsPerDetent, static_cast<uint32_t>(2));
    CHECK(knob.pull == InputPull::None);
    CHECK(knob.control.bindings[static_cast<size_t>(matp::InputGesture::Clockwise)].action ==
          matp::InputAction::Next);
    CHECK(knob.control.bindings[static_cast<size_t>(matp::InputGesture::CounterClockwise)]
              .action == matp::InputAction::VolumeDown); // left out, the default

    ClientConfig reread;
    CHECK(client_config::deserialize(client_config::serialize(config), reread, error));
    CHECK_EQ(reread.inputs.size(), static_cast<size_t>(2));
    CHECK(reread.inputs[1].control.bindings == knob.control.bindings);
    CHECK_EQ(reread.inputs[1].pinB, 6);
}

MULTIROOM_TEST(config_reads_and_writes_input_device_inputs)
{
    ClientConfig config;
    std::string error;
    CHECK(client_config::deserialize(
        "{\"inputGpioChip\":\"\",\"inputs\":["
        "{\"id\":\"play\",\"kind\":\"button\",\"device\":\"gpio-keys\",\"key\":\"KEY_PLAYPAUSE\"},"
        "{\"id\":\"odd\",\"kind\":\"button\",\"device\":\"/dev/input/event3\",\"key\":\"330\"},"
        "{\"id\":\"knob\",\"kind\":\"encoder\",\"device\":\"rotary@11\"},"
        "{\"id\":\"dial\",\"kind\":\"encoder\",\"device\":\"rotary@11\",\"axis\":\"REL_DIAL\"}]}",
        config, error));
    CHECK_EQ(config.inputs.size(), static_cast<size_t>(4));
    CHECK(config.inputs[0].isEvdev());
    CHECK_EQ(config.inputs[0].code, static_cast<uint16_t>(164));
    CHECK_EQ(config.inputs[1].code, static_cast<uint16_t>(330));
    CHECK_EQ(config.inputs[2].code, static_cast<uint16_t>(0)); // REL_X, the default
    CHECK_EQ(config.inputs[3].code, static_cast<uint16_t>(7));

    const std::string text = client_config::serialize(config);
    CHECK(text.find("\"KEY_PLAYPAUSE\"") != std::string::npos);
    CHECK(text.find("\"330\"") != std::string::npos);
    CHECK(text.find("\"REL_X\"") != std::string::npos);
    CHECK(text.find("\"pull\"") == std::string::npos);
    ClientConfig reread;
    CHECK(client_config::deserialize(text, reread, error));
    CHECK_EQ(reread.inputs.size(), static_cast<size_t>(4));
    CHECK_EQ(reread.inputs[1].device, std::string("/dev/input/event3"));
    CHECK_EQ(reread.inputs[3].code, static_cast<uint16_t>(7));

    uint16_t code = 0;
    CHECK(parseInputKey("BTN_0", code) && code == 0x100);
    CHECK(!parseInputKey("KEY_NOPE", code));
    CHECK(!parseInputKey("768", code)); // above KEY_MAX
    CHECK(!parseInputAxis("16", code)); // above REL_MAX
}

MULTIROOM_TEST(config_refuses_miswired_inputs)
{
    const char* refused[] = {
        "{\"inputs\":[{\"id\":\"k\",\"kind\":\"encoder\",\"pin\":5}]}", // a button's key
        "{\"inputs\":[{\"id\":\"b\",\"kind\":\"button\"}]}",            // no pin
        "{\"inputs\":[{\"id\":\"a\",\"kind\":\"button\",\"pin\":4},"
        "{\"id\":\"b\",\"kind\":\"button\",\"pin\":4}]}", // one line, two inputs
        "{\"inputs\":[{\"id\":\"a\",\"kind\":\"button\",\"pin\":4},"
        "{\"id\":\"a\",\"kind\":\"button\",\"pin\":5}]}", // one id, two inputs
        "{\"inputs\":[{\"id\":\"k\",\"kind\":\"encoder\",\"pinA\":5,\"pinB\":5}]}",
        "{\"inputs\":[{\"id\":\"k\",\"kind\":\"encoder\",\"pinA\":5,\"pinB\":6,"
        "\"stepsPerDetent\":3}]}",
        "{\"inputs\":[{\"id\":\"b\",\"kind\":\"switch\",\"pin\":4}]}",
        // input devices: no GPIO keys, a key for a button, one key read once
        "{\"inputs\":[{\"id\":\"b\",\"kind\":\"button\",\"device\":\"d\",\"key\":\"KEY_1\","
        "\"pull\":\"up\"}]}",
        "{\"inputs\":[{\"id\":\"b\",\"kind\":\"button\",\"device\":\"d\"}]}",
        "{\"inputs\":[{\"id\":\"b\",\"kind\":\"button\",\"device\":\"d\",\"axis\":\"REL_X\"}]}",
        "{\"inputs\":[{\"id\":\"b\",\"kind\":\"button\",\"device\":\"\",\"key\":\"KEY_1\"}]}",
        "{\"inputs\":[{\"id\":\"k\",\"kind\":\"encoder\",\"device\":\"d\",\"axis\":\"REL_NOPE\"}]}",
        "{\"inputs\":[{\"id\":\"a\",\"kind\":\"button\",\"device\":\"d\",\"key\":\"KEY_1\"},"
        "{\"id\":\"b\",\"kind\":\"button\",\"device\":\"d\",\"key\":\"2\"}]}",
    };
    for (const char* text : refused)
    {
        ClientConfig config;
        std::string error;
        CHECK(!client_config::deserialize(text, config, error));
        CHECK(!error.empty());
    }
}

MULTIROOM_TEST(input_click_is_sent_on_release_when_nothing_else_is_bound)
{
    std::vector<InputDefinition> inputs = {button("play", 17)};
    InputController controller(inputs);

    // A bouncing press and a bouncing release: one click, 20 ms after the
    // release settled, and not a moment before.
    const std::vector<RawInputEvent> events = {
        level(0, true, 10), level(0, false, 11), level(0, true, 12),
        level(0, false, 100), level(0, true, 101), level(0, false, 103)};
    std::vector<matp::InputActionEvent> out = run(controller, events, 122);
    CHECK_EQ(out.size(), static_cast<size_t>(0));

    std::vector<matp::InputActionEvent> later;
    controller.poll(123 * kMs, later);
    CHECK_EQ(later.size(), static_cast<size_t>(1));
    CHECK(later[0].gesture == matp::InputGesture::Click);
    CHECK(later[0].binding.action == matp::InputAction::TogglePause);
    CHECK_EQ(later[0].inputId, std::string("play"));
}

MULTIROOM_TEST(input_level_callback_is_debounced_and_independent_of_action_bindings)
{
    std::vector<InputDefinition> inputs = {button("unassigned", 17), encoder("knob")};
    bind(inputs[0], matp::InputGesture::Click, matp::InputAction::None);
    InputController controller(inputs);
    std::vector<bool> levels;
    controller.setButtonStateCallback([&levels](size_t index, bool pressed) {
        CHECK_EQ(index, static_cast<size_t>(0));
        levels.push_back(pressed);
    });
    const auto actions = run(controller,
        {level(0, true, 10), level(0, false, 11), level(0, true, 12),
         detents(1, 1, 50), level(0, false, 100), level(0, true, 101), level(0, false, 103)}, 150);
    CHECK_EQ(levels.size(), static_cast<size_t>(2));
    CHECK(levels[0] && !levels[1]);
    for (const auto& action : actions)
        CHECK(action.inputId != "unassigned");
}

MULTIROOM_TEST(input_a_hold_is_a_click_until_long_press_is_bound)
{
    std::vector<InputDefinition> inputs = {button("play", 17)};
    InputController controller(inputs);
    std::vector<matp::InputActionEvent> out =
        run(controller, {level(0, true, 0), level(0, false, 2000)}, 2100);
    CHECK_EQ(out.size(), static_cast<size_t>(1));
    CHECK(out[0].gesture == matp::InputGesture::Click);

    bind(inputs[0], matp::InputGesture::LongPress, matp::InputAction::Next);
    InputController bound(inputs);
    out = run(bound, {level(0, true, 0)}, 1999);
    CHECK_EQ(out.size(), static_cast<size_t>(0));
    out = run(bound, {level(0, true, 0)}, 2000);
    CHECK_EQ(out.size(), static_cast<size_t>(1));
    CHECK(out[0].gesture == matp::InputGesture::LongPress);
    CHECK(out[0].binding.action == matp::InputAction::Next);

    // Released after it fired: nothing more.
    InputController released(inputs);
    out = run(released, {level(0, true, 0), level(0, false, 2500)}, 3000);
    CHECK_EQ(out.size(), static_cast<size_t>(1));
    CHECK(out[0].gesture == matp::InputGesture::LongPress);
}

MULTIROOM_TEST(input_a_release_just_before_the_long_press_deadline_is_a_click)
{
    std::vector<InputDefinition> inputs = {button("play", 17)};
    bind(inputs[0], matp::InputGesture::LongPress, matp::InputAction::Next);
    InputController controller(inputs);

    // The release at 1995 ms settles at 2015 ms, after the 2000 ms deadline;
    // it is still a click, because the level was known to be up since 1995.
    const std::vector<matp::InputActionEvent> out =
        run(controller, {level(0, true, 0), level(0, false, 1995)}, 2100);
    CHECK_EQ(out.size(), static_cast<size_t>(1));
    CHECK(out[0].gesture == matp::InputGesture::Click);
}

MULTIROOM_TEST(input_double_click_waits_for_the_second_press_only_when_bound)
{
    std::vector<InputDefinition> inputs = {button("play", 17)};
    bind(inputs[0], matp::InputGesture::DoubleClick, matp::InputAction::Next);
    InputController controller(inputs);

    std::vector<matp::InputActionEvent> out = run(
        controller, {level(0, true, 0), level(0, false, 80), level(0, true, 200), level(0, false, 280)},
        700);
    CHECK_EQ(out.size(), static_cast<size_t>(1));
    CHECK(out[0].gesture == matp::InputGesture::DoubleClick);
    CHECK(out[0].binding.action == matp::InputAction::Next);

    // A single press waits the double-click window out (300 ms from the
    // release), then is a click.
    InputController single(inputs);
    out = run(single, {level(0, true, 0), level(0, false, 80)}, 379);
    CHECK_EQ(out.size(), static_cast<size_t>(0));

    single.poll(400 * kMs, out);
    CHECK_EQ(out.size(), static_cast<size_t>(1));
    CHECK(out[0].gesture == matp::InputGesture::Click);
}

MULTIROOM_TEST(input_a_held_second_press_is_a_long_press_not_a_double_click)
{
    std::vector<InputDefinition> inputs = {button("power", 17)};
    bind(inputs[0], matp::InputGesture::DoubleClick, matp::InputAction::Next);
    bind(inputs[0], matp::InputGesture::LongPress, matp::InputAction::DeepSleep);

    // A short second press is still the double click.
    InputController quick(inputs);
    std::vector<matp::InputActionEvent> out = run(
        quick, {level(0, true, 0), level(0, false, 100), level(0, true, 200), level(0, false, 300)},
        700);
    CHECK_EQ(out.size(), static_cast<size_t>(1));
    CHECK(out[0].gesture == matp::InputGesture::DoubleClick);

    // Held for the long-press time, it fires while held, and the release
    // sends nothing more.
    InputController held(inputs);
    out = run(held, {level(0, true, 0), level(0, false, 100), level(0, true, 200)}, 2199);
    CHECK_EQ(out.size(), static_cast<size_t>(0));
    CHECK_EQ(held.waitMs(2199 * kMs, 200), static_cast<uint32_t>(1));
    held.poll(2200 * kMs, out);
    CHECK_EQ(out.size(), static_cast<size_t>(1));
    CHECK(out[0].gesture == matp::InputGesture::LongPress);
    CHECK(out[0].binding.action == matp::InputAction::DeepSleep);

    out = run(held, {level(0, false, 2600)}, 3000);
    CHECK_EQ(out.size(), static_cast<size_t>(0));
}

MULTIROOM_TEST(input_a_gesture_bound_to_none_sends_nothing)
{
    std::vector<InputDefinition> inputs = {button("play", 17)};
    bind(inputs[0], matp::InputGesture::Click, matp::InputAction::None);
    InputController controller(inputs);
    CHECK_EQ(run(controller, {level(0, true, 0), level(0, false, 80)}, 500).size(),
             static_cast<size_t>(0));
}

MULTIROOM_TEST(input_encoder_detents_are_coalesced_and_split_by_direction)
{
    std::vector<InputDefinition> inputs = {encoder("knob")};
    InputController controller(inputs);

    // The first detent goes at once; the next three within 50 ms are summed.
    std::vector<matp::InputActionEvent> out = run(
        controller, {detents(0, 1, 0), detents(0, 1, 10), detents(0, 2, 20)}, 49);
    CHECK_EQ(out.size(), static_cast<size_t>(1));
    CHECK_EQ(out[0].count, static_cast<uint32_t>(1));
    controller.poll(50 * kMs, out);
    CHECK_EQ(out.size(), static_cast<size_t>(2));
    CHECK(out[1].gesture == matp::InputGesture::Clockwise);
    CHECK_EQ(out[1].count, static_cast<uint32_t>(3));
    CHECK(out[1].binding.action == matp::InputAction::VolumeUp);

    // Turned back inside the window: what was pending goes out first, alone.
    InputController back(inputs);
    std::vector<matp::InputActionEvent> turned;
    back.feed(detents(0, 1, 0), turned);
    back.poll(0, turned);
    back.feed(detents(0, 2, 10 ), turned);
    back.feed(detents(0, -1, 20), turned);
    CHECK_EQ(turned.size(), static_cast<size_t>(2));
    CHECK_EQ(turned[1].count, static_cast<uint32_t>(2));
    back.poll(70 * kMs, turned);
    CHECK_EQ(turned.size(), static_cast<size_t>(3));
    CHECK(turned[2].gesture == matp::InputGesture::CounterClockwise);
    CHECK(turned[2].binding.action == matp::InputAction::VolumeDown);
}

MULTIROOM_TEST(input_wait_is_short_only_while_a_gesture_is_undecided)
{
    std::vector<InputDefinition> inputs = {button("play", 17)};
    InputController controller(inputs);
    CHECK_EQ(controller.waitMs(0, 200), static_cast<uint32_t>(200));

    std::vector<matp::InputActionEvent> out;
    controller.feed(level(0, true, 100), out);
    CHECK_EQ(controller.waitMs(105 * kMs, 200), static_cast<uint32_t>(15)); // the debounce
    controller.poll(120 * kMs, out);
    CHECK_EQ(controller.waitMs(120 * kMs, 200), static_cast<uint32_t>(200)); // nothing bound to wait for
}

MULTIROOM_TEST(input_simulated_gestures_resolve_through_the_bindings)
{
    std::vector<InputDefinition> inputs = {button("play", 17), encoder("knob")};
    matp::InputActionEvent event;
    std::string error;
    CHECK(resolveInputAction(inputs, "knob", matp::InputGesture::Clockwise, 4, event, error));
    CHECK(event.binding.action == matp::InputAction::VolumeUp);
    CHECK_EQ(event.count, static_cast<uint32_t>(4));

    CHECK(!resolveInputAction(inputs, "knob", matp::InputGesture::Click, 1, event, error));
    CHECK(!resolveInputAction(inputs, "play", matp::InputGesture::LongPress, 1, event, error));
    CHECK(!resolveInputAction(inputs, "gone", matp::InputGesture::Click, 1, event, error));
}

MULTIROOM_TEST(input_queue_hands_events_over_and_wakes_the_loop)
{
    InputEventQueue queue;
    int wakes = 0;
    queue.setWake([&wakes]() { ++wakes; });
    queue.push(level(0, true, 1));
    queue.push(level(0, false, 2));
    std::vector<RawInputEvent> taken;
    queue.take(taken);
    CHECK_EQ(taken.size(), static_cast<size_t>(2));
    CHECK_EQ(wakes, 2);
    queue.take(taken);
    CHECK_EQ(taken.size(), static_cast<size_t>(0));

    for (size_t i = 0; i < InputEventQueue::kCapacity + 3; ++i)
        queue.push(level(0, i % 2 == 0, i));
    CHECK_EQ(queue.dropped(), static_cast<uint32_t>(3));
}

MULTIROOM_TEST(input_queue_full_of_detents_still_takes_a_release)
{
    // A press, then a knob turned fast enough to fill the queue, then the
    // release 100 ms after the press: it must arrive, or the hold becomes a
    // long press.
    std::vector<InputDefinition> inputs{button("POWER", 6), encoder("knob")};
    bind(inputs[0], matp::InputGesture::LongPress, matp::InputAction::DeepSleep);
    InputEventQueue queue;
    queue.push(level(0, true, 0));
    for (size_t i = 1; i < InputEventQueue::kCapacity; ++i)
        queue.push(detents(1, i % 2 == 0 ? 1 : 2, 1));
    queue.push(level(0, false, 100));
    CHECK_EQ(queue.dropped(), static_cast<uint32_t>(0));

    std::vector<RawInputEvent> taken;
    queue.take(taken);
    CHECK_EQ(taken.size(), static_cast<size_t>(3));
    CHECK_EQ(taken[1].detents, static_cast<int32_t>(511 * 1 + 512 * 2));

    InputController controller(inputs);
    for (const auto& action : run(controller, taken, 3000))
        CHECK(action.binding.action != matp::InputAction::DeepSleep);
}

MULTIROOM_TEST(input_queue_compaction_keeps_a_reversal_as_two_actions)
{
    std::vector<InputDefinition> inputs{encoder("knob")};
    InputEventQueue queue;
    for (size_t i = 0; i < InputEventQueue::kCapacity / 2; ++i)
        queue.push(detents(0, 1, 1));
    for (size_t i = 0; i < InputEventQueue::kCapacity / 2; ++i)
        queue.push(detents(0, -1, 2));
    queue.push(detents(0, -1, 3)); // full: compacted, not dropped
    CHECK_EQ(queue.dropped(), static_cast<uint32_t>(0));

    std::vector<RawInputEvent> taken;
    queue.take(taken);
    CHECK_EQ(taken.size(), static_cast<size_t>(3)); // each run, then the one that made room
    CHECK_EQ(taken[0].detents, static_cast<int32_t>(512));
    CHECK_EQ(taken[1].detents, static_cast<int32_t>(-512));

    InputController controller(inputs);
    const std::vector<matp::InputActionEvent> actions = run(controller, taken, 200);
    CHECK_EQ(actions.size(), static_cast<size_t>(2));
    CHECK(actions[0].gesture == matp::InputGesture::Clockwise);
    CHECK_EQ(actions[0].count, static_cast<uint32_t>(512));
    CHECK(actions[1].gesture == matp::InputGesture::CounterClockwise);
    CHECK_EQ(actions[1].count, static_cast<uint32_t>(513));
}

MULTIROOM_TEST(input_resynchronized_level_is_state_not_a_long_hold)
{
    // A 100 ms press whose release the kernel dropped; the level read back
    // afterwards says the button is up.
    std::vector<InputDefinition> inputs{button("POWER", 6)};
    bind(inputs[0], matp::InputGesture::LongPress, matp::InputAction::DeepSleep);
    InputController controller(inputs);
    std::vector<matp::InputActionEvent> actions;
    controller.feed(level(0, true, 1000), actions);
    controller.poll(1100 * kMs, actions);

    RawInputEvent up = level(0, false, 4000);
    up.resync = true;
    controller.feed(up, actions);
    controller.poll(4000 * kMs, actions);
    controller.poll(8000 * kMs, actions);
    CHECK(actions.empty());

    // Read back held: nothing until it is let go.
    RawInputEvent held = level(0, true, 9000);
    held.resync = true;
    controller.feed(held, actions);
    controller.poll(12000 * kMs, actions);
    CHECK(actions.empty());
    controller.feed(level(0, false, 12100), actions);
    controller.poll(13000 * kMs, actions);
    controller.feed(level(0, true, 13100), actions);
    controller.poll(16000 * kMs, actions);
    CHECK_EQ(actions.size(), static_cast<size_t>(1)); // a real hold is one again
    CHECK(actions[0].binding.action == matp::InputAction::DeepSleep);
}

MULTIROOM_TEST(input_events_lost_cancel_a_pending_long_press)
{
    std::vector<InputDefinition> inputs{button("POWER", 6)};
    bind(inputs[0], matp::InputGesture::LongPress, matp::InputAction::DeepSleep);
    InputController controller(inputs);
    std::vector<matp::InputActionEvent> actions;
    controller.feed(level(0, true, 0), actions);
    controller.poll(100 * kMs, actions);

    // The release was among what the queue dropped.
    controller.eventsLost();
    controller.poll(3000 * kMs, actions);
    CHECK(actions.empty());

    // The next press and release are a gesture again once the release is seen.
    controller.feed(level(0, false, 3100), actions);
    controller.feed(level(0, true, 3200), actions);
    controller.poll(3300 * kMs, actions);
    controller.feed(level(0, false, 3400), actions);
    controller.poll(6000 * kMs, actions);
    CHECK(actions.empty() || actions[0].binding.action != matp::InputAction::DeepSleep);
}
