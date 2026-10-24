#pragma once

// The control commands, wired to the client's own state.
//
// Everything here runs on the thread that owns that state -- the main thread,
// draining CommandQueue -- so none of the state below needs a lock and none of
// it is ever touched from a socket thread or from the audio thread.

#include "app/CommandLine.h"
#include "audio/AudioClockSnapshot.h"
#include "audio/AudioDeviceCache.h"
#include "audio/DriftEstimator.h"
#include "audio/HardwareClock.h"
#include "audio/HardwareVolume.h"
#include "audio/TimelineSource.h"
#include "config/ClientConfig.h"
#include "control/ClientStatus.h"
#include "control/ControlProtocol.h"
#include "matp/AudioStream.h"
#include "net/ServerConnection.h"

#include <cstdint>
#include <functional>
#include <string>

namespace multiroom {

class LoopTiming;
class StreamPlayback;

struct ClientRuntime
{
    ClientConfig* config = nullptr; // never null in a running client
    std::string configPath;

    // This run's command-line overrides and the config as the file had it
    // before them, so that saving one setting does not persist an override.
    // Write the config with saveConfig, never client_config::save directly.
    CommandLineOptions commandLine;
    ClientConfig storedConfig;

    AudioDeviceCacheData* deviceCache = nullptr; // never null in a running client

    // Null in tests and in any client without a connection thread; the status
    // then falls back to what the configuration says.
    ServerConnection* server = nullptr;

    // The MATP handshake running on that connection. Null wherever the
    // connection is, and read only from this thread.
    matp::Session* session = nullptr;

    // The second connection of MATP section 3 and the stream it carries. Both
    // null in tests and in any client without a playback path; the stream
    // block is then left out of the status entirely.
    ServerConnection* audioServer = nullptr;
    const matp::AudioStream* audioStream = nullptr;

    // The playback path those two belong to, for what SET_BUFFER changes on
    // it. Null wherever they are.
    StreamPlayback* playback = nullptr;

    uint64_t startedAtNs = 0;
    bool powerIdleTimerRunning = false;
    uint64_t powerIdleSinceNs = 0;
    bool deepSleepTimerRunning = false;
    uint64_t deepSleepIdleSinceNs = 0;
    int deepSleepProcess = 0; // Linux helper; reaped if it fails before stopping us
    int restartProcess = 0; // appliance restart helper; waits for the API reply

    // Scheduled playback (MATP sections 20 and 21), published by the main
    // thread for the same reason the playback fields below are: reading it
    // here would mean a status request calling into the driver.
    TimelineState timelineState = TimelineState::Idle;
    bool startScheduled = false;
    // The zone's media timeline (MATP section 5), so nanoseconds. The two
    // frame counts beside it are this machine's own device axis, which is the
    // distinction section 33 turns on and the compiler cannot see.
    uint64_t startTimelineNs = 0;
    uint64_t startDeviceFrame = 0;
    uint32_t startInMs = 0; // until the scheduled frame, 0 once it has passed
    bool hasTimelineNsAudible = false;
    uint64_t timelineNsAudible = 0;
    uint32_t timelineUnderrunFrames = 0;
    uint32_t timelineSkippedFrames = 0;
    uint32_t timelineFlushedFrames = 0; // dropped by a FLUSH (section 54)

    // The scheduled pause or stop (sections 37 and 38), and the room's volume
    // (section 39). `appliedGain` is what the render callback last multiplied
    // by, which is how a scheduled change is seen to have taken effect.
    bool endScheduled = false;
    uint64_t endTimelineNs = 0;
    double volumeGain = 1.0;
    bool volumeMuted = false;
    bool volumeScheduled = false;
    double appliedGain = 1.0;

    // Whether the open device's own control plays that volume instead of the
    // render path (appliedGain then stays 1.0), and what the control last read
    // back as. The capability and range come from the device cache.
    bool hardwareVolumeActive = false;
    bool hasHardwareVolumeState = false;
    HardwareVolumeState hardwareVolumeState;

    // The room's Audio Control as the last SESSION_CONFIG said (MATP section
    // 59), BitPerfect until one has: the reading that never scales a sample
    // it was not allowed to. And the volume path resolved from it, the
    // configured Volume Control and the device, published by StreamPlayback.
    bool audioControlKnown = false;
    matp::AudioControl audioControl = matp::AudioControl::BitPerfect;
    VolumePath volumePath;

    // The drift-correction actuator and the hardware clock, published by
    // StreamPlayback (audio/HardwareClock.h).
    DriftCorrectionState driftCorrection;

    // What is playing (sections 17 and 23). Known once the sound card has
    // reached a boundary the server announced, or as soon as a TRACK_INFO
    // names the track already running; a stream whose first boundary is still
    // ahead and that was told nothing has a timeline but no track name.
    bool trackKnown = false;
    std::string trackId;
    std::string trackTitle;
    // False for a track a TRACK_INFO named: that message carries no position,
    // and reporting 0 ns for it would put the room at the start of the
    // timeline.
    bool trackTimelineNsKnown = false;
    uint64_t trackTimelineNs = 0;
    uint32_t pendingTrackBoundaries = 0;

    // Where the main loop spends its time, for `diagnostics`; null in a
    // client that runs no loop (the test tone, the tests).
    const LoopTiming* loopTiming = nullptr;

    // Result of device selection, refreshed after every rescan.
    bool deviceSelected = false;
    std::string deviceId;
    std::string deviceName;
    bool formatSelected = false;
    AudioFormat format;
    AudioAccessMode accessMode = AudioAccessMode::Shared;

    // Playback state, published by the main thread rather than read from the
    // engine here: a status request must not turn into a driver call at an
    // arbitrary moment, and it keeps this class testable with no hardware.
    PlaybackState playbackState = PlaybackState::Idle;
    uint64_t underruns = 0;
    uint32_t bufferFrames = 0;
    AudioClockSnapshot lastClock; // quality Unavailable while nothing plays
    // What the sound card was granted, set by the supervisor's open and close
    // hooks only; invalid while it is closed.
    AudioFormat deviceFormat;

    // Why playback is not running. Empty while it is running and while nobody
    // has asked for it, which is what keeps the error out of the response.
    std::string playbackError;
    uint32_t playbackAttempts = 0;
    uint32_t playbackRecoveries = 0;
    uint32_t playbackRetryMs = 0;

    // Null while no playback is running.
    const DriftEstimator* drift = nullptr;

    // How this machine is doing (phase 20): sampled by sampleSystem() below
    // on the main thread and read by status and SYSTEM_REPORT alike. Only the
    // latest sample is kept.
    SystemSampler systemSampler;
    uint64_t systemNextNs = 0;
    bool hasSystem = false;
    SystemMetrics system;

    // What reads the buttons and encoders (phase 23): the backend's name, and
    // why it is not reading them when it is not. Both empty on a client with
    // no inputs configured.
    std::string inputBackend;
    std::string inputBackendError;

    // Runs the capability scan, writes the cache and re-selects the device.
    // Returns false and fills the message when that is not possible at all.
    std::function<bool(std::string& error)> rescanAudio;
};

// Writes config to runtime.configPath without this run's command-line overrides.
bool saveConfig(ClientRuntime& runtime, const ClientConfig& config, std::string& error);

ClientStatus buildClientStatus(const ClientRuntime& runtime);

// Mirrors a supervisor's view of playback into the runtime the control API
// reads, so a status request never turns into a driver call at an arbitrary
// moment. Both playback paths use it -- the test tone and the server's stream
// -- because the block they fill is the same one, and a second copy of it
// would be the reason the two disagree.
void publishPlayback(ClientRuntime& runtime, const PlaybackSupervisor& playback,
                     const AudioEngine& engine, uint64_t nowNs);

// Samples the machine every 2 s into the runtime. Main loop only: never the
// audio thread and never the control thread, because on Linux a sample reads
// files in /proc.
void sampleSystem(ClientRuntime& runtime, uint64_t nowNs);

// Whether this client can run the deepSleep input action.
bool deepSleepSupported();

class ClientCommandHandler : public IControlHandler
{
public:
    explicit ClientCommandHandler(ClientRuntime& runtime);

    json::Value handle(const ControlRequest& request) override;

    // Physical and simulated input share local power handling before MATP.
    bool dispatchInputAction(const matp::InputActionEvent& event, std::string& error);
    void pollPower(uint64_t nowNs);

private:
    json::Value handleStatus(const ControlRequest& request) const;
    json::Value handleAudioDevices(const ControlRequest& request) const;
    json::Value handleRescanAudio(const ControlRequest& request);
    json::Value handleGetServer(const ControlRequest& request) const;
    json::Value handleSetServer(const ControlRequest& request);
    json::Value handleGetClientId(const ControlRequest& request) const;
    json::Value handleSetClientId(const ControlRequest& request);
    json::Value handleSetName(const ControlRequest& request);
    json::Value handleSetDriftCorrection(const ControlRequest& request);
    json::Value handleSetAudioDevice(const ControlRequest& request);
    json::Value handleInputs(const ControlRequest& request) const;
    json::Value handleSetInputBindings(const ControlRequest& request);
    json::Value handleSimulateInput(const ControlRequest& request);
    json::Value handleDiagnostics(const ControlRequest& request) const;

    ClientRuntime& runtime_;
};

} // namespace multiroom
