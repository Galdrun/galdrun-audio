#pragma once

#include "audio/AudioBackend.h"
#include "audio/AudioDeviceInfo.h"
#include "matp/Input.h"
#include "matp/Protocol.h"
#include "util/Json.h"
#include "util/Logger.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace multiroom {

enum class InputPull
{
    Up,
    Down,
    None
};

const char* inputPullName(InputPull pull);
bool parseInputPull(const std::string& name, InputPull& out);

// Linux input event codes (linux/input-event-codes.h), by name, so client.json
// says "KEY_PLAYPAUSE" rather than 164. A key is EV_KEY, an axis EV_REL; a
// name the table lacks is written as its number and read back as one.
std::string inputKeyName(uint16_t code);
bool parseInputKey(const std::string& name, uint16_t& out);
std::string inputAxisName(uint16_t code);
bool parseInputAxis(const std::string& name, uint16_t& out);

// One button or rotary encoder wired to this machine (phase 23, MATP section
// 57). The wiring is the board's and only client.json sets it; the bindings in
// `control` are what the server may change.
//
// Two kinds of wiring: a GPIO line (`pin`, or `pinA`/`pinB`) read directly,
// or -- Linux only -- a key or relative axis of an input device (`device`),
// which is how the kernel's gpio-keys and rotary-encoder overlays, USB
// keyboards and remotes appear.
struct InputDefinition
{
    matp::InputControl control;

    int pin = -1;  // a button's line
    int pinA = -1; // an encoder's two lines
    int pinB = -1;

    // An evdev input: a /dev/input path or a device name (EVIOCGNAME), and
    // the button's key or the encoder's axis. Empty for a GPIO input.
    std::string device;
    uint16_t code = 0;

    bool isEvdev() const { return !device.empty(); }

    InputPull pull = InputPull::Up;

    // A button to ground behind a pull-up reads low when pressed. An encoder
    // has no polarity: inverting both lines leaves the direction as it was.
    bool activeLow = true;

    // Quadrature steps per detent: 4 for most encoders, 2 or 1 for some.
    uint32_t stepsPerDetent = 4;
};

// Persistent client settings (client.json).
//
// Command-line options override these values in memory but are not written
// back; only changes made through the control API are persisted.
struct ClientConfig
{
    std::string server; // "host:port", empty until configured

    // False takes this client out of the system without uninstalling it: no
    // server connection; its disabled discovery status hides it from lists, while
    // the control API and its settings page keep running. `server` is kept
    // for when it is switched back on.
    bool active = true;

    // Identity sent in the MATP HELLO message: 16 random hex digits
    // (phase 26). The server keys this client's own output stream, drift
    // model, resampler state and everything a high-end server keeps about the
    // room on it, and finds this client on the network by it (the discovery
    // beacon), so it must stay stable across restarts and reconnects -- hence
    // a persisted setting. Empty only in a file no client has started on yet:
    // startup generates one and writes it back (completeIdentity).
    std::string clientId;

    // What people call the room, e.g. "Kitchen" (MATP section 60): a label
    // and nothing else, so changing it changes no server state. Empty only in
    // a file no client has started on yet, or one written before phase 26.
    std::string name;

    std::string audioDeviceId; // empty selects the system default device

    // Exclusive access by default: bit-perfect, and the hardware clock we
    // measure is the sound card's rather than the system mixer's.
    AudioAccessPreference audioAccessMode = AudioAccessPreference::PreferExclusive;

    uint32_t preferredSampleRate = 48000;

    // ESP32 only (the I2S backend); the other platforms ignore both. Read when
    // the backend is created, so a change takes effect on the next start.
    I2sSettings i2s;

    // The size of the stream's ring buffer, in milliseconds of audio (MATP
    // section 58). Memory and nothing else: how full the server keeps it is
    // the server's own target (section 18), and a buffer smaller than that
    // target is simply kept full. It has to hold the server's start minimum
    // (1000 ms by default) or the room is never ready. The one setting on this
    // list that costs memory: 2500 ms of 48 kHz stereo 24-in-32 is 1 MB once
    // rounded up to a power of two frames.
    uint32_t bufferMs = 2500;

    // How SET_VOLUME is played (MATP section 59): the device's own control,
    // scaling the samples, not at all, or chosen from the room's Audio Control
    // and the device. The server sets it with SET_VOLUME_CONTROL; this file is
    // where it is kept.
    matp::VolumeControlMode volumeControl = matp::VolumeControlMode::Auto;

    // How the server corrects this room's drift when it corrects it: by
    // resampling, or by trimming the DAC clock where the board has a verified
    // trim (audio/HardwareClock.h). Reported in AUDIO_CAPABILITIES; the server
    // sets it with SET_DRIFT_CORRECTION and decides whether it is used.
    matp::DriftCorrectionMode driftCorrection = matp::DriftCorrectionMode::Auto;

    // Fades the audio over 10 ms wherever playback cuts it -- a start, a
    // pause, a stop, a seek, an underrun -- so that no edge is a click in the
    // speakers (TimelineSource, De-click). Bitperfect rooms included: only the
    // samples at an edge change. Read when the stream is created.
    bool declick = true;

    // Raumfeld status LEDs; boot/update indications remain enabled.
    bool ledsOff = false;

    // Zero disables automatic audio standby. Network and buttons stay active.
    uint32_t standbyAfterSeconds = 0;
    // Zero disables automatic deep sleep; otherwise 60 to 86400. Counted from
    // the same idle start as standby, and through it; only a client that can
    // run the deepSleep action (Connector 2 firmware) acts on it.
    uint32_t deepSleepAfterSeconds = 0;

    uint16_t controlPort = 4720;
    // Every interface by default: a server finds this client by its beacon and
    // attaches it through this API (setServer), so the API has to be reachable
    // from the network. There is no authentication, so anything on the LAN can
    // drive it; "127.0.0.1" closes it to this machine and discovery with it.
    std::string controlBindAddress = "0.0.0.0";

    // Buttons and encoders (phase 23). Empty on a machine without any, which
    // is every desktop. The chip is the Linux GPIO character device their
    // lines are offsets on; the ESP32 numbers its GPIOs directly and ignores
    // it. On Linux, input devices nothing here names that look like buttons
    // or a knob are added at startup (appendDiscoveredInputs).
    std::vector<InputDefinition> inputs;
    std::string inputGpioChip = "/dev/gpiochip0";

    LogLevel logLevel = LogLevel::Info;
};

// Splits "host:port" or "[ipv6]:port". Returns false for an empty host, a
// missing separator, or a port outside 1..65535.
bool parseServerAddress(const std::string& text, std::string& host, uint16_t& port);

// Zero, or a minute to a day: deep sleep entered within about 30 s of boot
// does not reach its power state on Connector 2.
inline bool validDeepSleepAfterSeconds(int64_t seconds)
{
    return seconds == 0 || (seconds >= 60 && seconds <= 86400);
}
constexpr const char* kDeepSleepAfterSecondsError =
    "deepSleepAfterSeconds must be 0 or an integer from 60 to 86400";

namespace client_config {

enum class LoadResult
{
    Ok,
    Missing, // no file yet; caller keeps the defaults
    Invalid
};

const char* loadResultName(LoadResult result);

LoadResult load(const std::string& path, ClientConfig& out, std::string& error);

// Refused once sealSaves() has been called.
bool save(const std::string& path, const ClientConfig& config, std::string& error);

// Process-wide, and taken by save() as well: held across a load, change and
// save that another thread's save must not land in the middle of (the ESP32
// settings page, whose client keeps a snapshot of its own).
std::unique_lock<std::recursive_mutex> lockSaves();

// Every save after this is refused: a restart is about to load what was just
// written, and a snapshot taken before it must not replace it.
void sealSaves();

std::string defaultPath();

std::string serialize(const ClientConfig& config);
bool deserialize(const std::string& text, ClientConfig& out, std::string& error);

// One entry of the inputs array: wiring, then bindings. `electrical` adds
// what only client.json needs (activeLow, pull, stepsPerDetent).
json::Value inputToJson(const InputDefinition& input, bool electrical);

// The i2sExternalFormats array, as client.json and the ESP32 settings page
// carry it: [{"sampleRate": 48000, "sampleType": "s24le"}, ...], two channels
// each. Parsing checks the shape only; range, layout and duplicates are
// validate()'s.
json::Value i2sFormatsToJson(const std::vector<AudioFormat>& formats);
bool i2sFormatsFromJson(const json::Value& value, std::vector<AudioFormat>& out,
                        std::string& error);

// Field-level sanity check, run after loading and before saving.
bool validate(const ClientConfig& config, std::string& error);

// Fills in the identity at startup (phase 26), true when it changed and has
// to be written back:
//   * no clientId: a fresh config -- a random id, and the host name as the
//     name (the id when `hostName` is no valid name);
//   * a clientId but no name: written before the name was its own setting,
//     when the id was the name people saw -- that becomes the name and a
//     random id replaces it, once.
bool completeIdentity(ClientConfig& config, const std::string& hostName);

} // namespace client_config
} // namespace multiroom
