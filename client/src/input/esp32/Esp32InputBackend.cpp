#include "input/esp32/Esp32InputBackend.h"

#include "util/Logger.h"
#include "util/Time.h"

#include "driver/gpio.h"
#include "driver/pulse_cnt.h"
#include "esp_pthread.h"

#include <chrono>

namespace multiroom {
namespace {

const int kTaskPriority = 3;
const int kTaskCore = 0;
const size_t kTaskStack = 3072;
const auto kSamplePeriod = std::chrono::milliseconds(5);

// The hardware counter's range. Reaching a limit resets it to zero, and the
// driver's accumulator (accum_count, with a watch point on each limit) adds
// what it held, so pcnt_unit_get_count() reads one count that never resets
// and no edge is lost between a read and a clear.
const int kCountLimit = 30000;

// Shorter pulses than this are contact noise, filtered by the PCNT itself.
const uint32_t kGlitchNs = 1000;

gpio_pull_mode_t pullMode(InputPull pull)
{
    switch (pull)
    {
    case InputPull::Up:
        return GPIO_PULLUP_ONLY;
    case InputPull::Down:
        return GPIO_PULLDOWN_ONLY;
    case InputPull::None:
        return GPIO_FLOATING;
    }
    return GPIO_FLOATING;
}

std::string espError(const std::string& what, esp_err_t result)
{
    return what + ": " + esp_err_to_name(result);
}

// Both channels of one unit, the usual quadrature arrangement: each line is
// counted on its edges with the other as the direction, which gives four
// counts a full cycle.
esp_err_t configureEncoder(int pinA, int pinB, pcnt_unit_handle_t& unit,
                           pcnt_channel_handle_t (&channels)[2])
{
    pcnt_unit_config_t unitConfig = {};
    unitConfig.low_limit = -kCountLimit;
    unitConfig.high_limit = kCountLimit;
    unitConfig.flags.accum_count = 1;
    esp_err_t result = pcnt_new_unit(&unitConfig, &unit);
    if (result != ESP_OK)
        return result;

    pcnt_glitch_filter_config_t filter = {};
    filter.max_glitch_ns = kGlitchNs;
    if ((result = pcnt_unit_set_glitch_filter(unit, &filter)) != ESP_OK)
        return result;

    pcnt_chan_config_t a = {};
    a.edge_gpio_num = pinA;
    a.level_gpio_num = pinB;
    if ((result = pcnt_new_channel(unit, &a, &channels[0])) != ESP_OK)
        return result;
    pcnt_chan_config_t b = {};
    b.edge_gpio_num = pinB;
    b.level_gpio_num = pinA;
    if ((result = pcnt_new_channel(unit, &b, &channels[1])) != ESP_OK)
        return result;

    pcnt_channel_set_edge_action(channels[0], PCNT_CHANNEL_EDGE_ACTION_DECREASE,
                                 PCNT_CHANNEL_EDGE_ACTION_INCREASE);
    pcnt_channel_set_level_action(channels[0], PCNT_CHANNEL_LEVEL_ACTION_KEEP,
                                  PCNT_CHANNEL_LEVEL_ACTION_INVERSE);
    pcnt_channel_set_edge_action(channels[1], PCNT_CHANNEL_EDGE_ACTION_INCREASE,
                                 PCNT_CHANNEL_EDGE_ACTION_DECREASE);
    pcnt_channel_set_level_action(channels[1], PCNT_CHANNEL_LEVEL_ACTION_KEEP,
                                  PCNT_CHANNEL_LEVEL_ACTION_INVERSE);
    if ((result = pcnt_unit_add_watch_point(unit, kCountLimit)) != ESP_OK ||
        (result = pcnt_unit_add_watch_point(unit, -kCountLimit)) != ESP_OK)
        return result;

    if ((result = pcnt_unit_enable(unit)) != ESP_OK)
        return result;
    if ((result = pcnt_unit_clear_count(unit)) != ESP_OK)
        return result;
    return pcnt_unit_start(unit);
}

} // namespace

Esp32InputBackend::~Esp32InputBackend()
{
    stop();
}

bool Esp32InputBackend::start(const std::vector<InputDefinition>& inputs, const std::string&,
                              InputEventQueue& queue, std::string& error)
{
    stop();
    if (inputs.empty())
        return true;

    for (size_t i = 0; i < inputs.size(); ++i)
    {
        const InputDefinition& definition = inputs[i];
        if (definition.isEvdev())
        {
            error = "input " + definition.control.id + ": an input device is Linux only";
            releaseAll();
            return false;
        }
        Input input;
        input.index = static_cast<uint16_t>(i);
        input.encoder = definition.control.kind == matp::InputKind::Encoder;
        input.activeLow = definition.activeLow;
        input.stepsPerDetent = static_cast<int32_t>(definition.stepsPerDetent);

        const int pins[] = {input.encoder ? definition.pinA : definition.pin, definition.pinB};
        for (int pin : pins)
        {
            if (pin < 0)
                continue;
            if (!GPIO_IS_VALID_GPIO(pin))
            {
                error = "input " + definition.control.id + ": GPIO " + std::to_string(pin) +
                        " does not exist on this chip";
                releaseAll();
                return false;
            }
        }

        esp_err_t result = ESP_OK;
        if (input.encoder)
        {
            pcnt_unit_handle_t unit = nullptr;
            pcnt_channel_handle_t channels[2] = {nullptr, nullptr};
            result = configureEncoder(definition.pinA, definition.pinB, unit, channels);
            input.unit = unit;
            input.channels[0] = channels[0];
            input.channels[1] = channels[1];
            if (result == ESP_OK)
                result = gpio_set_pull_mode(static_cast<gpio_num_t>(definition.pinA),
                                            pullMode(definition.pull));
            if (result == ESP_OK)
                result = gpio_set_pull_mode(static_cast<gpio_num_t>(definition.pinB),
                                            pullMode(definition.pull));
        }
        else
        {
            input.pin = definition.pin;
            gpio_config_t config = {};
            config.pin_bit_mask = 1ULL << definition.pin;
            config.mode = GPIO_MODE_INPUT;
            config.intr_type = GPIO_INTR_DISABLE;
            result = gpio_config(&config);
            if (result == ESP_OK)
                result = gpio_set_pull_mode(static_cast<gpio_num_t>(definition.pin),
                                            pullMode(definition.pull));
        }
        inputs_.push_back(input);
        if (result != ESP_OK)
        {
            // GPIO 34-39 have no pull resistors; the board's own must hold them.
            error = espError("input " + definition.control.id, result);
            releaseAll();
            return false;
        }
    }

    queue_ = &queue;
    stopRequested_.store(false);

    esp_pthread_cfg_t threadConfig = esp_pthread_get_default_config();
    threadConfig.stack_size = kTaskStack;
    threadConfig.prio = kTaskPriority;
    threadConfig.pin_to_core = kTaskCore;
    threadConfig.thread_name = "client_input";
    esp_pthread_set_cfg(&threadConfig);
    thread_ = std::thread(&Esp32InputBackend::run, this);
    const esp_pthread_cfg_t defaults = esp_pthread_get_default_config();
    esp_pthread_set_cfg(&defaults);

    MULTIROOM_INFO("reading %u input(s)", static_cast<unsigned>(inputs_.size()));
    return true;
}

void Esp32InputBackend::stop()
{
    stopRequested_.store(true);
    if (thread_.joinable())
        thread_.join();
    releaseAll();
}

void Esp32InputBackend::releaseAll()
{
    for (Input& input : inputs_)
    {
        if (input.unit == nullptr)
            continue;
        pcnt_unit_handle_t unit = static_cast<pcnt_unit_handle_t>(input.unit);
        pcnt_unit_stop(unit);
        pcnt_unit_disable(unit);
        for (void* channel : input.channels)
        {
            if (channel != nullptr)
                pcnt_del_channel(static_cast<pcnt_channel_handle_t>(channel));
        }
        pcnt_del_unit(unit);
    }
    inputs_.clear();
}

void Esp32InputBackend::run()
{
    // The first pass reports a button held at start as pressed.
    for (Input& input : inputs_)
        input.pressed = false;

    while (!stopRequested_.load())
    {
        const uint64_t nowNs = monotonicNowNs();
        for (Input& input : inputs_)
        {
            RawInputEvent event;
            event.index = input.index;
            event.atNs = nowNs;

            if (!input.encoder)
            {
                const bool high = gpio_get_level(static_cast<gpio_num_t>(input.pin)) != 0;
                const bool pressed = high != input.activeLow;
                if (pressed == input.pressed)
                    continue;
                input.pressed = pressed;
                event.type = RawInputEvent::Type::Level;
                event.pressed = pressed;
                queue_->push(event);
                continue;
            }

            pcnt_unit_handle_t unit = static_cast<pcnt_unit_handle_t>(input.unit);
            int count = 0;
            if (pcnt_unit_get_count(unit, &count) != ESP_OK)
                continue;
            // Unsigned, so a count that wraps after 2^31 edges is still told apart.
            input.steps += static_cast<int32_t>(static_cast<uint32_t>(count) -
                                                static_cast<uint32_t>(input.lastCount));
            input.lastCount = count;
            if (input.steps >= input.stepsPerDetent || input.steps <= -input.stepsPerDetent)
            {
                event.type = RawInputEvent::Type::Detents;
                event.detents = input.steps / input.stepsPerDetent;
                input.steps %= input.stepsPerDetent;
                queue_->push(event);
            }
        }
        std::this_thread::sleep_for(kSamplePeriod);
    }
}

} // namespace multiroom
