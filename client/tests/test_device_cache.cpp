#include "TestSupport.h"

#include "audio/AudioDeviceCache.h"
#include "audio/FormatProbe.h"
#include "util/FileUtil.h"

using namespace multiroom;

namespace {

AudioFormat makeFormat(uint32_t rate, uint16_t channels, AudioFormat::SampleType type)
{
    AudioFormat format;
    format.sampleRate = rate;
    format.channels = channels;
    format.sampleType = type;
    return format;
}

AudioDeviceCacheData makeCacheWithOneDevice()
{
    AudioDeviceInfo device;
    device.id = "hw:CARD=USB,DEV=0";
    device.name = "USB DAC";
    device.description = "Generic USB Audio";
    device.isDefault = true;
    device.hasMixFormat = true;
    device.mixFormat = makeFormat(48000, 2, AudioFormat::SampleType::Float32);

    // Shared mode goes through the system mixer, which only offers its own
    // rate; exclusive mode reaches the hardware's real capabilities.
    device.sharedFormats.push_back(makeFormat(48000, 2, AudioFormat::SampleType::Float32));

    device.exclusiveFormats.push_back(makeFormat(44100, 2, AudioFormat::SampleType::Int16));
    device.exclusiveFormats.push_back(makeFormat(48000, 2, AudioFormat::SampleType::Int16));
    device.exclusiveFormats.push_back(makeFormat(48000, 2, AudioFormat::SampleType::Int24In32));
    device.exclusiveFormats.push_back(makeFormat(96000, 2, AudioFormat::SampleType::Int32));

    device.buffer.minPeriodNs = 3000000;
    device.buffer.defaultPeriodNs = 10000000;
    device.buffer.minPeriodFrames = 48;
    device.buffer.maxPeriodFrames = 8192;
    device.buffer.minBufferFrames = 96;
    device.buffer.maxBufferFrames = 32768;

    device.hardwareVolume.available = true;
    device.hardwareVolume.control = "PCM";
    device.hardwareVolume.minStep = 0;
    device.hardwareVolume.maxStep = 255;
    device.hardwareVolume.hasDecibels = true;
    device.hardwareVolume.minDb = -102.5;
    device.hardwareVolume.maxDb = 4.0;
    device.hardwareVolume.hasMute = true;

    AudioDeviceCacheData data;
    data.devices.push_back(device);
    return data;
}

const AudioAccessPreference kPrefer = AudioAccessPreference::PreferExclusive;
const AudioAccessPreference kExclusiveOnly = AudioAccessPreference::ExclusiveOnly;
const AudioAccessPreference kSharedOnly = AudioAccessPreference::SharedOnly;

std::string cacheDocument(const char* schemaVersion, const char* platform, const char* devices)
{
    return std::string("{\"schemaVersion\":") + schemaVersion +
           ",\"generatedAt\":\"2026-09-12T12:00:00Z\",\"platform\":\"" + platform +
           "\",\"devices\":" + devices + "}";
}

} // namespace

MULTIROOM_TEST(device_cache_round_trips_through_disk)
{
    multiroomtest::TempDirectory temp("cache");
    const std::string path = temp.file("audio_devices.json");

    AudioDeviceCacheData original = makeCacheWithOneDevice();
    std::string error;
    CHECK(device_cache::save(path, original, error));

    // save() stamps the metadata itself.
    CHECK_EQ(original.schemaVersion, device_cache::kSchemaVersion);
    CHECK_EQ(original.platform, std::string(device_cache::currentPlatformName()));
    CHECK(!original.generatedAt.empty());
    CHECK(!fileutil::exists(path + ".tmp"));

    AudioDeviceCacheData loaded;
    CHECK(device_cache::load(path, loaded, error) == device_cache::LoadResult::Ok);
    CHECK_EQ(loaded.devices.size(), static_cast<size_t>(1));

    const AudioDeviceInfo& device = loaded.devices[0];
    CHECK_EQ(device.id, std::string("hw:CARD=USB,DEV=0"));
    CHECK_EQ(device.name, std::string("USB DAC"));
    CHECK_EQ(device.description, std::string("Generic USB Audio"));
    CHECK_EQ(device.isDefault, true);
    CHECK_EQ(device.hasMixFormat, true);
    CHECK(device.mixFormat.sampleType == AudioFormat::SampleType::Float32);

    // The two lists must survive separately: collapsing them would make the
    // client claim exclusive-only formats are available through the mixer.
    CHECK_EQ(device.sharedFormats.size(), static_cast<size_t>(1));
    CHECK_EQ(device.exclusiveFormats.size(), static_cast<size_t>(4));
    CHECK(device.supports(makeFormat(48000, 2, AudioFormat::SampleType::Int24In32),
                          AudioAccessMode::Exclusive));
    CHECK(!device.supports(makeFormat(48000, 2, AudioFormat::SampleType::Int24In32),
                           AudioAccessMode::Shared));
    CHECK(device.supportsAnyMode(makeFormat(96000, 2, AudioFormat::SampleType::Int32)));
    CHECK(!device.supportsAnyMode(makeFormat(48000, 6, AudioFormat::SampleType::Int16)));

    CHECK_EQ(device.buffer.minPeriodNs, static_cast<uint64_t>(3000000));
    CHECK_EQ(device.buffer.maxBufferFrames, static_cast<uint32_t>(32768));

    CHECK(device.hardwareVolume.available);
    CHECK_EQ(device.hardwareVolume.control, std::string("PCM"));
    CHECK_EQ(device.hardwareVolume.maxStep, static_cast<int64_t>(255));
    CHECK(device.hardwareVolume.hasDecibels);
    CHECK_EQ(device.hardwareVolume.minDb, -102.5);
    CHECK_EQ(device.hardwareVolume.maxDb, 4.0);
    CHECK(device.hardwareVolume.hasMute);
}

MULTIROOM_TEST(device_cache_hardware_volume_that_does_not_add_up_is_software)
{
    AudioDeviceCacheData data;
    std::string error;
    const char* platform = device_cache::currentPlatformName();

    // No range, and a range in dB that is empty: the first is no control at
    // all, the second a control without decibels. Neither invalidates the file.
    CHECK(device_cache::deserialize(
              cacheDocument("3", platform,
                            "[{\"id\":\"a\",\"name\":\"A\",\"hardwareVolume\":"
                            "{\"available\":true,\"control\":\"Master\"}},"
                            "{\"id\":\"b\",\"name\":\"B\",\"hardwareVolume\":"
                            "{\"available\":true,\"control\":\"PCM\",\"minStep\":0,"
                            "\"maxStep\":31,\"minDb\":0,\"maxDb\":0}}]"),
              data, error) == device_cache::LoadResult::Ok);
    CHECK(!data.devices[0].hardwareVolume.available);
    CHECK(data.devices[1].hardwareVolume.available);
    CHECK(!data.devices[1].hardwareVolume.hasDecibels);
    CHECK_EQ(data.devices[1].hardwareVolume.maxStep, static_cast<int64_t>(31));

    // And a device without one says so rather than leaving the key out.
    AudioDeviceCacheData none;
    none.devices.push_back(AudioDeviceInfo());
    none.devices[0].id = "c";
    const json::Value document = device_cache::toJson(none);
    const json::Value* volume = document.find("devices")->at(0).find("hardwareVolume");
    CHECK(volume != nullptr);
    bool available = true;
    CHECK(json::getBool(*volume, "available", available));
    CHECK(!available);
}

MULTIROOM_TEST(device_cache_lookup_helpers)
{
    const AudioDeviceCacheData data = makeCacheWithOneDevice();
    CHECK(data.findById("hw:CARD=USB,DEV=0") != nullptr);
    CHECK(data.findById("nope") == nullptr);
    CHECK(data.findById("") == nullptr);
    CHECK(data.findDefault() != nullptr);
    CHECK_EQ(data.findDefault()->id, std::string("hw:CARD=USB,DEV=0"));

    AudioDeviceCacheData empty;
    CHECK(empty.findDefault() == nullptr);
}

MULTIROOM_TEST(device_best_format_prefers_exclusive_access)
{
    const AudioDeviceCacheData data = makeCacheWithOneDevice();
    const AudioDeviceInfo& device = data.devices[0];

    AudioFormat chosen;
    AudioAccessMode mode = AudioAccessMode::Shared;

    // 48 kHz exists in both modes; exclusive must win, because the system
    // mixer would otherwise sit between us and the hardware clock.
    CHECK(device.bestFormatForRate(48000, 2, kPrefer, chosen, mode));
    CHECK(mode == AudioAccessMode::Exclusive);
    CHECK(chosen.sampleType == AudioFormat::SampleType::Int24In32);
    CHECK_EQ(chosen.channels, static_cast<uint16_t>(2));

    CHECK(device.bestFormatForRate(96000, 2, kPrefer, chosen, mode));
    CHECK(chosen.sampleType == AudioFormat::SampleType::Int32);

    CHECK(!device.bestFormatForRate(192000, 2, kPrefer, chosen, mode));
}

MULTIROOM_TEST(device_best_format_falls_back_to_shared)
{
    AudioDeviceInfo device;
    device.id = "shared-only";
    device.sharedFormats.push_back(makeFormat(48000, 2, AudioFormat::SampleType::Float32));

    AudioFormat chosen;
    AudioAccessMode mode = AudioAccessMode::Exclusive;
    CHECK(device.bestFormatForRate(48000, 2, kPrefer, chosen, mode));
    CHECK(mode == AudioAccessMode::Shared);
    CHECK(chosen.sampleType == AudioFormat::SampleType::Float32);
}

MULTIROOM_TEST(choose_output_format_falls_back_by_rate)
{
    const AudioDeviceCacheData data = makeCacheWithOneDevice();
    const AudioDeviceInfo& device = data.devices[0];

    AudioFormat chosen;
    AudioAccessMode mode = AudioAccessMode::Shared;

    // Preferred rate available.
    CHECK(chooseOutputFormat(device, 96000, 2, kPrefer, chosen, mode));
    CHECK_EQ(chosen.sampleRate, static_cast<uint32_t>(96000));

    // 192 kHz is not supported: fall back to 48000 before anything else.
    CHECK(chooseOutputFormat(device, 192000, 2, kPrefer, chosen, mode));
    CHECK_EQ(chosen.sampleRate, static_cast<uint32_t>(48000));

    // A device with neither 48000 nor 44100 falls back to its highest rate.
    AudioDeviceInfo odd;
    odd.id = "odd";
    odd.exclusiveFormats.push_back(makeFormat(88200, 2, AudioFormat::SampleType::Int16));
    odd.exclusiveFormats.push_back(makeFormat(176400, 2, AudioFormat::SampleType::Int16));
    CHECK(chooseOutputFormat(odd, 48000, 2, kPrefer, chosen, mode));
    CHECK_EQ(chosen.sampleRate, static_cast<uint32_t>(176400));

    // A device with no formats at all fails rather than inventing one.
    AudioDeviceInfo silent;
    silent.id = "silent";
    CHECK(!chooseOutputFormat(silent, 48000, 2, kPrefer, chosen, mode));
}

MULTIROOM_TEST(access_preference_controls_mode_selection)
{
    const AudioDeviceCacheData data = makeCacheWithOneDevice();
    const AudioDeviceInfo& device = data.devices[0];

    AudioFormat chosen;
    AudioAccessMode mode = AudioAccessMode::Shared;

    // 48 kHz exists in both lists, so the preference alone decides.
    CHECK(device.bestFormatForRate(48000, 2, kPrefer, chosen, mode));
    CHECK(mode == AudioAccessMode::Exclusive);

    CHECK(device.bestFormatForRate(48000, 2, kSharedOnly, chosen, mode));
    CHECK(mode == AudioAccessMode::Shared);
    CHECK(chosen.sampleType == AudioFormat::SampleType::Float32);

    CHECK(device.bestFormatForRate(48000, 2, kExclusiveOnly, chosen, mode));
    CHECK(mode == AudioAccessMode::Exclusive);

    // 96 kHz is exclusive-only on this device, so shared-only must fail rather
    // than quietly returning a rate the mixer would have to resample.
    CHECK(device.bestFormatForRate(96000, 2, kExclusiveOnly, chosen, mode));
    CHECK(!device.bestFormatForRate(96000, 2, kSharedOnly, chosen, mode));
}

MULTIROOM_TEST(exclusive_only_fails_instead_of_falling_back)
{
    AudioDeviceInfo device;
    device.id = "shared-only";
    device.sharedFormats.push_back(makeFormat(48000, 2, AudioFormat::SampleType::Float32));

    AudioFormat chosen;
    AudioAccessMode mode = AudioAccessMode::Shared;

    // The whole point of exclusive-only: a device that cannot do exclusive
    // must produce a hard failure, not silent playback through the mixer.
    CHECK(!device.bestFormatForRate(48000, 2, kExclusiveOnly, chosen, mode));
    CHECK(!chooseOutputFormat(device, 48000, 2, kExclusiveOnly, chosen, mode));

    // The same device works fine under the default preference.
    CHECK(chooseOutputFormat(device, 48000, 2, kPrefer, chosen, mode));
    CHECK(mode == AudioAccessMode::Shared);
}

MULTIROOM_TEST(access_preference_restricts_the_highest_rate_fallback)
{
    // 88200 only exists in shared mode, 44100 only in exclusive mode. The
    // last-resort "highest rate" search must not cross the preference.
    AudioDeviceInfo device;
    device.id = "mixed";
    device.sharedFormats.push_back(makeFormat(88200, 2, AudioFormat::SampleType::Int16));
    device.exclusiveFormats.push_back(makeFormat(176400, 2, AudioFormat::SampleType::Int16));

    AudioFormat chosen;
    AudioAccessMode mode = AudioAccessMode::Shared;

    CHECK(chooseOutputFormat(device, 192000, 2, kExclusiveOnly, chosen, mode));
    CHECK_EQ(chosen.sampleRate, static_cast<uint32_t>(176400));
    CHECK(mode == AudioAccessMode::Exclusive);

    CHECK(chooseOutputFormat(device, 192000, 2, kSharedOnly, chosen, mode));
    CHECK_EQ(chosen.sampleRate, static_cast<uint32_t>(88200));
    CHECK(mode == AudioAccessMode::Shared);
}

MULTIROOM_TEST(access_preference_names_round_trip)
{
    AudioAccessPreference preference = AudioAccessPreference::SharedOnly;
    const AudioAccessPreference all[] = {AudioAccessPreference::PreferExclusive,
                                         AudioAccessPreference::ExclusiveOnly,
                                         AudioAccessPreference::SharedOnly};
    for (size_t i = 0; i < 3; ++i)
    {
        CHECK(parseAudioAccessPreference(audioAccessPreferenceName(all[i]), preference));
        CHECK(preference == all[i]);
    }
    CHECK(!parseAudioAccessPreference("exclusiveonly", preference));
    CHECK(!parseAudioAccessPreference("", preference));
}

MULTIROOM_TEST(device_cache_reports_missing_file)
{
    multiroomtest::TempDirectory temp("cache_missing");
    AudioDeviceCacheData data;
    std::string error;
    CHECK(device_cache::load(temp.file("absent.json"), data, error) ==
          device_cache::LoadResult::Missing);
    CHECK(device_cache::requiresRescan(device_cache::LoadResult::Missing));
}

MULTIROOM_TEST(device_cache_rejects_unknown_schema_version)
{
    AudioDeviceCacheData data;
    std::string error;

    CHECK(device_cache::deserialize(
              cacheDocument("99", device_cache::currentPlatformName(), "[]"), data, error) ==
          device_cache::LoadResult::SchemaMismatch);
    // Schema 1 used a single supportedFormats list and must trigger a rescan
    // rather than being read as if shared and exclusive were the same thing.
    CHECK(device_cache::deserialize(
              cacheDocument("1", device_cache::currentPlatformName(), "[]"), data, error) ==
          device_cache::LoadResult::SchemaMismatch);
    CHECK(device_cache::requiresRescan(device_cache::LoadResult::SchemaMismatch));
}

MULTIROOM_TEST(device_cache_rejects_foreign_platform)
{
    AudioDeviceCacheData data;
    std::string error;
    const char* foreign =
        std::string(device_cache::currentPlatformName()) == "linux" ? "windows" : "linux";

    CHECK(device_cache::deserialize(cacheDocument("3", foreign, "[]"), data, error) ==
          device_cache::LoadResult::PlatformMismatch);
}

MULTIROOM_TEST(device_cache_rejects_invalid_documents)
{
    AudioDeviceCacheData data;
    std::string error;
    const char* platform = device_cache::currentPlatformName();

    CHECK(device_cache::deserialize("", data, error) == device_cache::LoadResult::Invalid);
    CHECK(device_cache::deserialize("{ truncated", data, error) ==
          device_cache::LoadResult::Invalid);
    CHECK(device_cache::deserialize("[]", data, error) == device_cache::LoadResult::Invalid);

    // Missing schemaVersion, missing platform, missing devices.
    CHECK(device_cache::deserialize("{\"platform\":\"linux\",\"devices\":[]}", data, error) ==
          device_cache::LoadResult::Invalid);
    CHECK(device_cache::deserialize("{\"schemaVersion\":3,\"devices\":[]}", data, error) ==
          device_cache::LoadResult::Invalid);
    CHECK(device_cache::deserialize(
              std::string("{\"schemaVersion\":3,\"platform\":\"") + platform + "\"}", data,
              error) == device_cache::LoadResult::Invalid);

    // Device entries without an id, and a broken format entry.
    CHECK(device_cache::deserialize(cacheDocument("3", platform, "[{\"name\":\"x\"}]"), data,
                                    error) == device_cache::LoadResult::Invalid);
    CHECK(device_cache::deserialize(
              cacheDocument("3", platform,
                            "[{\"id\":\"a\",\"name\":\"x\",\"exclusiveFormats\":"
                            "[{\"sampleRate\":48000,\"channels\":2,"
                            "\"sampleType\":\"int11\"}]}]"),
              data, error) == device_cache::LoadResult::Invalid);
    CHECK(device_cache::deserialize(
              cacheDocument("3", platform, "[{\"id\":\"a\",\"name\":\"x\",\"sharedFormats\":5}]"),
              data, error) == device_cache::LoadResult::Invalid);
    CHECK(!error.empty());
}

MULTIROOM_TEST(device_cache_accepts_minimal_device_entry)
{
    AudioDeviceCacheData data;
    std::string error;
    const char* platform = device_cache::currentPlatformName();

    CHECK(device_cache::deserialize(
              cacheDocument("3", platform, "[{\"id\":\"a\",\"name\":\"Device A\"}]"), data,
              error) == device_cache::LoadResult::Ok);
    CHECK_EQ(data.devices.size(), static_cast<size_t>(1));
    CHECK(data.devices[0].sharedFormats.empty());
    CHECK(data.devices[0].exclusiveFormats.empty());
    CHECK_EQ(data.devices[0].hasMixFormat, false);
    CHECK_EQ(data.devices[0].isDefault, false);
}

MULTIROOM_TEST(device_cache_partial_file_is_never_left_behind)
{
    multiroomtest::TempDirectory temp("cache_atomic");
    const std::string path = temp.file("audio_devices.json");

    AudioDeviceCacheData first = makeCacheWithOneDevice();
    std::string error;
    CHECK(device_cache::save(path, first, error));

    // A second save replaces the file wholesale; the old content must be gone
    // and the result must still parse.
    AudioDeviceCacheData second;
    CHECK(device_cache::save(path, second, error));

    AudioDeviceCacheData loaded;
    CHECK(device_cache::load(path, loaded, error) == device_cache::LoadResult::Ok);
    CHECK_EQ(loaded.devices.size(), static_cast<size_t>(0));
    CHECK(!fileutil::exists(path + ".tmp"));
}

MULTIROOM_TEST(audio_format_helpers)
{
    CHECK_EQ(AudioFormat::bytesPerSample(AudioFormat::SampleType::Int16),
             static_cast<uint32_t>(2));
    CHECK_EQ(AudioFormat::bytesPerSample(AudioFormat::SampleType::Int24),
             static_cast<uint32_t>(3));
    CHECK_EQ(AudioFormat::bytesPerSample(AudioFormat::SampleType::Int24In32),
             static_cast<uint32_t>(4));
    CHECK_EQ(AudioFormat::validBits(AudioFormat::SampleType::Int24In32),
             static_cast<uint32_t>(24));

    const AudioFormat format = makeFormat(48000, 2, AudioFormat::SampleType::Int24);
    CHECK_EQ(format.bytesPerFrame(), static_cast<uint32_t>(6));
    CHECK(format.isValid());

    AudioFormat::SampleType type = AudioFormat::SampleType::Int16;
    for (size_t i = 0; i < probe::kSampleTypeCount; ++i)
    {
        // Every probed type must survive the name round-trip used by the cache.
        CHECK(parseSampleType(sampleTypeName(probe::kSampleTypes[i]), type));
        CHECK(type == probe::kSampleTypes[i]);
    }
    CHECK(!parseSampleType("int12", type));
}
