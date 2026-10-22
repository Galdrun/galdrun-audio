#pragma once

#include "audio/AudioDeviceInfo.h"
#include "util/Json.h"

#include <string>
#include <vector>

namespace multiroom {

// On-disk result of a full capability scan.
//
// The scan opens every device with every rate/format/channel combination and
// is far too slow to run at each start, so it is written once and reloaded
// afterwards. Anything that invalidates the file simply triggers a rescan --
// the cache is never repaired in place.
struct AudioDeviceCacheData
{
    int schemaVersion = 0;
    std::string generatedAt; // ISO 8601 UTC
    std::string platform;    // "windows" | "linux"
    std::vector<AudioDeviceInfo> devices;

    const AudioDeviceInfo* findById(const std::string& id) const;
    const AudioDeviceInfo* findDefault() const;
};

namespace device_cache {

// Bump whenever the serialized shape changes. An unknown version forces a
// rescan rather than a best-effort read.
const int kSchemaVersion = 3;

enum class LoadResult
{
    Ok,
    Missing,          // file does not exist
    Invalid,          // unreadable, not JSON, or structurally wrong
    SchemaMismatch,   // written by a different client version
    PlatformMismatch  // config directory shared between OSes
};

const char* loadResultName(LoadResult result);

// True when the result means the hardware has to be scanned again.
bool requiresRescan(LoadResult result);

// On anything but Ok, out is left untouched and error explains why.
LoadResult load(const std::string& path, AudioDeviceCacheData& out, std::string& error);

// Written atomically; a crash mid-write never leaves a partial file.
// schemaVersion, platform and generatedAt are filled in by this function.
bool save(const std::string& path, AudioDeviceCacheData& data, std::string& error);

std::string defaultPath();

// "windows" | "linux" | "unknown"
const char* currentPlatformName();

// Exposed for tests and for the control API's audioDevices response, which
// embeds the document as a value instead of re-parsing the serialized text.
json::Value toJson(const AudioDeviceCacheData& data);
std::string serialize(const AudioDeviceCacheData& data);

// One device's hardware volume as the cache writes it, which the status
// command reuses. Always an object, "available": false included: a caller
// must be able to tell a device without hardware volume from one never asked.
json::Value hardwareVolumeToJson(const HardwareVolumeInfo& volume);

// One format as the cache writes it ({sampleRate, channels, sampleType}),
// which the status command reuses too.
json::Value formatToJson(const AudioFormat& format);
LoadResult deserialize(const std::string& text, AudioDeviceCacheData& out, std::string& error);

} // namespace device_cache
} // namespace multiroom
