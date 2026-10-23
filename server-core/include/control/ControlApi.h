#pragma once

// The high-end server's control API (phase 17): what it says, and what that
// does to the zones and the rooms.
//
// One compact JSON object per line in each direction, request and response,
// in the shape the client's own control API already uses:
//
//     {"command":"zone.create","params":{"zone":"living"},"id":7}
//     {"ok":true,"command":"zone.create","id":7,"zone":"living","created":true}
//     {"ok":false,"command":"zone.play","error":"notFound","message":"no zone kitchen"}
//
// `id` is optional, of any type, and echoed unchanged. `error` is one of a
// fixed set of strings a caller may switch on; `message` is for a person.
//
// Four rules carry it, and the roadmap states all four.
//
//   * **Desired state over deltas.** `zone.setClients` takes the whole
//     membership and reconciles it, and every command is idempotent: a
//     replayed one answers `ok` and changes nothing. That is what lets the
//     high-end server re-assert what it wants after either end restarts,
//     without knowing what it sent before.
//   * **Unknown fields are an error, not a shrug.** A misspelled key is
//     refused with the key named, because the caller is code and a silently
//     ignored field surfaces as an audio problem hours later.
//   * **Status takes debugging seriously**, and above all "why is this room
//     not bitperfect": the plan, its reason, the settings as stored, whether
//     correction is disabled and why, and the measured numbers behind it.
//   * **It is not MATP.** Another port, another parser, another thread. The
//     only thing crossing to the rooms is a `RoomCommand` through the same
//     mailbox a zone uses.
//
// **A room's zone and settings are asked for, not imposed.** Moving a room or
// changing how it is served changes the format its device should be open in,
// and only the client can reopen its device. So the API records what it
// wants in the `RoomDirectory`, takes the room out of any zone it no longer
// belongs in, and asks its session to re-negotiate (section 12); the room is
// admitted to the zone it belongs in when its device answers, through the
// same hook every room is admitted by. A room that is not connected is
// admitted the same way when it next is.
//
// **Volume and a forced rejoin (phase 25)** reach machinery phases 14 and 16
// built. A room's volume and mute are desired state like its settings: kept
// in the directory, sent again every time the room is admitted to a zone, and
// never applied here -- the gain is the client's (section 39). A rejoin is
// `Zone::rejoin` with `HardResync`, refused for exactly the rooms that
// refuses.
//
// **A room's output latency (phase 27)** is desired state the same way: kept
// in the directory, bounded by the prebuffer less one packet because the room
// plays that far ahead out of what it holds, and given to the zone on every
// admission. A change on a room that is playing moves it once.
//
// **Events (phase 24)** replace polling `status` for what the high-end server
// has to react to in time: the next source wanted, "now playing", a skipped
// source, the end, the transport, and the rooms coming, going and falling
// silent. A connection asks for them with `events.subscribe` and gets them as
// lines on that same connection, told apart from responses by their first
// key:
//
//     {"event":"zone.sourceNeeded","zone":"living","afterSourceId":"s1","remainingNs":180000000000}
//
// The core announces and never asks: nothing here waits for an answer, and a
// high-end server that is not listening costs a zone that ends when its
// sources do. Subscription ends with the connection; a reconnecting caller
// subscribes again and reads `status` once, because events missed while it was
// away are not replayed. A room is named by its client id, a source by the
// high-end server's own sourceId, never by a position in a list.
//
// **Threading.** `handleLine` runs on the control listener's thread, one
// request at a time. `sessionHooks()` runs on the MATP listener's. The two
// meet only in thread-safe classes -- the directory, the zone manager and the
// zones -- and the resampler is the one thing here that is not: it is
// replaced only while no zone exists, which is when nothing reads it.

#include "client/ClientSession.h"
#include "config/ServerConfig.h"
#include "client/OutputPlanner.h"
#include "control/RoomDirectory.h"
#include "discovery/DiscoveredClients.h"
#include "matp/RoomCommand.h"
#include "resampler/ResamplerRegistry.h"
#include "source/MediaOrigin.h"
#include "util/Clock.h"
#include "util/Json.h"
#include "zone/ZoneManager.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

namespace multiroom {

class ControlApi
{
public:
    // Everything here outlives the API. `planner` is the one every zone plans
    // with, held by reference by the zone manager as well -- which is why
    // `resampler.configure` assigns into it rather than replacing it.
    // `discovered` is what the beacons said, read by `discovery.clients`
    // (phase 33). `sendToRoom` is where a re-negotiation goes: the MATP listener's
    // mailbox, as for a zone's messages. `media` is the one origin
    // `source.stage` accepts a URI from (phase 20). `publishEvent` is where
    // the room events this API notices go, one whole line at a time: the
    // control listener's `publish`, which never waits (phase 24).
    ControlApi(ZoneManager& zones, RoomDirectory& rooms, const DiscoveredClients& discovered,
               OutputPlanner& planner,
               const ResamplerRegistry& registry, const IClock& clock, MediaOrigin media,
               std::function<bool(const RoomCommand&)> sendToRoom,
               std::function<void(const std::string&)> publishEvent);

    // The audio section the core started with, and how audio.configure writes
    // a change back: `persist` validates the whole config with the change in
    // it and saves it, or says why not. Without a call here audio.settings and
    // audio.configure answer `unavailable`.
    using AudioPersist = std::function<bool(const AudioConfig& audio, std::string& error)>;
    void setAudioConfig(AudioConfig audio, AudioPersist persist);

    // The MATP port a client is pointed at by `client.attach` when the caller
    // names no server. Known only once the room listener is up; until then
    // such an attach answers `unavailable`.
    void setRoomPort(uint16_t port) noexcept { roomPort_.store(port); }

    ControlApi(const ControlApi&) = delete;
    ControlApi& operator=(const ControlApi&) = delete;

    // One request line in, exactly one response line out -- newline included
    // -- for every input, garbage too. `subscribed` is the calling
    // connection's own flag, which `events.subscribe` and
    // `events.unsubscribe` set and clear.
    [[nodiscard]] std::string handleLine(const std::string& line, bool& subscribed);

    // A zone's event as the line a subscriber reads, newline included. Static
    // because the zones are given their sink before this API exists.
    [[nodiscard]] static std::string eventLine(const ZoneEvent& event);

    // The five seams between a MATP session and the zones, which were
    // main.cpp's until the zone a room belongs in became the API's to say.
    // Called on the MATP listener's thread.
    [[nodiscard]] ClientSessionHooks sessionHooks();

    // This process's own figures for `server.system` (phase 34), sampled when
    // at least 2 s have passed since the last sample. Called often, from a
    // thread that holds no zone lock -- the discovery listener's -- because a
    // sample reads files on Linux.
    void sampleSystem();

    // What one command produced: its fields, or an error. Defined beside the
    // commands, which are the only things that build one.
    struct Reply;

private:
    Reply status();
    Reply clients();
    Reply zonesList();
    Reply discoveryClients();
    Reply clientAttach(const json::Value& params);
    Reply clientDetach(const json::Value& params);
    // Both: the client's own `setServer`, through its beacon's address.
    Reply pointClient(const std::string& name, const std::string& server);
    Reply clientFirmwareSettings(const json::Value& params, const std::string& command);
    // `command` sent to a discovered client with a recent beacon: its answer
    // as the body, an object with a boolean `ok`, or a failure when there is
    // no such client or no response. A refusal is an answer, mapped by the
    // caller; nothing in the body is forwarded unless the caller copies it.
    Reply callClient(const std::string& name, const std::string& command,
                     const json::Value& params);
    Reply zoneCreate(const json::Value& params);
    Reply zoneDelete(const json::Value& params);
    Reply zoneSetClients(const json::Value& params);
    Reply sourceStage(const json::Value& params);
    Reply sourceClearStaged(const json::Value& params);
    Reply zonePlay(const json::Value& params);
    Reply zonePause(const json::Value& params);
    Reply zoneStop(const json::Value& params);
    Reply zoneSeek(const json::Value& params);
    Reply clientSetGrouped(const json::Value& params);
    Reply clientSetAudioControl(const json::Value& params);
    Reply clientSetVolume(const json::Value& params);
    Reply clientRejoin(const json::Value& params);
    Reply clientSetOutputLatency(const json::Value& params);
    Reply clientSetEqualizer(const json::Value& params);
    Reply clientSetAudioDevice(const json::Value& params);
    Reply clientRescanAudio(const json::Value& params);
    Reply clientSetBuffer(const json::Value& params);
    Reply clientSetName(const json::Value& params);
    Reply clientWake(const json::Value& params);
    Reply clientForget(const json::Value& params);
    Reply clientSetVolumeControl(const json::Value& params);
    Reply clientSetDriftCorrection(const json::Value& params);
    Reply clientSetInputBindings(const json::Value& params);
    Reply resamplerConfigure(const json::Value& params);
    Reply audioSettings();
    Reply audioConfigure(const json::Value& params);

    // A stored setting changed: the room is planned again, which for a room
    // in a zone means leaving it and being re-admitted on its new answer.
    void replan(const ClientId& client);

    // Its grouping changed: a room playing in a zone takes the plan that
    // follows on the stream it has, without leaving the line, wherever its
    // device can stay as it is (Zone::replanInPlace); otherwise replan().
    void regroup(const ClientId& client);

    // Takes a connected room out of whatever zone it is in and asks it to
    // choose its format again, which puts it in the zone the directory names
    // once its device answers. `cause` is what `room.left` says, if it was in
    // one.
    void renegotiate(const RoomRecord& room, const char* cause);

    // Takes a room out of whatever zone it is in, for good, and says so as
    // `room.left` with `cause` -- silent when it was in none.
    void releaseRoom(const ClientId& client, const char* cause);

    // `room.notReady`: a room asked into `zone` that is silent, and why.
    void announceNotReady(const ClientId& client, const ZoneId& zone, const std::string& reason);

    void publish(const json::Value& event);

    [[nodiscard]] json::Value clientJson(const RoomRecord& room, uint64_t nowNs) const;
    [[nodiscard]] json::Value zoneJson(const Zone& zone) const;
    [[nodiscard]] json::Value serverJson() const;

    // Phase 42: the least `memoryLimitMb` audio.configure accepts now.
    [[nodiscard]] uint64_t memoryLimitFloorMb() const;

    ZoneManager& zones_;
    RoomDirectory& rooms_;
    const DiscoveredClients& discovered_;
    std::atomic<uint16_t> roomPort_{0};
    OutputPlanner& planner_;
    const ResamplerRegistry& registry_;
    const IClock& clock_;
    const MediaOrigin media_;
    std::function<bool(const RoomCommand&)> sendToRoom_;
    std::function<void(const std::string&)> publishEvent_;

    // audio.settings and audio.configure; the control thread's alone.
    bool hasAudio_ = false;
    AudioConfig audio_;
    AudioPersist persistAudio_;
    const uint64_t startedAtNs_;

    // The sampler belongs to whoever calls sampleSystem(); only the latest
    // sample is shared with the command thread.
    SystemSampler systemSampler_;
    uint64_t systemNextNs_ = 0;
    mutable std::mutex systemMutex_;
    bool hasSystem_ = false; // guarded by systemMutex_
    SystemMetrics system_;   // guarded by systemMutex_
};

} // namespace multiroom
