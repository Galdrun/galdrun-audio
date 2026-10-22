#include "audio/AudioDeviceCache.h"

#include "util/FileUtil.h"
#include "util/Json.h"
#include "util/Time.h"

namespace multiroom {
namespace device_cache {

json::Value formatToJson(const AudioFormat& format)
{
    json::Value object = json::Value::makeObject();
    object["sampleRate"] = json::Value(format.sampleRate);
    object["channels"] = json::Value(static_cast<uint32_t>(format.channels));
    object["sampleType"] = json::Value(sampleTypeName(format.sampleType));
    return object;
}

} // namespace device_cache

namespace {

bool formatFromJson(const json::Value& object, AudioFormat& out)
{
    if (!object.isObject())
        return false;

    int64_t sampleRate = 0;
    int64_t channels = 0;
    std::string sampleType;
    if (!json::getInt(object, "sampleRate", sampleRate) ||
        !json::getInt(object, "channels", channels) ||
        !json::getString(object, "sampleType", sampleType))
    {
        return false;
    }
    if (sampleRate <= 0 || sampleRate > 3000000 || channels <= 0 || channels > 64)
        return false;

    AudioFormat format;
    format.sampleRate = static_cast<uint32_t>(sampleRate);
    format.channels = static_cast<uint16_t>(channels);
    if (!parseSampleType(sampleType, format.sampleType))
        return false;

    out = format;
    return true;
}

json::Value bufferLimitsToJson(const AudioBufferLimits& limits)
{
    json::Value object = json::Value::makeObject();
    object["minPeriodNs"] = json::Value(limits.minPeriodNs);
    object["defaultPeriodNs"] = json::Value(limits.defaultPeriodNs);
    object["minPeriodFrames"] = json::Value(limits.minPeriodFrames);
    object["maxPeriodFrames"] = json::Value(limits.maxPeriodFrames);
    object["minBufferFrames"] = json::Value(limits.minBufferFrames);
    object["maxBufferFrames"] = json::Value(limits.maxBufferFrames);
    return object;
}

// Buffer limits are advisory; missing or odd fields fall back to zero rather
// than invalidating the whole cache file.
void bufferLimitsFromJson(const json::Value& object, AudioBufferLimits& out)
{
    if (!object.isObject())
        return;

    int64_t value = 0;
    if (json::getInt(object, "minPeriodNs", value) && value >= 0)
        out.minPeriodNs = static_cast<uint64_t>(value);
    if (json::getInt(object, "defaultPeriodNs", value) && value >= 0)
        out.defaultPeriodNs = static_cast<uint64_t>(value);
    if (json::getInt(object, "minPeriodFrames", value) && value >= 0)
        out.minPeriodFrames = static_cast<uint32_t>(value);
    if (json::getInt(object, "maxPeriodFrames", value) && value >= 0)
        out.maxPeriodFrames = static_cast<uint32_t>(value);
    if (json::getInt(object, "minBufferFrames", value) && value >= 0)
        out.minBufferFrames = static_cast<uint32_t>(value);
    if (json::getInt(object, "maxBufferFrames", value) && value >= 0)
        out.maxBufferFrames = static_cast<uint32_t>(value);
}

// Advisory like the buffer limits: anything missing or inconsistent leaves
// the device without hardware volume, which is the software path and never
// wrong, rather than invalidating the cache.
void hardwareVolumeFromJson(const json::Value& object, HardwareVolumeInfo& out)
{
    HardwareVolumeInfo volume;
    if (!object.isObject() || !json::getBool(object, "available", volume.available) ||
        !volume.available)
        return;
    if (!json::getString(object, "control", volume.control) || volume.control.empty() ||
        !json::getInt(object, "minStep", volume.minStep) ||
        !json::getInt(object, "maxStep", volume.maxStep) || volume.maxStep <= volume.minStep)
        return;
    volume.hasDecibels = json::getDouble(object, "minDb", volume.minDb) &&
                         json::getDouble(object, "maxDb", volume.maxDb) &&
                         volume.maxDb > volume.minDb;
    if (!volume.hasDecibels)
        volume.minDb = volume.maxDb = 0.0;
    json::getBool(object, "hasMute", volume.hasMute);
    out = volume;
}

// Advisory too: the stream only uses the trim once the device has opened it
// at its nominal rate, so a stale entry costs a refused handshake at worst.
void hardwareClockFromJson(const json::Value& object, HardwareClockInfo& out)
{
    HardwareClockInfo clock;
    int64_t minPpm = 0, maxPpm = 0, step = 0;
    if (!object.isObject() || !json::getBool(object, "available", clock.available) ||
        !clock.available || !json::getString(object, "control", clock.control) ||
        clock.control.empty() || !json::getInt(object, "minPpm", minPpm) ||
        !json::getInt(object, "maxPpm", maxPpm) || !json::getInt(object, "stepPpm", step) ||
        minPpm > 0 || maxPpm < 0 || minPpm >= maxPpm || minPpm < -10000 || maxPpm > 10000 ||
        step <= 0 || step > maxPpm - minPpm)
        return;
    clock.minPpm = static_cast<int32_t>(minPpm);
    clock.maxPpm = static_cast<int32_t>(maxPpm);
    clock.stepPpm = static_cast<uint32_t>(step);
    out = clock;
}

json::Value hardwareClockToJson(const HardwareClockInfo& clock)
{
    json::Value object = json::Value::makeObject();
    object["available"] = json::Value(clock.available);
    if (!clock.available)
        return object;
    object["control"] = json::Value(clock.control);
    object["minPpm"] = json::Value(clock.minPpm);
    object["maxPpm"] = json::Value(clock.maxPpm);
    object["stepPpm"] = json::Value(clock.stepPpm);
    return object;
}

json::Value formatListToJson(const std::vector<AudioFormat>& formats)
{
    json::Value array = json::Value::makeArray();
    for (size_t i = 0; i < formats.size(); ++i)
        array.pushBack(device_cache::formatToJson(formats[i]));
    return array;
}

// A missing list is empty; a present list with a bad entry invalidates the
// whole cache, because silently dropping a format would make the client report
// wrong capabilities to the server.
bool formatListFromJson(const json::Value& object, const char* key,
                        std::vector<AudioFormat>& out, std::string& error)
{
    const json::Value* array = object.find(key);
    if (array == nullptr || array->isNull())
        return true;
    if (!array->isArray())
    {
        error = std::string(key) + " is not an array";
        return false;
    }
    out.reserve(array->size());
    for (size_t i = 0; i < array->size(); ++i)
    {
        AudioFormat format;
        if (!formatFromJson(array->at(i), format))
        {
            error = std::string("invalid entry in ") + key;
            return false;
        }
        out.push_back(format);
    }
    return true;
}

json::Value deviceToJson(const AudioDeviceInfo& device)
{
    json::Value object = json::Value::makeObject();
    object["id"] = json::Value(device.id);
    object["name"] = json::Value(device.name);
    object["description"] = json::Value(device.description);
    object["isDefault"] = json::Value(device.isDefault);
    if (device.hasMixFormat)
        object["mixFormat"] = device_cache::formatToJson(device.mixFormat);
    object["buffer"] = bufferLimitsToJson(device.buffer);
    object["sharedFormats"] = formatListToJson(device.sharedFormats);
    object["exclusiveFormats"] = formatListToJson(device.exclusiveFormats);
    object["hardwareVolume"] = device_cache::hardwareVolumeToJson(device.hardwareVolume);
    if (device.hardwareClock.available)
        object["hardwareClock"] = hardwareClockToJson(device.hardwareClock);

    return object;
}

bool deviceFromJson(const json::Value& object, AudioDeviceInfo& out, std::string& error)
{
    if (!object.isObject())
    {
        error = "device entry is not an object";
        return false;
    }

    AudioDeviceInfo device;
    if (!json::getString(object, "id", device.id) || device.id.empty())
    {
        error = "device entry has no id";
        return false;
    }
    if (!json::getString(object, "name", device.name))
    {
        error = "device entry has no name";
        return false;
    }
    json::getString(object, "description", device.description);
    json::getBool(object, "isDefault", device.isDefault);

    const json::Value* mixFormat = object.find("mixFormat");
    if (mixFormat != nullptr && !mixFormat->isNull())
        device.hasMixFormat = formatFromJson(*mixFormat, device.mixFormat);

    const json::Value* buffer = object.find("buffer");
    if (buffer != nullptr)
        bufferLimitsFromJson(*buffer, device.buffer);

    const json::Value* hardwareVolume = object.find("hardwareVolume");
    if (hardwareVolume != nullptr)
        hardwareVolumeFromJson(*hardwareVolume, device.hardwareVolume);
    if (const json::Value* hardwareClock = object.find("hardwareClock"))
        hardwareClockFromJson(*hardwareClock, device.hardwareClock);

    if (!formatListFromJson(object, "sharedFormats", device.sharedFormats, error))
    {
        error += " of device " + device.id;
        return false;
    }
    if (!formatListFromJson(object, "exclusiveFormats", device.exclusiveFormats, error))
    {
        error += " of device " + device.id;
        return false;
    }

    out = device;
    return true;
}

} // namespace

const AudioDeviceInfo* AudioDeviceCacheData::findById(const std::string& id) const
{
    if (id.empty())
        return nullptr;
    for (size_t i = 0; i < devices.size(); ++i)
    {
        if (devices[i].id == id)
            return &devices[i];
    }
    return nullptr;
}

const AudioDeviceInfo* AudioDeviceCacheData::findDefault() const
{
    for (size_t i = 0; i < devices.size(); ++i)
    {
        if (devices[i].isDefault)
            return &devices[i];
    }
    return devices.empty() ? nullptr : &devices[0];
}

namespace device_cache {

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
    case LoadResult::SchemaMismatch:
        return "schema mismatch";
    case LoadResult::PlatformMismatch:
        return "platform mismatch";
    }
    return "unknown";
}

bool requiresRescan(LoadResult result)
{
    return result != LoadResult::Ok;
}

const char* currentPlatformName()
{
#if defined(_WIN32)
    return "windows";
#elif defined(ESP_PLATFORM)
    return "esp32";
#elif defined(__linux__)
    return "linux";
#else
    return "unknown";
#endif
}

std::string defaultPath()
{
    return fileutil::joinPath(fileutil::userConfigDirectory(), "audio_devices.json");
}

json::Value toJson(const AudioDeviceCacheData& data)
{
    json::Value root = json::Value::makeObject();
    root["schemaVersion"] = json::Value(static_cast<int64_t>(data.schemaVersion));
    root["generatedAt"] = json::Value(data.generatedAt);
    root["platform"] = json::Value(data.platform);

    json::Value devices = json::Value::makeArray();
    for (size_t i = 0; i < data.devices.size(); ++i)
        devices.pushBack(deviceToJson(data.devices[i]));
    root["devices"] = devices;

    return root;
}

json::Value hardwareVolumeToJson(const HardwareVolumeInfo& volume)
{
    json::Value object = json::Value::makeObject();
    object["available"] = json::Value(volume.available);
    if (!volume.available)
        return object;
    object["control"] = json::Value(volume.control);
    object["minStep"] = json::Value(volume.minStep);
    object["maxStep"] = json::Value(volume.maxStep);
    if (volume.hasDecibels)
    {
        object["minDb"] = json::Value(volume.minDb);
        object["maxDb"] = json::Value(volume.maxDb);
    }
    object["hasMute"] = json::Value(volume.hasMute);
    return object;
}

std::string serialize(const AudioDeviceCacheData& data)
{
    return json::serialize(toJson(data), true);
}

LoadResult deserialize(const std::string& text, AudioDeviceCacheData& out, std::string& error)
{
    json::Value root;
    if (!json::parse(text, root, error))
        return LoadResult::Invalid;
    if (!root.isObject())
    {
        error = "root value is not an object";
        return LoadResult::Invalid;
    }

    int64_t schemaVersion = 0;
    if (!json::getInt(root, "schemaVersion", schemaVersion))
    {
        error = "missing schemaVersion";
        return LoadResult::Invalid;
    }
    if (schemaVersion != kSchemaVersion)
    {
        error = "unsupported schema version";
        return LoadResult::SchemaMismatch;
    }

    AudioDeviceCacheData data;
    data.schemaVersion = static_cast<int>(schemaVersion);
    json::getString(root, "generatedAt", data.generatedAt);
    if (!json::getString(root, "platform", data.platform) || data.platform.empty())
    {
        error = "missing platform";
        return LoadResult::Invalid;
    }
    if (data.platform != currentPlatformName())
    {
        error = "cache was written on platform " + data.platform;
        return LoadResult::PlatformMismatch;
    }

    const json::Value* devices = root.find("devices");
    if (devices == nullptr || !devices->isArray())
    {
        error = "missing devices array";
        return LoadResult::Invalid;
    }
    data.devices.reserve(devices->size());
    for (size_t i = 0; i < devices->size(); ++i)
    {
        AudioDeviceInfo device;
        if (!deviceFromJson(devices->at(i), device, error))
            return LoadResult::Invalid;
        data.devices.push_back(device);
    }

    out = data;
    return LoadResult::Ok;
}

LoadResult load(const std::string& path, AudioDeviceCacheData& out, std::string& error)
{
    if (!fileutil::exists(path))
    {
        error = "file not found";
        return LoadResult::Missing;
    }
    std::string text;
    if (!fileutil::readTextFile(path, text, error))
        return LoadResult::Invalid;
    return deserialize(text, out, error);
}

bool save(const std::string& path, AudioDeviceCacheData& data, std::string& error)
{
    data.schemaVersion = kSchemaVersion;
    data.platform = currentPlatformName();
    data.generatedAt = iso8601UtcNow();
    return fileutil::writeTextFileAtomic(path, serialize(data), error);
}

} // namespace device_cache
} // namespace multiroom
