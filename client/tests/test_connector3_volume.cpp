// Exercise the real room-volume adapter with MCU replies, without a sound card.
#undef NDEBUG
#include "audio/linux/AlsaMixer.h"
#include "config/BoardConfig.h"
#include <cassert>
#include <limits>
#include <vector>

namespace {
long mute = 0, level = 67;
bool available = true, wrongReadback = false;
std::vector<std::string> commands;
}

namespace multiroom::board_config {
const json::Value& current()
{
    static const auto board = [] {
        auto value = json::Value::makeObject();
        value["mcuAudioControls"] = json::Value(true);
        return value;
    }();
    return board;
}
bool enabled(const json::Value& board, const char* key)
{
    bool value = false;
    return json::getBool(board, key, value) && value;
}
}

namespace multiroom {
bool connector3McuRequest(const std::string& command, long& readMute, long& readLevel,
                          std::string& error)
{
    commands.push_back(command);
    if (!available) { error = "MCU unavailable"; return false; }
    if (command.compare(0, 7, "volume ") == 0) level = std::stol(command.substr(7));
    else if (command.compare(0, 5, "mute ") == 0) mute = std::stol(command.substr(5));
    else assert(command == "get");
    readMute = mute;
    readLevel = wrongReadback ? level + 1 : level;
    return true;
}
}

int main()
{
    using namespace multiroom;
    HardwareVolumeInfo info;
    assert(detectAlsaHardwareVolume("Connector3", info));
    assert(info.available && info.control == "Output Level" && info.hasMute &&
           info.minStep == 0 && info.maxStep == 100 && !info.hasDecibels);
    AudioDeviceInfo device;
    device.id = "hw:CARD=Connector3,DEV=0";
    device.hardwareVolume = info;
    auto volume = openAlsaHardwareVolume(device);
    assert(volume);
    std::string error;
    commands.clear();
    assert(volume->set(0.427, true, error));
    assert((commands == std::vector<std::string>{"mute 1", "volume 42"}));
    HardwareVolumeState state;
    assert(volume->read(state, error) && state.gain == 0.42 && state.muted);
    commands.clear();
    assert(volume->set(0.3, false, error));
    assert((commands == std::vector<std::string>{"volume 30", "mute 0"}));
    assert(volume->setMute(true, error) && level == 30 && mute == 1);
    assert(volume->set(std::numeric_limits<double>::quiet_NaN(), true, error) && level == 0);
    assert(volume->set(2, true, error) && level == 100);
    wrongReadback = true;
    error.clear();
    assert(!volume->set(0.2, false, error) && !error.empty() && mute == 1);
    available = false;
    assert(!volume->read(state, error));
    assert(!detectAlsaHardwareVolume("Connector3", info));
    device.id = "default";
    assert(!openAlsaHardwareVolume(device));
    return 0;
}
