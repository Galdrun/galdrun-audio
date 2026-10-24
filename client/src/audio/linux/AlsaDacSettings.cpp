#include "config/DacSettings.h"
#include "config/BoardConfig.h"
#include "util/FileUtil.h"
#include <alsa/asoundlib.h>
#include <algorithm>
#include <cmath>
#include <vector>
#include <cctype>
#include <climits>

namespace multiroom::dac_settings {
namespace {
// CS4271 power-up values from the firmware driver, expressed as ALSA values
// (volume, capture and high-pass switches invert their register fields). The
// board's "Drift compensator" is deliberately not one of them: it belongs to
// synchronization (audio/HardwareClock.h), which keeps it transient and resets
// it whenever a device opens, so it is neither listed, written nor saved here.
const struct { const char* name; long value; } kControls[] = {
    {"Master Playback Volume", 127}, {"Master Playback Switch", 1}, {"Master Capture Switch", 1},
    {"Digital Loopback Switch", 0}, {"Auto-Mute Switch", 1}, {"De-emphasis Switch", 0},
    {"Dither 16-Bit Data Switch", 0}, {"High Pass Filter Switch", 1},
    {"Left Channel Inversion Switch", 0}, {"Right Channel Inversion Switch", 0},
    {"Slow Roll Off Filter Switch", 0}, {"Soft Ramp Switch", 1}, {"Soft Ramp-Down Switch", 0},
    {"Soft Volume Ramp-Up Switch", 0}, {"Zero Cross Switch", 0},
    {"Left Output Source", 2}, {"Right Output Source", 1}, {"Link Playback Volume Switch", 0}
};

// The client owner thread selects one concrete ALSA card. Unselected Pi images
// expose no DAC controls and never touch another card during restore.
std::string selectedCard;
std::string roomVolumeElement;
bool genericAvailable = false;

bool generic() { return board_config::enabled(board_config::current(), "selectedCardDacControls"); }

std::string settingsPath(const std::string& path)
{
    return generic() ? path + "." + selectedCard : path;
}

bool clockControl(const std::string& name)
{
    std::string lower = name;
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return lower.find("clock") != std::string::npos || lower.find("drift") != std::string::npos ||
           lower.find("ppm") != std::string::npos || lower.find("pll") != std::string::npos ||
           lower.find("rate") != std::string::npos || lower.find("frequency") != std::string::npos ||
           lower.find("sync") != std::string::npos || lower.find("trim") != std::string::npos;
}

struct Control {
    unsigned int id = 0;
    std::string name;
    snd_ctl_elem_type_t type = SND_CTL_ELEM_TYPE_NONE;
    long minimum = 0, maximum = 1, step = 1;
    long defaultValue = 0;
    std::vector<long> values;
    std::vector<std::string> items;
};

struct Card {
    snd_ctl_t* handle = nullptr;
    ~Card() { if (handle != nullptr) snd_ctl_close(handle); }
};

bool failed(int result, std::string& error)
{
    if (result >= 0) return false;
    error = std::string("DAC control failed: ") + snd_strerror(result);
    return true;
}

bool readControls(Card& card, std::vector<Control>& controls, std::string& error)
{
    const std::string device = generic() ? "hw:CARD=" + selectedCard : "hw:Connector2";
    if (generic() && selectedCard.empty()) { error = "Select an ALSA output first"; return false; }
    if (failed(snd_ctl_open(&card.handle, device.c_str(), SND_CTL_NONBLOCK), error)) return false;
    std::vector<std::pair<std::string, long>> definitions;
    if (generic())
    {
        snd_ctl_elem_list_t* list;
        snd_ctl_elem_list_alloca(&list);
        if (failed(snd_ctl_elem_list(card.handle, list), error)) return false;
        const unsigned count = snd_ctl_elem_list_get_count(list);
        if (count > 512) { error = "Too many ALSA controls"; return false; }
        if (failed(snd_ctl_elem_list_alloc_space(list, count), error)) return false;
        const int result = snd_ctl_elem_list(card.handle, list);
        if (result >= 0)
            for (unsigned i = 0; i < snd_ctl_elem_list_get_used(list); ++i)
            {
                const std::string name = snd_ctl_elem_list_get_name(list, i);
                if (snd_ctl_elem_list_get_interface(list, i) == SND_CTL_ELEM_IFACE_MIXER &&
                    snd_ctl_elem_list_get_index(list, i) == 0 &&
                    snd_ctl_elem_list_get_device(list, i) == 0 &&
                    snd_ctl_elem_list_get_subdevice(list, i) == 0 && !clockControl(name))
                    definitions.emplace_back(name, 0);
            }
        snd_ctl_elem_list_free_space(list);
        if (failed(result, error)) return false;
    }
    else for (const auto& initial : kControls) definitions.emplace_back(initial.name, initial.value);
    snd_ctl_elem_id_t* id;
    snd_ctl_elem_info_t* info;
    snd_ctl_elem_value_t* value;
    snd_ctl_elem_id_alloca(&id);
    snd_ctl_elem_info_alloca(&info);
    snd_ctl_elem_value_alloca(&value);
    for (const auto& initial : definitions)
    {
        const char* name = initial.first.c_str();
        snd_ctl_elem_id_clear(id);
        snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_MIXER);
        snd_ctl_elem_id_set_name(id, name);
        snd_ctl_elem_info_clear(info);
        snd_ctl_elem_info_set_id(info, id);
        const int result = snd_ctl_elem_info(card.handle, info);
        if (result == -ENOENT) continue; // Older firmware lacks the output-source controls.
        if (failed(result, error)) return false;
        if (!snd_ctl_elem_info_is_readable(info) || !snd_ctl_elem_info_is_writable(info) ||
            (generic() && (snd_ctl_elem_info_is_volatile(info) || snd_ctl_elem_info_is_inactive(info)))) continue;
        Control control;
        control.id = snd_ctl_elem_info_get_numid(info);
        control.name = name;
        control.defaultValue = initial.second;
        control.type = snd_ctl_elem_info_get_type(info);
        const unsigned int count = snd_ctl_elem_info_get_count(info);
        if (count == 0 || count > (generic() ? 32u : 2u)) continue;
        if (control.type == SND_CTL_ELEM_TYPE_INTEGER)
        {
            control.minimum = snd_ctl_elem_info_get_min(info);
            control.maximum = snd_ctl_elem_info_get_max(info);
            control.step = std::max(1L, snd_ctl_elem_info_get_step(info));
            if (generic() && control.name.find("Volume") != std::string::npos)
            {
                // Drivers provide card-specific scaling. Never expose a gain
                // boost through DAC settings, or guess the scale of a raw control.
                long zeroDb = 0;
                if (snd_ctl_convert_from_dB(card.handle, id, 0, &zeroDb, -1) < 0) continue;
                control.maximum = std::min(control.maximum, zeroDb);
            }
            if (control.minimum < INT32_MIN || control.maximum > INT32_MAX ||
                control.minimum > control.maximum || control.step > INT32_MAX) continue;
        }
        else if (control.type == SND_CTL_ELEM_TYPE_ENUMERATED)
        {
            const unsigned int items = snd_ctl_elem_info_get_items(info);
            if (items == 0 || items > 128) continue;
            control.maximum = items - 1;
            for (unsigned int item = 0; item < items; ++item)
            {
                snd_ctl_elem_info_set_item(info, item);
                if (failed(snd_ctl_elem_info(card.handle, info), error)) return false;
                control.items.emplace_back(snd_ctl_elem_info_get_item_name(info));
            }
        }
        else if (control.type != SND_CTL_ELEM_TYPE_BOOLEAN)
            continue;
        snd_ctl_elem_value_clear(value);
        snd_ctl_elem_value_set_numid(value, control.id);
        if (failed(snd_ctl_elem_read(card.handle, value), error)) return false;
        for (unsigned int channel = 0; channel < count; ++channel)
            control.values.push_back(control.type == SND_CTL_ELEM_TYPE_ENUMERATED ?
                static_cast<long>(snd_ctl_elem_value_get_enumerated(value, channel)) :
                snd_ctl_elem_value_get_integer(value, channel));
        controls.push_back(std::move(control));
    }
    if (controls.empty()) { error = "DAC controls are unavailable"; return false; }
    return true;
}

json::Value valuesJson(const std::vector<long>& values)
{
    auto result = json::Value::makeArray();
    for (long value : values) result.pushBack(json::Value(static_cast<int32_t>(value)));
    return result;
}

bool writeControl(Card& card, const Control& control, const std::vector<long>& values, std::string& error)
{
    snd_ctl_elem_value_t* value;
    snd_ctl_elem_value_alloca(&value);
    snd_ctl_elem_value_set_numid(value, control.id);
    for (unsigned int channel = 0; channel < values.size(); ++channel)
    {
        if (control.type == SND_CTL_ELEM_TYPE_ENUMERATED)
            snd_ctl_elem_value_set_enumerated(value, channel, static_cast<unsigned int>(values[channel]));
        else snd_ctl_elem_value_set_integer(value, channel, values[channel]);
    }
    if (failed(snd_ctl_elem_write(card.handle, value), error) ||
        failed(snd_ctl_elem_read(card.handle, value), error)) return false;
    for (unsigned int channel = 0; channel < values.size(); ++channel)
    {
        const long actual = control.type == SND_CTL_ELEM_TYPE_ENUMERATED ?
            static_cast<long>(snd_ctl_elem_value_get_enumerated(value, channel)) :
            snd_ctl_elem_value_get_integer(value, channel);
        if (actual != values[channel]) { error = "DAC readback differs from requested value"; return false; }
    }
    return true;
}

bool readSaved(const std::string& path, json::Value& saved, std::string& error)
{
    saved = json::Value::makeObject();
    if (!fileutil::exists(path)) return true;
    std::string text;
    if (!fileutil::readTextFile(path, text, error)) return false;
    if (text.size() > 32768 || !json::parse(text, saved, error) || !saved.isObject())
    { error = "invalid saved DAC settings"; return false; }
    // Earlier firmware saved the clock trim with the codec settings; restoring
    // it would start a resampled room off nominal. The next save drops it.
    json::Value kept = json::Value::makeObject();
    for (const auto& member : saved.members())
        if (member.first != "Drift compensator") kept[member.first] = member.second;
    saved = kept;
    return true;
}

// Validate the entire batch before touching a register, then roll it back if
// applying, readback or atomic persistence fails. Called on the main thread.
bool apply(const json::Value& changes, const std::string* path, std::string& error, bool replaceSaved = false)
{
    Card card;
    std::vector<Control> controls;
    if (!readControls(card, controls, error)) return false;
    if (!changes.isObject() || changes.members().size() > controls.size())
    { error = "DAC settings must be an object of control value arrays"; return false; }
    std::vector<std::pair<size_t, std::vector<long>>> pending;
    for (const auto& member : changes.members())
    {
        const auto found = std::find_if(controls.begin(), controls.end(),
            [&](const Control& control) { return control.name == member.first; });
        if (found == controls.end() || !member.second.isArray() || member.second.size() != found->values.size())
        { error = "unknown DAC control or incorrect channel count: " + member.first; return false; }
        std::vector<long> values;
        for (size_t channel = 0; channel < member.second.size(); ++channel)
        {
            const auto& entry = member.second.at(channel);
            const double value = entry.asDouble();
            if (!entry.isNumber() || !std::isfinite(value) || std::floor(value) != value ||
                value < found->minimum || value > found->maximum ||
                (static_cast<int64_t>(value) - found->minimum) % found->step != 0)
            { error = "invalid DAC value: " + member.first; return false; }
            values.push_back(static_cast<long>(value));
        }
        pending.emplace_back(static_cast<size_t>(found - controls.begin()), std::move(values));
    }
    json::Value saved = json::Value::makeObject();
    if (path != nullptr && !replaceSaved && !readSaved(*path, saved, error)) return false;
    size_t applied = 0;
    bool ok = true;
    for (const auto& item : pending)
    {
        ++applied; // Also restore the failing control: a partial write may have occurred.
        if (!writeControl(card, controls[item.first], item.second, error)) { ok = false; break; }
        if (path != nullptr) saved[controls[item.first].name] = valuesJson(item.second);
    }
    if (ok && path != nullptr)
        ok = fileutil::writeTextFileAtomic(*path, json::serialize(saved) + "\n", error);
    if (!ok)
    {
        std::string rollback;
        while (applied > 0)
        {
            const auto& control = controls[pending[--applied].first];
            if (!writeControl(card, control, control.values, rollback))
                error += "; DAC rollback failed: " + rollback;
        }
    }
    return ok;
}
} // namespace

bool supported()
{
    return board_config::enabled(board_config::current(), "dacControls") && (!generic() || genericAvailable);
}

bool supported(const std::string& boardPath)
{
    return board_config::enabled(board_config::read(boardPath), "dacControls");
}

bool load(json::Value& settings, std::string& error)
{
    Card card;
    std::vector<Control> controls;
    if (!readControls(card, controls, error)) return false;
    settings = json::Value::makeObject();
    settings["codec"] = json::Value(generic() ? selectedCard : "CS4271");
    if (generic()) settings["card"] = json::Value(selectedCard);
    settings["resetSource"] = json::Value(generic() ? "initial card settings" : "firmware defaults");
    settings["controls"] = json::Value::makeArray();
    for (const auto& control : controls)
    {
        auto entry = json::Value::makeObject();
        entry["name"] = json::Value(control.name);
        entry["type"] = json::Value(control.type == SND_CTL_ELEM_TYPE_BOOLEAN ? "boolean" :
                                    control.type == SND_CTL_ELEM_TYPE_ENUMERATED ? "enum" : "integer");
        entry["min"] = json::Value(static_cast<int32_t>(control.minimum));
        entry["max"] = json::Value(static_cast<int32_t>(control.maximum));
        entry["step"] = json::Value(static_cast<int32_t>(control.step));
        entry["values"] = valuesJson(control.values);
        entry["items"] = json::Value::makeArray();
        for (const auto& item : control.items) entry["items"].pushBack(json::Value(item));
        settings["controls"].pushBack(std::move(entry));
    }
    return true;
}

bool save(const json::Value& changes, json::Value& settings, std::string& error, const std::string& path)
{
    error.clear();
    const std::string selectedPath = settingsPath(path);
    return apply(changes, &selectedPath, error) && load(settings, error);
}

bool restore(std::string& error, const std::string& path)
{
    const std::string selectedPath = settingsPath(path);
    if (!fileutil::exists(selectedPath)) return true;
    json::Value saved;
    return readSaved(selectedPath, saved, error) && apply(saved, nullptr, error);
}

bool isRoomVolumeControl(const std::string& name)
{
    if (generic()) return !roomVolumeElement.empty() &&
        (name == roomVolumeElement + " Playback Volume" || name == roomVolumeElement + " Playback Switch");
    return name == "Master Playback Volume" || name == "Master Playback Switch";
}

bool reset(json::Value& settings, std::string& error, bool keepRoomVolume, const std::string& path)
{
    error.clear();
    Card card;
    std::vector<Control> controls;
    if (!readControls(card, controls, error)) return false;
    auto defaults = json::Value::makeObject();
    const std::string selectedPath = settingsPath(path);
    json::Value baseline;
    if (generic() && !readSaved(selectedPath + ".defaults", baseline, error)) return false;
    for (const auto& control : controls)
        if (!keepRoomVolume || !isRoomVolumeControl(control.name))
        {
            if (generic())
            {
                const auto* initial = baseline.find(control.name);
                if (initial != nullptr) defaults[control.name] = *initial;
            }
            else defaults[control.name] = valuesJson(std::vector<long>(control.values.size(), control.defaultValue));
        }
    return apply(defaults, &selectedPath, error, true) && load(settings, error);
}
bool selectDevice(const std::string& deviceId, const std::string& volumeElement, std::string& error)
{
    if (!generic()) return true;
    roomVolumeElement = volumeElement;
    const std::string prefix = "hw:CARD=";
    std::string card;
    if (deviceId.compare(0, prefix.size(), prefix) == 0)
        card = deviceId.substr(prefix.size(), deviceId.find(',') - prefix.size());
    if (card.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != std::string::npos)
        card.clear();
    if (card == selectedCard && (card.empty() || genericAvailable)) return true;
    selectedCard = card;
    genericAvailable = false;
    if (card.empty()) return true;
    Card handle;
    std::vector<Control> controls;
    if (!readControls(handle, controls, error)) return true; // Fixed-output cards have no mixer settings.
    genericAvailable = true;
    const std::string baselinePath = settingsPath("/etc/" MULTIROOM_BRAND_SLUG "-dac.json") + ".defaults";
    if (!fileutil::exists(baselinePath))
    {
        auto baseline = json::Value::makeObject();
        for (const auto& control : controls)
        {
            auto values = control.values;
            for (auto& value : values) value = std::max(control.minimum, std::min(control.maximum, value));
            baseline[control.name] = valuesJson(values);
        }
        if (!fileutil::writeTextFileAtomic(baselinePath, json::serialize(baseline) + "\n", error))
        {
            genericAvailable = false;
            return false;
        }
    }
    return restore(error);
}
} // namespace multiroom::dac_settings
