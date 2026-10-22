#pragma once

// Where the client's main loop spends its time, for the `diagnostics`
// command. Every control command waits for this loop -- it is the one thread
// that owns the state -- so a slow step here is a slow answer everywhere: on
// the settings page, in the server's `clients`, to a script.
//
// Each step of a pass is timed and summed into a window of kWindowNs; the
// last complete window is what is reported, so a figure is never older than
// two windows and never covers less than one. Main thread only.

#include "util/Json.h"

#include <array>
#include <cstdint>

namespace multiroom {

class LoopTiming
{
public:
    enum Step
    {
        Sample,   // sampleSystem: CPU, memory, Wi-Fi
        Session,  // MATP: handshake, clock sync, reports
        Playback, // StreamPlayback::poll: device, timeline
        Beacon,
        Inputs,   // buttons and encoders
        Commands, // control commands, run by the queue
        Wait,     // idle in the queue, waiting for a command or the next due
        kSteps
    };

    static constexpr uint64_t kWindowNs = 10ULL * 1000000000ULL;

    static const char* stepName(Step step);

    void add(Step step, uint64_t ns);

    // A pass ended at `nowNs`: its busy time (every step but Wait) is a sample
    // of the pass length, and a window that is complete is published.
    void endPass(uint64_t nowNs);

    // {"windowMs":..,"passes":..,"busyMaxUs":..,"steps":{"<step>":{"avgUs":..,"maxUs":..,"totalMs":..}}}
    // for the last complete window; {} before the first.
    json::Value toJson() const;

private:
    struct Figures
    {
        uint64_t totalNs = 0;
        uint64_t maxNs = 0;
    };
    struct Window
    {
        uint64_t startNs = 0;
        uint64_t lengthNs = 0;
        uint64_t passes = 0;
        uint64_t busyMaxNs = 0;
        std::array<Figures, kSteps> steps{};
    };

    Window current_{};
    Window last_{};
    bool hasLast_ = false;
    uint64_t passBusyNs_ = 0;
};

} // namespace multiroom
