#include "app/ClientCommandHandler.h"
#include "discovery/NodeId.h"

#include "app/DeviceSelection.h"
#include "app/InputSettings.h"
#include "app/LoopTiming.h"
#include "app/TaskReport.h"
#include "app/Version.h"
#include "input/InputController.h"
#include "util/Logger.h"
#include "util/Time.h"
#include "util/FileUtil.h"
#if defined(__linux__) && !defined(ESP_PLATFORM)
#include "net/NetworkSettings.h"
#include "config/BoardConfig.h"
#include "config/CpuSettings.h"
#include "net/SshSettings.h"
#include "config/DacSettings.h"
#include <dirent.h>
#include <spawn.h>
#include <unistd.h>
#include <sys/wait.h>
#include <cctype>
#include <cstring>
#include <fstream>
extern char** environ;
#endif

namespace multiroom {

bool saveConfig(ClientRuntime& runtime, const ClientConfig& config, std::string& error)
{
    // configToSave() retires overrides the new value replaces; that only holds
    // once the save did, since a failed one leaves the override in effect.
    CommandLineOptions options = runtime.commandLine;
    if (!client_config::save(runtime.configPath,
                             configToSave(options, runtime.storedConfig, config), error))
        return false;
    runtime.commandLine = options;
    return true;
}

namespace {

bool restartSupported()
{
#if defined(__linux__) && !defined(ESP_PLATFORM)
    return ::geteuid() == 0 && ::access("/usr/sbin/galdrun-restart", X_OK) == 0;
#else
    return false; // ESP32's web server uses its existing deferred restart.
#endif
}

bool requestRestart(ClientRuntime& runtime, std::string& error)
{
    if (!restartSupported())
    {
        error = "Restart is unavailable on this client";
        return false;
    }
#if defined(__linux__) && !defined(ESP_PLATFORM)
    if (runtime.restartProcess != 0) return true;
    char* args[] = {const_cast<char*>("/usr/sbin/galdrun-restart"), nullptr};
    pid_t pid = 0;
    const int result = ::posix_spawn(&pid, args[0], nullptr, nullptr, args, environ);
    if (result != 0)
    {
        error = std::string("Cannot start restart helper: ") + std::strerror(result);
        return false;
    }
    runtime.restartProcess = static_cast<int>(pid);
#else
    (void)runtime;
#endif
    return true;
}

#if defined(__linux__) && !defined(ESP_PLATFORM)
const char* const kDeepSleepHelper = "/usr/sbin/raumfeld-suspend";

std::string firstLine(const std::string& path)
{
    std::ifstream file(path);
    std::string line;
    std::getline(file, line);
    return line;
}

// What a Wake-on-LAN magic packet must name: the wired interface with a link,
// when the server is reached over Ethernet. Empty over Wi-Fi, where the
// helper arms nothing and only POWER wakes the device.
std::string wiredWakeMac(const ClientRuntime& runtime)
{
    if (runtime.server == nullptr || runtime.server->networkLink() != "ethernet")
        return {};
    DIR* directory = ::opendir("/sys/class/net");
    if (directory == nullptr)
        return {};
    std::string mac;
    while (const dirent* entry = ::readdir(directory))
    {
        const std::string base = std::string("/sys/class/net/") + entry->d_name;
        if (entry->d_name[0] == '.' || ::access((base + "/wireless").c_str(), F_OK) == 0 ||
            ::access((base + "/phy80211").c_str(), F_OK) == 0 ||
            firstLine(base + "/type") != "1" || firstLine(base + "/carrier") != "1")
            continue;
        mac = firstLine(base + "/address");
        for (char& c : mac)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        break;
    }
    ::closedir(directory);
    return mac;
}
#endif

// The live connection state when there is a connection, and what the
// configuration implies when there is not.
ServerConnectionState currentServerState(const ClientRuntime& runtime)
{
    if (runtime.server != nullptr)
        return runtime.server->state();

    const bool configured = runtime.config != nullptr && !runtime.config->server.empty();
    return configured ? ServerConnectionState::Disconnected
                      : ServerConnectionState::NotConfigured;
}

} // namespace

// Only the Connector 2 firmware ships the suspend helper.
bool deepSleepSupported()
{
#if defined(__linux__) && !defined(ESP_PLATFORM)
    return ::geteuid() == 0 && ::access(kDeepSleepHelper, X_OK) == 0;
#else
    return false;
#endif
}

void sampleSystem(ClientRuntime& runtime, uint64_t nowNs)
{
    if (nowNs < runtime.systemNextNs)
        return;
    runtime.systemNextNs = nowNs + 2000ULL * 1000000ULL;
    runtime.system = runtime.systemSampler.sample();
    runtime.hasSystem = true;
}

void publishPlayback(ClientRuntime& runtime, const PlaybackSupervisor& playback,
                     const AudioEngine& engine, uint64_t nowNs)
{
    runtime.playbackState = playback.state();
    runtime.playbackError = playback.lastError();
    runtime.playbackAttempts = playback.failedAttempts();
    runtime.playbackRecoveries = playback.recoveries();
    runtime.playbackRetryMs = playback.retryInMs(nowNs);

    if (playback.state() == PlaybackState::Playing)
    {
        runtime.underruns = engine.underruns();
        runtime.bufferFrames = engine.bufferFrames();
        return;
    }

    // The underrun count stays at what the dead stream reached -- zeroing it
    // would claim a clean run that did not happen -- but there is no period
    // and no clock to report while nothing is open.
    runtime.bufferFrames = 0;
    runtime.lastClock = AudioClockSnapshot();
}

ClientStatus buildClientStatus(const ClientRuntime& runtime)
{
    ClientStatus status;
    if (runtime.session != nullptr && runtime.playback != nullptr)
    {
        status.powerState = runtime.session->powerState();
        if (runtime.config != nullptr)
        {
            status.standbyAfterSeconds = runtime.config->standbyAfterSeconds;
            status.deepSleepAfterSeconds = runtime.config->deepSleepAfterSeconds;
        }
    }
    status.version = versionString();
    status.platform = device_cache::currentPlatformName();

    if (runtime.startedAtNs != 0)
    {
        const uint64_t now = monotonicNowNs();
        if (now > runtime.startedAtNs)
            status.uptimeSeconds = (now - runtime.startedAtNs) / 1000000000ULL;
    }

    if (runtime.config != nullptr)
    {
        status.clientId = runtime.config->clientId;
        status.name = runtime.config->name;
        status.server = runtime.config->server;
    }
    status.serverState = currentServerState(runtime);

    if (runtime.session != nullptr)
    {
        const matp::Session& session = *runtime.session;
        status.sessionState = session.state();
        status.sessionId = session.sessionId();
        status.sessionFormatSelected = session.hasStreamFormat();
        status.sessionFormat = session.streamFormat();
        status.sessionFormatPlayable = session.streamFormatPlayable();
        status.sessionError = session.lastError();

        const matp::ClockModel& clock = session.clock();
        status.hasClockSync = clock.hasEstimate();
        if (status.hasClockSync)
        {
            status.clockOffsetNs = clock.offsetNs();
            status.hasClockSkew = clock.hasSkew();
            status.clockSkewPpm = clock.skewPpm();
            status.clockRoundTripNs = clock.bestRoundTripNs();
            status.clockLastRoundTripNs = clock.lastRoundTripNs();
            status.clockUncertaintyNs = clock.uncertaintyNs();
            status.clockSampleCount = clock.sampleCount();
            status.clockWindowSeconds = clock.windowSpanSeconds();
        }
        status.clockSyncRequests = session.clockSyncRequests();
        status.clockSyncReplies = session.clockSyncReplies();
        status.clockSyncResults = session.clockSyncResults();
        status.streamPrepared = session.streamPrepared();
    }

    if (status.streamPrepared && runtime.audioStream != nullptr)
    {
        const matp::AudioStream& stream = *runtime.audioStream;
        const matp::BufferStatus buffer = stream.status();

        status.streamPackets = stream.packets();
        status.streamFrames = stream.frames();
        status.streamWaits = stream.waits();
        status.streamReattachments = stream.reattachments();
        status.streamRejoins = stream.rejoins();
        status.streamFlushes = stream.flushes();
        status.streamError = stream.lastError();
        status.streamHasContent = buffer.hasContent;
        status.streamPlayheadTimelineNs = buffer.playheadTimelineNs;
        status.streamBufferedUntilTimelineNs = buffer.bufferedUntilTimelineNs;
        status.streamBufferedDurationNs = buffer.bufferedDurationNs;
        status.streamBufferedFrames = buffer.streamFrames;
        status.streamCapacityFrames = stream.capacityFrames();

        status.timelineState = runtime.timelineState;
        status.startScheduled = runtime.startScheduled;
        status.startTimelineNs = runtime.startTimelineNs;
        status.startDeviceFrame = runtime.startDeviceFrame;
        status.startInMs = runtime.startInMs;
        status.hasTimelineNsAudible = runtime.hasTimelineNsAudible;
        status.timelineNsAudible = runtime.timelineNsAudible;
        status.timelineUnderrunFrames = runtime.timelineUnderrunFrames;
        status.timelineSkippedFrames = runtime.timelineSkippedFrames;
        status.timelineFlushedFrames = runtime.timelineFlushedFrames;
        status.endScheduled = runtime.endScheduled;
        status.endTimelineNs = runtime.endTimelineNs;
        status.volumeGain = runtime.volumeGain;
        status.volumeMuted = runtime.volumeMuted;
        status.volumeScheduled = runtime.volumeScheduled;
        status.appliedGain = runtime.appliedGain;
        status.trackKnown = runtime.trackKnown;
        status.trackId = runtime.trackId;
        status.trackTitle = runtime.trackTitle;
        status.hasTrackTimelineNs = runtime.trackTimelineNsKnown;
        status.trackTimelineNs = runtime.trackTimelineNs;
        status.pendingTrackBoundaries = runtime.pendingTrackBoundaries;

        // Milliseconds as well as frames: the buffer is configured in
        // milliseconds and a fill level in frames cannot be compared with it
        // by eye.
        const uint32_t rate = status.sessionFormat.sampleRate;
        if (rate != 0)
        {
            status.streamBufferedMs =
                static_cast<uint32_t>(static_cast<uint64_t>(buffer.streamFrames) * 1000 / rate);
            status.streamCapacityMs = static_cast<uint32_t>(
                static_cast<uint64_t>(status.streamCapacityFrames) * 1000 / rate);
        }

        if (runtime.audioServer != nullptr)
        {
            status.audioServer = runtime.audioServer->server();
            status.audioServerState = runtime.audioServer->state();
        }
    }

    if (runtime.deviceCache != nullptr)
    {
        status.deviceCount = static_cast<uint32_t>(runtime.deviceCache->devices.size());
        status.audioScannedAt = runtime.deviceCache->generatedAt;
        if (runtime.deviceSelected)
        {
            if (const AudioDeviceInfo* device = runtime.deviceCache->findById(runtime.deviceId))
            {
                status.hardwareVolume = device->hardwareVolume;
                status.hardwareClock = device->hardwareClock;
            }
        }
    }

    status.deviceSelected = runtime.deviceSelected;
    status.deviceId = runtime.deviceId;
    status.deviceName = runtime.deviceName;
    status.formatSelected = runtime.formatSelected;
    status.accessMode = runtime.accessMode;
    status.audioControlKnown = runtime.audioControlKnown;
    status.audioControl = runtime.audioControl;
    if (runtime.config != nullptr)
    {
        status.volumeControl = runtime.config->volumeControl;
        status.driftCorrection = runtime.config->driftCorrection;
    }
    status.driftCorrectionState = runtime.driftCorrection;
    status.volumePath = runtime.volumePath;
    status.hardwareVolumeActive = runtime.hardwareVolumeActive;
    status.hasHardwareVolumeState = runtime.hasHardwareVolumeState;
    status.hardwareVolumeState = runtime.hardwareVolumeState;

    status.playbackState = runtime.playbackState;
    status.underruns = runtime.underruns;
    status.bufferFrames = runtime.bufferFrames;
    status.deviceFormat = runtime.deviceFormat;
    status.clock = runtime.lastClock;
    status.playbackError = runtime.playbackError;
    status.playbackAttempts = runtime.playbackAttempts;
    status.playbackRecoveries = runtime.playbackRecoveries;
    status.playbackRetryMs = runtime.playbackRetryMs;

    // Reported only when there is a fit behind it. A zero here would be
    // indistinguishable from a perfect clock, which is exactly the mistake
    // PLAYBACK_REPORT must not make later either.
    if (runtime.drift != nullptr && runtime.drift->hasEstimate())
    {
        status.hasDrift = true;
        status.measuredSampleRate = runtime.drift->estimatedSampleRate();
        status.driftPpm = runtime.drift->driftPpm();
        status.driftStandardErrorPpm = runtime.drift->standardErrorPpm();
        status.driftConfidence = runtime.drift->confidence();
        status.driftSampleCount = runtime.drift->sampleCount();
        status.driftWindowSeconds = runtime.drift->windowSpanSeconds();
    }

    status.hasSystem = runtime.hasSystem;
    status.system = runtime.system;

    return status;
}

ClientCommandHandler::ClientCommandHandler(ClientRuntime& runtime) : runtime_(runtime) {}

bool ClientCommandHandler::dispatchInputAction(const matp::InputActionEvent& event,
                                                std::string& error)
{
    using matp::InputAction;
    const InputAction action = event.binding.action;
    if (action == InputAction::DeepSleep)
    {
#if defined(__linux__) && !defined(ESP_PLATFORM)
        if (runtime_.deepSleepProcess != 0)
        {
            error = "deep sleep is already being requested";
            return false;
        }
        const char* helper = kDeepSleepHelper;
        if (!deepSleepSupported())
        {
            error = "deep sleep requires the Connector 2 firmware helper and root permissions";
            return false;
        }
        pid_t child = 0;
        char* const arguments[] = {const_cast<char*>(helper), nullptr};
        const int result = ::posix_spawn(&child, helper, nullptr, nullptr, arguments, environ);
        if (result != 0)
        {
            error = std::string("could not start deep sleep: ") + std::strerror(result);
            return false;
        }
        runtime_.deepSleepProcess = static_cast<int>(child);
        if (runtime_.session != nullptr)
            runtime_.session->announceSleep(wiredWakeMac(runtime_));
        return true;
#else
        error = "deep sleep is not supported on this platform";
        return false;
#endif
    }
    if (action == InputAction::StandbyOff || action == InputAction::StandbyOn ||
        action == InputAction::ToggleStandby)
    {
        if (runtime_.session == nullptr || runtime_.playback == nullptr)
        {
            error = "no playback path for power control";
            return false;
        }
        const bool on = action == InputAction::StandbyOff ||
            (action == InputAction::ToggleStandby &&
             std::string(runtime_.session->powerState()) != "on");
        runtime_.powerIdleTimerRunning = false;
        // A power toggle is one operation, even for a burst of encoder ticks.
        return runtime_.session->setPower(on, error);
    }
    if (runtime_.session == nullptr || !runtime_.session->sendInputAction(event))
    {
        error = "no session to send it on";
        return false;
    }
    return true;
}

void ClientCommandHandler::pollPower(uint64_t nowNs)
{
#if defined(__linux__) && !defined(ESP_PLATFORM)
    if (runtime_.restartProcess != 0)
    {
        int status = 0;
        if (::waitpid(runtime_.restartProcess, &status, WNOHANG) > 0)
        {
            runtime_.restartProcess = 0;
            if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
                MULTIROOM_WARN("restart helper failed; see system log");
        }
    }
    if (runtime_.deepSleepProcess != 0)
    {
        int status = 0;
        if (::waitpid(runtime_.deepSleepProcess, &status, WNOHANG) > 0)
        {
            runtime_.deepSleepProcess = 0;
            if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
            {
                MULTIROOM_WARN("deep sleep helper failed; see system log");
                if (runtime_.session != nullptr)
                    runtime_.session->resumeAfterFailedSleep();
            }
        }
    }
#endif
    const bool idle = runtime_.timelineState == TimelineState::Idle ||
                      runtime_.timelineState == TimelineState::Paused ||
                      runtime_.timelineState == TimelineState::Stopped;

    // Deep sleep counts from the same idle start, on through standby: a
    // parked room is still idle. Playback restarts the count.
    if (runtime_.config == nullptr || runtime_.config->deepSleepAfterSeconds == 0 ||
        runtime_.session == nullptr || !idle)
    {
        runtime_.deepSleepTimerRunning = false;
    }
    else if (!runtime_.deepSleepTimerRunning || nowNs < runtime_.deepSleepIdleSinceNs)
    {
        runtime_.deepSleepTimerRunning = true;
        runtime_.deepSleepIdleSinceNs = nowNs;
    }
    else if (nowNs - runtime_.deepSleepIdleSinceNs >=
             static_cast<uint64_t>(runtime_.config->deepSleepAfterSeconds) * 1000000000ULL)
    {
        runtime_.deepSleepTimerRunning = false;
        matp::InputActionEvent event;
        event.binding.action = matp::InputAction::DeepSleep;
        std::string error;
        if (!dispatchInputAction(event, error))
            MULTIROOM_WARN("automatic deep sleep: %s", error.c_str());
    }

    if (runtime_.config == nullptr || runtime_.config->standbyAfterSeconds == 0 ||
        runtime_.session == nullptr || !idle ||
        std::string(runtime_.session->powerState()) != "on")
    {
        runtime_.powerIdleTimerRunning = false;
        return;
    }
    if (!runtime_.powerIdleTimerRunning || nowNs < runtime_.powerIdleSinceNs)
    {
        runtime_.powerIdleTimerRunning = true;
        runtime_.powerIdleSinceNs = nowNs;
    }
    if (nowNs - runtime_.powerIdleSinceNs <
        static_cast<uint64_t>(runtime_.config->standbyAfterSeconds) * 1000000000ULL)
        return;
    runtime_.powerIdleTimerRunning = false;
    std::string error;
    if (!runtime_.session->setPower(false, error, true))
        MULTIROOM_WARN("standby: %s", error.c_str());
}

json::Value ClientCommandHandler::handle(const ControlRequest& request)
{
    switch (request.command)
    {
    case ControlCommand::Restart:
    {
        if (!request.params.members().empty())
            return makeControlError(request, control_error::kInvalidArgument, "restart takes no parameters");
        std::string error;
        if (!requestRestart(runtime_, error))
            return makeControlError(request, restartSupported() ? control_error::kFailed : control_error::kUnavailable, error);
        auto response = makeControlSuccess(request);
        response["restarting"] = json::Value(true);
        return response;
    }
    case ControlCommand::GetPower:
    case ControlCommand::SetPower:
    case ControlCommand::StandbyOff:
    case ControlCommand::StandbyOn:
    case ControlCommand::ToggleStandby:
    case ControlCommand::DeepSleep:
    {
        const bool supported = runtime_.session != nullptr && runtime_.playback != nullptr;
        auto response = makeControlSuccess(request);
        response["supported"] = json::Value(supported);
        response["restart"] = json::Value(restartSupported());
        if (!supported || runtime_.config == nullptr)
            return request.command == ControlCommand::GetPower ? response :
                makeControlError(request, control_error::kUnavailable,
                                 "no playback path for power control");
        if (request.command == ControlCommand::SetPower)
        {
            // Either delay, or both; a field left out keeps its value.
            ClientConfig next = *runtime_.config;
            size_t known = 0;
            int64_t seconds = 0;
            if (request.params.find("standbyAfterSeconds") != nullptr)
            {
                ++known;
                if (!json::getInt(request.params, "standbyAfterSeconds", seconds) ||
                    seconds < 0 || seconds > 86400)
                    return makeControlError(request, control_error::kInvalidArgument,
                        "standbyAfterSeconds must be an integer from 0 to 86400");
                next.standbyAfterSeconds = static_cast<uint32_t>(seconds);
            }
            if (request.params.find("deepSleepAfterSeconds") != nullptr)
            {
                ++known;
                if (!json::getInt(request.params, "deepSleepAfterSeconds", seconds) ||
                    !validDeepSleepAfterSeconds(seconds))
                    return makeControlError(request, control_error::kInvalidArgument,
                                            kDeepSleepAfterSecondsError);
                if (seconds != 0 && !deepSleepSupported())
                    return makeControlError(request, control_error::kUnavailable,
                                            "this client cannot deep sleep");
                next.deepSleepAfterSeconds = static_cast<uint32_t>(seconds);
            }
            if (known == 0 || known != request.params.members().size())
                return makeControlError(request, control_error::kInvalidArgument,
                    "power setting takes standbyAfterSeconds and/or deepSleepAfterSeconds only");
            std::string error;
            if (!saveConfig(runtime_, next, error))
                return makeControlError(request, control_error::kFailed, error);
            runtime_.config->standbyAfterSeconds = next.standbyAfterSeconds;
            runtime_.config->deepSleepAfterSeconds = next.deepSleepAfterSeconds;
            runtime_.powerIdleTimerRunning = false;
            runtime_.deepSleepTimerRunning = false;
        }
        else if (request.command != ControlCommand::GetPower)
        {
            if (!request.params.members().empty())
                return makeControlError(request, control_error::kInvalidArgument,
                                         "power commands take no parameters");
            matp::InputActionEvent event;
            event.binding.action = request.command == ControlCommand::StandbyOff ? matp::InputAction::StandbyOff :
                request.command == ControlCommand::StandbyOn ? matp::InputAction::StandbyOn :
                request.command == ControlCommand::DeepSleep ? matp::InputAction::DeepSleep :
                                                               matp::InputAction::ToggleStandby;
            std::string error;
            if (!dispatchInputAction(event, error))
                return makeControlError(request, control_error::kFailed, error);
        }
        response["state"] = json::Value(runtime_.session->powerState());
        response["standbyAfterSeconds"] = json::Value(runtime_.config->standbyAfterSeconds);
        response["deepSleepAfterSeconds"] = json::Value(runtime_.config->deepSleepAfterSeconds);
        response["deepSleep"] = json::Value(deepSleepSupported());
        return response;
    }
    case ControlCommand::GetLeds:
    case ControlCommand::SetLeds:
    {
        auto response = makeControlSuccess(request);
        bool supported = false;
#if defined(__linux__) && !defined(ESP_PLATFORM)
        supported = board_config::enabled(board_config::current(), "statusLeds");
#endif
        response["supported"] = json::Value(supported);
        if (!supported)
            return request.command == ControlCommand::GetLeds ? response :
                makeControlError(request, control_error::kUnavailable, "LED settings are not enabled on this firmware");
        if (runtime_.config == nullptr)
            return makeControlError(request, control_error::kUnavailable, "no configuration loaded");
        if (request.command == ControlCommand::SetLeds)
        {
            bool off = false;
            if (request.params.members().size() != 1 || !json::getBool(request.params, "off", off))
                return makeControlError(request, control_error::kInvalidArgument, "LED setting must contain only an off boolean");
            ClientConfig next = *runtime_.config;
            next.ledsOff = off;
            std::string saveError;
            if (!saveConfig(runtime_, next, saveError))
                return makeControlError(request, control_error::kFailed, saveError);
            runtime_.config->ledsOff = off;
        }
        response["off"] = json::Value(runtime_.config->ledsOff);
        return response;
    }
    case ControlCommand::GetAudioProfile:
    case ControlCommand::SetAudioProfile:
    {
        auto response = makeControlSuccess(request);
        response["supported"] = json::Value(false);
#if defined(__linux__) && !defined(ESP_PLATFORM)
        const char* helper = "/usr/sbin/galdrun-audio-profile";
        if (!board_config::enabled(board_config::current(), "audioProfiles") || ::access(helper, X_OK) != 0)
            return request.command == ControlCommand::GetAudioProfile ? response :
                makeControlError(request, control_error::kUnavailable, "Audio profiles are unavailable");
        std::string text, error;
        json::Value profiles;
        if (!fileutil::readTextFile("/usr/share/galdrun-audio-profiles.json", text, error) ||
            !json::parse(text, profiles, error) || !profiles.isArray())
            return makeControlError(request, control_error::kFailed, "Cannot read audio profiles");
        if (request.command == ControlCommand::SetAudioProfile)
        {
            if (!restartSupported())
                return makeControlError(request, control_error::kUnavailable, "Audio profile changes require restart support");
            std::string profile;
            if (request.params.members().size() != 1 || !json::getString(request.params, "profile", profile))
                return makeControlError(request, control_error::kInvalidArgument, "Expected one audio profile");
            bool known = profile == "rollback";
            for (size_t i = 0; i < profiles.size(); ++i)
            {
                std::string id;
                json::getString(profiles.at(i), "id", id);
                known |= profile == id;
            }
            if (!known)
                return makeControlError(request, control_error::kInvalidArgument, "Unknown audio profile");
            char* args[] = {const_cast<char*>(helper), const_cast<char*>(profile.c_str()), nullptr};
            pid_t pid = 0;
            if (::posix_spawn(&pid, helper, nullptr, nullptr, args, environ) != 0)
                return makeControlError(request, control_error::kFailed, "Cannot start audio profile helper");
            int status = 0;
            pid_t waited;
            do { waited = ::waitpid(pid, &status, 0); } while (waited < 0 && errno == EINTR);
            if (waited != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
                return makeControlError(request, control_error::kFailed,
                    "Profile rejected; check board wiring, conflicting boot overlays and /run/log/galdrun-client.log");
        }
        text.clear();
        if (!fileutil::readTextFile("/boot/galdrun-audio.txt", text, error))
            return makeControlError(request, control_error::kFailed, error);
        std::string selected = "usb";
        for (size_t i = 0; i < profiles.size(); ++i)
        {
            std::string id;
            json::getString(profiles.at(i), "id", id);
            if (text.find("# Galdrun audio profile: " + id + "\n") != std::string::npos) selected = id;
        }
        std::string bootProfile;
        const bool captured = fileutil::readTextFile("/run/galdrun-audio-boot.txt", bootProfile, error);
        response["supported"] = json::Value(true);
        response["profiles"] = profiles;
        response["profile"] = json::Value(selected);
        response["rebootRequired"] = json::Value(!captured || bootProfile != text);
        if (request.command == ControlCommand::SetAudioProfile && (!captured || bootProfile != text))
        {
            if (!requestRestart(runtime_, error))
                return makeControlError(request, control_error::kFailed, "Profile saved, but " + error);
            response["restarting"] = json::Value(true);
        }
#else
        if (request.command == ControlCommand::SetAudioProfile)
            return makeControlError(request, control_error::kUnavailable, "Audio profiles are unavailable");
#endif
        return response;
    }
    case ControlCommand::GetDac:
    case ControlCommand::SetDac:
    case ControlCommand::ResetDac:
    {
        auto response = makeControlSuccess(request);
#if defined(__linux__) && !defined(ESP_PLATFORM)
        const bool supported = dac_settings::supported();
        response["supported"] = json::Value(supported);
        if (supported)
        {
            json::Value changes = request.params;
            if (request.command != ControlCommand::GetDac &&
                board_config::enabled(board_config::current(), "selectedCardDacControls"))
            {
                std::string expectedCard;
                const std::string prefix = "hw:CARD=";
                const auto comma = runtime_.deviceId.find(',');
                const std::string selectedCard = runtime_.deviceId.compare(0, prefix.size(), prefix) == 0 ?
                    runtime_.deviceId.substr(prefix.size(), comma - prefix.size()) : std::string();
                if (!json::getString(request.params, "card", expectedCard) || expectedCard != selectedCard ||
                    request.params.members().size() != (request.command == ControlCommand::SetDac ? 2u : 1u))
                    return makeControlError(request, control_error::kInvalidArgument, "DAC card changed; reload its settings");
                if (request.command == ControlCommand::SetDac)
                {
                    const auto* controls = request.params.find("controls");
                    if (controls == nullptr || !controls->isObject())
                        return makeControlError(request, control_error::kInvalidArgument, "Expected DAC controls");
                    changes = *controls;
                }
            }
            // The room's volume owns the DAC's master level and switch when it
            // plays on this card, which has hardware volume: the webpage locks
            // them, and neither a change nor a reset here writes them.
            const AudioDeviceInfo* device = runtime_.deviceSelected && runtime_.deviceCache != nullptr
                                                ? runtime_.deviceCache->findById(runtime_.deviceId)
                                                : nullptr;
            const bool roomVolume = device != nullptr && device->hardwareVolume.available &&
                (device->id.find("CARD=Connector2,") != std::string::npos ||
                 board_config::enabled(board_config::current(), "selectedCardDacControls"));
            if (roomVolume && request.command == ControlCommand::SetDac && changes.isObject())
            {
                for (const auto& member : changes.members())
                    if (dac_settings::isRoomVolumeControl(member.first))
                        return makeControlError(request, control_error::kInvalidArgument,
                                                member.first + " is set by the room's volume");
            }
            json::Value settings;
            std::string error;
            const bool ok = request.command == ControlCommand::SetDac ?
                dac_settings::save(changes, settings, error) :
                request.command == ControlCommand::ResetDac ? dac_settings::reset(settings, error, roomVolume) :
                dac_settings::load(settings, error);
            if (!ok) return makeControlError(request, control_error::kFailed, error);
            response["dac"] = settings;
            response["roomVolume"] = json::Value(roomVolume);
            response["roomVolumeControls"] = json::Value::makeArray();
            if (roomVolume)
                for (size_t i = 0; i < settings["controls"].size(); ++i)
                {
                    std::string name;
                    json::getString(settings["controls"].at(i), "name", name);
                    if (dac_settings::isRoomVolumeControl(name))
                        response["roomVolumeControls"].pushBack(json::Value(name));
                }
            return response;
        }
#else
        response["supported"] = json::Value(false);
#endif
        return request.command == ControlCommand::GetDac ? response :
            makeControlError(request, control_error::kUnavailable, "DAC settings are not enabled on this firmware");
    }
    case ControlCommand::GetCpu:
    case ControlCommand::SetCpu:
    {
        json::Value response = makeControlSuccess(request);
#if defined(__linux__) && !defined(ESP_PLATFORM)
        const bool supported = cpu_settings::supported();
        response["supported"] = json::Value(supported);
        if (supported)
        {
            json::Value settings;
            std::string error;
            const bool ok = request.command == ControlCommand::SetCpu ?
                            cpu_settings::save(request.params, settings, error) :
                            cpu_settings::load(settings, error);
            if (!ok) return makeControlError(request, control_error::kFailed, error);
            response["cpu"] = settings;
            return response;
        }
#else
        response["supported"] = json::Value(false);
#endif
        return request.command == ControlCommand::GetCpu ? response :
               makeControlError(request, control_error::kUnavailable,
                                "CPU settings are managed by this device's operating system");
    }
    case ControlCommand::GetSsh:
    case ControlCommand::SetSsh:
    {
        json::Value response = makeControlSuccess(request);
#if defined(__linux__) && !defined(ESP_PLATFORM)
        const bool supported = ssh_settings::supported();
        response["supported"] = json::Value(supported);
        if (supported)
        {
            json::Value settings;
            std::string error;
            const bool ok = request.command == ControlCommand::SetSsh ?
                            ssh_settings::save(request.params, settings, error) :
                            ssh_settings::load(settings, error);
            if (!ok) return makeControlError(request, control_error::kFailed, error);
            response["ssh"] = settings;
            return response;
        }
#else
        response["supported"] = json::Value(false);
#endif
        return request.command == ControlCommand::GetSsh ? response :
               makeControlError(request, control_error::kUnavailable,
                                "SSH is managed by this device's operating system");
    }
    case ControlCommand::ScanNetworks:
    case ControlCommand::GetNetworks:
    {
#if defined(__linux__) && !defined(ESP_PLATFORM)
        if (network_settings::supported())
        {
            json::Value result;
            std::string error;
            if (!network_settings::scan(request.command == ControlCommand::ScanNetworks, result, error))
                return makeControlError(request, control_error::kFailed, error);
            json::Value response = makeControlSuccess(request);
            response["scan"] = result;
            return response;
        }
#endif
        return makeControlError(request, control_error::kUnavailable,
                                "Wi-Fi scanning is not enabled on this firmware");
    }
    case ControlCommand::GetNetwork:
    case ControlCommand::SetNetwork:
    {
        json::Value response = makeControlSuccess(request);
#if defined(__linux__) && !defined(ESP_PLATFORM)
        const bool supported = network_settings::supported();
        response["supported"] = json::Value(supported);
        if (supported)
        {
            json::Value settings;
            std::string error;
            if (request.command == ControlCommand::SetNetwork)
            {
                if (!network_settings::save(request.params, settings, error))
                    return makeControlError(request, control_error::kFailed, error);
                response["applyDelaySeconds"] = json::Value(2);
            }
            else if (!network_settings::load(settings, error))
                return makeControlError(request, control_error::kFailed, error);
            response["network"] = network_settings::publicSettings(settings);
            return response;
        }
#else
        response["supported"] = json::Value(false);
#endif
        return request.command == ControlCommand::GetNetwork ? response :
               makeControlError(request, control_error::kUnavailable,
                                "network settings are managed by this device's operating system");
    }
    case ControlCommand::Status:
        return handleStatus(request);
    case ControlCommand::AudioDevices:
        return handleAudioDevices(request);
    case ControlCommand::RescanAudio:
        return handleRescanAudio(request);
    case ControlCommand::GetServer:
        return handleGetServer(request);
    case ControlCommand::SetServer:
        return handleSetServer(request);
    case ControlCommand::GetClientId:
        return handleGetClientId(request);
    case ControlCommand::SetClientId:
        return handleSetClientId(request);
    case ControlCommand::SetName:
        return handleSetName(request);
    case ControlCommand::SetDriftCorrection:
        return handleSetDriftCorrection(request);
    case ControlCommand::SetAudioDevice:
        return handleSetAudioDevice(request);
    case ControlCommand::Inputs:
        return handleInputs(request);
    case ControlCommand::SetInputBindings:
        return handleSetInputBindings(request);
    case ControlCommand::SimulateInput:
        return handleSimulateInput(request);
    case ControlCommand::Diagnostics:
        return handleDiagnostics(request);
    }
    // Unreachable: dispatchControlLine rejects unknown commands before this.
    return makeControlError(request, control_error::kUnknownCommand, "unhandled command");
}

json::Value ClientCommandHandler::handleStatus(const ControlRequest& request) const
{
    json::Value response = makeControlSuccess(request);
    response["status"] = toJson(buildClientStatus(runtime_));
    return response;
}

// Why the client answers slowly, when it does: the main loop's steps over the
// last complete window, and on an ESP32 every FreeRTOS task with its core.
json::Value ClientCommandHandler::handleDiagnostics(const ControlRequest& request) const
{
    json::Value response = makeControlSuccess(request);
    response["loop"] =
        runtime_.loopTiming != nullptr ? runtime_.loopTiming->toJson() : json::Value::makeObject();
#if defined(ESP_PLATFORM)
    response["tasks"] = esp32TaskReport();
#endif
    // Cumulative, so a caller takes the difference of two reports.
    if (runtime_.audioServer != nullptr)
    {
        json::Value receive = json::Value::makeObject();
        receive["reads"] = json::Value(runtime_.audioServer->receiveCalls());
        receive["bytes"] = json::Value(runtime_.audioServer->receivedBytes());
        receive["readUs"] = json::Value(runtime_.audioServer->receiveNs() / 1000);
        receive["handlerUs"] = json::Value(runtime_.audioServer->handlerNs() / 1000);
        response["audioReceive"] = receive;
    }
    return response;
}

json::Value ClientCommandHandler::handleAudioDevices(const ControlRequest& request) const
{
    if (runtime_.deviceCache == nullptr)
    {
        return makeControlError(request, control_error::kUnavailable,
                                "no device cache is loaded");
    }
    json::Value response = makeControlSuccess(request);
    // The cache document verbatim, so a caller sees exactly what the client
    // reports to the server as AUDIO_CAPABILITIES.
    response["cache"] = device_cache::toJson(*runtime_.deviceCache);
    response["selectedDeviceId"] = json::Value(runtime_.deviceId);
#if defined(__linux__) && !defined(ESP_PLATFORM)
    response["requiresConfiguredDevice"] = json::Value(
        board_config::enabled(board_config::current(), "requireConfiguredAudioDevice"));
#endif
    // As client.json has it: empty is the default device, whatever was picked.
    if (runtime_.config != nullptr)
        response["configuredDeviceId"] = json::Value(runtime_.config->audioDeviceId);
    return response;
}

json::Value ClientCommandHandler::handleRescanAudio(const ControlRequest& request)
{
    if (!runtime_.rescanAudio)
    {
        return makeControlError(request, control_error::kUnavailable,
                                "this client cannot rescan audio devices");
    }

    const uint64_t started = monotonicNowNs();
    std::string error;
    if (!runtime_.rescanAudio(error))
        return makeControlError(request, control_error::kFailed, error);

    json::Value response = makeControlSuccess(request);
    response["deviceCount"] =
        json::Value(runtime_.deviceCache != nullptr
                        ? static_cast<uint32_t>(runtime_.deviceCache->devices.size())
                        : 0u);
    response["durationMs"] =
        json::Value(static_cast<uint64_t>((monotonicNowNs() - started) / 1000000ULL));
    response["deviceSelected"] = json::Value(runtime_.deviceSelected);
    if (runtime_.deviceSelected)
        response["deviceId"] = json::Value(runtime_.deviceId);
    return response;
}

json::Value ClientCommandHandler::handleGetServer(const ControlRequest& request) const
{
    if (runtime_.config == nullptr)
        return makeControlError(request, control_error::kUnavailable, "no configuration loaded");

    json::Value response = makeControlSuccess(request);
    response["server"] = json::Value(runtime_.config->server);
    response["state"] = json::Value(serverConnectionStateName(currentServerState(runtime_)));
    return response;
}

json::Value ClientCommandHandler::handleSetServer(const ControlRequest& request)
{
    if (runtime_.config == nullptr)
        return makeControlError(request, control_error::kUnavailable, "no configuration loaded");

    std::string address;
    if (!json::getString(request.params, "server", address))
    {
        return makeControlError(request, control_error::kInvalidArgument,
                                "params.server must be a string");
    }

    // An empty address clears the setting; anything else has to parse, so a
    // typo is refused here rather than failing at connect time.
    if (!address.empty())
    {
        std::string host;
        uint16_t port = 0;
        if (!parseServerAddress(address, host, port))
        {
            return makeControlError(request, control_error::kInvalidArgument,
                                    "invalid server address \"" + address + "\"");
        }
    }

    const std::string previous = runtime_.config->server;
    if (address == previous)
    {
        json::Value response = makeControlSuccess(request);
        response["server"] = json::Value(address);
        response["changed"] = json::Value(false);
        return response;
    }

    runtime_.config->server = address;

    std::string saveError;
    if (!saveConfig(runtime_, *runtime_.config, saveError))
    {
        // A setServer that cannot be persisted must not leave the running
        // client pointing somewhere the file does not agree with.
        runtime_.config->server = previous;
        return makeControlError(request, control_error::kFailed,
                                "could not write " + runtime_.configPath + ": " + saveError);
    }

    // The other half: the connection drops what it has and goes to the new
    // address, or stays idle when the address was cleared.
    if (runtime_.server != nullptr)
    {
        std::string connectError;
        // The address was validated above, so this cannot fail -- which is not
        // a reason to drop the answer on the floor if it ever does.
        if (!runtime_.server->setServer(address, connectError))
            MULTIROOM_ERROR("could not apply the new server address: %s", connectError.c_str());
    }

    MULTIROOM_INFO("server changed to %s", address.empty() ? "<not configured>" : address.c_str());

    json::Value response = makeControlSuccess(request);
    response["server"] = json::Value(address);
    response["changed"] = json::Value(true);
    return response;
}

json::Value ClientCommandHandler::handleGetClientId(const ControlRequest& request) const
{
    if (runtime_.config == nullptr)
        return makeControlError(request, control_error::kUnavailable, "no configuration loaded");

    json::Value response = makeControlSuccess(request);
    response["clientId"] = json::Value(runtime_.config->clientId);
    return response;
}

json::Value ClientCommandHandler::handleSetClientId(const ControlRequest& request)
{
    if (runtime_.config == nullptr)
        return makeControlError(request, control_error::kUnavailable, "no configuration loaded");

    std::string clientId;
    if (!json::getString(request.params, "clientId", clientId))
    {
        return makeControlError(request, control_error::kInvalidArgument,
                                "params.clientId must be a string");
    }
    // Unlike the server address, the id cannot be cleared: the server keys the
    // client on it, and startup would only replace an empty one with a new
    // random id -- a different room.
    if (!discovery::isValidNodeId(clientId))
    {
        return makeControlError(request, control_error::kInvalidArgument,
                                "invalid client id \"" + clientId +
                                    "\": 1 to 64 bytes, no control characters or "
                                    "surrounding blanks");
    }

    const std::string previous = runtime_.config->clientId;
    if (clientId == previous)
    {
        json::Value response = makeControlSuccess(request);
        response["clientId"] = json::Value(clientId);
        response["changed"] = json::Value(false);
        return response;
    }

    runtime_.config->clientId = clientId;

    std::string saveError;
    if (!saveConfig(runtime_, *runtime_.config, saveError))
    {
        runtime_.config->clientId = previous;
        return makeControlError(request, control_error::kFailed,
                                "could not write " + runtime_.configPath + ": " + saveError);
    }

    if (runtime_.session != nullptr)
        runtime_.session->setClientId(clientId);

    MULTIROOM_INFO("client id changed from %s to %s", previous.c_str(), clientId.c_str());

    json::Value response = makeControlSuccess(request);
    response["clientId"] = json::Value(clientId);
    response["changed"] = json::Value(true);
    return response;
}

// The room's name (MATP section 60): changeName's path, which a live session
// reports as ROOM_NAME. Unlike setClientId, nothing reconnects.
json::Value ClientCommandHandler::handleSetName(const ControlRequest& request)
{
    if (runtime_.config == nullptr)
        return makeControlError(request, control_error::kUnavailable, "no configuration loaded");

    std::string name;
    if (!json::getString(request.params, "name", name))
        return makeControlError(request, control_error::kInvalidArgument,
                                "params.name must be a string");

    std::string error;
    const NameChange result = changeName(runtime_, name, error);
    if (result == NameChange::Refused)
        return makeControlError(request, control_error::kInvalidArgument, error);
    if (result == NameChange::NotSaved)
        return makeControlError(request, control_error::kFailed, error);

    json::Value response = makeControlSuccess(request);
    response["name"] = json::Value(runtime_.config->name);
    response["changed"] = json::Value(result == NameChange::Changed);
    return response;
}

// The drift-correction actuator: changeDriftCorrection's path, which a live
// session reports as new capabilities and the server renegotiates from.
json::Value ClientCommandHandler::handleSetDriftCorrection(const ControlRequest& request)
{
    if (runtime_.config == nullptr)
        return makeControlError(request, control_error::kUnavailable, "no configuration loaded");

    std::string text;
    matp::DriftCorrectionMode mode = matp::DriftCorrectionMode::Resampling;
    if (!json::getString(request.params, "driftCorrection", text) ||
        !matp::parseDriftCorrectionMode(text, mode))
        return makeControlError(request, control_error::kInvalidArgument,
                                "params.driftCorrection must be resampling, hardwareClock or auto");

    std::string error;
    const DriftCorrectionChange result = changeDriftCorrection(runtime_, mode, error);
    if (result == DriftCorrectionChange::Refused)
        return makeControlError(request, control_error::kInvalidArgument, error);
    if (result == DriftCorrectionChange::NotSaved)
        return makeControlError(request, control_error::kFailed, error);

    json::Value response = makeControlSuccess(request);
    response["driftCorrection"] =
        json::Value(matp::driftCorrectionModeName(runtime_.config->driftCorrection));
    response["changed"] = json::Value(result == DriftCorrectionChange::Changed);
    return response;
}

json::Value ClientCommandHandler::handleSetAudioDevice(const ControlRequest& request)
{
    if (runtime_.config == nullptr || runtime_.deviceCache == nullptr)
        return makeControlError(request, control_error::kUnavailable, "no configuration loaded");

    std::string deviceId;
    if (!json::getString(request.params, "deviceId", deviceId))
    {
        return makeControlError(request, control_error::kInvalidArgument,
                                "params.deviceId must be a string (empty for the default)");
    }

    std::string error;
    const AudioDeviceChange result = changeAudioDevice(runtime_, deviceId, error);
    if (result == AudioDeviceChange::UnknownDevice)
        return makeControlError(request, control_error::kInvalidArgument, error);
    if (result == AudioDeviceChange::NotSaved)
        return makeControlError(request, control_error::kFailed, error);

    json::Value response = makeControlSuccess(request);
    response["deviceId"] = json::Value(deviceId);
    response["changed"] = json::Value(result == AudioDeviceChange::Changed);
    response["deviceSelected"] = json::Value(runtime_.deviceSelected);
    if (runtime_.deviceSelected)
        response["selectedDeviceId"] = json::Value(runtime_.deviceId);
    return response;
}

// The wiring as well as the bindings: this is the room's own API, and "which
// pin did I put the knob on" is a question asked at the room.
json::Value ClientCommandHandler::handleInputs(const ControlRequest& request) const
{
    if (runtime_.config == nullptr)
        return makeControlError(request, control_error::kUnavailable, "no configuration loaded");

    json::Value inputs = json::Value::makeArray();
    for (const InputDefinition& input : runtime_.config->inputs)
        inputs.pushBack(client_config::inputToJson(input, false));

    json::Value response = makeControlSuccess(request);
    response["inputs"] = inputs;
    response["deepSleep"] = json::Value(deepSleepSupported());
    if (!runtime_.inputBackend.empty())
        response["backend"] = json::Value(runtime_.inputBackend);
    if (!runtime_.inputBackendError.empty())
        response["backendError"] = json::Value(runtime_.inputBackendError);
    return response;
}

json::Value ClientCommandHandler::handleSetInputBindings(const ControlRequest& request)
{
    if (runtime_.config == nullptr)
        return makeControlError(request, control_error::kUnavailable, "no configuration loaded");

    std::string inputId;
    const json::Value* bindings = request.params.find("bindings");
    if (!json::getString(request.params, "id", inputId) || bindings == nullptr)
    {
        return makeControlError(request, control_error::kInvalidArgument,
                                "params needs id (a string) and bindings (an object)");
    }

    std::string error;
    const InputBindingChange result = changeInputBindings(runtime_, inputId, *bindings, error);
    if (result == InputBindingChange::UnknownInput || result == InputBindingChange::Invalid)
        return makeControlError(request, control_error::kInvalidArgument, error);
    if (result == InputBindingChange::NotSaved)
        return makeControlError(request, control_error::kFailed, error);

    for (const InputDefinition& input : runtime_.config->inputs)
    {
        if (input.control.id != inputId)
            continue;
        json::Value response = makeControlSuccess(request);
        response["id"] = json::Value(inputId);
        response["changed"] = json::Value(result == InputBindingChange::Changed);
        response["bindings"] =
            matp::inputBindingsToJson(input.control.kind, input.control.bindings);
        return response;
    }
    return makeControlError(request, control_error::kFailed, "the input disappeared");
}

// A gesture without the hardware: resolved through the same bindings and sent
// through the same session as a real one, which is how the server's half is
// exercised on a desktop.
json::Value ClientCommandHandler::handleSimulateInput(const ControlRequest& request)
{
    if (runtime_.config == nullptr)
        return makeControlError(request, control_error::kUnavailable, "no configuration loaded");

    std::string inputId;
    std::string gestureName;
    matp::InputGesture gesture = matp::InputGesture::Click;
    int64_t count = 1;
    if (!json::getString(request.params, "id", inputId) ||
        !json::getString(request.params, "gesture", gestureName) ||
        !matp::parseInputGesture(gestureName, gesture) ||
        (request.params.find("count") != nullptr && !json::getInt(request.params, "count", count)))
    {
        return makeControlError(request, control_error::kInvalidArgument,
                                "params needs id, gesture (click, doubleClick, longPress, "
                                "clockwise, counterClockwise) and optionally count");
    }

    matp::InputActionEvent event;
    std::string error;
    if (count < 1 || count > matp::kMaxInputActionCount ||
        !resolveInputAction(runtime_.config->inputs, inputId, gesture,
                            static_cast<uint32_t>(count), event, error))
    {
        return makeControlError(request, control_error::kInvalidArgument,
                                error.empty() ? "count out of range" : error);
    }
    if (!dispatchInputAction(event, error))
        return makeControlError(request, control_error::kUnavailable, error);

    json::Value response = makeControlSuccess(request);
    response["action"] = json::Value(matp::inputActionName(event.binding.action));
    response["count"] = json::Value(event.count);
    return response;
}

} // namespace multiroom
