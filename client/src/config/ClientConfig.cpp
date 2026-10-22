#include "config/ClientConfig.h"
#include "discovery/NodeId.h"
#include "matp/Protocol.h"
#include "system/TextUtil.h"

#include "util/FileUtil.h"
#include "util/Json.h"

#include <algorithm>
#include <cstdlib>

namespace multiroom {
namespace {

// MATP section 18 recommends 500-1000 ms on wired Ethernet and 1000-3000 ms on
// Wi-Fi. The range below is wider at both ends -- a LAN prototype may want to
// see how little it can get away with, and a bad link may need more than the
// section's worst case -- but not so wide that a typo turns into a ring buffer
// the target board cannot hold.
const int64_t kMinBufferMs = 1000;
const int64_t kMaxBufferMs = 11000;

// What the ring buffer held on top of the old `prebufferMs`, which a file
// written before `bufferMs` still names: read as the buffer it gave.
const int64_t kLegacyHeadroomMs = 1000;

// What an ESP32 I2S peripheral can be clocked at, with room on both sides for
// an external clock that is not one of the usual rates.
const int64_t kMinI2sSampleRate = 8000;
const int64_t kMaxI2sSampleRate = 384000;

// More than every rate in both layouts; the bound is for a hand-edited file.
const size_t kMaxI2sExternalFormats = 64;

bool isDigits(const std::string& text)
{
    if (text.empty())
        return false;
    for (size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] < '0' || text[i] > '9')
            return false;
    }
    return true;
}

// A GPIO number on the ESP32, a line offset on a Linux GPIO chip. Either way
// small; this only catches a typo, the backend checks the line exists.
const int64_t kMaxInputPin = 1023;

bool pinFromJson(const json::Value& object, const char* key, int& out, std::string& error)
{
    int64_t value = 0;
    if (!json::getInt(object, key, value) || value < 0 || value > kMaxInputPin)
    {
        error = std::string(key) + " must be a GPIO number from 0 to " +
                std::to_string(kMaxInputPin);
        return false;
    }
    out = static_cast<int>(value);
    return true;
}

struct EventCodeName
{
    const char* name;
    uint16_t code;
};

// The keys a media remote, a macro pad or a gpio-keys overlay is likely to
// send; anything else is still reachable by its number. Values from
// linux/input-event-codes.h, which is not available on every build host.
const EventCodeName kKeyNames[] = {
    {"KEY_ESC", 1},          {"KEY_1", 2},           {"KEY_2", 3},
    {"KEY_3", 4},            {"KEY_4", 5},           {"KEY_5", 6},
    {"KEY_6", 7},            {"KEY_7", 8},           {"KEY_8", 9},
    {"KEY_9", 10},           {"KEY_0", 11},          {"KEY_Q", 16},
    {"KEY_W", 17},           {"KEY_E", 18},          {"KEY_R", 19},
    {"KEY_T", 20},           {"KEY_Y", 21},          {"KEY_U", 22},
    {"KEY_I", 23},           {"KEY_O", 24},          {"KEY_P", 25},
    {"KEY_ENTER", 28},       {"KEY_A", 30},          {"KEY_S", 31},
    {"KEY_D", 32},           {"KEY_F", 33},          {"KEY_G", 34},
    {"KEY_H", 35},           {"KEY_J", 36},          {"KEY_K", 37},
    {"KEY_L", 38},           {"KEY_Z", 44},          {"KEY_X", 45},
    {"KEY_C", 46},           {"KEY_V", 47},          {"KEY_B", 48},
    {"KEY_N", 49},           {"KEY_M", 50},          {"KEY_SPACE", 57},
    {"KEY_F1", 59},          {"KEY_F2", 60},         {"KEY_F3", 61},
    {"KEY_F4", 62},          {"KEY_F5", 63},         {"KEY_F6", 64},
    {"KEY_F7", 65},          {"KEY_F8", 66},         {"KEY_F9", 67},
    {"KEY_F10", 68},         {"KEY_F11", 87},        {"KEY_F12", 88},
    {"KEY_HOME", 102},       {"KEY_UP", 103},        {"KEY_LEFT", 105},
    {"KEY_RIGHT", 106},      {"KEY_DOWN", 108},      {"KEY_MUTE", 113},
    {"KEY_VOLUMEDOWN", 114}, {"KEY_VOLUMEUP", 115},  {"KEY_POWER", 116},
    {"KEY_PAUSE", 119},      {"KEY_STOP", 128},      {"KEY_MENU", 139},
    {"KEY_BACK", 158},       {"KEY_NEXTSONG", 163},  {"KEY_PLAYPAUSE", 164},
    {"KEY_PREVIOUSSONG", 165}, {"KEY_STOPCD", 166},  {"KEY_REWIND", 168},
    {"KEY_PLAYCD", 200},     {"KEY_PAUSECD", 201},   {"KEY_PLAY", 207},
    {"KEY_FASTFORWARD", 208}, {"BTN_0", 0x100},      {"BTN_1", 0x101},
    {"BTN_2", 0x102},        {"BTN_3", 0x103},       {"BTN_4", 0x104},
    {"BTN_5", 0x105},        {"BTN_6", 0x106},       {"BTN_7", 0x107},
    {"BTN_8", 0x108},        {"BTN_9", 0x109},       {"KEY_OK", 0x160},
    {"KEY_SELECT", 0x161},   {"KEY_NEXT", 0x197},    {"KEY_PREVIOUS", 0x19c},
};
const long kMaxKeyCode = 0x2ff; // KEY_MAX

const EventCodeName kAxisNames[] = {
    {"REL_X", 0},    {"REL_Y", 1},     {"REL_Z", 2},    {"REL_HWHEEL", 6},
    {"REL_DIAL", 7}, {"REL_WHEEL", 8}, {"REL_MISC", 9},
};
const long kMaxAxisCode = 0x0f; // REL_MAX

template <size_t N>
std::string eventCodeName(const EventCodeName (&names)[N], uint16_t code)
{
    for (const EventCodeName& entry : names)
    {
        if (entry.code == code)
            return entry.name;
    }
    return std::to_string(code);
}

template <size_t N>
bool parseEventCode(const EventCodeName (&names)[N], long max, const std::string& name,
                    uint16_t& out)
{
    for (const EventCodeName& entry : names)
    {
        if (name == entry.name)
        {
            out = entry.code;
            return true;
        }
    }
    if (!isDigits(name) || name.size() > 5)
        return false;
    const long value = std::strtol(name.c_str(), nullptr, 10);
    if (value > max)
        return false;
    out = static_cast<uint16_t>(value);
    return true;
}

// Strict like the rest of the file, and stricter in one way: a key the input's
// kind does not have is an error, because "pin" on an encoder is a wiring
// mistake that would otherwise surface as a knob that does nothing.
bool inputFromJson(const json::Value& object, InputDefinition& out, std::string& error)
{
    if (!object.isObject())
    {
        error = "must be an object";
        return false;
    }

    InputDefinition input;
    std::string kind;
    if (!json::getString(object, "id", input.control.id) ||
        !matp::isValidInputId(input.control.id))
    {
        error = "id must be 1 to 64 bytes without control characters or surrounding blanks";
        return false;
    }
    if (!json::getString(object, "kind", kind) || !matp::parseInputKind(kind, input.control.kind))
    {
        error = "kind must be button or encoder";
        return false;
    }
    const bool button = input.control.kind == matp::InputKind::Button;
    const bool evdev = object.find("device") != nullptr;

    for (const json::Value::Member& member : object.members())
    {
        const std::string& key = member.first;
        const bool common = key == "id" || key == "kind" || key == "bindings";
        bool own = false;
        if (evdev)
            own = key == "device" || key == (button ? "key" : "axis");
        else if (button)
            own = key == "pin" || key == "activeLow" || key == "pull";
        else
            own = key == "pinA" || key == "pinB" || key == "stepsPerDetent" || key == "pull";
        if (!common && !own)
        {
            error = std::string(evdev ? "an input device's " : "an input of kind ") + kind +
                    " has no " + key;
            return false;
        }
    }

    if (evdev)
    {
        std::string code;
        if (!json::getString(object, "device", input.device) || input.device.empty())
        {
            error = "device must be an input device's path or name";
            return false;
        }
        if (button &&
            (!json::getString(object, "key", code) || !parseInputKey(code, input.code)))
        {
            error = "key must be a key name such as KEY_PLAYPAUSE, or its number";
            return false;
        }
        // The rotary-encoder overlay's default axis.
        if (!button && object.find("axis") != nullptr &&
            (!json::getString(object, "axis", code) || !parseInputAxis(code, input.code)))
        {
            error = "axis must be a relative axis such as REL_X, or its number";
            return false;
        }
    }
    else if (button)
    {
        if (!pinFromJson(object, "pin", input.pin, error))
            return false;
        if (object.find("activeLow") != nullptr &&
            !json::getBool(object, "activeLow", input.activeLow))
        {
            error = "activeLow must be a boolean";
            return false;
        }
    }
    else
    {
        if (!pinFromJson(object, "pinA", input.pinA, error) ||
            !pinFromJson(object, "pinB", input.pinB, error))
            return false;
        if (object.find("stepsPerDetent") != nullptr)
        {
            int64_t steps = 0;
            if (!json::getInt(object, "stepsPerDetent", steps) ||
                (steps != 1 && steps != 2 && steps != 4))
            {
                error = "stepsPerDetent must be 1, 2 or 4";
                return false;
            }
            input.stepsPerDetent = static_cast<uint32_t>(steps);
        }
    }

    if (object.find("pull") != nullptr)
    {
        std::string pull;
        if (!json::getString(object, "pull", pull) || !parseInputPull(pull, input.pull))
        {
            error = "pull must be up, down or none";
            return false;
        }
    }

    input.control.bindings = matp::defaultInputBindings(input.control.kind);
    if (const json::Value* bindings = object.find("bindings"))
    {
        if (!matp::inputBindingsFromJson(*bindings, input.control.kind, input.control.bindings,
                                         error))
            return false;
    }

    out = input;
    return true;
}

bool parsePort(const std::string& text, uint16_t& out)
{
    if (!isDigits(text) || text.size() > 5)
        return false;
    const long value = std::strtol(text.c_str(), nullptr, 10);
    if (value < 1 || value > 65535)
        return false;
    out = static_cast<uint16_t>(value);
    return true;
}

} // namespace

const char* inputPullName(InputPull pull)
{
    switch (pull)
    {
    case InputPull::Up:
        return "up";
    case InputPull::Down:
        return "down";
    case InputPull::None:
        return "none";
    }
    return "unknown";
}

bool parseInputPull(const std::string& name, InputPull& out)
{
    const InputPull known[] = {InputPull::Up, InputPull::Down, InputPull::None};
    for (InputPull pull : known)
    {
        if (name == inputPullName(pull))
        {
            out = pull;
            return true;
        }
    }
    return false;
}

std::string inputKeyName(uint16_t code)
{
    return eventCodeName(kKeyNames, code);
}

bool parseInputKey(const std::string& name, uint16_t& out)
{
    return parseEventCode(kKeyNames, kMaxKeyCode, name, out);
}

std::string inputAxisName(uint16_t code)
{
    return eventCodeName(kAxisNames, code);
}

bool parseInputAxis(const std::string& name, uint16_t& out)
{
    return parseEventCode(kAxisNames, kMaxAxisCode, name, out);
}

bool parseServerAddress(const std::string& text, std::string& host, uint16_t& port)
{
    if (text.empty())
        return false;

    // Bracketed IPv6 literal: [::1]:4711
    if (text[0] == '[')
    {
        const size_t closing = text.find(']');
        if (closing == std::string::npos || closing == 1)
            return false;
        if (closing + 1 >= text.size() || text[closing + 1] != ':')
            return false;
        uint16_t parsedPort = 0;
        if (!parsePort(text.substr(closing + 2), parsedPort))
            return false;
        host = text.substr(1, closing - 1);
        port = parsedPort;
        return true;
    }

    const size_t separator = text.rfind(':');
    if (separator == std::string::npos || separator == 0)
        return false;
    // A bare IPv6 address without brackets is ambiguous; reject it.
    if (text.find(':') != separator)
        return false;

    uint16_t parsedPort = 0;
    if (!parsePort(text.substr(separator + 1), parsedPort))
        return false;

    host = text.substr(0, separator);
    port = parsedPort;
    return true;
}

namespace client_config {

json::Value inputToJson(const InputDefinition& input, bool electrical)
{
    json::Value object = json::Value::makeObject();
    object["id"] = json::Value(input.control.id);
    object["kind"] = json::Value(matp::inputKindName(input.control.kind));
    if (input.isEvdev())
    {
        object["device"] = json::Value(input.device);
        if (input.control.kind == matp::InputKind::Button)
            object["key"] = json::Value(inputKeyName(input.code));
        else
            object["axis"] = json::Value(inputAxisName(input.code));
    }
    else
    {
        if (input.control.kind == matp::InputKind::Button)
        {
            object["pin"] = json::Value(static_cast<int32_t>(input.pin));
            if (electrical)
                object["activeLow"] = json::Value(input.activeLow);
        }
        else
        {
            object["pinA"] = json::Value(static_cast<int32_t>(input.pinA));
            object["pinB"] = json::Value(static_cast<int32_t>(input.pinB));
            if (electrical)
                object["stepsPerDetent"] = json::Value(input.stepsPerDetent);
        }
        if (electrical)
            object["pull"] = json::Value(inputPullName(input.pull));
    }
    object["bindings"] = matp::inputBindingsToJson(input.control.kind, input.control.bindings);
    return object;
}

const char* loadResultName(LoadResult result)
{
    switch (result)
    {
    case LoadResult::Ok:
        return "ok";
    case LoadResult::Missing:
        return "missing";
    case LoadResult::Invalid:
        return "invalid";
    }
    return "unknown";
}

std::string defaultPath()
{
    return fileutil::joinPath(fileutil::userConfigDirectory(), "client.json");
}

std::string serialize(const ClientConfig& config)
{
    json::Value root = json::Value::makeObject();
    root["server"] = json::Value(config.server);
    root["clientId"] = json::Value(config.clientId);
    root["name"] = json::Value(config.name);
    root["audioDeviceId"] = json::Value(config.audioDeviceId);
    root["audioAccessMode"] = json::Value(audioAccessPreferenceName(config.audioAccessMode));
    root["preferredSampleRate"] = json::Value(config.preferredSampleRate);
    root["i2sClockMode"] = json::Value(i2sClockModeName(config.i2s.clockMode));
    root["i2sSlaveSampleRate"] = json::Value(config.i2s.slaveSampleRate);
    root["i2sExternalFormats"] = i2sFormatsToJson(config.i2s.externalFormats);
    root["bufferMs"] = json::Value(config.bufferMs);
    root["volumeControl"] = json::Value(matp::volumeControlModeName(config.volumeControl));
    root["driftCorrection"] = json::Value(matp::driftCorrectionModeName(config.driftCorrection));
    root["declick"] = json::Value(config.declick);
    root["ledsOff"] = json::Value(config.ledsOff);
    root["standbyAfterSeconds"] = json::Value(config.standbyAfterSeconds);
    root["deepSleepAfterSeconds"] = json::Value(config.deepSleepAfterSeconds);
    root["controlPort"] = json::Value(static_cast<uint32_t>(config.controlPort));
    root["controlBindAddress"] = json::Value(config.controlBindAddress);
    root["logLevel"] = json::Value(text_util::toLower(log::levelName(config.logLevel)));
    json::Value inputs = json::Value::makeArray();
    for (const InputDefinition& input : config.inputs)
        inputs.pushBack(inputToJson(input, true));
    root["inputs"] = inputs;
    root["inputGpioChip"] = json::Value(config.inputGpioChip);
    return json::serialize(root, true);
}

bool deserialize(const std::string& text, ClientConfig& out, std::string& error)
{
    json::Value root;
    if (!json::parse(text, root, error))
        return false;
    if (!root.isObject())
    {
        error = "root value is not an object";
        return false;
    }

    // Every field is optional; a missing key keeps the default. A key that is
    // present with the wrong type is an error, so typos are not silently
    // ignored.
    ClientConfig config;

    if (root.find("server") != nullptr && !json::getString(root, "server", config.server))
    {
        error = "server must be a string";
        return false;
    }
    if (root.find("clientId") != nullptr && !json::getString(root, "clientId", config.clientId))
    {
        error = "clientId must be a string";
        return false;
    }
    if (root.find("name") != nullptr && !json::getString(root, "name", config.name))
    {
        error = "name must be a string";
        return false;
    }
    if (root.find("audioDeviceId") != nullptr &&
        !json::getString(root, "audioDeviceId", config.audioDeviceId))
    {
        error = "audioDeviceId must be a string";
        return false;
    }

    if (root.find("audioAccessMode") != nullptr)
    {
        std::string mode;
        if (!json::getString(root, "audioAccessMode", mode) ||
            !parseAudioAccessPreference(mode, config.audioAccessMode))
        {
            error = "audioAccessMode must be one of exclusive, exclusive-only, shared";
            return false;
        }
    }

    if (root.find("preferredSampleRate") != nullptr)
    {
        int64_t value = 0;
        if (!json::getInt(root, "preferredSampleRate", value))
        {
            error = "preferredSampleRate must be a number";
            return false;
        }
        if (value <= 0 || value > 3000000)
        {
            error = "preferredSampleRate out of range";
            return false;
        }
        config.preferredSampleRate = static_cast<uint32_t>(value);
    }

    if (root.find("i2sClockMode") != nullptr)
    {
        std::string mode;
        if (!json::getString(root, "i2sClockMode", mode) ||
            !parseI2sClockMode(mode, config.i2s.clockMode))
        {
            error = "i2sClockMode must be one of master, slave";
            return false;
        }
    }

    if (root.find("i2sSlaveSampleRate") != nullptr)
    {
        int64_t value = 0;
        if (!json::getInt(root, "i2sSlaveSampleRate", value))
        {
            error = "i2sSlaveSampleRate must be a number";
            return false;
        }
        if (value < kMinI2sSampleRate || value > kMaxI2sSampleRate)
        {
            error = "i2sSlaveSampleRate out of range";
            return false;
        }
        config.i2s.slaveSampleRate = static_cast<uint32_t>(value);
    }

    if (const json::Value* formats = root.find("i2sExternalFormats"))
    {
        if (!i2sFormatsFromJson(*formats, config.i2s.externalFormats, error))
            return false;
    }

    // `bufferMs` wins over the old key when a file has both.
    const char* const bufferKey = root.find("bufferMs") != nullptr      ? "bufferMs"
                                  : root.find("prebufferMs") != nullptr ? "prebufferMs"
                                                                        : nullptr;
    if (bufferKey != nullptr)
    {
        int64_t value = 0;
        if (!json::getInt(root, bufferKey, value))
        {
            error = std::string(bufferKey) + " must be a number";
            return false;
        }
        if (bufferKey != std::string("bufferMs"))
            value += kLegacyHeadroomMs;
        if (value < kMinBufferMs || value > kMaxBufferMs)
        {
            error = "bufferMs out of range";
            return false;
        }
        config.bufferMs = static_cast<uint32_t>(value);
    }

    if (const json::Value* volume = root.find("volumeControl"))
    {
        if (!volume->isString() ||
            !matp::parseVolumeControlMode(volume->asString(), config.volumeControl))
        {
            error = "volumeControl must be auto, hardware, software or disabled";
            return false;
        }
    }

    if (const json::Value* drift = root.find("driftCorrection"))
    {
        if (!drift->isString() ||
            !matp::parseDriftCorrectionMode(drift->asString(), config.driftCorrection))
        {
            error = "driftCorrection must be resampling, hardwareClock or auto";
            return false;
        }
    }

    if (root.find("declick") != nullptr && !json::getBool(root, "declick", config.declick))
    {
        error = "declick must be true or false";
        return false;
    }

    if (root.find("ledsOff") != nullptr && !json::getBool(root, "ledsOff", config.ledsOff))
    {
        error = "ledsOff must be true or false";
        return false;
    }

    if (root.find("standbyAfterSeconds") != nullptr)
    {
        int64_t seconds = 0;
        if (!json::getInt(root, "standbyAfterSeconds", seconds) || seconds < 0 || seconds > 86400)
        {
            error = "standbyAfterSeconds must be an integer from 0 to 86400";
            return false;
        }
        config.standbyAfterSeconds = static_cast<uint32_t>(seconds);
    }

    if (root.find("deepSleepAfterSeconds") != nullptr)
    {
        int64_t seconds = 0;
        if (!json::getInt(root, "deepSleepAfterSeconds", seconds) ||
            !validDeepSleepAfterSeconds(seconds))
        {
            error = kDeepSleepAfterSecondsError;
            return false;
        }
        config.deepSleepAfterSeconds = static_cast<uint32_t>(seconds);
    }

    if (root.find("controlPort") != nullptr)
    {
        int64_t value = 0;
        if (!json::getInt(root, "controlPort", value))
        {
            error = "controlPort must be a number";
            return false;
        }
        if (value < 1 || value > 65535)
        {
            error = "controlPort out of range";
            return false;
        }
        config.controlPort = static_cast<uint16_t>(value);
    }

    if (root.find("controlBindAddress") != nullptr &&
        !json::getString(root, "controlBindAddress", config.controlBindAddress))
    {
        error = "controlBindAddress must be a string";
        return false;
    }

    if (root.find("logLevel") != nullptr)
    {
        std::string level;
        if (!json::getString(root, "logLevel", level) ||
            !log::parseLevel(level, config.logLevel))
        {
            error = "logLevel must be one of error, warn, info, debug, trace";
            return false;
        }
    }

    if (const json::Value* inputs = root.find("inputs"))
    {
        if (!inputs->isArray())
        {
            error = "inputs must be an array";
            return false;
        }
        for (size_t i = 0; i < inputs->size(); ++i)
        {
            InputDefinition input;
            if (!inputFromJson(inputs->at(i), input, error))
            {
                error = "inputs[" + std::to_string(i) + "]: " + error;
                return false;
            }
            config.inputs.push_back(input);
        }
    }

    if (root.find("inputGpioChip") != nullptr &&
        !json::getString(root, "inputGpioChip", config.inputGpioChip))
    {
        error = "inputGpioChip must be a string";
        return false;
    }

    if (!validate(config, error))
        return false;

    out = config;
    return true;
}

json::Value i2sFormatsToJson(const std::vector<AudioFormat>& formats)
{
    json::Value list = json::Value::makeArray();
    for (const AudioFormat& format : formats)
    {
        json::Value entry = json::Value::makeObject();
        entry["sampleRate"] = json::Value(format.sampleRate);
        entry["sampleType"] = json::Value(sampleTypeName(format.sampleType));
        list.pushBack(entry);
    }
    return list;
}

bool i2sFormatsFromJson(const json::Value& value, std::vector<AudioFormat>& out,
                        std::string& error)
{
    if (!value.isArray())
    {
        error = "i2sExternalFormats must be an array";
        return false;
    }
    std::vector<AudioFormat> parsed;
    for (size_t i = 0; i < value.size(); ++i)
    {
        const json::Value& entry = value.at(i);
        int64_t rate = 0;
        std::string type;
        AudioFormat format;
        format.channels = 2;
        if (!entry.isObject() || !json::getInt(entry, "sampleRate", rate) || rate <= 0 ||
            rate > kMaxI2sSampleRate || !json::getString(entry, "sampleType", type) ||
            !parseSampleType(type, format.sampleType))
        {
            error = "i2sExternalFormats[" + std::to_string(i) +
                    "] needs a sampleRate and a sampleType";
            return false;
        }
        format.sampleRate = static_cast<uint32_t>(rate);
        parsed.push_back(format);
    }
    out = parsed;
    return true;
}

bool validate(const ClientConfig& config, std::string& error)
{
    if (!config.clientId.empty() && !discovery::isValidNodeId(config.clientId))
    {
        error = "clientId must be 1 to 64 bytes without control characters or surrounding blanks";
        return false;
    }
    if (!config.name.empty() && !matp::isValidRoomName(config.name))
    {
        error = "name must be 1 to 64 bytes without control characters or surrounding blanks";
        return false;
    }
    if (!config.server.empty())
    {
        std::string host;
        uint16_t port = 0;
        if (!parseServerAddress(config.server, host, port))
        {
            error = "invalid server address";
            return false;
        }
    }
    if (config.preferredSampleRate == 0)
    {
        error = "preferredSampleRate must be greater than zero";
        return false;
    }
    if (config.i2s.slaveSampleRate < kMinI2sSampleRate ||
        config.i2s.slaveSampleRate > kMaxI2sSampleRate)
    {
        error = "i2sSlaveSampleRate must be between " + std::to_string(kMinI2sSampleRate) +
                " and " + std::to_string(kMaxI2sSampleRate);
        return false;
    }
    const std::vector<AudioFormat>& externalFormats = config.i2s.externalFormats;
    if (externalFormats.empty() || externalFormats.size() > kMaxI2sExternalFormats)
    {
        error = "i2sExternalFormats must list 1 to " + std::to_string(kMaxI2sExternalFormats) +
                " formats";
        return false;
    }
    const std::vector<uint32_t>& rates = i2sSampleRates();
    for (size_t i = 0; i < externalFormats.size(); ++i)
    {
        const AudioFormat& format = externalFormats[i];
        if (std::find(rates.begin(), rates.end(), format.sampleRate) == rates.end())
        {
            error = "i2sExternalFormats: " + std::to_string(format.sampleRate) +
                    " Hz is not a rate the I2S output clocks";
            return false;
        }
        if (!isI2sSampleType(format.sampleType) || format.channels != 2)
        {
            error = std::string("i2sExternalFormats: the I2S output plays two channels of "
                                "int16 or int32, not ") +
                    describeFormat(format);
            return false;
        }
        if (std::find(externalFormats.begin(), externalFormats.begin() + i, format) !=
            externalFormats.begin() + i)
        {
            error = "i2sExternalFormats lists " + describeFormat(format) + " twice";
            return false;
        }
    }
    if (config.bufferMs < kMinBufferMs || config.bufferMs > kMaxBufferMs)
    {
        error = "bufferMs must be between " + std::to_string(kMinBufferMs) + " and " +
                std::to_string(kMaxBufferMs);
        return false;
    }
    if (config.controlPort == 0)
    {
        error = "controlPort must be greater than zero";
        return false;
    }
    if (config.standbyAfterSeconds > 86400)
    {
        error = "standbyAfterSeconds must be an integer from 0 to 86400";
        return false;
    }
    if (!validDeepSleepAfterSeconds(config.deepSleepAfterSeconds))
    {
        error = kDeepSleepAfterSecondsError;
        return false;
    }
    if (config.controlBindAddress.empty())
    {
        error = "controlBindAddress must not be empty";
        return false;
    }

    if (config.inputs.size() > matp::kMaxInputs)
    {
        error = "inputs lists more than " + std::to_string(matp::kMaxInputs) + " entries";
        return false;
    }
    // Two inputs on one line would each see the other's presses, and two
    // with one id could not be told apart by the server.
    std::vector<int> pins;
    bool gpio = false;
    for (size_t i = 0; i < config.inputs.size(); ++i)
    {
        const InputDefinition& input = config.inputs[i];
        for (size_t j = 0; j < i; ++j)
        {
            const InputDefinition& other = config.inputs[j];
            if (other.control.id == input.control.id)
            {
                error = "inputs: id " + input.control.id + " is used twice";
                return false;
            }
            if (input.isEvdev() && other.device == input.device &&
                other.control.kind == input.control.kind && other.code == input.code)
            {
                error = "inputs: " + input.control.id + " and " + other.control.id +
                        " read the same key or axis of " + input.device;
                return false;
            }
        }
        if (input.isEvdev())
            continue;
        gpio = true;
        std::vector<int> own;
        if (input.control.kind == matp::InputKind::Button)
            own.push_back(input.pin);
        else
            own = {input.pinA, input.pinB};
        for (int pin : own)
        {
            if (pin < 0)
            {
                error = "inputs: " + input.control.id + " has no pin";
                return false;
            }
            for (int used : pins)
            {
                if (used == pin)
                {
                    error = "inputs: GPIO " + std::to_string(pin) + " is used twice";
                    return false;
                }
            }
            pins.push_back(pin);
        }
    }
    if (gpio && config.inputGpioChip.empty())
    {
        error = "inputGpioChip must not be empty";
        return false;
    }
    return true;
}

LoadResult load(const std::string& path, ClientConfig& out, std::string& error)
{
    if (!fileutil::exists(path))
    {
        error = "file not found";
        return LoadResult::Missing;
    }
    std::string text;
    if (!fileutil::readTextFile(path, text, error))
        return LoadResult::Invalid;
    // Notepad and PowerShell's Set-Content -Encoding utf8 write a UTF-8 BOM,
    // which JSON does not allow. One is stripped here, where a hand-edited
    // file comes in; the parser itself stays strict.
    if (text.compare(0, 3, "\xEF\xBB\xBF") == 0)
        text.erase(0, 3);
    if (!deserialize(text, out, error))
        return LoadResult::Invalid;
    return LoadResult::Ok;
}

namespace {
std::recursive_mutex g_saveMutex;
bool g_savesSealed = false; // under g_saveMutex
} // namespace

std::unique_lock<std::recursive_mutex> lockSaves()
{
    return std::unique_lock<std::recursive_mutex>(g_saveMutex);
}

void sealSaves()
{
    std::lock_guard<std::recursive_mutex> lock(g_saveMutex);
    g_savesSealed = true;
}

bool save(const std::string& path, const ClientConfig& config, std::string& error)
{
    if (!validate(config, error))
        return false;
    std::lock_guard<std::recursive_mutex> lock(g_saveMutex);
    if (g_savesSealed)
    {
        error = "new settings were just saved and the device is restarting to use them";
        return false;
    }
    return fileutil::writeTextFileAtomic(path, serialize(config), error);
}

bool completeIdentity(ClientConfig& config, const std::string& hostName)
{
    if (config.clientId.empty())
    {
        config.clientId = discovery::generateRandomId();
        if (config.name.empty())
            config.name = matp::isValidRoomName(hostName) ? hostName : config.clientId;
        return true;
    }
    if (config.name.empty())
    {
        config.name = config.clientId;
        config.clientId = discovery::generateRandomId();
        return true;
    }
    return false;
}

} // namespace client_config
} // namespace multiroom
