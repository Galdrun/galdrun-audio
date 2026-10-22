#include "TestSupport.h"

#include "app/ClientCommandHandler.h"
#include "app/DeviceSelection.h"
#include "app/StreamPlayback.h"
#include "matp/Session.h"
#include "util/FileUtil.h"
#include "util/Time.h"

#include <string>

using namespace multiroom;

namespace {

// The whole point of publishing playback state into ClientRuntime rather than
// reading it from the engine is that these tests need no sound card.
struct Fixture
{
    ClientConfig config;
    AudioDeviceCacheData cache;
    ClientRuntime runtime;

    Fixture()
    {
        config.clientId = "livingroom";
        config.server = "192.168.1.10:4711";

        cache.schemaVersion = device_cache::kSchemaVersion;
        cache.platform = device_cache::currentPlatformName();
        cache.generatedAt = "2026-09-12T10:00:00Z";

        AudioDeviceInfo device;
        device.id = "hw:CARD=Audio,DEV=0";
        device.name = "Test Output";
        device.isDefault = true;
        AudioFormat format;
        format.sampleRate = 48000;
        format.channels = 2;
        format.sampleType = AudioFormat::SampleType::Int24In32;
        device.exclusiveFormats.push_back(format);
        cache.devices.push_back(device);

        runtime.config = &config;
        runtime.deviceCache = &cache;
        runtime.startedAtNs = monotonicNowNs();
        runtime.deviceSelected = true;
        runtime.deviceId = device.id;
        runtime.deviceName = device.name;
        runtime.formatSelected = true;
        runtime.format = format;
        runtime.accessMode = AudioAccessMode::Exclusive;
    }
};

ControlRequest makeRequest(const char* name, ControlCommand command)
{
    ControlRequest request;
    request.command = command;
    request.name = name;
    request.params = json::Value::makeObject();
    return request;
}

const json::Value& object(const json::Value& parent, const char* key)
{
    const json::Value* value = parent.find(key);
    CHECK(value != nullptr);
    CHECK(value->isObject());
    return *value;
}

std::string fieldString(const json::Value& parent, const char* key)
{
    const json::Value* value = parent.find(key);
    return value != nullptr ? value->asString() : std::string();
}

bool fieldBool(const json::Value& parent, const char* key, bool fallback)
{
    const json::Value* value = parent.find(key);
    return value != nullptr ? value->asBool(fallback) : fallback;
}

} // namespace

MULTIROOM_TEST(deep_sleep_is_refused_locally_on_a_host_without_the_board_helper)
{
    Fixture fixture;
    ClientCommandHandler handler(fixture.runtime);
    matp::InputActionEvent event;
    event.binding.action = matp::InputAction::DeepSleep;
    std::string error;
    CHECK(!handler.dispatchInputAction(event, error));
    CHECK(error.find("deep sleep") != std::string::npos);
    CHECK(error.find("no session") == std::string::npos);
}

MULTIROOM_TEST(power_commands_and_button_actions_work_without_a_server)
{
    Fixture fixture;
    ServerConnection connection;
    matp::Session session(connection, matp::Identity{}, {});
    StreamPlayback playback(fixture.runtime, matp::Identity{}, fixture.config.bufferMs);
    fixture.runtime.session = &session;
    fixture.runtime.playback = &playback;
    ClientCommandHandler handler(fixture.runtime);
    CHECK_EQ(fieldString(handler.handle(makeRequest("standbyOn", ControlCommand::StandbyOn)), "state"), std::string("standbyManual"));
    CHECK_EQ(fieldString(handler.handle(makeRequest("standbyOn", ControlCommand::StandbyOn)), "state"), std::string("standbyManual"));
    matp::InputActionEvent event;
    event.binding.action = matp::InputAction::ToggleStandby;
    std::string error;
    CHECK(handler.dispatchInputAction(event, error));
    CHECK_EQ(std::string(session.powerState()), std::string("on"));
    CHECK(handler.dispatchInputAction(event, error));
    CHECK_EQ(std::string(session.powerState()), std::string("standbyManual"));
    CHECK_EQ(fieldString(handler.handle(makeRequest("standbyOff", ControlCommand::StandbyOff)), "state"), std::string("on"));
    const auto status = handler.handle(makeRequest("status", ControlCommand::Status));
    CHECK_EQ(fieldString(object(object(status, "status"), "power"), "state"), std::string("on"));
    InputDefinition button;
    button.control.id = "power";
    button.control.bindings[static_cast<size_t>(matp::InputGesture::Click)].action = matp::InputAction::ToggleStandby;
    fixture.config.inputs.push_back(button);
    auto simulate = makeRequest("simulateInput", ControlCommand::SimulateInput);
    simulate.params["id"] = json::Value("power");
    simulate.params["gesture"] = json::Value("click");
    CHECK(fieldBool(handler.handle(simulate), "ok", false));
    CHECK_EQ(std::string(session.powerState()), std::string("standbyManual"));
}

MULTIROOM_TEST(automatic_standby_counts_only_idle_time_and_can_be_disabled)
{
    Fixture fixture;
    ServerConnection connection;
    matp::Session session(connection, matp::Identity{}, {});
    fixture.runtime.session = &session;
    fixture.config.standbyAfterSeconds = 2;
    ClientCommandHandler handler(fixture.runtime);
    handler.pollPower(0);
    handler.pollPower(1000000000ULL);
    CHECK_EQ(std::string(session.powerState()), std::string("on"));
    fixture.runtime.timelineState = TimelineState::Playing;
    handler.pollPower(5000000000ULL);
    fixture.runtime.timelineState = TimelineState::Paused;
    handler.pollPower(6000000000ULL);
    handler.pollPower(7999999999ULL);
    CHECK_EQ(std::string(session.powerState()), std::string("on"));
    handler.pollPower(8000000000ULL);
    CHECK_EQ(std::string(session.powerState()), std::string("standby"));
    std::string error;
    CHECK(session.setPower(true, error));
    fixture.config.standbyAfterSeconds = 0;
    handler.pollPower(9000000000ULL);
    handler.pollPower(999000000000ULL);
    CHECK_EQ(std::string(session.powerState()), std::string("on"));
}

MULTIROOM_TEST(power_timeout_is_validated_and_persisted_before_it_changes)
{
    multiroomtest::TempDirectory temp("power");
    Fixture fixture;
    ServerConnection connection;
    matp::Session session(connection, matp::Identity{}, {});
    StreamPlayback playback(fixture.runtime, matp::Identity{}, fixture.config.bufferMs);
    fixture.runtime.session = &session;
    fixture.runtime.playback = &playback;
    fixture.runtime.configPath = temp.file("client.json");
    ClientCommandHandler handler(fixture.runtime);
    auto request = makeRequest("setPower", ControlCommand::SetPower);
    request.params["standbyAfterSeconds"] = json::Value(900);
    CHECK(fieldBool(handler.handle(request), "ok", false));
    ClientConfig loaded;
    std::string error;
    CHECK(client_config::load(fixture.runtime.configPath, loaded, error) == client_config::LoadResult::Ok);
    CHECK_EQ(loaded.standbyAfterSeconds, uint32_t(900));
    for (const json::Value& value : {json::Value(-1), json::Value(86401), json::Value(1.5), json::Value("900"), json::Value(true)})
    {
        request.params["standbyAfterSeconds"] = value;
        CHECK_EQ(fieldString(handler.handle(request), "error"), std::string(control_error::kInvalidArgument));
        CHECK_EQ(fixture.config.standbyAfterSeconds, uint32_t(900));
    }
    request.params["standbyAfterSeconds"] = json::Value(0);
    fixture.runtime.configPath = temp.path();
    CHECK_EQ(fieldString(handler.handle(request), "error"), std::string(control_error::kFailed));
    CHECK_EQ(fixture.config.standbyAfterSeconds, uint32_t(900));
}

MULTIROOM_TEST(command_handler_sets_either_power_delay_and_reports_both)
{
    multiroomtest::TempDirectory temp("power-both");
    Fixture fixture;
    ServerConnection connection;
    matp::Session session(connection, matp::Identity{}, {});
    StreamPlayback playback(fixture.runtime, matp::Identity{}, fixture.config.bufferMs);
    fixture.runtime.session = &session;
    fixture.runtime.playback = &playback;
    fixture.runtime.configPath = temp.file("client.json");
    ClientCommandHandler handler(fixture.runtime);

    auto request = makeRequest("setPower", ControlCommand::SetPower);
    request.params["deepSleepAfterSeconds"] = json::Value(0); // off is fine anywhere
    auto response = handler.handle(request);
    CHECK(fieldBool(response, "ok", false));
    CHECK(response.find("deepSleepAfterSeconds") != nullptr);
    CHECK(response.find("deepSleep") != nullptr);

    request.params["deepSleepAfterSeconds"] = json::Value(30);
    CHECK_EQ(fieldString(handler.handle(request), "error"), std::string(control_error::kInvalidArgument));
    request = makeRequest("setPower", ControlCommand::SetPower);
    request.params["standbyAfterSeconds"] = json::Value(600);
    request.params["other"] = json::Value(1);
    CHECK_EQ(fieldString(handler.handle(request), "error"), std::string(control_error::kInvalidArgument));
    CHECK_EQ(fixture.config.standbyAfterSeconds, uint32_t(0));
    request = makeRequest("setPower", ControlCommand::SetPower);
    CHECK_EQ(fieldString(handler.handle(request), "error"), std::string(control_error::kInvalidArgument));

    // A delay only a deep-sleep-capable client may set.
    request.params["deepSleepAfterSeconds"] = json::Value(3600);
    if (!deepSleepSupported())
    {
        CHECK_EQ(fieldString(handler.handle(request), "error"), std::string(control_error::kUnavailable));
        CHECK_EQ(fixture.config.deepSleepAfterSeconds, uint32_t(0));
        CHECK_EQ(fieldString(handler.handle(makeRequest("deepSleep", ControlCommand::DeepSleep)), "error"),
                 std::string(control_error::kFailed));
    }
}

MULTIROOM_TEST(status_reports_the_documented_shape)
{
    Fixture fixture;
    ClientCommandHandler handler(fixture.runtime);

    const json::Value response = handler.handle(makeRequest("status", ControlCommand::Status));
    CHECK_EQ(fieldBool(response, "ok", false), true);
    CHECK_EQ(fieldString(response, "command"), std::string("status"));

    const json::Value& status = object(response, "status");
    CHECK_EQ(fieldString(status, "clientId"), std::string("livingroom"));
    CHECK(!fieldString(status, "version").empty());
    CHECK(!fieldString(status, "platform").empty());
    CHECK(status.find("uptimeSeconds") != nullptr);

    const json::Value& server = object(status, "server");
    CHECK_EQ(fieldString(server, "address"), std::string("192.168.1.10:4711"));
    CHECK_EQ(fieldString(server, "state"), std::string("disconnected"));

    const json::Value& audio = object(status, "audio");
    CHECK_EQ(audio.find("deviceCount")->asInt(), static_cast<int64_t>(1));
    CHECK_EQ(fieldString(audio, "scannedAt"), std::string("2026-09-12T10:00:00Z"));
    CHECK_EQ(fieldBool(audio, "deviceSelected", false), true);
    CHECK_EQ(fieldString(audio, "deviceId"), std::string("hw:CARD=Audio,DEV=0"));
    CHECK_EQ(fieldString(audio, "deviceName"), std::string("Test Output"));
    CHECK_EQ(fieldBool(audio, "formatSelected", false), true);
    CHECK_EQ(fieldString(audio, "accessMode"), std::string("exclusive"));
    CHECK_EQ(fieldBool(audio, "playing", true), false);
    CHECK_EQ(fieldString(audio, "playbackState"), std::string("idle"));
    CHECK_EQ(audio.find("underruns")->asInt(), static_cast<int64_t>(0));
    CHECK_EQ(audio.find("recoveries")->asInt(), static_cast<int64_t>(0));

    // Closed: the selected format is not what the sound card runs at.
    CHECK(audio.find("format") == nullptr);

    fixture.runtime.deviceFormat.sampleRate = 96000;
    fixture.runtime.deviceFormat.channels = 2;
    fixture.runtime.deviceFormat.sampleType = AudioFormat::SampleType::Int24In32;
    const json::Value open = handler.handle(makeRequest("status", ControlCommand::Status));
    const json::Value& format = object(object(object(open, "status"), "audio"), "format");
    CHECK_EQ(format.find("sampleRate")->asInt(), static_cast<int64_t>(96000));
    CHECK_EQ(format.find("channels")->asInt(), static_cast<int64_t>(2));
    CHECK_EQ(fieldString(format, "sampleType"), std::string("int24in32"));
}

MULTIROOM_TEST(status_reports_the_device_hardware_volume)
{
    Fixture fixture;
    ClientCommandHandler handler(fixture.runtime);

    // Scanned, but not in use: no device is open.
    HardwareVolumeInfo& info = fixture.cache.devices[0].hardwareVolume;
    info.available = true;
    info.control = "Digital";
    info.maxStep = 207;
    info.hasDecibels = true;
    info.minDb = -103.5;
    info.maxDb = 0.0;
    info.hasMute = true;
    {
        const json::Value response =
            handler.handle(makeRequest("status", ControlCommand::Status));
        const json::Value& volume = object(object(object(response, "status"), "audio"),
                                           "hardwareVolume");
        CHECK_EQ(fieldBool(volume, "available", false), true);
        CHECK_EQ(fieldString(volume, "control"), std::string("Digital"));
        CHECK_EQ(volume.find("maxStep")->asInt(), static_cast<int64_t>(207));
        CHECK_EQ(volume.find("minDb")->asDouble(), -103.5);
        CHECK_EQ(fieldBool(volume, "active", true), false);
        CHECK(volume.find("current") == nullptr);
    }

    // In use, with what the control reads back.
    fixture.runtime.hardwareVolumeActive = true;
    fixture.runtime.hasHardwareVolumeState = true;
    fixture.runtime.hardwareVolumeState.gain = 0.5;
    fixture.runtime.hardwareVolumeState.hasDecibels = true;
    fixture.runtime.hardwareVolumeState.db = -6.0;
    const json::Value response = handler.handle(makeRequest("status", ControlCommand::Status));
    const json::Value& volume =
        object(object(object(response, "status"), "audio"), "hardwareVolume");
    CHECK_EQ(fieldBool(volume, "active", false), true);
    const json::Value& current = object(volume, "current");
    CHECK_EQ(current.find("gain")->asDouble(), 0.5);
    CHECK_EQ(current.find("db")->asDouble(), -6.0);
    CHECK_EQ(fieldBool(current, "muted", true), false);
}

MULTIROOM_TEST(status_omits_clock_and_drift_when_there_are_none)
{
    Fixture fixture;
    ClientCommandHandler handler(fixture.runtime);

    const json::Value response = handler.handle(makeRequest("status", ControlCommand::Status));
    const json::Value& status = object(response, "status");

    // Absent rather than zero: a caller must be able to tell "no clock" from
    // "a clock reading zero", which is what phase 13 will depend on.
    CHECK(status.find("clock") == nullptr);
    CHECK(status.find("drift") == nullptr);
}

MULTIROOM_TEST(status_reports_the_clock_once_playback_publishes_one)
{
    Fixture fixture;
    fixture.runtime.playbackState = PlaybackState::Playing;
    fixture.runtime.underruns = 3;
    fixture.runtime.bufferFrames = 480;
    fixture.runtime.lastClock.deviceFramePosition = 96000;
    fixture.runtime.lastClock.monotonicTimeNs = 2000000000ULL;
    fixture.runtime.lastClock.nominalSampleRate = 48000;
    fixture.runtime.lastClock.quality = AudioClockQuality::Hardware;

    ClientCommandHandler handler(fixture.runtime);
    const json::Value response = handler.handle(makeRequest("status", ControlCommand::Status));
    const json::Value& status = object(response, "status");
    const json::Value& audio = object(status, "audio");
    CHECK_EQ(fieldBool(audio, "playing", false), true);
    CHECK_EQ(fieldString(audio, "playbackState"), std::string("playing"));
    CHECK_EQ(audio.find("underruns")->asInt(), static_cast<int64_t>(3));
    CHECK_EQ(audio.find("bufferFrames")->asInt(), static_cast<int64_t>(480));
    // Healthy playback carries no error, so the key is absent rather than an
    // empty string a caller would have to test for.
    CHECK(audio.find("error") == nullptr);

    const json::Value& clock = object(status, "clock");
    CHECK_EQ(fieldString(clock, "quality"), std::string("hardware"));
    CHECK_EQ(clock.find("deviceFramePosition")->asInt(), static_cast<int64_t>(96000));
    CHECK_EQ(clock.find("nominalSampleRate")->asInt(), static_cast<int64_t>(48000));

    // Still no drift: the estimator has published no fit.
    CHECK(status.find("drift") == nullptr);
}

MULTIROOM_TEST(status_reports_why_playback_is_not_running)
{
    Fixture fixture;
    fixture.runtime.playbackState = PlaybackState::Recovering;
    fixture.runtime.playbackError = "the device was removed or reconfigured";
    fixture.runtime.playbackAttempts = 2;
    fixture.runtime.playbackRecoveries = 1;
    fixture.runtime.playbackRetryMs = 1800;
    fixture.runtime.underruns = 7;

    ClientCommandHandler handler(fixture.runtime);
    const json::Value response = handler.handle(makeRequest("status", ControlCommand::Status));
    const json::Value& audio = object(object(response, "status"), "audio");

    CHECK_EQ(fieldBool(audio, "playing", true), false);
    CHECK_EQ(fieldString(audio, "playbackState"), std::string("recovering"));
    CHECK_EQ(fieldString(audio, "error"), std::string("the device was removed or reconfigured"));
    CHECK_EQ(audio.find("failedAttempts")->asInt(), static_cast<int64_t>(2));
    CHECK_EQ(audio.find("retryInMs")->asInt(), static_cast<int64_t>(1800));
    CHECK_EQ(audio.find("recoveries")->asInt(), static_cast<int64_t>(1));
    // The count the dead stream reached, not a zero that would claim a clean
    // run.
    CHECK_EQ(audio.find("underruns")->asInt(), static_cast<int64_t>(7));
    // Nothing is open, so there is no granted period to report.
    CHECK(audio.find("bufferFrames") == nullptr);
}

MULTIROOM_TEST(status_reports_a_device_without_a_usable_format)
{
    Fixture fixture;
    fixture.runtime.formatSelected = false;

    ClientCommandHandler handler(fixture.runtime);
    const json::Value response = handler.handle(makeRequest("status", ControlCommand::Status));
    const json::Value& audio = object(object(response, "status"), "audio");

    CHECK_EQ(fieldBool(audio, "deviceSelected", false), true);
    CHECK_EQ(fieldBool(audio, "formatSelected", true), false);
    CHECK(audio.find("format") == nullptr);
    CHECK(audio.find("accessMode") == nullptr);
}

MULTIROOM_TEST(status_reports_an_unconfigured_server)
{
    Fixture fixture;
    fixture.config.server.clear();

    ClientCommandHandler handler(fixture.runtime);
    const json::Value response = handler.handle(makeRequest("status", ControlCommand::Status));
    const json::Value& server = object(object(response, "status"), "server");
    CHECK_EQ(fieldString(server, "address"), std::string());
    CHECK_EQ(fieldString(server, "state"), std::string("notConfigured"));
}

MULTIROOM_TEST(audio_devices_returns_the_cache_document)
{
    Fixture fixture;
    ClientCommandHandler handler(fixture.runtime);

    const json::Value response =
        handler.handle(makeRequest("audioDevices", ControlCommand::AudioDevices));
    CHECK_EQ(fieldBool(response, "ok", false), true);

    const json::Value& cache = object(response, "cache");
    CHECK_EQ(cache.find("schemaVersion")->asInt(),
             static_cast<int64_t>(device_cache::kSchemaVersion));
    CHECK_EQ(fieldString(cache, "generatedAt"), std::string("2026-09-12T10:00:00Z"));

    const json::Value* devices = cache.find("devices");
    CHECK(devices != nullptr);
    CHECK(devices->isArray());
    CHECK_EQ(devices->size(), static_cast<size_t>(1));
    CHECK_EQ(fieldString(devices->at(0), "id"), std::string("hw:CARD=Audio,DEV=0"));
}

MULTIROOM_TEST(get_server_returns_the_configured_address)
{
    Fixture fixture;
    ClientCommandHandler handler(fixture.runtime);

    const json::Value response =
        handler.handle(makeRequest("getServer", ControlCommand::GetServer));
    CHECK_EQ(fieldBool(response, "ok", false), true);
    CHECK_EQ(fieldString(response, "server"), std::string("192.168.1.10:4711"));
    CHECK_EQ(fieldString(response, "state"), std::string("disconnected"));
}

MULTIROOM_TEST(set_server_validates_and_persists)
{
    multiroomtest::TempDirectory temp("control");
    Fixture fixture;
    fixture.runtime.configPath = temp.file("client.json");
    ClientCommandHandler handler(fixture.runtime);

    ControlRequest request = makeRequest("setServer", ControlCommand::SetServer);
    request.params["server"] = json::Value("10.0.0.5:4711");

    const json::Value response = handler.handle(request);
    CHECK_EQ(fieldBool(response, "ok", false), true);
    CHECK_EQ(fieldString(response, "server"), std::string("10.0.0.5:4711"));
    CHECK_EQ(fieldBool(response, "changed", false), true);
    CHECK_EQ(fixture.config.server, std::string("10.0.0.5:4711"));

    // Persisted, not only changed in memory: a restart has to keep it.
    ClientConfig reloaded;
    std::string error;
    CHECK(client_config::load(fixture.runtime.configPath, reloaded, error) ==
          client_config::LoadResult::Ok);
    CHECK_EQ(reloaded.server, std::string("10.0.0.5:4711"));
}

MULTIROOM_TEST(set_server_that_cannot_be_saved_keeps_the_command_line_override)
{
    multiroomtest::TempDirectory temp("control");
    Fixture fixture;
    fixture.runtime.storedConfig.server = "10.0.0.1:4711";
    fixture.runtime.commandLine.hasServer = true;
    fixture.runtime.commandLine.server = fixture.config.server;
    ClientCommandHandler handler(fixture.runtime);

    fixture.runtime.configPath = temp.path();
    ControlRequest request = makeRequest("setServer", ControlCommand::SetServer);
    request.params["server"] = json::Value("10.0.0.5:4711");
    CHECK_EQ(fieldString(handler.handle(request), "error"), std::string(control_error::kFailed));
    CHECK_EQ(fixture.config.server, std::string("192.168.1.10:4711"));

    // A later, unrelated save must still write the stored address, not the
    // one given on the command line for this run only.
    fixture.runtime.configPath = temp.file("client.json");
    ClientConfig next = fixture.config;
    next.standbyAfterSeconds = 900;
    std::string error;
    CHECK(saveConfig(fixture.runtime, next, error));
    ClientConfig reloaded;
    CHECK(client_config::load(fixture.runtime.configPath, reloaded, error) ==
          client_config::LoadResult::Ok);
    CHECK_EQ(reloaded.server, std::string("10.0.0.1:4711"));
}

MULTIROOM_TEST(set_server_reports_an_unchanged_address)
{
    multiroomtest::TempDirectory temp("control");
    Fixture fixture;
    fixture.runtime.configPath = temp.file("client.json");
    ClientCommandHandler handler(fixture.runtime);

    ControlRequest request = makeRequest("setServer", ControlCommand::SetServer);
    request.params["server"] = json::Value("192.168.1.10:4711");

    const json::Value response = handler.handle(request);
    CHECK_EQ(fieldBool(response, "ok", false), true);
    CHECK_EQ(fieldBool(response, "changed", true), false);
    // Nothing was written, so no file was created either.
    CHECK(!fileutil::exists(fixture.runtime.configPath));
}

MULTIROOM_TEST(set_server_clears_the_address_on_an_empty_string)
{
    multiroomtest::TempDirectory temp("control");
    Fixture fixture;
    fixture.runtime.configPath = temp.file("client.json");
    ClientCommandHandler handler(fixture.runtime);

    ControlRequest request = makeRequest("setServer", ControlCommand::SetServer);
    request.params["server"] = json::Value("");

    const json::Value response = handler.handle(request);
    CHECK_EQ(fieldBool(response, "ok", false), true);
    CHECK_EQ(fixture.config.server, std::string());
}

MULTIROOM_TEST(set_server_rejects_a_bad_address_and_changes_nothing)
{
    multiroomtest::TempDirectory temp("control");
    Fixture fixture;
    fixture.runtime.configPath = temp.file("client.json");
    ClientCommandHandler handler(fixture.runtime);

    const char* const bad[] = {"192.168.1.10", "host:0", "host:70000", "::1:4711"};
    for (const char* address : bad)
    {
        ControlRequest request = makeRequest("setServer", ControlCommand::SetServer);
        request.params["server"] = json::Value(address);

        const json::Value response = handler.handle(request);
        CHECK_EQ(fieldBool(response, "ok", true), false);
        CHECK_EQ(fieldString(response, "error"), std::string(control_error::kInvalidArgument));
        CHECK_EQ(fixture.config.server, std::string("192.168.1.10:4711"));
    }

    // A missing or wrongly typed params.server is the same class of error.
    ControlRequest missing = makeRequest("setServer", ControlCommand::SetServer);
    CHECK_EQ(fieldString(handler.handle(missing), "error"),
             std::string(control_error::kInvalidArgument));

    ControlRequest wrongType = makeRequest("setServer", ControlCommand::SetServer);
    wrongType.params["server"] = json::Value(static_cast<int64_t>(4711));
    CHECK_EQ(fieldString(handler.handle(wrongType), "error"),
             std::string(control_error::kInvalidArgument));

    CHECK(!fileutil::exists(fixture.runtime.configPath));
}

MULTIROOM_TEST(set_server_leaves_the_client_untouched_when_it_cannot_persist)
{
    Fixture fixture;
    // A directory that cannot be created, so the atomic write has to fail.
    fixture.runtime.configPath = "";
    ClientCommandHandler handler(fixture.runtime);

    ControlRequest request = makeRequest("setServer", ControlCommand::SetServer);
    request.params["server"] = json::Value("10.0.0.5:4711");

    const json::Value response = handler.handle(request);
    CHECK_EQ(fieldBool(response, "ok", true), false);
    CHECK_EQ(fieldString(response, "error"), std::string(control_error::kFailed));
    // The running client must not point somewhere the file does not agree with.
    CHECK_EQ(fixture.config.server, std::string("192.168.1.10:4711"));
}

MULTIROOM_TEST(get_client_id_returns_the_configured_id)
{
    Fixture fixture;
    ClientCommandHandler handler(fixture.runtime);

    const json::Value response =
        handler.handle(makeRequest("getClientId", ControlCommand::GetClientId));
    CHECK_EQ(fieldBool(response, "ok", false), true);
    CHECK_EQ(fieldString(response, "clientId"), std::string("livingroom"));
}

MULTIROOM_TEST(set_client_id_validates_and_persists)
{
    multiroomtest::TempDirectory temp("control");
    Fixture fixture;
    fixture.runtime.configPath = temp.file("client.json");
    ClientCommandHandler handler(fixture.runtime);

    ControlRequest request = makeRequest("setClientId", ControlCommand::SetClientId);
    request.params["clientId"] = json::Value("kitchen");

    const json::Value response = handler.handle(request);
    CHECK_EQ(fieldBool(response, "ok", false), true);
    CHECK_EQ(fieldString(response, "clientId"), std::string("kitchen"));
    CHECK_EQ(fieldBool(response, "changed", false), true);
    CHECK_EQ(fixture.config.clientId, std::string("kitchen"));

    ClientConfig reloaded;
    std::string error;
    CHECK(client_config::load(fixture.runtime.configPath, reloaded, error) ==
          client_config::LoadResult::Ok);
    CHECK_EQ(reloaded.clientId, std::string("kitchen"));

    // The same id again is not a change and writes nothing.
    CHECK(fileutil::removeFile(fixture.runtime.configPath));
    const json::Value again = handler.handle(request);
    CHECK_EQ(fieldBool(again, "changed", true), false);
    CHECK(!fileutil::exists(fixture.runtime.configPath));
}

MULTIROOM_TEST(set_client_id_rejects_a_bad_id_and_changes_nothing)
{
    multiroomtest::TempDirectory temp("control");
    Fixture fixture;
    fixture.runtime.configPath = temp.file("client.json");
    ClientCommandHandler handler(fixture.runtime);

    // Empty included: an id cannot be cleared, only replaced.
    const char* const bad[] = {"", " kitchen", "kit\nchen"};
    for (const char* id : bad)
    {
        ControlRequest request = makeRequest("setClientId", ControlCommand::SetClientId);
        request.params["clientId"] = json::Value(id);
        const json::Value response = handler.handle(request);
        CHECK_EQ(fieldBool(response, "ok", true), false);
        CHECK_EQ(fieldString(response, "error"), std::string(control_error::kInvalidArgument));
    }

    ControlRequest missing = makeRequest("setClientId", ControlCommand::SetClientId);
    CHECK_EQ(fieldString(handler.handle(missing), "error"),
             std::string(control_error::kInvalidArgument));

    CHECK_EQ(fixture.config.clientId, std::string("livingroom"));
    CHECK(!fileutil::exists(fixture.runtime.configPath));
}

MULTIROOM_TEST(set_client_id_leaves_the_client_untouched_when_it_cannot_persist)
{
    Fixture fixture;
    fixture.runtime.configPath = "";
    ClientCommandHandler handler(fixture.runtime);

    ControlRequest request = makeRequest("setClientId", ControlCommand::SetClientId);
    request.params["clientId"] = json::Value("kitchen");

    const json::Value response = handler.handle(request);
    CHECK_EQ(fieldString(response, "error"), std::string(control_error::kFailed));
    CHECK_EQ(fixture.config.clientId, std::string("livingroom"));
}

MULTIROOM_TEST(rescan_audio_runs_the_scan_and_reports_the_result)
{
    Fixture fixture;
    int calls = 0;
    fixture.runtime.rescanAudio = [&fixture, &calls](std::string&) {
        ++calls;
        fixture.cache.devices.clear();
        fixture.runtime.deviceSelected = false;
        return true;
    };
    ClientCommandHandler handler(fixture.runtime);

    const json::Value response =
        handler.handle(makeRequest("rescanAudio", ControlCommand::RescanAudio));
    CHECK_EQ(calls, 1);
    CHECK_EQ(fieldBool(response, "ok", false), true);
    CHECK_EQ(response.find("deviceCount")->asInt(), static_cast<int64_t>(0));
    CHECK_EQ(fieldBool(response, "deviceSelected", true), false);
    CHECK(response.find("durationMs") != nullptr);
}

MULTIROOM_TEST(rescan_audio_reports_a_failing_scan)
{
    Fixture fixture;
    fixture.runtime.rescanAudio = [](std::string& error) {
        error = "no audio backend compiled in for this platform";
        return false;
    };
    ClientCommandHandler handler(fixture.runtime);

    const json::Value response =
        handler.handle(makeRequest("rescanAudio", ControlCommand::RescanAudio));
    CHECK_EQ(fieldBool(response, "ok", true), false);
    CHECK_EQ(fieldString(response, "error"), std::string(control_error::kFailed));
    CHECK_EQ(fieldString(response, "message"),
             std::string("no audio backend compiled in for this platform"));
}

MULTIROOM_TEST(status_leaves_the_position_out_of_a_track_that_has_none)
{
    // TRACK_INFO (section 17) names what is playing and carries no timeline
    // frame, unlike a boundary. Reporting 0 for it would put the room at the
    // very start of the timeline.
    ClientStatus status;
    status.streamPrepared = true;
    status.trackKnown = true;
    status.trackId = "track-A";
    status.trackTitle = "First Song";
    status.hasTrackTimelineNs = false;

    const json::Value stream = object(toJson(status), "stream");
    const json::Value& track = object(stream, "track");
    CHECK_EQ(fieldString(track, "trackId"), std::string("track-A"));
    CHECK_EQ(fieldString(track, "title"), std::string("First Song"));
    CHECK(track.find("timelineNs") == nullptr);

    // A boundary the sound card reached does carry one, and 0 ns is a real
    // position rather than an absent one.
    status.hasTrackTimelineNs = true;
    status.trackTimelineNs = 180000000000ULL;
    const json::Value withPosition = object(toJson(status), "stream");
    const json::Value& placed = object(withPosition, "track");
    CHECK(placed.find("timelineNs") != nullptr);
    CHECK_EQ(placed.find("timelineNs")->asInt(), static_cast<int64_t>(180000000000ULL));
}

MULTIROOM_TEST(status_answers_the_system_block_once_the_main_loop_has_sampled)
{
    Fixture fixture;
    ClientCommandHandler handler(fixture.runtime);

    json::Value response = handler.handle(makeRequest("status", ControlCommand::Status));
    CHECK(object(response, "status").find("system") == nullptr);

    sampleSystem(fixture.runtime, monotonicNowNs());
    response = handler.handle(makeRequest("status", ControlCommand::Status));
    const json::Value* system = object(response, "status").find("system");
    CHECK(system != nullptr);
    int64_t bytes = 0;
    CHECK(json::getInt(*system, "memoryBytes", bytes));
    CHECK(bytes > 0);
    CHECK(json::getInt(*system, "uptimeMs", bytes));
    // A rate needs two samples, and one is all there has been.
    CHECK(system->find("cpuPercent") == nullptr);
}

MULTIROOM_TEST(set_audio_device_validates_persists_and_reselects)
{
    multiroomtest::TempDirectory temp("control");
    Fixture fixture;
    fixture.runtime.configPath = temp.file("client.json");

    AudioDeviceInfo second = fixture.cache.devices[0];
    second.id = "hw:CARD=USB,DEV=0";
    second.name = "USB DAC";
    second.isDefault = false;
    fixture.cache.devices.push_back(second);
    ClientCommandHandler handler(fixture.runtime);

    ControlRequest request = makeRequest("setAudioDevice", ControlCommand::SetAudioDevice);
    request.params["deviceId"] = json::Value("hw:CARD=USB,DEV=0");

    const json::Value response = handler.handle(request);
    CHECK_EQ(fieldBool(response, "ok", false), true);
    CHECK_EQ(fieldBool(response, "changed", false), true);
    CHECK_EQ(fieldString(response, "selectedDeviceId"), std::string("hw:CARD=USB,DEV=0"));
    CHECK_EQ(fixture.runtime.deviceId, std::string("hw:CARD=USB,DEV=0"));

    ClientConfig reloaded;
    std::string error;
    CHECK(client_config::load(fixture.runtime.configPath, reloaded, error) ==
          client_config::LoadResult::Ok);
    CHECK_EQ(reloaded.audioDeviceId, std::string("hw:CARD=USB,DEV=0"));

    // Empty is the system default, and is a change back.
    request.params["deviceId"] = json::Value("");
    const json::Value back = handler.handle(request);
    CHECK_EQ(fieldBool(back, "changed", false), true);
    CHECK_EQ(fixture.runtime.deviceId, std::string("hw:CARD=Audio,DEV=0"));
    CHECK_EQ(fieldBool(handler.handle(request), "changed", true), false);
}

MULTIROOM_TEST(set_audio_device_refuses_a_device_the_scan_did_not_find)
{
    multiroomtest::TempDirectory temp("control");
    Fixture fixture;
    fixture.runtime.configPath = temp.file("client.json");
    ClientCommandHandler handler(fixture.runtime);

    ControlRequest request = makeRequest("setAudioDevice", ControlCommand::SetAudioDevice);
    request.params["deviceId"] = json::Value("hw:CARD=Gone,DEV=0");
    CHECK_EQ(fieldString(handler.handle(request), "error"),
             std::string(control_error::kInvalidArgument));

    ControlRequest missing = makeRequest("setAudioDevice", ControlCommand::SetAudioDevice);
    CHECK_EQ(fieldString(handler.handle(missing), "error"),
             std::string(control_error::kInvalidArgument));

    CHECK_EQ(fixture.config.audioDeviceId, std::string());
    CHECK(!fileutil::exists(fixture.runtime.configPath));
}

MULTIROOM_TEST(set_audio_device_leaves_the_client_untouched_when_it_cannot_persist)
{
    Fixture fixture;
    fixture.runtime.configPath = "";
    ClientCommandHandler handler(fixture.runtime);

    ControlRequest request = makeRequest("setAudioDevice", ControlCommand::SetAudioDevice);
    request.params["deviceId"] = json::Value("hw:CARD=Audio,DEV=0");
    CHECK_EQ(fieldString(handler.handle(request), "error"), std::string(control_error::kFailed));
    CHECK_EQ(fixture.config.audioDeviceId, std::string());
}

MULTIROOM_TEST(change_buffer_validates_persists_and_never_clamps)
{
    multiroomtest::TempDirectory temp("control");
    Fixture fixture;
    fixture.runtime.configPath = temp.file("client.json");
    std::string error;

    CHECK(changeBuffer(fixture.runtime, 3000, error) == BufferChange::Changed);
    CHECK_EQ(fixture.config.bufferMs, static_cast<uint32_t>(3000));
    ClientConfig reloaded;
    CHECK(client_config::load(fixture.runtime.configPath, reloaded, error) ==
          client_config::LoadResult::Ok);
    CHECK_EQ(reloaded.bufferMs, static_cast<uint32_t>(3000));
    CHECK(changeBuffer(fixture.runtime, 3000, error) == BufferChange::Unchanged);

    // Out of range is refused as asked, not moved to the nearest bound.
    CHECK(changeBuffer(fixture.runtime, 500, error) == BufferChange::Refused);
    CHECK(changeBuffer(fixture.runtime, 20000, error) == BufferChange::Refused);
    CHECK(error.find("bufferMs") != std::string::npos);
    CHECK_EQ(fixture.config.bufferMs, static_cast<uint32_t>(3000));

    fixture.runtime.configPath = "";
    CHECK(changeBuffer(fixture.runtime, 1000, error) == BufferChange::NotSaved);
    CHECK_EQ(fixture.config.bufferMs, static_cast<uint32_t>(3000));
}

MULTIROOM_TEST(change_buffer_refuses_a_size_no_format_of_the_device_fits)
{
    multiroomtest::TempDirectory temp("control");
    Fixture fixture;
    fixture.runtime.configPath = temp.file("client.json");

    // Eleven seconds of 384 kHz is past what the ring buffer can hold at all,
    // on any platform; ten less is not.
    AudioFormat wide;
    wide.sampleRate = 384000;
    wide.channels = 8;
    wide.sampleType = AudioFormat::SampleType::Int32;
    fixture.cache.devices[0].exclusiveFormats.assign(1, wide);

    std::string error;
    CHECK(!streamBufferFits(wide, 11000, error));
    CHECK(streamBufferFits(wide, 1000, error));
    CHECK(changeBuffer(fixture.runtime, 11000, error) == BufferChange::Refused);
    CHECK(error.find("no format of Test Output") != std::string::npos);
    CHECK_EQ(fixture.config.bufferMs, static_cast<uint32_t>(2500));
    CHECK(!fileutil::exists(fixture.runtime.configPath));

    // The capability report leaves out what does not fit at the configured
    // target, and says so when that is everything.
    matp::CapabilityReport report;
    fixture.config.bufferMs = 11000;
    CHECK(!currentCapabilities(fixture.runtime, report, error));
    fixture.config.bufferMs = 1000;
    CHECK(currentCapabilities(fixture.runtime, report, error));
    CHECK_EQ(report.formats.size(), static_cast<size_t>(1));
    CHECK_EQ(report.accessMode, std::string(audioAccessModeName(fixture.runtime.accessMode)));

    // The device's hardware volume goes with its formats, always said.
    CHECK(report.hasHardwareVolume);
    CHECK(!report.hardwareVolume.available);
    HardwareVolumeInfo& volume = fixture.cache.devices[0].hardwareVolume;
    volume.available = true;
    volume.control = "PCM";
    volume.maxStep = 255;
    volume.hasDecibels = true;
    volume.minDb = -102.0;
    volume.maxDb = 4.0;
    CHECK(currentCapabilities(fixture.runtime, report, error));
    CHECK(report.hardwareVolume.available);
    CHECK_EQ(report.hardwareVolume.control, std::string("PCM"));
    CHECK_EQ(report.hardwareVolume.steps, static_cast<uint32_t>(256));
    CHECK_EQ(report.hardwareVolume.minDb, -102.0);
}

MULTIROOM_TEST(input_commands_list_rebind_persist_and_refuse)
{
    multiroomtest::TempDirectory temp("control");
    Fixture fixture;
    fixture.runtime.configPath = temp.file("client.json");
    InputDefinition play;
    play.control.id = "play";
    play.control.bindings = matp::defaultInputBindings(matp::InputKind::Button);
    play.pin = 17;
    fixture.config.inputs.push_back(play);
    ClientCommandHandler handler(fixture.runtime);

    const json::Value listed = handler.handle(makeRequest("inputs", ControlCommand::Inputs));
    CHECK_EQ(fieldBool(listed, "ok", false), true);
    CHECK_EQ(listed.find("inputs")->size(), static_cast<size_t>(1));

    ControlRequest request = makeRequest("setInputBindings", ControlCommand::SetInputBindings);
    request.params["id"] = json::Value("play");
    json::Value bindings;
    std::string error;
    CHECK(json::parse("{\"longPress\":{\"action\":\"volumeDown\",\"step\":0.1,\"scope\":\"zone\"}}",
                      bindings, error));
    request.params["bindings"] = bindings;
    CHECK_EQ(fieldBool(handler.handle(request), "changed", false), true);
    CHECK_EQ(fieldBool(handler.handle(request), "changed", true), false);

    ClientConfig reloaded;
    CHECK(client_config::load(fixture.runtime.configPath, reloaded, error) ==
          client_config::LoadResult::Ok);
    const matp::InputBinding& hold =
        reloaded.inputs[0].control.bindings[static_cast<size_t>(matp::InputGesture::LongPress)];
    CHECK(hold.action == matp::InputAction::VolumeDown);
    CHECK(hold.scope == matp::VolumeScope::Zone);

    request.params["id"] = json::Value("gone");
    CHECK_EQ(fieldString(handler.handle(request), "error"),
             std::string(control_error::kInvalidArgument));

    // No session to send on: refused, and said so.
    ControlRequest simulate = makeRequest("simulateInput", ControlCommand::SimulateInput);
    simulate.params["id"] = json::Value("play");
    simulate.params["gesture"] = json::Value("click");
    CHECK_EQ(fieldString(handler.handle(simulate), "error"),
             std::string(control_error::kUnavailable));
    simulate.params["gesture"] = json::Value("clockwise");
    CHECK_EQ(fieldString(handler.handle(simulate), "error"),
             std::string(control_error::kInvalidArgument));
}

MULTIROOM_TEST(volume_control_changes_are_saved_and_software_is_refused_under_bitperfect)
{
    multiroomtest::TempDirectory temp("volumecontrol");
    Fixture fixture;
    fixture.runtime.configPath = temp.file("client.json");
    std::string error;

    fixture.runtime.audioControl = matp::AudioControl::BitPerfect;
    CHECK(changeVolumeControl(fixture.runtime, matp::VolumeControlMode::Software, error) ==
          VolumeControlChange::Refused);
    CHECK(fixture.config.volumeControl == matp::VolumeControlMode::Auto);
    CHECK(!fileutil::exists(fixture.runtime.configPath));

    applyAudioControl(fixture.runtime, matp::AudioControl::Auto, true);
    CHECK(changeVolumeControl(fixture.runtime, matp::VolumeControlMode::Software, error) ==
          VolumeControlChange::Changed);
    ClientConfig saved;
    CHECK(client_config::load(fixture.runtime.configPath, saved, error) ==
          client_config::LoadResult::Ok);
    CHECK(saved.volumeControl == matp::VolumeControlMode::Software);

    // The room becoming bitPerfect takes software back to auto, saved.
    applyAudioControl(fixture.runtime, matp::AudioControl::BitPerfect, true);
    CHECK(fixture.config.volumeControl == matp::VolumeControlMode::Auto);
    CHECK(client_config::load(fixture.runtime.configPath, saved, error) ==
          client_config::LoadResult::Ok);
    CHECK(saved.volumeControl == matp::VolumeControlMode::Auto);

    const json::Value response = ClientCommandHandler(fixture.runtime)
                                     .handle(makeRequest("status", ControlCommand::Status));
    const json::Value& audio = object(object(response, "status"), "audio");
    CHECK_EQ(fieldString(audio, "audioControl"), std::string("bitPerfect"));
    CHECK_EQ(fieldString(object(audio, "volumeControl"), "configured"), std::string("auto"));
}

MULTIROOM_TEST(set_name_validates_persists_and_never_touches_the_id)
{
    multiroomtest::TempDirectory temp("control");
    Fixture fixture;
    fixture.runtime.configPath = temp.file("client.json");
    ClientCommandHandler handler(fixture.runtime);

    ControlRequest request = makeRequest("setName", ControlCommand::SetName);
    request.params["name"] = json::Value("Arbeitsraum");
    const json::Value response = handler.handle(request);
    CHECK_EQ(fieldBool(response, "ok", false), true);
    CHECK_EQ(fieldString(response, "name"), std::string("Arbeitsraum"));
    CHECK_EQ(fieldBool(response, "changed", false), true);
    CHECK_EQ(fixture.config.name, std::string("Arbeitsraum"));
    CHECK_EQ(fixture.config.clientId, std::string("livingroom"));

    ClientConfig reloaded;
    std::string error;
    CHECK(client_config::load(fixture.runtime.configPath, reloaded, error) ==
          client_config::LoadResult::Ok);
    CHECK_EQ(reloaded.name, std::string("Arbeitsraum"));

    CHECK_EQ(fieldBool(handler.handle(request), "changed", true), false);

    for (const char* bad : {"", " Arbeitsraum", "Arbeits\nraum"})
    {
        ControlRequest refused = makeRequest("setName", ControlCommand::SetName);
        refused.params["name"] = json::Value(bad);
        CHECK_EQ(fieldString(handler.handle(refused), "error"),
                 std::string(control_error::kInvalidArgument));
    }
    CHECK_EQ(fixture.config.name, std::string("Arbeitsraum"));
}
