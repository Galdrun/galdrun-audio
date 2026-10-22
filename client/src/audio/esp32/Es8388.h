#pragma once

#include "audio/AudioFormat.h"

#include <cstdint>
#include <string>

namespace multiroom {
namespace es8388 {

// The ES8388 codec on the ESP32-A1S, driven over I2C. Playback only: the ADC
// stays powered down. The room's volume is the DAC's own digital attenuation
// (audio/HardwareVolume.h), so the samples on the I2S bus stay unscaled; the
// output stage is left at 0 dB.
//
// The I2C bus is created on first use and kept for the life of the process: a
// rescan runs on a second backend while the first one plays, and both must
// see the same bus rather than fight over the peripheral.

enum class Detection
{
    Present,
    Absent,
    // An AC101 answers instead -- the older A1S revision. Reported rather than
    // mistaken for an empty board, because it needs a driver this build does
    // not have.
    Ac101
};

// Probed once; later calls return the first answer.
Detection detect();

// Configures and powers the DAC, keeping it muted and the amplifier off.
// Call enableOutput() only after I2S is running and the output has settled.
// `codecIsClockMaster` is the
// I2S slave mode: the codec then drives the bit and word clocks from the MCLK
// the ESP32 still supplies.
bool start(const AudioFormat& format, bool codecIsClockMaster, std::string& error);

// Restores the latest setVolume() settings and enables the amplifier.
bool enableOutput(std::string& error);

// Mutes the DAC, powers it down and switches the amplifier off.
void stop();

// The DAC's digital volume: 0 dB down to -96 dB in 0.5 dB steps, and its
// mute. Kept across stop() and start(); enableOutput() puts it back, so a reopened
// device plays at the volume the room had. volume() returns the latest
// settings, and false while the running DAC may not hold them because a write
// failed.
const uint8_t kMaxAttenuation = 192; // steps of 0.5 dB
bool setVolume(uint8_t attenuation, bool muted);
bool volume(uint8_t& attenuation, bool& muted);

} // namespace es8388
} // namespace multiroom
