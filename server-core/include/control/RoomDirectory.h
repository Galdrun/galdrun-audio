#pragma once

// Every room this server knows about, and what the high-end server wants for
// each of them (phase 17).
//
// Two kinds of fact live in one record, because status reports them side by
// side and the question it is asked -- "why is this room not playing, and not
// bitperfect" -- needs both:
//
//   * **what was asked for**: the zone the room belongs in, its two
//     settings, its volume and its output latency. These are the control
//     API's, they survive the room's connection going away (section 43's "same room, briefly deaf"), and
//     they are what a reconnect is admitted with;
//   * **what its connection last said**: session, device, capabilities, the
//     device's answer to PREPARE_STREAM and the last PLAYBACK_REPORT. These
//     are copied here from the MATP listener's thread as they arrive, because
//     the sessions themselves belong to that thread and are never handed out.
//
// **Bounded.** A record the control API would create is refused once the
// directory holds `maxRooms`; a record for a room that connects is always
// made, because the listener's connection ceiling bounds those already. A
// record that has nothing left in it -- no connection, no zone and no
// setting of its own -- is forgotten on the spot, so a room that connected
// once and was never configured costs nothing after it goes.
//
// **Threading.** One mutex over the map, taken for a copy or an assignment
// and never across a call out of this class.

#include "client/ClientSession.h"
#include "client/OutputPlan.h"
#include "core/Ids.h"
#include "matp/Protocol.h"
#include "processing/Equalizer.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace multiroom {

struct RoomRecord
{
    ClientId client;

    // --- asked for, by the control API --------------------------------------

    ZoneId zone; // empty: in no zone

    // Absent means the server's default from `audio` in server.json, which is
    // reported as such rather than copied in -- a room that never had a
    // setting of its own follows a changed default.
    std::optional<AudioControl> audioControl;

    // Whether the room shares its zone with another room, as the Server's
    // wanted membership says. The Server
    // owns it and sends it again after a restart, so false is "not told".
    bool grouped = false;

    // Section 39's two, as last asked for (phase 25). Absent means never
    // asked, which is the client's own default and is not sent. Kept because
    // a replayed request must change nothing and a room that comes back must
    // be put back as it was asked to be: both are sent every time the room is
    // admitted to a zone. The gain itself is the client's -- nothing here
    // touches a sample.
    std::optional<double> volume;
    std::optional<bool> muted;

    // Phase 27: how much later than its card this room is heard, through a
    // receiver or speakers with their own DSP. Absent means never asked, and
    // is the same as zero. Sent to the zone on every admission, like the two
    // above, because the zone forgets it with the room's membership.
    std::optional<uint64_t> outputLatencyMs;

    // The room's equalizer. Absent means never
    // asked, which is no equalizer. Unlike volume, this one does touch
    // samples -- on the resampled path only, never on a bitperfect stream.
    std::optional<EqualizerSettings> equalizer;

    // --- said, by its connection --------------------------------------------

    bool connected = false;
    SessionId session;
    std::string deviceName;
    std::string address; // the client's IP; kept after a disconnect as the last known

    // Section 60: the room's name, empty until it has said one. Kept after a
    // disconnect like the address, because a room that is away is still shown
    // by its name; `hasName` says this connection said it, and is dropped with
    // it like the reports below. Neither keeps a record alive.
    std::string name;
    bool hasName = false;
    bool hasCapabilities = false;
    ClientCapabilities capabilities;
    bool hasHardwareVolume = false; // unknown for a client that does not say
    matp::HardwareVolumeCapability hardwareVolume;
    // The client's drift-correction setting and its device's clock trim, as
    // reported; `capabilities.hardwareClock` is the trim in use.
    matp::DriftCorrectionMode driftCorrection = matp::DriftCorrectionMode::Resampling;
    matp::HardwareClockCapability hardwareClock;

    // Section 12's answer to PREPARE_STREAM, once there has been one.
    bool streamAnswered = false;
    bool streamReady = false;
    bool streamStandby = false;
    std::string powerState;
    std::string wakeOnLan; // with powerState "sleep", kept while it is away
    AudioFormat grantedFormat;
    std::string streamNotReadyReason;

    // Section 25, and the server instant it arrived at.
    bool hasReport = false;
    matp::PlaybackReport lastReport;
    uint64_t lastReportNs = 0;

    // Section 55 (phase 34), and the server instant it arrived at. Dropped with
    // the connection, so a figure is never older than one.
    bool hasSystem = false;
    SystemMetrics system;
    uint64_t systemNs = 0;

    // Section 56: the room's sound cards, the one it is configured for and the
    // one it plays on. Dropped with the connection like the metrics: the
    // client owns the choice and says it again on every session.
    bool hasAudioDevices = false;
    matp::AudioDeviceList audioDevices;

    // Section 58: the buffer size the room reported, dropped with the
    // connection for the same reason: the client owns it and reports it
    // again on every session.
    bool hasBuffer = false;
    uint32_t bufferMs = 0;

    // Section 59: the room's Volume Control and the path its client resolved
    // from it, dropped with the connection for the same reason: the client
    // owns the setting and reports it again on every session.
    bool hasVolumeConfig = false;
    matp::VolumeConfig volumeConfig;

    // Section 57: the room's buttons and encoders and their bindings, dropped
    // with the connection for the same reason: the client owns them.
    bool hasInputs = false;
    std::vector<matp::InputControl> inputs;
    bool deepSleep = false;
};

class RoomDirectory
{
public:
    // Sixty-four rooms is four times the member ceiling of a zone and far past
    // a house; each record is a few hundred bytes.
    static constexpr size_t kDefaultMaxRooms = 64;

    explicit RoomDirectory(RoomAudioSettings defaults, size_t maxRooms = kDefaultMaxRooms);

    RoomDirectory(const RoomDirectory&) = delete;
    RoomDirectory& operator=(const RoomDirectory&) = delete;

    // --- the MATP listener's thread ------------------------------------------

    // AUDIO_CAPABILITIES has arrived: the room has a name, a session and a
    // device, which is the first moment it can be placed anywhere. True when
    // that is a connection this record did not hold yet -- the capabilities
    // are re-sent on the same session whenever the device changes, and
    // re-asked on every re-negotiation, and neither is a room arriving.
    bool onCapabilities(const ClientSession& session);
    void onStreamReady(const ClientSession& session);
    void onPlaybackReport(const ClientId& client, const matp::PlaybackReport& report,
                          uint64_t nowNs);
    void onSystemReport(const ClientSession& session, const SystemMetrics& metrics,
                        uint64_t nowNs);
    // These three return whether the report was taken: false for a session
    // the record no longer holds, whose word is not the room's any more and
    // is not announced either.
    bool onAudioDevices(const ClientSession& session, const matp::AudioDeviceList& devices);
    bool onBufferConfig(const ClientSession& session, uint32_t bufferMs);
    bool onRoomName(const ClientSession& session, const std::string& name);
    bool onVolumeConfig(const ClientSession& session, const matp::VolumeConfig& config);
    void onInputControls(const ClientSession& session, const std::vector<matp::InputControl>& inputs,
                         bool deepSleep);

    // A session ended. Only the session the record holds is taken as the
    // room's connection going: a reconnect registers its new session before
    // the displaced one finishes closing, and that close must not mark the
    // room that has just come back as gone. True when it was taken as the
    // room's connection going.
    bool onClosed(const ClientSession& session);

    // --- the control API's thread --------------------------------------------

    // Applies `change` to the record for `client`, making one if there is
    // none. False, with nothing changed, when that would take the directory
    // past its ceiling. `change` runs under the lock and must not call back
    // into this class.
    [[nodiscard]] bool update(const ClientId& client,
                              const std::function<void(RoomRecord&)>& change);

    // Drops the record for `client` whatever it holds -- zone, settings,
    // name. False when there was none.
    bool forget(const ClientId& client);

    // --- anyone ----------------------------------------------------------------

    [[nodiscard]] std::optional<RoomRecord> find(const ClientId& client) const;

    // Every record, by client id.
    [[nodiscard]] std::vector<RoomRecord> rooms() const;

    // The zone a room belongs in; empty when none.
    [[nodiscard]] ZoneId zoneFor(const ClientId& client) const;

    // Its settings as they will be planned: its own Audio Control where it has
    // one, the server's default otherwise, and its grouping.
    [[nodiscard]] RoomAudioSettings preference(const ClientId& client) const;
    [[nodiscard]] RoomAudioSettings defaults() const noexcept { return defaults_; }

    [[nodiscard]] size_t size() const;
    [[nodiscard]] size_t maxRooms() const noexcept { return maxRooms_; }

private:
    // Forgets a record with nothing left in it. Called under mutex_.
    void pruneLocked(std::map<ClientId, RoomRecord>::iterator entry);

    const RoomAudioSettings defaults_;
    const size_t maxRooms_;

    mutable std::mutex mutex_;
    std::map<ClientId, RoomRecord> rooms_; // guarded by mutex_
};

} // namespace multiroom
