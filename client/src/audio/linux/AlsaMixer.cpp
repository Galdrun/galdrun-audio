#include "audio/linux/AlsaMixer.h"

#include "config/BoardConfig.h"
#include "util/Json.h"
#include "util/Logger.h"

#include <alsa/asoundlib.h>

#include <cmath>

namespace multiroom {
namespace {

// The names drivers give the control that sets the output level, most
// specific first. "Digital" is the usual one on I2S DAC boards.
const char* const kPreferredElements[] = {"Master", "PCM", "Digital", "Speaker", "Headphone",
                                          "Line Out"};

// Owns a snd_mixer_t across the early exits below.
class MixerHandle
{
public:
    MixerHandle() = default;
    ~MixerHandle() { close(); }

    MixerHandle(const MixerHandle&) = delete;
    MixerHandle& operator=(const MixerHandle&) = delete;

    bool open(const std::string& controlName, std::string& error)
    {
        int result = snd_mixer_open(&mixer_, 0);
        if (result < 0)
        {
            mixer_ = nullptr;
            error = std::string("cannot open a mixer: ") + snd_strerror(result);
            return false;
        }
        if ((result = snd_mixer_attach(mixer_, controlName.c_str())) < 0 ||
            (result = snd_mixer_selem_register(mixer_, nullptr, nullptr)) < 0 ||
            (result = snd_mixer_load(mixer_)) < 0)
        {
            error = "cannot load the mixer of " + controlName + ": " + snd_strerror(result);
            close();
            return false;
        }
        return true;
    }

    void close()
    {
        if (mixer_ != nullptr)
        {
            snd_mixer_close(mixer_);
            mixer_ = nullptr;
        }
    }

    snd_mixer_t* get() const { return mixer_; }

private:
    snd_mixer_t* mixer_ = nullptr;
};

bool isPlaybackVolume(snd_mixer_elem_t* element)
{
    return element != nullptr && snd_mixer_selem_is_active(element) != 0 &&
           snd_mixer_selem_has_playback_volume(element) != 0;
}

snd_mixer_elem_t* findElement(snd_mixer_t* mixer, const char* name)
{
    snd_mixer_selem_id_t* id = nullptr;
    snd_mixer_selem_id_alloca(&id);
    snd_mixer_selem_id_set_index(id, 0);
    snd_mixer_selem_id_set_name(id, name);
    snd_mixer_elem_t* element = snd_mixer_find_selem(mixer, id);
    return isPlaybackVolume(element) ? element : nullptr;
}

snd_mixer_elem_t* chooseElement(snd_mixer_t* mixer)
{
    for (const char* name : kPreferredElements)
    {
        if (snd_mixer_elem_t* element = findElement(mixer, name))
            return element;
    }

    // None of the usual names: take the card's only playback volume, and no
    // pick among several, where the others may be a loopback or a monitor.
    snd_mixer_elem_t* only = nullptr;
    for (snd_mixer_elem_t* element = snd_mixer_first_elem(mixer); element != nullptr;
         element = snd_mixer_elem_next(element))
    {
        if (!isPlaybackVolume(element))
            continue;
        if (only != nullptr)
            return nullptr;
        only = element;
    }
    return only;
}

bool describeElement(snd_mixer_elem_t* element, HardwareVolumeInfo& out)
{
    long minStep = 0;
    long maxStep = 0;
    if (snd_mixer_selem_get_playback_volume_range(element, &minStep, &maxStep) < 0 ||
        maxStep <= minStep)
    {
        return false; // a control with one step cannot set a volume
    }

    HardwareVolumeInfo info;
    info.available = true;
    info.control = snd_mixer_selem_get_name(element);
    info.minStep = minStep;
    info.maxStep = maxStep;

    long minDb = 0;
    long maxDb = 0;
    if (snd_mixer_selem_get_playback_dB_range(element, &minDb, &maxDb) == 0 && maxDb > minDb)
    {
        info.hasDecibels = true;
        info.minDb = static_cast<double>(minDb) / 100.0; // ALSA counts in 0.01 dB
        info.maxDb = static_cast<double>(maxDb) / 100.0;
    }
    info.hasMute = snd_mixer_selem_has_playback_switch(element) != 0;

    out = info;
    return true;
}

// "hw:CARD=x,DEV=n" -> "hw:CARD=x", the card's control device. False for
// "default" and anything else that is not one card.
bool controlNameForDevice(const std::string& deviceId, std::string& out)
{
    const std::string prefix = "hw:CARD=";
    if (deviceId.compare(0, prefix.size(), prefix) != 0)
        return false;
    out = deviceId.substr(0, deviceId.find(','));
    return out.size() > prefix.size();
}

class AlsaHardwareVolume final : public IHardwareVolume
{
public:
    explicit AlsaHardwareVolume(const HardwareVolumeInfo& info) : info_(info) {}

    bool open(const std::string& controlName, std::string& error)
    {
        if (!mixer_.open(controlName, error))
            return false;
        element_ = findElement(mixer_.get(), info_.control.c_str());
        if (element_ == nullptr)
        {
            error = "the mixer of " + controlName + " has no playback volume \"" +
                    info_.control + "\" any more";
            return false;
        }
        return true;
    }

    bool set(double gain, bool muted, std::string& error) override
    {
        int result = 0;
        if (muted && !info_.hasMute)
        {
            result = snd_mixer_selem_set_playback_volume_all(element_, info_.minStep);
        }
        else if (info_.hasDecibels)
        {
            // Rounded down to a step: never louder than the room asked for.
            const double db = hardware_volume::decibelsForGain(info_, gain);
            result = snd_mixer_selem_set_playback_dB_all(
                element_, static_cast<long>(std::lround(db * 100.0)), -1);
        }
        else
        {
            result = snd_mixer_selem_set_playback_volume_all(
                element_, static_cast<long>(hardware_volume::stepForGain(info_, gain)));
        }

        if (result >= 0 && info_.hasMute)
            result = snd_mixer_selem_set_playback_switch_all(element_, muted ? 0 : 1);

        if (result < 0)
        {
            error = "cannot set \"" + info_.control + "\": " + snd_strerror(result);
            return false;
        }
        return true;
    }

    bool setMute(bool muted, std::string& error) override
    {
        const int result =
            info_.hasMute ? snd_mixer_selem_set_playback_switch_all(element_, muted ? 0 : 1)
                          : -ENOTSUP;
        if (result < 0)
        {
            error = "cannot mute \"" + info_.control + "\": " + snd_strerror(result);
            return false;
        }
        return true;
    }

    bool read(HardwareVolumeState& out, std::string& error) override
    {
        // Takes in what changed since the last read, alsamixer included.
        snd_mixer_handle_events(mixer_.get());

        const snd_mixer_selem_channel_id_t channel = SND_MIXER_SCHN_FRONT_LEFT;
        HardwareVolumeState state;
        int result = 0;
        if (info_.hasDecibels)
        {
            long db = 0;
            result = snd_mixer_selem_get_playback_dB(element_, channel, &db);
            state.hasDecibels = true;
            state.db = static_cast<double>(db) / 100.0;
            state.gain = hardware_volume::gainForDecibels(info_, state.db);
        }
        else
        {
            long step = 0;
            result = snd_mixer_selem_get_playback_volume(element_, channel, &step);
            state.gain = hardware_volume::gainForStep(info_, step);
        }

        int on = 1;
        if (result >= 0 && info_.hasMute)
            result = snd_mixer_selem_get_playback_switch(element_, channel, &on);
        state.muted = on == 0;

        if (result < 0)
        {
            error = "cannot read \"" + info_.control + "\": " + snd_strerror(result);
            return false;
        }
        out = state;
        return true;
    }

private:
    HardwareVolumeInfo info_;
    MixerHandle mixer_;
    snd_mixer_elem_t* element_ = nullptr; // owned by mixer_
};

} // namespace

bool detectAlsaHardwareVolume(const std::string& cardId, HardwareVolumeInfo& out)
{
    const std::string controlName = "hw:CARD=" + cardId;
    MixerHandle mixer;
    std::string error;
    if (!mixer.open(controlName, error))
    {
        MULTIROOM_DEBUG("alsa: %s", error.c_str());
        return false;
    }

    snd_mixer_elem_t* element = chooseElement(mixer.get());
    if (element == nullptr || !describeElement(element, out))
    {
        MULTIROOM_DEBUG("alsa: %s has no playback volume to use; the room's volume stays in "
                  "software",
                  controlName.c_str());
        return false;
    }
    return true;
}

namespace {

// The board's verified clock trim, if it has one. Read once: the board file
// is part of the firmware image.
const std::string& boardClockControl()
{
    static const std::string control = [] {
        std::string name;
        json::getString(board_config::current(), "hardwareClockControl", name);
        return name;
    }();
    return control;
}

// One retained control handle for the trim.
class AlsaHardwareClock final : public IHardwareClock
{
public:
    ~AlsaHardwareClock() override
    {
        if (handle_ != nullptr)
            snd_ctl_close(handle_);
    }

    bool open(const std::string& controlName, const HardwareClockInfo& info, std::string& error)
    {
        info_ = info;
        int result = snd_ctl_open(&handle_, controlName.c_str(), 0);
        if (result < 0)
        {
            handle_ = nullptr;
            error = "cannot open " + controlName + ": " + snd_strerror(result);
            return false;
        }
        snd_ctl_elem_info_t* elementInfo;
        snd_ctl_elem_info_alloca(&elementInfo);
        if (!find(handle_, info.control, elementInfo, error))
            return false;
        numid_ = snd_ctl_elem_info_get_numid(elementInfo);
        return true;
    }

    bool set(int32_t ppm, std::string& error) override
    {
        if (ppm < info_.minPpm || ppm > info_.maxPpm)
        {
            error = "clock trim " + std::to_string(ppm) + " ppm is outside the device's range";
            return false;
        }
        snd_ctl_elem_value_t* value;
        snd_ctl_elem_value_alloca(&value);
        snd_ctl_elem_value_set_numid(value, numid_);
        snd_ctl_elem_value_set_integer(value, 0, ppm);
        int result = snd_ctl_elem_write(handle_, value);
        if (result >= 0)
            result = snd_ctl_elem_read(handle_, value);
        if (result < 0)
        {
            error = "cannot set \"" + info_.control + "\": " + snd_strerror(result);
            return false;
        }
        if (snd_ctl_elem_value_get_integer(value, 0) != ppm)
        {
            error = "\"" + info_.control + "\" reads back a different trim";
            return false;
        }
        return true;
    }

    // The element by name, as a writable one-channel integer. Shared with the
    // scan so both agree on what counts as the control.
    static bool find(snd_ctl_t* handle, const std::string& name, snd_ctl_elem_info_t* info,
                     std::string& error)
    {
        snd_ctl_elem_id_t* id;
        snd_ctl_elem_id_alloca(&id);
        snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_MIXER);
        snd_ctl_elem_id_set_name(id, name.c_str());
        snd_ctl_elem_info_set_id(info, id);
        const int result = snd_ctl_elem_info(handle, info);
        if (result < 0 || snd_ctl_elem_info_get_type(info) != SND_CTL_ELEM_TYPE_INTEGER ||
            snd_ctl_elem_info_get_count(info) != 1 || !snd_ctl_elem_info_is_readable(info) ||
            !snd_ctl_elem_info_is_writable(info))
        {
            error = "no writable one-channel integer control \"" + name + "\"";
            return false;
        }
        return true;
    }

private:
    snd_ctl_t* handle_ = nullptr;
    unsigned int numid_ = 0;
    HardwareClockInfo info_;
};

} // namespace

bool detectAlsaHardwareClock(const std::string& cardId, HardwareClockInfo& out)
{
    const std::string& control = boardClockControl();
    if (control.empty())
        return false;

    snd_ctl_t* handle = nullptr;
    if (snd_ctl_open(&handle, ("hw:CARD=" + cardId).c_str(), 0) < 0)
        return false;
    snd_ctl_elem_info_t* info;
    snd_ctl_elem_info_alloca(&info);
    std::string error;
    const bool found = AlsaHardwareClock::find(handle, control, info, error);
    const long minimum = found ? snd_ctl_elem_info_get_min(info) : 0;
    const long maximum = found ? snd_ctl_elem_info_get_max(info) : 0;
    const long step = found ? snd_ctl_elem_info_get_step(info) : 0;
    snd_ctl_close(handle);
    if (!found || minimum > 0 || maximum < 0 || minimum >= maximum || minimum < -10000 ||
        maximum > 10000)
    {
        MULTIROOM_DEBUG("alsa: hw:CARD=%s: %s", cardId.c_str(),
                  found ? "the clock trim's range does not include 0" : error.c_str());
        return false;
    }

    out.available = true;
    out.control = control;
    out.minPpm = static_cast<int32_t>(minimum);
    out.maxPpm = static_cast<int32_t>(maximum);
    out.stepPpm = step > 0 ? static_cast<uint32_t>(step) : 1;
    return true;
}

std::unique_ptr<IHardwareClock> openAlsaHardwareClock(const AudioDeviceInfo& device)
{
    std::string controlName;
    if (!device.hardwareClock.available || !controlNameForDevice(device.id, controlName))
        return nullptr;

    std::unique_ptr<AlsaHardwareClock> clock(new AlsaHardwareClock());
    std::string error;
    if (!clock->open(controlName, device.hardwareClock, error))
    {
        MULTIROOM_WARN("alsa: %s; drift stays with the server's resampler", error.c_str());
        return nullptr;
    }
    return clock;
}

std::unique_ptr<IHardwareVolume> openAlsaHardwareVolume(const AudioDeviceInfo& device)
{
    std::string controlName;
    if (!device.hardwareVolume.available || !controlNameForDevice(device.id, controlName))
        return nullptr;

    std::unique_ptr<AlsaHardwareVolume> volume(new AlsaHardwareVolume(device.hardwareVolume));
    std::string error;
    if (!volume->open(controlName, error))
    {
        MULTIROOM_WARN("alsa: %s; the room's volume stays in software", error.c_str());
        return nullptr;
    }
    return volume;
}

} // namespace multiroom
