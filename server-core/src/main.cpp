// Startup order, the run loop and the shutdown signal. Nothing that needs a
// test lives here -- everything below main() is in the library, so the test
// binary can reach it.
//
// SHUTDOWN ORDER, stated here and nowhere else:
//
//     1. the control API      -- so nothing new is accepted or started
//     2. the zones            -- each stops its clients, then its pipeline
//     3. the listeners        -- control and audio
//     4. the logger           -- last, so every step above can still report
//
// Phase 8 added step 3's control listener, phase 9 the audio one beside it,
// phase 10 step 2, and phase 17 step 1: the line-based JSON API the high-end
// server drives. With it went the one zone the config file used to name --
// zones, sources and which room is in which are now the caller's to say, and
// the hooks that joined a session to a zone moved into `ControlApi`, because
// which zone a room belongs in is now the API's answer.
//
// Phase 11 added the one thing that runs the other way. Every other seam here
// carries an event from a session to a zone; START_AT carries a decision from
// a zone's pipeline thread back to a session, and the sessions belong to the
// listener's thread. `ControlServer::send` is that crossing -- phase 14 gives
// it five more message types, phase 17 the re-negotiation -- and the atomic
// pointer below is only how something created before the listener reaches it
// afterwards.
//
// Phase 29 moved all of it into runServer(), so that `--service` runs the same
// startup and the same shutdown order on the Service Control Manager's thread,
// and the SCM's stop sets the same flag a signal does.

#include "app/CommandLine.h"
#include "app/WindowsService.h"
#include "audio/BlockPool.h"
#include "audio/NativePcmStore.h"
#include "client/OutputPlanner.h"
#include "config/ServerConfig.h"
#include "control/ControlApi.h"
#include "control/ControlListener.h"
#include "control/RoomDirectory.h"
#include "decode/DecoderFactory.h"
#include "discovery/DiscoveredClients.h"
#include "discovery/BeaconSender.h"
#include "discovery/DiscoveryListener.h"
#include "discovery/NodeId.h"
#include "matp/AudioServer.h"
#include "matp/CoreBeacon.h"
#include "matp/ControlServer.h"
#include "resampler/BuiltinResamplers.h"
#include "resampler/ResamplerRegistry.h"
#include "util/Clock.h"
#include "util/Logger.h"
#include "zone/Zone.h"
#include "zone/ZoneManager.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace multiroom;

// The only thing a signal handler may touch.
volatile std::sig_atomic_t g_stopRequested = 0;

extern "C" void onStopSignal(int)
{
    g_stopRequested = 1;
}

// Which resampler every resampled room in this server is converted by.
//
// An empty `audio.resampler` means "whichever this build has": the two build
// options decide what is registered, and a build with neither registers
// nothing and is a server that can still serve every room whose format
// matches its zone's. A name that is not in the build is refused by the
// planner rather than at the first room that needs converting.
//
// Where a build has both, **a converter that can vary its ratio wins**.
// Registration order is alphabetical, and letting that decide would make
// `r8brain` the default and turn drift correction off for every room in the
// house — a choice with real consequences from phase 13 onwards, made by a
// letter. A deployment that wants the fixed-ratio one names it.
std::string resolveResamplerId(const ResamplerRegistry& registry, const std::string& configured)
{
    if (!configured.empty())
        return configured;

    const std::vector<std::string> available = registry.ids();
    for (const std::string& id : available)
    {
        const auto capabilities = registry.capabilities(id);
        if (capabilities && capabilities->variableRatio)
            return id;
    }
    return available.empty() ? std::string() : available.front();
}

// Everything from reading the config to the end of the shutdown order. It is
// main()'s body, and under `--service` the service thread's (phase 29): the
// startup and the shutdown are the same, and so is the flag that ends it.
int runServer(const CommandLineOptions& options, bool asService);

} // namespace

int main(int argc, char** argv)
{
    using namespace multiroom;

    auto options = parseCommandLine(argc, argv);
    if (!options)
    {
        std::fprintf(stderr, "%s\n\n%s", describe(options.error()).c_str(),
                     commandLineUsage().c_str());
        return 2;
    }

    if (options->showHelp)
    {
        std::fputs(commandLineUsage().c_str(), stdout);
        return 0;
    }
    if (options->showVersion)
    {
        std::printf(MULTIROOM_BRAND_SLUG "-core %s\n", MULTIROOM_SERVER_VERSION);
        return 0;
    }

#if defined(_WIN32)
    if (options->service)
        return service::runAsWindowsService([&options] { return runServer(*options, true); },
                                            [] { g_stopRequested = 1; });
#endif
    return runServer(*options, false);
}

namespace {

int runServer(const CommandLineOptions& options, bool asService)
{
    using namespace multiroom;

    // The command line is applied before the config is read, so a problem with
    // reading the config is itself visible at the requested level.
    if (options.logLevel)
        log::setLevel(*options.logLevel);
    if (options.logFile && !log::setLogFile(*options.logFile))
    {
        std::fprintf(stderr, "cannot open log file %s\n", options.logFile->c_str());
        return 2;
    }

    const std::filesystem::path configPath =
        options.configPath.empty() ? server_config::defaultPath() : options.configPath;

    ServerConfig config;
    if (auto loaded = server_config::load(configPath); loaded)
    {
        config = *loaded;
    }
    else if (loaded.error().code == ConfigError::Missing)
    {
        // Not an error: a first start has no file yet, and the defaults are a
        // working server.
        MULTIROOM_INFO("no config at {}, using defaults", configPath.string());
    }
    else
    {
        // A config that exists and is wrong is fatal. Starting on the defaults
        // instead would run a deployment on settings nobody chose.
        std::fprintf(stderr, "%s: %s\n", configPath.string().c_str(),
                     describe(loaded.error()).c_str());
        return 2;
    }

    // Before the command line is applied, so that only the id is written back
    // and not this run's overrides. The high-end server stores the core it
    // uses by this id, so it has to survive the restart. Logged once the log
    // is open.
    std::string generatedId;
    std::string generatedIdSaveError;
    if (config.coreId.empty())
    {
        config.coreId = discovery::generateNodeId(MULTIROOM_BRAND_SLUG "-core", discovery::hostName());
        generatedId = config.coreId;
        if (auto saved = server_config::save(configPath, config); !saved)
            generatedIdSaveError = describe(saved.error());
    }

    // The command line wins over the file, and still is not written back.
    if (options.logLevel)
        config.logLevel = *options.logLevel;
    log::setLevel(config.logLevel);
    log::setMaxFileBytes(config.logMaxBytes);
    const std::filesystem::path logFile = server_config::logFilePath(config, configPath);
    if (!options.logFile && !logFile.empty() && !log::setLogFile(logFile.string()))
    {
        std::fprintf(stderr, "cannot open log file %s\n", logFile.string().c_str());
        return 2;
    }

    MULTIROOM_INFO(MULTIROOM_BRAND_SLUG "-core {} starting{}", MULTIROOM_SERVER_VERSION,
             asService ? " as a Windows service" : "");
    MULTIROOM_INFO("config {}", configPath.string());
    if (!generatedId.empty() && generatedIdSaveError.empty())
        MULTIROOM_INFO("no core id configured; generated {}", generatedId);
    else if (!generatedId.empty())
        MULTIROOM_WARN("generated core id {} could not be saved, the next start will pick another: {}",
                 generatedId, generatedIdSaveError);

    // Phase 42: one ceiling for every zone's audio, and no memory set aside
    // for it. Each zone allocates its own blocks while it plays; this counts
    // them.
    constexpr uint64_t kMb = 1024ULL * 1024ULL;
    const size_t blockBytes = blockBytesFor(config.audio.maxSampleRate, config.audio.maxChannels,
                                            config.audio.packetDurationMs);
    AudioMemory memory(blockBytes, static_cast<uint64_t>(config.audio.memoryLimitMb) * kMb);

    // The audio memory ceiling is a number in the log, not a hope.
    {
        AudioFormat cd;
        cd.sampleRate = 44100;
        cd.channels = 2;
        cd.sampleType = AudioFormat::SampleType::Int16;
        const uint64_t limit = memory.limitBytes();
        const uint64_t ceilingZone =
            blocksFor(blockBytes, config.audio.largestFormat(), NativePcmStore::kDefaultRetentionNs) *
            blockBytes;
        const uint64_t cdZone =
            blocksFor(blockBytes, cd, NativePcmStore::kDefaultRetentionNs) * blockBytes;
        MULTIROOM_INFO("audio memory {} MB: {} zones at {}, {} at {}; blocks of {} bytes, {} ms each at "
                 "the ceiling",
                 config.audio.memoryLimitMb, ceilingZone == 0 ? 0 : limit / ceilingZone,
                 describeFormat(config.audio.largestFormat()), cdZone == 0 ? 0 : limit / cdZone,
                 describeFormat(cd), blockBytes, config.audio.packetDurationMs);
    }

    ResamplerRegistry resamplers;
    if (const auto registered = registerBuiltinResamplers(resamplers); !registered)
    {
        MULTIROOM_ERROR("resamplers: {}", describe(registered.error()));
        log::closeLogFile();
        return 1;
    }

    // One planner for the whole server, and the line it logs about the
    // resampler is phase 6's startup line. resampler.configure swaps it.
    const std::string resamplerId = resolveResamplerId(resamplers, config.audio.resampler);
    std::optional<SharedPlanner> planner;
    if (resamplerId.empty())
    {
        // A build with both resampler options off. Not a broken server: it
        // serves every room whose device can play its zone's format exactly,
        // and refuses by name any room that would need converting.
        planner.emplace(OutputPlanner::withoutResampler());
    }
    else
    {
        auto created = OutputPlanner::create(resamplers, resamplerId);
        if (!created)
        {
            MULTIROOM_ERROR("audio.resampler: {}", describe(created.error()));
                log::closeLogFile();
            return 1;
        }
        planner.emplace(std::move(*created));
    }

    const SystemClock clock;
    const uint64_t startedAtNs = clock.monotonicNowNs();

    MediaFetchSettings fetch;
    fetch.stallTimeoutMs = config.media.stallTimeoutMs;
    fetch.retries = config.media.retries;
    fetch.token = config.media.token;
    std::unique_ptr<IDecoderFactory> decoders = makeMediaDecoderFactory(clock, fetch);

    // The MATP control listener does not exist yet: it is started after
    // everything a session's hooks reach, because a room that connected first
    // would be answered by nothing. So a zone's messages, and the control
    // API's re-negotiations, go through a sender that reads the listener once
    // there is one -- and before there is, there is no room connected for a
    // message to be sent to.
    std::atomic<ControlServer*> roomListener{nullptr};
    const auto sendToRoom = [&roomListener](const RoomCommand& command) {
        ControlServer* listener = roomListener.load();
        return listener != nullptr && listener->send(command);
    };

    // Phase 24's events go the same way round: the zones and the MATP hooks
    // produce them, and the control API's listener -- started last -- is what
    // holds the connections that asked for them. Before it exists nobody can
    // have subscribed, so an event with nowhere to go is one nobody is owed.
    // A mutex rather than an atomic: the producers keep running through step
    // 1 of the shutdown, and one half-way into publish() must be finished
    // before the listener is destroyed. publish() never waits, so neither does
    // anything holding this.
    std::mutex eventMutex;
    ControlListener* eventListener = nullptr; // guarded by eventMutex
    const auto publishEvent = [&eventMutex, &eventListener](const std::string& line) {
        std::lock_guard<std::mutex> lock(eventMutex);
        if (eventListener != nullptr)
            eventListener->publish(line);
    };

    // What every room is planned with until the control API says otherwise
    // for that room.
    RoomDirectory rooms(RoomAudioSettings{config.audio.audioControl});

    ZoneSettings zoneSettings;
    zoneSettings.output.packetDurationMs = config.audio.packetDurationMs;
    zoneSettings.output.targetBufferNs =
        static_cast<uint64_t>(config.audio.targetBufferMs) * 1000000ULL;
    zoneSettings.audioControl = config.audio.audioControl;
    zoneSettings.dither = config.audio.dither;
    zoneSettings.roomPreference = [&rooms](const ClientId& client) {
        return rooms.preference(client);
    };
    zoneSettings.roomEqualizer = [&rooms](const ClientId& client) {
        const std::optional<RoomRecord> room = rooms.find(client);
        return room && room->equalizer ? *room->equalizer : EqualizerSettings{};
    };
    zoneSettings.start.minimumStartBufferNs =
        static_cast<uint64_t>(config.audio.minimumStartBufferMs) * 1000000ULL;
    zoneSettings.start.readyTimeoutNs =
        static_cast<uint64_t>(config.audio.clientReadyTimeoutMs) * 1000000ULL;
    zoneSettings.start.startDelayNs =
        static_cast<uint64_t>(config.audio.startDelayMs) * 1000000ULL;
    zoneSettings.seekFlushLeadNs = static_cast<uint64_t>(config.audio.seekFlushLeadMs) * 1000000ULL;
    zoneSettings.seekFillLeadNs = static_cast<uint64_t>(config.audio.seekFillLeadMs) * 1000000ULL;
    zoneSettings.sync.driftWindowNs =
        static_cast<uint64_t>(config.audio.driftWindowMs) * 1000000ULL;
    zoneSettings.sync.correctionLimitPpm = static_cast<double>(config.audio.correctionLimitPpm);
    zoneSettings.sync.convergenceLimitPpm = static_cast<double>(config.audio.convergenceLimitPpm);
    zoneSettings.sendCommand = sendToRoom;
    zoneSettings.onEvent = [&publishEvent](const ZoneEvent& event) {
        publishEvent(ControlApi::eventLine(event));
    };

    // Validated with the rest of the config, so a failure here is a config
    // that was never loaded through server_config -- still refused by name.
    auto media = MediaOrigin::parse(config.media.baseUrl);
    if (!media)
    {
        MULTIROOM_ERROR("media.baseUrl: {}", describe(media.error()));
        log::closeLogFile();
        return 1;
    }
    MULTIROOM_INFO("media only from {}, a stall given up on after {} ms, {} retries, {}",
             media->text(), fetch.stallTimeoutMs, fetch.retries,
             fetch.token.empty() ? "no token" : "with a token");
    // Phase 23: warned about rather than refused, because a closed LAN is a
    // legitimate deployment.
    if (fetch.token.empty() && !media->loopback())
        MULTIROOM_WARN("media.baseUrl {} is not on this machine and media.token is empty: anything "
                 "that can reach it can fetch what this server plays",
                 media->text());

    auto zones = std::make_unique<ZoneManager>(memory, *planner, resamplers, *decoders, clock,
                                               zoneSettings);
    // Filled by the discovery listener below, read by discovery.clients.
    DiscoveredClients discovered;
    ControlApi api(*zones, rooms, discovered, *planner, resamplers, clock, std::move(*media),
                   sendToRoom, publishEvent);
    // audio.configure writes the file as it is on disk with the change in it,
    // not this run's config: the command line's overrides stay out of it.
    api.setAudioConfig(config.audio, [&configPath](const AudioConfig& audio, std::string& error) {
        ServerConfig onDisk;
        if (std::filesystem::exists(configPath))
        {
            auto loaded = server_config::load(configPath);
            if (!loaded)
            {
                error = describe(loaded.error());
                return false;
            }
            onDisk = std::move(*loaded);
        }
        onDisk.audio.minimumStartBufferMs = audio.minimumStartBufferMs;
        onDisk.audio.clientReadyTimeoutMs = audio.clientReadyTimeoutMs;
        onDisk.audio.startDelayMs = audio.startDelayMs;
        onDisk.audio.seekFlushLeadMs = audio.seekFlushLeadMs;
        onDisk.audio.seekFillLeadMs = audio.seekFillLeadMs;
        onDisk.audio.memoryLimitMb = audio.memoryLimitMb;
        onDisk.audio.resampler = audio.resampler;
        if (auto valid = server_config::validate(onDisk); !valid)
        {
            error = describe(valid.error());
            return false;
        }
        if (auto saved = server_config::save(configPath, onDisk); !saved)
        {
            error = describe(saved.error());
            return false;
        }
        return true;
    });

    // Section 20's barrier, in the log once, because "nothing is audible yet"
    // and "the server is broken" look identical from a room.
    MULTIROOM_INFO("a zone waits for every room to hold {} ms and open its device, up to {} ms, "
             "then starts {} ms later",
             config.audio.minimumStartBufferMs, config.audio.clientReadyTimeoutMs,
             config.audio.startDelayMs);

    // The audio listener starts first: PREPARE_STREAM announces its port, and
    // a client that acted on the announcement before the port was bound would
    // fail its connection for no reason.
    AudioServerSettings audioSettings;
    audioSettings.port = config.network.audioPort;
    audioSettings.maxConnections = config.network.maxConnections;
    audioSettings.onAttach = [&zones](const StreamAttachment& attachment,
                                      std::unique_ptr<IPacketSink> sink) {
        const std::shared_ptr<Zone> owner = zones->zoneOf(attachment.client);
        return owner != nullptr && owner->attach(attachment, std::move(sink));
    };
    // Phase 35: a room that attached before a zone made it a member -- or
    // while it is in no zone -- is not refused while its session lives.
    audioSettings.readiness = [&zones, &rooms](const StreamAttachment& attachment) {
        if (const std::shared_ptr<Zone> owner = zones->zoneOf(attachment.client);
            owner != nullptr && owner->holds(attachment))
        {
            return AttachReadiness::Now;
        }
        const std::optional<RoomRecord> room = rooms.find(attachment.client);
        return room && room->connected && room->session == attachment.session
                   ? AttachReadiness::Later
                   : AttachReadiness::Never;
    };

    auto audio = AudioServer::start(clock, audioSettings);
    if (!audio)
    {
        MULTIROOM_ERROR("audio listener: {}", describe(audio.error()));
        zones.reset();
        log::closeLogFile();
        return 1;
    }

    ControlServerSettings listenerSettings;
    listenerSettings.port = config.network.controlPort;
    listenerSettings.maxConnections = config.network.maxConnections;
    listenerSettings.session.audioPort = (*audio)->port();
    listenerSettings.session.pingIntervalNs =
        static_cast<uint64_t>(config.network.pingIntervalMs) * 1000000ULL;
    listenerSettings.session.silenceTimeoutNs =
        static_cast<uint64_t>(config.network.silenceTimeoutMs) * 1000000ULL;
    listenerSettings.session.hooks = api.sessionHooks();

    auto control = ControlServer::start(clock, listenerSettings);
    if (!control)
    {
        // A port that is taken at startup is a configuration problem or a
        // second copy of this server, and both want an operator rather than a
        // retry loop.
        MULTIROOM_ERROR("control listener: {}", describe(control.error()));
        audio.value().reset();
        zones.reset();
        log::closeLogFile();
        return 1;
    }

    // From here a zone's pipeline thread and the control API can reach the
    // sessions, through the listener's own mailbox and on its own thread.
    roomListener.store(control->get());
    api.setRoomPort((*control)->port());

    // A taken port is fatal like the other three: a server that cannot hear
    // the beacons finds no client, and says so only by being empty.
    DiscoveryListenerSettings discoverySettings;
    discoverySettings.port = config.network.discoveryPort;
    discoverySettings.onTick = [&api]() { api.sampleSystem(); };
    discoverySettings.onActiveChanged = [&api](const std::string& client, bool active) {
        api.clientActiveChanged(client, active);
    };
    auto discovery = DiscoveryListener::start(discoverySettings, discovered, clock);
    if (!discovery)
    {
        MULTIROOM_ERROR("discovery listener: {}", describe(discovery.error()));
        roomListener.store(nullptr);
        zones->stop();
        control.value().reset();
        audio.value().reset();
        zones.reset();
        log::closeLogFile();
        return 1;
    }

    // Last to start and first to stop: nothing can be asked of the server
    // before everything that answers is there.
    ControlListenerSettings apiSettings;
    apiSettings.bindAddress = config.control.bindAddress;
    apiSettings.port = config.control.port;
    auto apiListener = ControlListener::start(
        apiSettings,
        [&api](const std::string& line, bool& subscribed) {
            return api.handleLine(line, subscribed);
        });
    if (!apiListener)
    {
        MULTIROOM_ERROR("control API: {}", describe(apiListener.error()));
        roomListener.store(nullptr);
        zones->stop();
        control.value().reset();
        audio.value().reset();
        zones.reset();
        log::closeLogFile();
        return 1;
    }

    {
        std::lock_guard<std::mutex> lock(eventMutex);
        eventListener = apiListener->get();
    }

    // Announces this core to the high-end server, which stores the one it uses
    // by id and follows it here. A control API bound to loopback can only be
    // reached from this machine, so the beacon stays on it too rather than
    // offering the network a core it cannot connect to. Not fatal: a
    // high-end server configured with this core's address does not need to
    // find it.
    discovery::BeaconSender beacon;
    {
        const std::string& bind = config.control.bindAddress;
        const bool loopbackOnly = bind.rfind("127.", 0) == 0 || bind == "localhost";
        std::string beaconError;
        if (!beacon.start(matp::kCoreBeaconPort, beaconError,
                          loopbackOnly ? std::string("127.0.0.1") : std::string(),
                          [](bool warning, const std::string& message) {
                              if (warning)
                                  MULTIROOM_WARN("{}", message);
                              else
                                  MULTIROOM_INFO("{}", message);
                          }))
            MULTIROOM_WARN("no core beacon: {}", beaconError);
    }
    matp::CoreBeacon beaconInfo;
    beaconInfo.coreId = config.coreId;
    beaconInfo.coreVersion = MULTIROOM_SERVER_VERSION;
    beaconInfo.controlPort = (*apiListener)->port();
    const std::string beaconText = matp::makeCoreBeacon(beaconInfo);

    std::signal(SIGINT, onStopSignal);
    std::signal(SIGTERM, onStopSignal);

    MULTIROOM_INFO("running as {}; {} to stop", config.coreId,
             asService ? "the service manager's stop" : "SIGINT or SIGTERM");

    // The listeners, every zone's pipeline and every room's output have their
    // own threads, so this one only waits for the signal and sends the beacon.
    // A listener whose select() failed has stopped for good, and a server
    // that kept running without it would be reachable in part and say so
    // only in the log: the same shutdown, then a failure exit, so that the
    // service manager restarts the whole server.
    bool listenerLost = false;
    while (g_stopRequested == 0)
    {
        if (!(*audio)->listening() || !(*control)->listening() || !(*discovery)->listening() ||
            !(*apiListener)->listening())
        {
            MULTIROOM_ERROR("a listener has stopped; shutting down to be restarted");
            listenerLost = true;
            break;
        }
        beacon.poll(clock.monotonicNowNs(), [&beaconText]() { return beaconText; });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    const uint64_t upNs = clock.monotonicNowNs() - startedAtNs;
    MULTIROOM_INFO("stopping after {} s, {} control connection(s) and {} audio connection(s) served",
             upNs / kNanosecondsPerSecond, (*control)->acceptedCount(),
             (*audio)->attachedCount());

    // The shutdown order above. Step 1 is the control API, so nothing new is
    // asked for; step 2 stops every zone, which stops its rooms and then its
    // pipeline; step 3 is the two MATP listeners, whose destructors stop their
    // threads and close every connection they still hold. The sender goes
    // before step 2, so that a pipeline thread on its way out cannot reach a
    // listener that is being destroyed -- and the event sink before step 1,
    // for the same reason.
    {
        std::lock_guard<std::mutex> lock(eventMutex);
        eventListener = nullptr;
    }
    apiListener.value().reset();
    discovery.value().reset();
    roomListener.store(nullptr);
    zones->stop();
    control.value().reset();
    audio.value().reset();
    zones.reset();
    decoders.reset();

    log::closeLogFile();
    return listenerLost ? 1 : 0;
}

} // namespace
