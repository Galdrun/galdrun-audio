#include "app/DeviceSelection.h"

#include "app/StreamPlayback.h"
#include "audio/AudioBackend.h"
#include "audio/FormatProbe.h"
#include "matp/Session.h"
#include "util/Logger.h"
#include "util/Time.h"
#if defined(__linux__) && !defined(ESP_PLATFORM)
#include "config/BoardConfig.h"
#include "config/DacSettings.h"
#endif

#include <algorithm>
#include <cstdio>
#include <memory>
#include <utility>
#include <vector>

namespace multiroom {
namespace {

void logFormats(const char* label, const std::vector<AudioFormat>& formats)
{
    if (formats.empty() || !log::enabled(LogLevel::Debug))
        return;

    // One line per rate keeps the output readable on devices that accept a
    // hundred combinations.
    for (size_t r = 0; r < probe::kSampleRateCount; ++r)
    {
        const uint32_t rate = probe::kSampleRates[r];
        std::string types;
        for (size_t i = 0; i < formats.size(); ++i)
        {
            if (formats[i].sampleRate != rate)
                continue;
            char entry[32];
            std::snprintf(entry, sizeof(entry), "%s/%uch", sampleTypeName(formats[i].sampleType),
                          static_cast<unsigned>(formats[i].channels));
            if (!types.empty())
                types += " ";
            types += entry;
        }
        if (!types.empty())
            MULTIROOM_DEBUG("      %s %lu Hz: %s", label, static_cast<unsigned long>(rate),
                      types.c_str());
    }
}

// Resolves the configured device, falling back to the default one so a stale
// audioDeviceId in client.json does not leave the client mute.
const AudioDeviceInfo* selectDevice(const AudioDeviceCacheData& cache,
                                    const std::string& configuredId)
{
#if defined(__linux__) && !defined(ESP_PLATFORM)
    const bool requireHardware = board_config::enabled(board_config::current(), "requireConfiguredAudioDevice");
    if (requireHardware && configuredId.compare(0, 8, "hw:CARD=") != 0)
        return nullptr;
#endif
    if (!configuredId.empty())
    {
        const AudioDeviceInfo* device = cache.findById(configuredId);
        if (device != nullptr)
            return device;
#if defined(__linux__) && !defined(ESP_PLATFORM)
        if (requireHardware)
            return nullptr;
#endif
        MULTIROOM_WARN("configured audio device %s not found, falling back to the default",
                 configuredId.c_str());
    }
#if defined(__linux__) && !defined(ESP_PLATFORM)
    if (requireHardware)
        return nullptr;
#endif
    return cache.findDefault();
}

} // namespace

void logDeviceSummary(const AudioDeviceCacheData& cache)
{
    MULTIROOM_INFO("audio devices: %lu, scanned %s",
             static_cast<unsigned long>(cache.devices.size()), cache.generatedAt.c_str());

    for (size_t i = 0; i < cache.devices.size(); ++i)
    {
        const AudioDeviceInfo& device = cache.devices[i];
        MULTIROOM_INFO("  %s%s", device.name.c_str(), device.isDefault ? " (default)" : "");
        MULTIROOM_INFO("    id: %s", device.id.c_str());
        MULTIROOM_INFO("    formats: %lu shared, %lu exclusive",
                 static_cast<unsigned long>(device.sharedFormats.size()),
                 static_cast<unsigned long>(device.exclusiveFormats.size()));
        if (device.hasMixFormat)
            MULTIROOM_INFO("    mix format: %s", describeFormat(device.mixFormat).c_str());
        if (device.buffer.minPeriodNs != 0 || device.buffer.defaultPeriodNs != 0)
            MULTIROOM_INFO("    period: min %lu us, default %lu us",
                     static_cast<unsigned long>(device.buffer.minPeriodNs / 1000),
                     static_cast<unsigned long>(device.buffer.defaultPeriodNs / 1000));

        logFormats("shared   ", device.sharedFormats);
        logFormats("exclusive", device.exclusiveFormats);
    }
}

bool rescanAudioDevices(const std::string& cachePath, const I2sSettings& i2s,
                        AudioDeviceCacheData& cache, std::string& error)
{
    std::unique_ptr<IAudioBackend> backend = createPlatformAudioBackend(i2s);
    if (!backend)
    {
        error = "no audio backend compiled in for this platform";
        MULTIROOM_ERROR("%s", error.c_str());
        return false;
    }

    MULTIROOM_INFO("audio scan started (%s)", platformAudioBackendName());
    const uint64_t started = monotonicNowNs();

    auto devices = backend->enumeratePlaybackDevices(CapabilityScan::Full);
    for (AudioDeviceInfo& device : devices)
    {
        const AudioDeviceInfo* previous = cache.findById(device.id);
        if (previous == nullptr)
            continue;
        if (device.exclusiveProbeBusy)
        {
            device.exclusiveFormats = previous->exclusiveFormats;
            device.buffer = previous->buffer;
        }
        if (device.sharedProbeBusy)
            device.sharedFormats = previous->sharedFormats;
        if (device.exclusiveProbeBusy || device.sharedProbeBusy)
            MULTIROOM_INFO("audio scan: %s is busy; retaining cached formats for the busy mode(s)",
                     device.id.c_str());
    }
    cache.devices = std::move(devices);

    const uint64_t elapsedMs = (monotonicNowNs() - started) / 1000000ULL;
    MULTIROOM_INFO("audio scan finished: %lu device(s) in %lu ms",
             static_cast<unsigned long>(cache.devices.size()),
             static_cast<unsigned long>(elapsedMs));

    if (cache.devices.empty())
    {
        const std::string backendError = backend->lastError();
        if (!backendError.empty())
            MULTIROOM_WARN("audio scan: %s", backendError.c_str());
    }

    std::string saveError;
    if (!device_cache::save(cachePath, cache, saveError))
        MULTIROOM_WARN("could not write audio cache %s: %s", cachePath.c_str(), saveError.c_str());
    else
        MULTIROOM_INFO("audio cache written to %s", cachePath.c_str());

    return true;
}

void applyDeviceSelection(ClientRuntime& runtime)
{
    const ClientConfig& config = *runtime.config;

    runtime.deviceSelected = false;
    runtime.formatSelected = false;
    runtime.deviceId.clear();
    runtime.deviceName.clear();
    runtime.format = AudioFormat();
    runtime.accessMode = AudioAccessMode::Shared;

    const AudioDeviceInfo* device = selectDevice(*runtime.deviceCache, config.audioDeviceId);
    if (device == nullptr)
    {
#if defined(__linux__) && !defined(ESP_PLATFORM)
        std::string error;
        dac_settings::selectDevice("", "", error);
#endif
        MULTIROOM_ERROR("no usable audio playback device");
        return;
    }

    runtime.deviceSelected = true;
    runtime.deviceId = device->id;
    runtime.deviceName = device->name;
#if defined(__linux__) && !defined(ESP_PLATFORM)
    std::string dacError;
    if (!dac_settings::selectDevice(device->id, device->hardwareVolume.control, dacError))
        MULTIROOM_WARN("cannot restore selected-card DAC settings: %s", dacError.c_str());
#endif

    MULTIROOM_INFO("selected device: %s [%s]", device->name.c_str(), device->id.c_str());
    MULTIROOM_INFO("access mode: %s", audioAccessPreferenceName(config.audioAccessMode));

    AudioFormat format;
    AudioAccessMode mode = AudioAccessMode::Shared;
    if (!chooseOutputFormat(*device, config.preferredSampleRate, 2, config.audioAccessMode,
                            format, mode))
    {
        if (config.audioAccessMode == AudioAccessPreference::ExclusiveOnly)
        {
            // Deliberate: exclusive-only means the user would rather see this
            // than have the client play through the mixer unnoticed.
            MULTIROOM_ERROR("device %s offers no exclusive-mode format and audioAccessMode is "
                      "exclusive-only",
                      device->id.c_str());
        }
        else
        {
            MULTIROOM_ERROR("device %s reported no usable playback format", device->id.c_str());
        }
        return;
    }

    MULTIROOM_INFO("selected format: %s (%s mode)", describeFormat(format).c_str(),
             audioAccessModeName(mode));
    runtime.formatSelected = true;
    runtime.format = format;
    runtime.accessMode = mode;

    // Worth saying out loud: in shared mode the system mixer resamples, so the
    // clock we can measure is its own and not the sound card's, which weakens
    // the drift figure the server corrects against.
    if (mode == AudioAccessMode::Shared &&
        config.audioAccessMode == AudioAccessPreference::PreferExclusive)
    {
        MULTIROOM_WARN("falling back to shared mode: %s offers no exclusive format at "
                 "%lu Hz; drift measurement will be less accurate",
                 device->name.c_str(), static_cast<unsigned long>(format.sampleRate));
    }

    if (format.sampleRate != config.preferredSampleRate)
        MULTIROOM_WARN("preferred sample rate %lu Hz is not supported by this device",
                 static_cast<unsigned long>(config.preferredSampleRate));
}

bool currentPlaybackTarget(const ClientRuntime& runtime, AudioEngine::Settings& settings,
                           std::string& error)
{
    if (!runtime.formatSelected)
    {
        error = runtime.deviceSelected ? "the selected device offers no usable playback format"
                                       : "no usable audio playback device";
        return false;
    }

    settings.deviceId = runtime.deviceId;
    settings.format = runtime.format;
    settings.accessMode = runtime.accessMode;
    return true;
}

bool currentCapabilities(const ClientRuntime& runtime, matp::CapabilityReport& report,
                         std::string& error)
{
    if (!runtime.deviceSelected)
    {
        error = "no usable audio playback device";
        return false;
    }
    if (!runtime.formatSelected)
    {
        error = "the selected device offers no usable playback format";
        return false;
    }

    const AudioDeviceInfo* device = runtime.deviceCache->findById(runtime.deviceId);
    if (device == nullptr)
    {
        error = "the selected device is no longer in the cache";
        return false;
    }

    report.deviceId = device->id;
    report.deviceName = device->name;
    report.preferredSampleRate = runtime.config->preferredSampleRate;
    report.accessMode = audioAccessModeName(runtime.accessMode);

    // What the scan found. The control can still fail to open, and the room
    // then plays in software; SET_VOLUME is the same message either way.
    const HardwareVolumeInfo& volume = device->hardwareVolume;
    report.hasHardwareVolume = true;
    report.hardwareVolume = matp::HardwareVolumeCapability();
    report.hardwareVolume.available = volume.available;
    if (volume.available)
    {
        report.hardwareVolume.control = volume.control;
        report.hardwareVolume.steps = static_cast<uint32_t>(volume.maxStep - volume.minStep + 1);
        report.hardwareVolume.hasDecibels = volume.hasDecibels;
        report.hardwareVolume.minDb = volume.minDb;
        report.hardwareVolume.maxDb = volume.maxDb;
        report.hardwareVolume.hasMute = volume.hasMute;
    }

    // The actuator asked for, and the device's verified clock trim. The server
    // selects the hardware clock only with all three, and the stream only
    // uses it once the opened device confirms it at its nominal rate.
    report.driftFeedback = true;
    report.driftCorrection = runtime.config->driftCorrection;
    report.hasHardwareClock = true;
    report.hardwareClock = matp::HardwareClockCapability();
    report.hardwareClock.available = device->hardwareClock.available;
    if (device->hardwareClock.available)
    {
        report.hardwareClock.control = device->hardwareClock.control;
        report.hardwareClock.minPpm = device->hardwareClock.minPpm;
        report.hardwareClock.maxPpm = device->hardwareClock.maxPpm;
        report.hardwareClock.stepPpm = device->hardwareClock.stepPpm;
    }
    // Only the formats of the mode the device will actually be opened in.
    // Reporting the other list as well would let the server pick a format that
    // forces the system mixer between the client and the sound card, and with
    // it the drift figure the server corrects against.
    const std::vector<AudioFormat>& formats = device->formats(runtime.accessMode);
    if (formats.empty())
    {
        error = "the selected device accepted no format in " +
                std::string(audioAccessModeName(runtime.accessMode)) + " mode";
        return false;
    }

    // And only those whose ring buffer fits at the configured size (MATP
    // section 58): on the ESP32 a larger buffer leaves the widest
    // formats without the memory to be played.
    report.formats.clear();
    std::string fitError;
    for (const AudioFormat& format : formats)
    {
        if (streamBufferFits(format, runtime.config->bufferMs, fitError))
            report.formats.push_back(format);
    }
    if (report.formats.empty())
    {
        error = "no format of the selected device fits the ring buffer: " + fitError;
        return false;
    }
    return true;
}

void currentAudioDevices(const ClientRuntime& runtime, matp::AudioDeviceList& out)
{
    out = matp::AudioDeviceList();
    out.configuredDeviceId = runtime.config->audioDeviceId;
    if (runtime.deviceSelected)
        out.selectedDeviceId = runtime.deviceId;

    const std::vector<AudioDeviceInfo>& devices = runtime.deviceCache->devices;
    for (size_t i = 0; i < devices.size() && i < matp::kMaxAudioDevices; ++i)
    {
        matp::AudioDeviceEntry entry;
        entry.id = devices[i].id;
        entry.name = devices[i].name;
        entry.isDefault = devices[i].isDefault;
        out.devices.push_back(entry);
    }
}

AudioDeviceChange changeAudioDevice(ClientRuntime& runtime, const std::string& deviceId,
                                    std::string& error)
{
#if defined(__linux__) && !defined(ESP_PLATFORM)
    if (!deviceId.empty() && board_config::enabled(board_config::current(), "requireConfiguredAudioDevice") &&
        deviceId.compare(0, 8, "hw:CARD=") != 0)
    {
        error = "select a concrete ALSA hardware output";
        return AudioDeviceChange::UnknownDevice;
    }
#endif
    // Only a device the last scan found: a typo stored here would be a stale
    // id the selection silently falls back from on every start.
    if (!deviceId.empty() && runtime.deviceCache->findById(deviceId) == nullptr)
    {
        error = "no audio device \"" + deviceId + "\"; rescan if it was just connected";
        return AudioDeviceChange::UnknownDevice;
    }

    ClientConfig& config = *runtime.config;
    const std::string previous = config.audioDeviceId;
    if (deviceId == previous)
        return AudioDeviceChange::Unchanged;

    config.audioDeviceId = deviceId;
    std::string saveError;
    if (!saveConfig(runtime, config, saveError))
    {
        config.audioDeviceId = previous;
        error = "could not write " + runtime.configPath + ": " + saveError;
        return AudioDeviceChange::NotSaved;
    }

    MULTIROOM_INFO("audio device changed to %s", deviceId.empty() ? "<system default>" : deviceId.c_str());
    applyDeviceSelection(runtime);
    if (runtime.playback != nullptr)
        runtime.playback->updateVolumePath();
    if (runtime.session != nullptr)
    {
        runtime.session->capabilitiesChanged();
        runtime.session->audioDevicesChanged();
    }
    return AudioDeviceChange::Changed;
}

BufferChange changeBuffer(ClientRuntime& runtime, uint32_t bufferMs, std::string& error)
{
    ClientConfig& config = *runtime.config;
    ClientConfig candidate = config;
    candidate.bufferMs = bufferMs;
    if (!client_config::validate(candidate, error))
        return BufferChange::Refused;
    if (bufferMs == config.bufferMs)
        return BufferChange::Unchanged;

    // A device left with no format to offer could not be negotiated with at
    // all, which is a room that went silent on a setting.
    const AudioDeviceInfo* device =
        runtime.deviceSelected ? runtime.deviceCache->findById(runtime.deviceId) : nullptr;
    if (device != nullptr)
    {
        const std::vector<AudioFormat>& formats = device->formats(runtime.accessMode);
        std::string fitError;
        const bool anyFits = std::any_of(
            formats.begin(), formats.end(), [&](const AudioFormat& format) {
                return streamBufferFits(format, bufferMs, fitError);
            });
        if (!formats.empty() && !anyFits)
        {
            error = "no format of " + device->name + " would fit: " + fitError;
            return BufferChange::Refused;
        }
    }
    if (runtime.playback != nullptr && !runtime.playback->bufferFits(bufferMs, error))
        return BufferChange::Refused;

    const uint32_t previous = config.bufferMs;
    config.bufferMs = bufferMs;
    std::string saveError;
    if (!saveConfig(runtime, config, saveError))
    {
        config.bufferMs = previous;
        error = "could not write " + runtime.configPath + ": " + saveError;
        return BufferChange::NotSaved;
    }

    MULTIROOM_INFO("buffer changed from %lu to %lu ms", static_cast<unsigned long>(previous),
             static_cast<unsigned long>(bufferMs));
    if (runtime.playback != nullptr)
        runtime.playback->setBufferMs(bufferMs);
    if (runtime.session != nullptr)
    {
        // The answer first, then what it did to the formats.
        runtime.session->bufferConfigChanged();
        runtime.session->capabilitiesChanged();
    }
    return BufferChange::Changed;
}

NameChange changeName(ClientRuntime& runtime, const std::string& name, std::string& error)
{
    if (!matp::isValidRoomName(name))
    {
        error = "invalid name \"" + name +
                "\": 1 to 64 bytes, no control characters or surrounding blanks";
        return NameChange::Refused;
    }
    ClientConfig& config = *runtime.config;
    if (name == config.name)
        return NameChange::Unchanged;

    const std::string previous = config.name;
    config.name = name;
    std::string saveError;
    if (!saveConfig(runtime, config, saveError))
    {
        config.name = previous;
        error = "could not write " + runtime.configPath + ": " + saveError;
        return NameChange::NotSaved;
    }

    MULTIROOM_INFO("room renamed from \"%s\" to \"%s\"", previous.c_str(), name.c_str());
    if (runtime.session != nullptr)
        runtime.session->roomNameChanged();
    return NameChange::Changed;
}

VolumeControlChange changeVolumeControl(ClientRuntime& runtime, matp::VolumeControlMode mode,
                                        std::string& error)
{
    if (mode == matp::VolumeControlMode::Software &&
        runtime.audioControl == matp::AudioControl::BitPerfect)
    {
        error = "software volume is not available while the room's Audio Control is bitPerfect";
        return VolumeControlChange::Refused;
    }

    ClientConfig& config = *runtime.config;
    const matp::VolumeControlMode previous = config.volumeControl;
    if (mode == previous)
        return VolumeControlChange::Unchanged;

    config.volumeControl = mode;
    std::string saveError;
    if (!saveConfig(runtime, config, saveError))
    {
        config.volumeControl = previous;
        error = "could not write " + runtime.configPath + ": " + saveError;
        return VolumeControlChange::NotSaved;
    }

    MULTIROOM_INFO("volume control changed from %s to %s", matp::volumeControlModeName(previous),
             matp::volumeControlModeName(mode));
    if (runtime.playback != nullptr)
        runtime.playback->updateVolumePath();
    if (runtime.session != nullptr)
        runtime.session->volumeConfigChanged();
    return VolumeControlChange::Changed;
}

DriftCorrectionChange changeDriftCorrection(ClientRuntime& runtime,
                                            matp::DriftCorrectionMode mode, std::string& error)
{
    ClientConfig& config = *runtime.config;
    const matp::DriftCorrectionMode previous = config.driftCorrection;
    if (mode == previous)
        return DriftCorrectionChange::Unchanged;

    const AudioDeviceInfo* device = runtime.deviceSelected && runtime.deviceCache != nullptr
                                        ? runtime.deviceCache->findById(runtime.deviceId)
                                        : nullptr;
    if (mode == matp::DriftCorrectionMode::HardwareClock &&
        (device == nullptr || !device->hardwareClock.available))
    {
        error = "the selected device has no verified hardware clock";
        return DriftCorrectionChange::Refused;
    }

    config.driftCorrection = mode;
    std::string saveError;
    if (!saveConfig(runtime, config, saveError))
    {
        config.driftCorrection = previous;
        error = "could not write " + runtime.configPath + ": " + saveError;
        return DriftCorrectionChange::NotSaved;
    }

    MULTIROOM_INFO("drift correction changed from %s to %s",
             matp::driftCorrectionModeName(previous), matp::driftCorrectionModeName(mode));
    if (runtime.session != nullptr)
        runtime.session->capabilitiesChanged();
    return DriftCorrectionChange::Changed;
}

void applyAudioControl(ClientRuntime& runtime, matp::AudioControl control, bool known)
{
    if (runtime.audioControlKnown != known || runtime.audioControl != control)
        MULTIROOM_INFO("audio control: %s%s", matp::audioControlName(control),
                 known ? "" : " (the server did not say; assumed)");
    runtime.audioControlKnown = known;
    runtime.audioControl = control;

    ClientConfig& config = *runtime.config;
    if (control == matp::AudioControl::BitPerfect &&
        config.volumeControl == matp::VolumeControlMode::Software)
    {
        config.volumeControl = matp::VolumeControlMode::Auto;
        std::string saveError;
        if (!saveConfig(runtime, config, saveError))
            MULTIROOM_WARN("could not write %s: %s", runtime.configPath.c_str(), saveError.c_str());
        MULTIROOM_WARN("volume control reset from software to auto: the room's Audio Control is "
                 "bitPerfect, which allows no software volume");
    }

    if (runtime.playback != nullptr)
        runtime.playback->updateVolumePath();
    if (runtime.session != nullptr)
        runtime.session->volumeConfigChanged();
}

} // namespace multiroom
