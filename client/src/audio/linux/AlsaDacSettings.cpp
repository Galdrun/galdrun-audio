#include "config/DacSettings.h"
#include "config/BoardConfig.h"
#include "util/FileUtil.h"
#include <alsa/asoundlib.h>
#include <algorithm>
#include <cmath>
#include <vector>

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
    if (failed(snd_ctl_open(&card.handle, "hw:Connector2", SND_CTL_NONBLOCK), error)) return false;
    snd_ctl_elem_id_t* id;
    snd_ctl_elem_info_t* info;
    snd_ctl_elem_value_t* value;
    snd_ctl_elem_id_alloca(&id);
    snd_ctl_elem_info_alloca(&info);
    snd_ctl_elem_value_alloca(&value);
    for (const auto& initial : kControls)
    {
        const char* name = initial.name;
        snd_ctl_elem_id_clear(id);
        snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_MIXER);
        snd_ctl_elem_id_set_name(id, name);
        snd_ctl_elem_info_clear(info);
        snd_ctl_elem_info_set_id(info, id);
        const int result = snd_ctl_elem_info(card.handle, info);
        if (result == -ENOENT) continue; // Older firmware lacks the output-source controls.
        if (failed(result, error)) return false;
        if (!snd_ctl_elem_info_is_readable(info) || !snd_ctl_elem_info_is_writable(info)) continue;
        Control control;
        control.id = snd_ctl_elem_info_get_numid(info);
        control.name = name;
        control.defaultValue = initial.value;
        control.type = snd_ctl_elem_info_get_type(info);
        const unsigned int count = snd_ctl_elem_info_get_count(info);
        if (count == 0 || count > 2) { error = "unsupported DAC control channel count"; return false; }
        if (control.type == SND_CTL_ELEM_TYPE_INTEGER)
        {
            control.minimum = snd_ctl_elem_info_get_min(info);
            control.maximum = snd_ctl_elem_info_get_max(info);
            control.step = std::max(1L, snd_ctl_elem_info_get_step(info));
        }
        else if (control.type == SND_CTL_ELEM_TYPE_ENUMERATED)
        {
            const unsigned int items = snd_ctl_elem_info_get_items(info);
            if (items == 0 || items > 32) { error = "unsupported DAC choices"; return false; }
            control.maximum = items - 1;
            for (unsigned int item = 0; item < items; ++item)
            {
                snd_ctl_elem_info_set_item(info, item);
                if (failed(snd_ctl_elem_info(card.handle, info), error)) return false;
                control.items.emplace_back(snd_ctl_elem_info_get_item_name(info));
            }
        }
        else if (control.type != SND_CTL_ELEM_TYPE_BOOLEAN)
        { error = "unsupported DAC control type"; return false; }
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
                (static_cast<long>(value) - found->minimum) % found->step != 0)
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
    return board_config::enabled(board_config::current(), "dacControls");
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
    settings["codec"] = json::Value("CS4271");
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
    return apply(changes, &path, error) && load(settings, error);
}

bool restore(std::string& error, const std::string& path)
{
    if (!fileutil::exists(path)) return true;
    json::Value saved;
    return readSaved(path, saved, error) && apply(saved, nullptr, error);
}

bool isRoomVolumeControl(const std::string& name)
{
    return name == "Master Playback Volume" || name == "Master Playback Switch";
}

bool reset(json::Value& settings, std::string& error, bool keepRoomVolume, const std::string& path)
{
    error.clear();
    Card card;
    std::vector<Control> controls;
    if (!readControls(card, controls, error)) return false;
    auto defaults = json::Value::makeObject();
    for (const auto& control : controls)
        if (!keepRoomVolume || !isRoomVolumeControl(control.name))
            defaults[control.name] = valuesJson(std::vector<long>(control.values.size(), control.defaultValue));
    return apply(defaults, &path, error, true) && load(settings, error);
}
} // namespace multiroom::dac_settings
