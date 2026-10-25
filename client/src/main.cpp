#include "app/ClientCommandHandler.h"
#include "app/CommandLine.h"
#include "app/DeviceSelection.h"
#include "app/LoopTiming.h"
#include "app/InputPump.h"
#include "app/SessionHooks.h"
#include "app/StreamPlayback.h"
#include "app/TestTone.h"
#include "app/Version.h"
#include "audio/AudioDeviceCache.h"
#include "config/ClientConfig.h"
#if defined(__linux__) && !defined(ESP_PLATFORM)
#include "config/CpuSettings.h"
#include "config/DacSettings.h"
#endif
#include "control/CommandQueue.h"
#include "control/ControlServer.h"
#include "matp/ClientBeacon.h"
#include "matp/ClientIdentity.h"
#include "matp/Protocol.h"
#include "matp/Session.h"
#include "discovery/BeaconSender.h"
#include "discovery/NodeId.h"
#include "input/InputBackend.h"
#include "net/ServerConnection.h"
#include "util/Logger.h"
#include "util/Time.h"

#include <csignal>
#include <cstdio>
#include <string>

using namespace multiroom;

namespace {

// Written from a signal handler, so nothing richer than sig_atomic_t: the
// handler may run on any thread and must touch nothing that allocates or
// locks.
volatile std::sig_atomic_t g_shutdownRequested = 0;

#if !defined(ESP_PLATFORM)
extern "C" void handleTerminationSignal(int)
{
    g_shutdownRequested = 1;
}
#endif

// How long the main loop blocks waiting for control commands. It bounds both
// the shutdown latency and nothing else -- a queued command wakes the loop
// immediately.
const uint32_t kIdleWaitMs = 200;

// The command handler, with what it takes counted as the loop's Commands step.
class TimedHandler final : public IControlHandler
{
public:
    TimedHandler(IControlHandler& inner, LoopTiming& timing) : inner_(inner), timing_(timing) {}

    json::Value handle(const ControlRequest& request) override
    {
        const uint64_t startNs = monotonicNowNs();
        json::Value response = inner_.handle(request);
        const uint64_t tookNs = monotonicNowNs() - startNs;
        totalNs_ += tookNs;
        timing_.add(LoopTiming::Commands, tookNs);
        return response;
    }

    uint64_t totalNs() const { return totalNs_; }

private:
    IControlHandler& inner_;
    LoopTiming& timing_;
    uint64_t totalNs_ = 0;
};

// Loads client.json, creating it with defaults on first start so the user has
// a file to edit.
bool loadOrCreateConfig(const std::string& path, ClientConfig& config)
{
    std::string error;
    const client_config::LoadResult result = client_config::load(path, config, error);
    switch (result)
    {
    case client_config::LoadResult::Ok:
        MULTIROOM_INFO("config loaded from %s", path.c_str());
        return true;

    case client_config::LoadResult::Missing:
    {
        MULTIROOM_INFO("no config at %s, writing defaults", path.c_str());
        std::string saveError;
        if (!client_config::save(path, config, saveError))
            MULTIROOM_WARN("could not write default config: %s", saveError.c_str());
        return true;
    }

    case client_config::LoadResult::Invalid:
        // Refuse to run on a broken config rather than silently reverting to
        // defaults and overwriting the user's file.
        MULTIROOM_ERROR("config %s is invalid: %s", path.c_str(), error.c_str());
        return false;
    }
    return false;
}

} // namespace

#if defined(ESP_PLATFORM)
// app_main (esp32/main/app_main.cpp) brings up Wi-Fi and the flash filesystem
// first and then calls this; C++ does not allow calling main itself.
int clientMain(int argc, char** argv)
#else
int main(int argc, char** argv)
#endif
{
    CommandLineOptions options;
    std::string commandLineError;
    if (!parseCommandLine(argc, argv, options, commandLineError))
    {
        std::fprintf(stderr, "%s\n\n%s", commandLineError.c_str(), usageText());
        return 2;
    }

    if (options.helpRequested)
    {
        std::fputs(usageText(), stdout);
        return 0;
    }
    if (options.versionRequested)
    {
        std::printf(MULTIROOM_BRAND_SLUG "-client %s\n", versionString());
        return 0;
    }

    if (options.hasLogLevel)
        log::setLevel(options.logLevel);

    // Before the first line worth keeping. A file that cannot be opened is a
    // warning and not a reason to refuse to start: stderr still works, and a
    // service that would not run because its log directory is missing is worse
    // than one that runs and says so.
    if (options.hasLogFile && !log::setLogFile(options.logFile))
        MULTIROOM_WARN("cannot write the log to %s; logging to stderr only",
                 options.logFile.c_str());

#if defined(__linux__) && !defined(ESP_PLATFORM)
    if (cpu_settings::supported())
    {
        std::string error;
        if (!cpu_settings::restore(error)) MULTIROOM_WARN("cannot restore CPU setting: %s", error.c_str());
    }
    if (dac_settings::supported())
    {
        std::string error;
        if (!dac_settings::restore(error)) MULTIROOM_WARN("cannot restore DAC settings: %s", error.c_str());
    }
#endif

    // Before the config load and the device scan, which can take seconds: the
    // default action would end the process there without flushing the log.
    // A Ctrl-C in that window lets startup finish and the main loop exits at
    // once.
    // The ESP32 has no process signals; it runs until it is reset.
#if !defined(ESP_PLATFORM)
    std::signal(SIGINT, handleTerminationSignal);
    std::signal(SIGTERM, handleTerminationSignal);
#endif

    MULTIROOM_INFO(MULTIROOM_BRAND_SLUG "-client %s starting", versionString());
    const uint64_t startedAtNs = monotonicNowNs();

    const std::string configPath =
        options.configPath.empty() ? client_config::defaultPath() : options.configPath;

    ClientConfig config;
    if (!loadOrCreateConfig(configPath, config))
        return 1;

    // Before the command line is applied, so that only the identity is
    // written back and not this run's overrides. The server keys this
    // client's stream on the id and finds it by it, so it has to survive the
    // restart.
    {
        const std::string previousId = config.clientId;
        if (client_config::completeIdentity(config, discovery::hostName()))
        {
            std::string saveError;
            if (!client_config::save(configPath, config, saveError))
                MULTIROOM_WARN("client id %s could not be saved, the next start will pick another: %s",
                         config.clientId.c_str(), saveError.c_str());
            else if (previousId.empty())
                MULTIROOM_INFO("no client id configured; generated %s, named \"%s\"",
                         config.clientId.c_str(), config.name.c_str());
            else
                MULTIROOM_INFO("the room is named \"%s\" now, its old id; its new id is %s, so "
                         "the server sees it as a new room once",
                         config.name.c_str(), config.clientId.c_str());
        }
    }

    const ClientConfig storedConfig = config;
    applyCommandLine(options, config);
    // The command line wins over the config file for this run.
    log::setLevel(options.hasLogLevel ? options.logLevel : config.logLevel);

    const std::string cachePath =
        options.audioCachePath.empty() ? device_cache::defaultPath() : options.audioCachePath;

    AudioDeviceCacheData cache;
    bool rescanNeeded = options.rescanAudio;
    if (options.rescanAudio)
    {
        MULTIROOM_INFO("audio rescan requested on the command line");
    }
    else
    {
        std::string cacheError;
        const device_cache::LoadResult result = device_cache::load(cachePath, cache, cacheError);
        if (result == device_cache::LoadResult::Ok && cache.devices.empty())
        {
            // Nothing to lose by scanning again, and a device plugged in since
            // the last scan would otherwise stay unseen until a manual rescan.
            MULTIROOM_INFO("audio cache %s lists no device, rescan required", cachePath.c_str());
            rescanNeeded = true;
        }
        else if (result == device_cache::LoadResult::Ok)
        {
            MULTIROOM_INFO("audio cache loaded from %s", cachePath.c_str());
        }
        else
        {
            MULTIROOM_INFO("audio cache %s unusable (%s: %s), rescan required", cachePath.c_str(),
                     device_cache::loadResultName(result), cacheError.c_str());
            rescanNeeded = true;
        }
    }

    if (rescanNeeded)
    {
        std::string scanError;
        if (!rescanAudioDevices(cachePath, config.i2s, cache, scanError))
            return 1;
    }

    logDeviceSummary(cache);

    MULTIROOM_INFO("server: %s", config.server.empty() ? "<not configured>" : config.server.c_str());
    if (!config.active)
        MULTIROOM_INFO("deactivated: announcing disabled status, no server connection");

    matp::Identity identity;
    identity.clientId = config.clientId;
    identity.clientVersion = versionString();
    identity.platform = matp::platformName();
    MULTIROOM_INFO("client id: %s (%s)", identity.clientId.c_str(), identity.platform.c_str());

    // Runs on its own thread and reconnects on its own; nothing below waits
    // for it. The MATP handshake on top of it is driven from the main loop.
    ServerConnection connection;
    std::string connectionError;
    // A deactivated client keeps its address but connects nowhere.
    if (!connection.setServer(config.active ? config.server : std::string(), connectionError))
    {
        // The config and the command line both validate the address, so this
        // is a contradiction rather than a user error.
        MULTIROOM_ERROR("%s", connectionError.c_str());
        return 1;
    }

    // Everything the control commands read or change lives here, owned by this
    // thread alone.
    ClientRuntime runtime;
    runtime.config = &config;
    runtime.configPath = configPath;
    runtime.commandLine = options;
    runtime.storedConfig = storedConfig;
    runtime.deviceCache = &cache;
    runtime.server = &connection;
    runtime.startedAtNs = startedAtNs;
    runtime.rescanAudio = [&cachePath, &runtime](std::string& scanError) {
        if (!rescanAudioDevices(cachePath, runtime.config->i2s, *runtime.deviceCache, scanError))
            return false;
        applyDeviceSelection(runtime);
        if (runtime.playback != nullptr)
            runtime.playback->updateVolumePath();
        // A rescan may have changed the device or its format list, and the
        // server has to re-negotiate this client's stream when it does
        // (MATP section 12).
        if (runtime.session != nullptr)
        {
            runtime.session->capabilitiesChanged();
            runtime.session->audioDevicesChanged();
        }
        return true;
    };

    applyDeviceSelection(runtime);

    matp::Session session(connection, identity,
                          [&runtime](matp::CapabilityReport& report, std::string& error) {
                              return currentCapabilities(runtime, report, error);
                          });
    runtime.session = &session;
    connectSessionHooks(session, runtime);

    ClientCommandHandler handler(runtime);
    LoopTiming loopTiming;
    runtime.loopTiming = &loopTiming;
    CommandQueue queue;
    ControlServer control;

    std::string controlError;
    if (!control.start(config.controlBindAddress, config.controlPort, queue, controlError))
    {
        // Almost always a second instance on the same port. Failing here is
        // better than running a client nobody can talk to.
        MULTIROOM_ERROR("%s", controlError.c_str());
        return 1;
    }

    // The connection thread does nothing with the bytes but buffer them; they
    // are decoded and answered from the loop below, on this thread.
    if (!connection.start(
            [&session](const void* data, size_t size) { session.onData(data, size); },
            connectionError))
    {
        MULTIROOM_ERROR("%s", connectionError.c_str());
        return 1;
    }

    int exitCode = 0;
    if (options.testToneSeconds > 0)
    {
        if (!runtime.formatSelected)
        {
            MULTIROOM_ERROR("cannot play a test tone without a usable device and format");
            exitCode = 1;
        }
        else if (!playTestTone(runtime, queue, handler, options.testToneSeconds,
                               options.testToneBuffered, g_shutdownRequested))
        {
            exitCode = 1;
        }
    }
    else
    {
        // The audio connection and the device the server's stream is prepared
        // on. Both stay idle until PREPARE_STREAM asks for them.
        StreamPlayback playback(runtime, identity, config.bufferMs);
        runtime.audioServer = &playback.connection();
        runtime.audioStream = &playback.stream();
        runtime.playback = &playback;
        connectStreamHooks(session, playback);

        // Not fatal: a client with a configured server does not need to be
        // found, and one that cannot open a UDP socket still plays.
        discovery::BeaconSender beacon;
        std::string beaconError;
        if (!beacon.start(matp::kDiscoveryPort, beaconError, std::string(),
                          [](bool warning, const std::string& message) {
                              if (warning)
                                  MULTIROOM_WARN("%s", message.c_str());
                              else
                                  MULTIROOM_INFO("%s", message.c_str());
                          }))
            MULTIROOM_WARN("no discovery beacon: %s", beaconError.c_str());

        // Buttons and encoders. Discovered before the pump takes the list,
        // whose length must not change after.
        appendDiscoveredInputs(config.inputs);
        InputPump inputs(config.inputs);
        inputs.start(runtime, control, queue, config.inputGpioChip);

        std::string audioError;
        if (!playback.start(audioError))
        {
            MULTIROOM_ERROR("%s", audioError.c_str());
            exitCode = 1;
        }
        else
        {
            MULTIROOM_INFO("running; control commands on %s:%u, Ctrl-C to stop",
                     config.controlBindAddress.c_str(),
                     static_cast<unsigned>(control.boundPort()));

            matp::ClientBeacon beaconInfo;
            beaconInfo.clientVersion = versionString();
            beaconInfo.platform = identity.platform;
            beaconInfo.host = matp::deviceName();
            beaconInfo.controlPort = control.boundPort();

            // Both connection threads run beside this loop and neither decides
            // anything: this thread drives the handshake, binds the audio
            // connection to the session, and serves the control API.
            // Each step timed for `diagnostics`: every command waits for this
            // loop, so its slowest step is the slowest answer.
            TimedHandler timedHandler(handler, loopTiming);
            uint64_t stepStartNs = monotonicNowNs();
            const auto step = [&](LoopTiming::Step which) {
                const uint64_t now = monotonicNowNs();
                loopTiming.add(which, now - stepStartNs);
                stepStartNs = now;
            };

            while (g_shutdownRequested == 0)
            {
                const uint64_t nowNs = monotonicNowNs();
                stepStartNs = nowNs;
                sampleSystem(runtime, nowNs);
                step(LoopTiming::Sample);
                session.poll(nowNs);
                step(LoopTiming::Session);
                playback.poll();
                handler.pollPower(nowNs);
                step(LoopTiming::Playback);
                beaconInfo.clientId = config.clientId; // setClientId changes it
                beaconInfo.name = config.name;         // setName this
                beaconInfo.server = config.server;     // and setServer this
                beaconInfo.active = config.active;
                beacon.poll(nowNs, [&]() { return matp::makeClientBeacon(beaconInfo); });
                step(LoopTiming::Beacon);

                inputs.poll(nowNs, handler);
                step(LoopTiming::Inputs);

                // Sooner than the idle timeout when a gesture is about to be
                // decided -- a click waits for its double-click window to
                // close.
                // What the commands took is counted by timedHandler; the rest
                // of the drain is waiting.
                const uint64_t commandsBefore = timedHandler.totalNs();
                queue.drain(timedHandler, playback.waitMs(inputs.waitMs(monotonicNowNs(), kIdleWaitMs)));
                const uint64_t drained = monotonicNowNs() - stepStartNs;
                const uint64_t commands = timedHandler.totalNs() - commandsBefore;
                loopTiming.add(LoopTiming::Wait, drained > commands ? drained - commands : 0);
                loopTiming.endPass(monotonicNowNs());
            }

            MULTIROOM_INFO("shutdown requested");
        }

        // Before the runtime stops pointing at it: the status command reads
        // both through the runtime, and this thread is the only one that does.
        runtime.playback = nullptr;
        runtime.audioStream = nullptr;
        runtime.audioServer = nullptr;
    }

    // The queue is shut down before the server: a control thread blocked in
    // handle() has to be released before stop() can join it.
    queue.shutdown();
    control.stop();
    connection.stop();

    log::closeLogFile();
    return exitCode;
}
