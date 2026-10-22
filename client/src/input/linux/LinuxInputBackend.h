#pragma once

// Buttons and encoders on Linux, from two sources, read by one thread:
//
//   * **GPIO lines** through the GPIO character device (/dev/gpiochipN, uAPI
//     v2, kernel 5.10 and later) -- no libgpiod, for the reason ALSA is used
//     directly: the kernel interface is a handful of ioctls. One line request
//     per input, both edges, the configured pull. Polarity is applied here
//     rather than with the kernel's active-low flag, so every edge this class
//     reads is a physical one and the encoder's quadrature table needs no
//     second meaning.
//   * **Input devices** (/dev/input/eventN, evdev): the kernel's gpio-keys and
//     rotary-encoder overlays, USB keyboards, media remotes. The kernel has
//     already debounced and decoded them; a key is a level, a relative axis a
//     count of detents. A device that is missing at start or unplugged later
//     is looked for again every two seconds, so a USB remote can come and go.
//
// Both carry CLOCK_MONOTONIC time stamps (evdev once asked to with
// EVIOCSCLOCKID), which is the clock monotonicNowNs() reads on Linux, so a
// bounce is timed where it happened rather than where this thread woke up.

#include "input/InputBackend.h"

#include <atomic>
#include <thread>
#include <utility>
#include <vector>

namespace multiroom {

class LinuxInputBackend : public IInputBackend
{
public:
    LinuxInputBackend() = default;
    ~LinuxInputBackend() override;

    bool start(const std::vector<InputDefinition>& inputs, const std::string& gpioChip,
               InputEventQueue& queue, std::string& error) override;
    void stop() override;

private:
    struct Line
    {
        int fd = -1;
        uint16_t index = 0; // into the configuration's inputs
        bool encoder = false;
        bool activeLow = true;
        uint32_t pinA = 0; // the button's line, or the encoder's first
        uint32_t stepsPerDetent = 4;
        uint8_t quadrature = 0; // A << 1 | B, last seen
        int32_t steps = 0;      // quadrature steps short of a detent
        uint32_t seqno = 0;     // the button's last event; the kernel counts from 1
    };

    struct Device
    {
        std::string spec; // client.json's `device`: a path or a name
        int fd = -1;
        bool monotonic = false; // the kernel stamps events with our clock
        bool missingLogged = false;
        bool dropped = false; // SYN_DROPPED: events are void until SYN_REPORT
        uint64_t retryAtNs = 0;
        std::vector<std::pair<uint16_t, uint16_t>> keys; // key code -> input index
        std::vector<std::pair<uint16_t, uint16_t>> axes; // axis code -> input index
    };

    bool claimLines(const std::vector<InputDefinition>& inputs, const std::string& gpioChip,
                    InputEventQueue& queue, std::string& error);
    void openDevice(Device& device, uint64_t nowNs);
    void closeDevice(Device& device, uint64_t nowNs);
    void pushKeyLevels(Device& device, uint64_t nowNs);
    void run();
    void readLine(Line& line);
    void readDevice(Device& device);
    void closeAll();

    std::vector<Line> lines_;
    std::vector<Device> devices_;
    InputEventQueue* queue_ = nullptr;
    std::atomic<bool> stopRequested_{false};
    std::thread thread_;
};

// The input devices nothing in `inputs` names yet that are plainly a few
// buttons or a knob -- the kernel's own (BUS_HOST: gpio-keys, rotary-encoder)
// with at most 16 keys, or with a relative axis and no mouse button -- each
// key a button and each axis an encoder, with the default bindings. Ids are
// "<device name> <key>". A keyboard or a remote is never guessed at; it is
// declared in client.json. KEY_POWER is excluded except on Connector 2,
// where it is the top audio button rather than a system power control.
void discoverEvdevInputs(std::vector<InputDefinition>& inputs);

} // namespace multiroom
