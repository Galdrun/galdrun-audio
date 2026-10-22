#pragma once

// Buttons and encoders on the ESP32.
//
// No GPIO interrupt at all: the I2S render task shares the chip's interrupt
// controllers, and a bouncing button can raise hundreds of edges a second. A
// task at priority 3 on core 0 -- below the pthreads the network and control
// run on, away from the render task on core 1 -- samples the button lines
// every 5 ms, and each encoder is counted by a PCNT unit in hardware, which
// catches edges far faster than any task could and costs the CPU nothing
// until the count is read.

#include "input/InputBackend.h"

#include <atomic>
#include <thread>
#include <vector>

namespace multiroom {

class Esp32InputBackend : public IInputBackend
{
public:
    Esp32InputBackend() = default;
    ~Esp32InputBackend() override;

    bool start(const std::vector<InputDefinition>& inputs, const std::string& gpioChip,
               InputEventQueue& queue, std::string& error) override;
    void stop() override;

private:
    struct Input
    {
        uint16_t index = 0; // into the configuration's inputs
        bool encoder = false;
        int pin = -1;
        bool activeLow = true;
        bool pressed = false;
        void* unit = nullptr; // pcnt_unit_handle_t, kept out of this header
        void* channels[2] = {nullptr, nullptr};
        int lastCount = 0;
        int32_t stepsPerDetent = 4;
        int32_t steps = 0; // counted, short of a detent
    };

    void run();
    void releaseAll();

    std::vector<Input> inputs_;
    InputEventQueue* queue_ = nullptr;
    std::atomic<bool> stopRequested_{false};
    std::thread thread_;
};

} // namespace multiroom
