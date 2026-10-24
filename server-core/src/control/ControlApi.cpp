#include "control/ControlApi.h"

#include "discovery/BeaconSender.h"
#include "discovery/ClientControl.h"

#include "util/Logger.h"

#include <algorithm>
#include <optional>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <set>
#include <string_view>
#include <utility>
#include <vector>

namespace multiroom {
namespace {

// Error codes are part of the interface: a caller switches on them, so they
// are stable strings and `message` carries the detail. The first six are the
// client's control API's own, so one caller can drive both; `notFound` is the
// one this API adds, because a zone or a client that does not exist is the
// mistake a program driving it makes most.
constexpr const char* kInvalidJson = "invalidJson";         // the line is not a JSON object
constexpr const char* kInvalidRequest = "invalidRequest";   // an object, but not a request
constexpr const char* kUnknownCommand = "unknownCommand";   // well formed, no such command
constexpr const char* kInvalidArgument = "invalidArgument"; // a known command, bad params
constexpr const char* kNotFound = "notFound";               // no such zone
constexpr const char* kFailed = "failed";                   // understood, and it did not work
constexpr const char* kUnavailable = "unavailable";         // not possible in this state

// How often the core samples itself: the high-end server polls status on the
// same 2 s tick (its phase 18), and the rooms report at that pace too.
constexpr uint64_t kSystemSampleIntervalNs = 2ULL * 1000000000ULL;

// How long `client.attach` and `client.detach` wait for a client to accept the
// connection, and again for its answer. The control thread waits with them.
constexpr uint32_t kClientCallTimeoutMs = 2000;

constexpr uint64_t kMb = 1024ULL * 1024ULL;

// Whole MB, rounded up: a bound read off it must hold what it was read from.
uint64_t megabytesUp(uint64_t bytes)
{
    return (bytes + kMb - 1) / kMb;
}

// Half this machine's memory in whole MB (phase 42's ceiling on
// `audio.memoryLimitMb`), or nothing where the machine does not say.
std::optional<uint64_t> memoryLimitCeilingMb()
{
    SystemSampler sampler;
    const SystemMetrics machine = sampler.sample();
    if (!machine.hasSystemMemoryTotalBytes)
        return std::nullopt;
    return machine.systemMemoryTotalBytes / 2 / kMb;
}

uint64_t millisecondsOf(uint64_t ns)
{
    return ns / 1000000ULL;
}

json::Value formatJson(const AudioFormat& format)
{
    json::Value out = json::Value::makeObject();
    out["sampleRate"] = json::Value(format.sampleRate);
    out["channels"] = json::Value(static_cast<uint32_t>(format.channels));
    out["sampleType"] = json::Value(sampleTypeName(format.sampleType));
    return out;
}

// The same shape client.setEqualizer takes.
json::Value equalizerJson(const EqualizerSettings& settings)
{
    json::Value bands = json::Value::makeArray();
    for (const EqBand& band : settings.bands)
    {
        json::Value b = json::Value::makeObject();
        b["type"] = json::Value(eqFilterTypeName(band.type));
        b["frequencyHz"] = json::Value(band.frequencyHz);
        b["gainDb"] = json::Value(band.gainDb);
        b["q"] = json::Value(band.q);
        bands.pushBack(std::move(b));
    }
    json::Value out = json::Value::makeObject();
    out["enabled"] = json::Value(settings.enabled);
    out["preampDb"] = json::Value(settings.preampDb);
    out["bands"] = std::move(bands);
    return out;
}

// What a room's client reported, spelled once for `clients` and for the
// event that announces it: a reader of either sees the same object.
json::Value capabilitiesJson(const ClientCapabilities& capabilities, bool hasHardwareVolume,
                             const matp::HardwareVolumeCapability& hardwareVolume,
                             const matp::HardwareClockCapability& hardwareClock)
{
    json::Value formats = json::Value::makeArray();
    for (const AudioFormat& format : capabilities.formats)
        formats.pushBack(formatJson(format));
    json::Value out = json::Value::makeObject();
    out["formats"] = std::move(formats);
    if (capabilities.preferredSampleRate != 0)
        out["preferredSampleRate"] = json::Value(capabilities.preferredSampleRate);
    if (!capabilities.accessMode.empty())
        out["accessMode"] = json::Value(capabilities.accessMode);
    if (hasHardwareVolume)
        out["hardwareVolume"] = matp::makeHardwareVolume(hardwareVolume);
    // The device's verified clock trim, whether or not it is in use.
    if (capabilities.driftFeedback)
    {
        json::Value clock = json::Value::makeObject();
        clock["available"] = json::Value(hardwareClock.available);
        if (hardwareClock.available)
        {
            if (!hardwareClock.control.empty())
                clock["control"] = json::Value(hardwareClock.control);
            clock["minPpm"] = json::Value(hardwareClock.minPpm);
            clock["maxPpm"] = json::Value(hardwareClock.maxPpm);
        }
        out["hardwareClock"] = std::move(clock);
    }
    return out;
}

json::Value audioDevicesJson(const matp::AudioDeviceList& list)
{
    const json::Value wire = matp::makeAudioDevices({}, list);
    json::Value out = json::Value::makeObject();
    out["configured"] = json::Value(list.configuredDeviceId);
    if (!list.selectedDeviceId.empty())
        out["selected"] = json::Value(list.selectedDeviceId);
    out["devices"] = *wire.find("devices");
    return out;
}

json::Value volumeControlJson(const matp::VolumeConfig& config)
{
    json::Value out = json::Value::makeObject();
    out["configured"] = json::Value(matp::volumeControlModeName(config.volumeControl));
    out["effective"] = json::Value(matp::volumeControlModeName(config.effective));
    if (!config.reason.empty())
        out["reason"] = json::Value(config.reason);
    return out;
}

json::Value segmentJson(const SourceSegment& segment)
{
    json::Value out = json::Value::makeObject();
    out["sourceId"] = json::Value(segment.sourceId);
    out["mediaUri"] = json::Value(segment.mediaUri);
    if (!segment.trackId.empty())
        out["trackId"] = json::Value(segment.trackId);
    if (!segment.title.empty())
        out["title"] = json::Value(segment.title);
    if (segment.startOffsetNs != 0)
        out["startOffsetNs"] = json::Value(segment.startOffsetNs);
    return out;
}

json::Value idList(const std::vector<ClientId>& ids)
{
    json::Value out = json::Value::makeArray();
    for (const ClientId& id : ids)
        out.pushBack(json::Value(id.str()));
    return out;
}

// An event line's first key is `event` where a response's is `ok`, which is
// how a caller reading one connection tells the two apart.
json::Value eventHead(const char* name)
{
    json::Value out = json::Value::makeObject();
    out["event"] = json::Value(name);
    return out;
}

// Stable spellings a caller may switch on; rejoinTriggerName() is the log's.
const char* triggerKey(RejoinTrigger trigger)
{
    switch (trigger)
    {
    case RejoinTrigger::Reconnect:
        return "reconnect";
    case RejoinTrigger::Evicted:
        return "evicted";
    case RejoinTrigger::DeviceRecovery:
        return "deviceRecovery";
    case RejoinTrigger::LeftBehind:
        return "lineRanPast";
    case RejoinTrigger::HardResync:
        return "hardResync";
    }
    return "unknown";
}

// The session's close reason as `room.disconnected` reports it: the three a
// caller acts on by name, and any other -- a protocol error, a socket error --
// as its own text.
std::string disconnectReason(const std::string& why)
{
    if (why == kClosedByPeer)
        return "closed";
    if (why == kReplacedByReconnect)
        return "displaced";
    if (why.starts_with(errorName(SessionError::PeerSilent)))
        return "timeout";
    return why;
}

} // namespace

// What a command produced: the fields of a success, or an error.
struct ControlApi::Reply
{
    json::Value body = json::Value::makeObject();
    const char* error = nullptr;
    std::string message;

    static Reply failure(const char* code, std::string text)
    {
        Reply reply;
        reply.error = code;
        reply.message = std::move(text);
        return reply;
    }
};

namespace {

using Reply = ControlApi::Reply;

// A Wake-on-LAN magic packet: six 0xff, then the MAC sixteen times, to UDP
// port 9 on every interface's broadcast address -- the beacon sender's
// listing, which knows which interfaces reach the LAN.
bool sendMagicPacket(const std::string& mac, std::string& error)
{
    std::string address;
    for (size_t i = 0; i + 1 < mac.size(); i += 3)
        address += static_cast<char>(std::stoi(mac.substr(i, 2), nullptr, 16));
    std::string packet(6, static_cast<char>(0xff));
    for (int i = 0; i < 16; ++i)
        packet += address;

    discovery::BeaconSender sender;
    if (!sender.start(9, error))
        return false;
    sender.poll(0, [&packet] { return packet; });
    const bool sent = sender.sent() > 0;
    if (!sent)
        error = sender.lastError();
    sender.stop();
    return sent;
}

// Six lower-case hex pairs separated by colons, as a client reports it.
bool isMacAddress(const std::string& mac)
{
    bool valid = mac.size() == 17;
    for (size_t i = 0; valid && i < mac.size(); ++i)
        valid = i % 3 == 2 ? mac[i] == ':'
                           : (mac[i] >= '0' && mac[i] <= '9') || (mac[i] >= 'a' && mac[i] <= 'f');
    return valid;
}

// A required, non-empty string. An empty id could address nothing.
bool requireString(const json::Value& params, const char* key, std::string& out, Reply& error)
{
    const json::Value* value = params.find(key);
    if (value == nullptr || !value->isString() || value->asString().empty())
    {
        error = Reply::failure(kInvalidArgument,
                               std::string("\"") + key + "\" must be a non-empty string");
        return false;
    }
    out = value->asString();
    return true;
}

// An optional string: absent is empty, present must be a string.
bool optionalString(const json::Value& params, const char* key, std::string& out, Reply& error)
{
    const json::Value* value = params.find(key);
    if (value == nullptr)
        return true;
    if (!value->isString())
    {
        error = Reply::failure(kInvalidArgument, std::string("\"") + key + "\" must be a string");
        return false;
    }
    out = value->asString();
    return true;
}

// A position in nanoseconds: a non-negative integer, left alone when it is
// absent and not `required`.
bool nanoseconds(const json::Value& params, const char* key, bool required, uint64_t& out,
                 Reply& error)
{
    const json::Value* value = params.find(key);
    if (value == nullptr && !required)
        return true;
    if (value == nullptr || !value->isIntegral() || value->asInt() < 0)
    {
        error = Reply::failure(kInvalidArgument,
                               std::string("\"") + key + "\" must be a non-negative integer");
        return false;
    }
    out = static_cast<uint64_t>(value->asInt());
    return true;
}

// Rejects a key the command does not take, naming it. A misspelled key that
// was quietly ignored would be a setting the caller believes it made.
bool onlyKeys(const json::Value& params, std::initializer_list<std::string_view> known,
              const std::string& command, Reply& error)
{
    for (const auto& member : params.members())
    {
        if (std::find(known.begin(), known.end(), member.first) == known.end())
        {
            error = Reply::failure(kInvalidArgument, "unknown key \"" + member.first +
                                                         "\" for " + command);
            return false;
        }
    }
    return true;
}

json::Value responseHead(bool ok, const std::string& command, const json::Value& id)
{
    json::Value response = json::Value::makeObject();
    response["ok"] = json::Value(ok);
    if (!command.empty())
        response["command"] = json::Value(command);
    if (!id.isNull())
        response["id"] = id;
    return response;
}

std::string encode(const json::Value& response)
{
    // Compact, and strings are escaped, so a payload can never carry a raw
    // newline and break the framing.
    std::string line = json::serialize(response, false);
    line.push_back('\n');
    return line;
}

std::string errorLine(const std::string& command, const json::Value& id, const char* code,
                      const std::string& message)
{
    json::Value response = responseHead(false, command, id);
    response["error"] = json::Value(code);
    response["message"] = json::Value(message);
    return encode(response);
}

} // namespace

ControlApi::ControlApi(ZoneManager& zones, RoomDirectory& rooms,
                       const DiscoveredClients& discovered, SharedPlanner& planner,
                       const ResamplerRegistry& registry, const IClock& clock, MediaOrigin media,
                       std::function<bool(const RoomCommand&)> sendToRoom,
                       std::function<void(const std::string&)> publishEvent)
    : zones_(zones), rooms_(rooms), discovered_(discovered), planner_(planner),
      registry_(registry), clock_(clock), media_(std::move(media)), sendToRoom_(std::move(sendToRoom)),
      publishEvent_(std::move(publishEvent)), startedAtNs_(clock.monotonicNowNs())
{
}

// ---------------------------------------------------------------------------

std::string ControlApi::handleLine(const std::string& line, bool& subscribed)
{
    json::Value root;
    std::string parseError;
    if (!json::parse(line, root, parseError))
        return errorLine({}, {}, kInvalidJson, parseError);
    if (!root.isObject())
        return errorLine({}, {}, kInvalidJson, "a request must be a JSON object");

    // The echo fields first, so even a refused request can be matched to its
    // response by a caller that pipelines them.
    json::Value id;
    if (const json::Value* given = root.find("id"); given != nullptr)
        id = *given;

    std::string command;
    if (!json::getString(root, "command", command) || command.empty())
        return errorLine({}, id, kInvalidRequest, "missing or empty \"command\" string");

    for (const auto& member : root.members())
    {
        if (member.first != "command" && member.first != "params" && member.first != "id")
            return errorLine(command, id, kInvalidRequest,
                             "unknown key \"" + member.first + "\" in the request");
    }

    json::Value params = json::Value::makeObject();
    if (const json::Value* given = root.find("params"); given != nullptr && !given->isNull())
    {
        if (!given->isObject())
            return errorLine(command, id, kInvalidRequest, "\"params\" must be an object");
        params = *given;
    }

    Reply reply;
    if (command == "status")
        reply = onlyKeys(params, {}, command, reply) ? status() : reply;
    else if (command == "clients")
        reply = onlyKeys(params, {}, command, reply) ? clients() : reply;
    else if (command == "zones")
        reply = onlyKeys(params, {}, command, reply) ? zonesList() : reply;
    else if (command == "zone.create")
        reply = onlyKeys(params, {"zone"}, command, reply) ? zoneCreate(params) : reply;
    else if (command == "zone.delete")
        reply = onlyKeys(params, {"zone"}, command, reply) ? zoneDelete(params) : reply;
    else if (command == "zone.setClients")
        reply = onlyKeys(params, {"zone", "clients"}, command, reply) ? zoneSetClients(params)
                                                                       : reply;
    else if (command == "source.stage")
        reply = onlyKeys(params,
                         {"zone", "sourceId", "mediaUri", "trackId", "title", "startOffsetNs"},
                         command, reply)
                    ? sourceStage(params)
                    : reply;
    else if (command == "source.clearStaged")
        reply = onlyKeys(params, {"zone"}, command, reply) ? sourceClearStaged(params) : reply;
    else if (command == "zone.play")
        reply = onlyKeys(params, {"zone"}, command, reply) ? zonePlay(params) : reply;
    else if (command == "zone.pause")
        reply = onlyKeys(params, {"zone"}, command, reply) ? zonePause(params) : reply;
    else if (command == "zone.stop")
        reply = onlyKeys(params, {"zone"}, command, reply) ? zoneStop(params) : reply;
    else if (command == "zone.seek")
        reply = onlyKeys(params, {"zone", "positionNs"}, command, reply) ? zoneSeek(params) : reply;
    else if (command == "client.setGrouped")
        reply = onlyKeys(params, {"client", "grouped"}, command, reply) ? clientSetGrouped(params)
                                                                        : reply;
    else if (command == "client.setAudioControl")
        reply = onlyKeys(params, {"client", "audioControl"}, command, reply)
                    ? clientSetAudioControl(params)
                    : reply;
    else if (command == "client.setVolume")
        reply = onlyKeys(params, {"client", "value", "muted", "applyAtTimelineNs"}, command, reply)
                    ? clientSetVolume(params)
                    : reply;
    else if (command == "client.rejoin")
        reply = onlyKeys(params, {"client"}, command, reply) ? clientRejoin(params) : reply;
    else if (command == "client.setOutputLatency")
        reply = onlyKeys(params, {"client", "latencyMs"}, command, reply)
                    ? clientSetOutputLatency(params)
                    : reply;
    else if (command == "client.setEqualizer")
        reply = onlyKeys(params, {"client", "enabled", "preampDb", "bands"}, command, reply)
                    ? clientSetEqualizer(params)
                    : reply;
    else if (command == "client.setAudioDevice")
        reply = onlyKeys(params, {"client", "deviceId"}, command, reply)
                    ? clientSetAudioDevice(params)
                    : reply;
    else if (command == "client.rescanAudio")
        reply = onlyKeys(params, {"client"}, command, reply) ? clientRescanAudio(params) : reply;
    else if (command == "audio.settings")
        reply = onlyKeys(params, {}, command, reply) ? audioSettings() : reply;
    else if (command == "audio.configure")
        reply = onlyKeys(params,
                         {"minimumStartBufferMs", "clientReadyTimeoutMs", "startDelayMs",
                          "seekFlushLeadMs", "seekFillLeadMs", "memoryLimitMb"},
                         command, reply)
                    ? audioConfigure(params)
                    : reply;
    else if (command == "client.setVolumeControl")
        reply = onlyKeys(params, {"client", "volumeControl"}, command, reply)
                    ? clientSetVolumeControl(params)
                    : reply;
    else if (command == "client.setDriftCorrection")
        reply = onlyKeys(params, {"client", "driftCorrection"}, command, reply)
                    ? clientSetDriftCorrection(params)
                    : reply;
    else if (command == "client.setBuffer")
        reply = onlyKeys(params, {"client", "bufferMs"}, command, reply)
                    ? clientSetBuffer(params)
                    : reply;
    else if (command == "client.setName")
        reply = onlyKeys(params, {"client", "name"}, command, reply) ? clientSetName(params)
                                                                    : reply;
    else if (command == "client.wake")
        reply = onlyKeys(params, {"client", "wakeOnLan"}, command, reply) ? clientWake(params) : reply;
    else if (command == "client.forget")
        reply = onlyKeys(params, {"client"}, command, reply) ? clientForget(params) : reply;
    else if (command == "client.setInputBindings")
        reply = onlyKeys(params, {"client", "inputId", "bindings"}, command, reply)
                    ? clientSetInputBindings(params)
                    : reply;
    else if (command == "discovery.clients")
        reply = onlyKeys(params, {}, command, reply) ? discoveryClients() : reply;
    else if (command == "client.attach")
        reply = onlyKeys(params, {"client", "server"}, command, reply) ? clientAttach(params)
                                                                       : reply;
    else if (command == "client.detach")
        reply = onlyKeys(params, {"client"}, command, reply) ? clientDetach(params) : reply;
    else if (command == "client.getNetwork" || command == "client.getLeds")
        reply = onlyKeys(params, {"client"}, command, reply)
                    ? clientFirmwareSettings(params, command.substr(7)) : reply;
    else if (command == "client.setEthernetSpeed")
        reply = onlyKeys(params, {"client", "speedMbps"}, command, reply)
                    ? clientFirmwareSettings(params, "setNetwork") : reply;
    else if (command == "client.setLeds")
        reply = onlyKeys(params, {"client", "off"}, command, reply)
                    ? clientFirmwareSettings(params, "setLeds") : reply;
    else if (command == "client.getPower")
        reply = onlyKeys(params, {"client"}, command, reply)
                    ? clientFirmwareSettings(params, "getPower") : reply;
    else if (command == "client.setPower")
        reply = onlyKeys(params, {"client", "standbyAfterSeconds", "deepSleepAfterSeconds"}, command, reply)
                    ? clientFirmwareSettings(params, "setPower") : reply;
    else if (command == "client.power")
    {
        // The client's own power commands; deep sleep takes it off the
        // network, and client.wake (or POWER) brings it back.
        std::string action;
        if (onlyKeys(params, {"client", "action"}, command, reply) &&
            requireString(params, "action", action, reply))
        {
            reply = action == "standbyOn" || action == "standbyOff" || action == "toggleStandby" ||
                            action == "deepSleep"
                        ? clientFirmwareSettings(params, action)
                        : Reply::failure(kInvalidArgument,
                                         "action must be standbyOn, standbyOff, toggleStandby or deepSleep");
        }
    }
    else if (command == "resampler.configure")
        reply = onlyKeys(params, {"resampler"}, command, reply) ? resamplerConfigure(params)
                                                                 : reply;
    else if (command == "events.subscribe" || command == "events.unsubscribe")
    {
        // The connection's own business, so it is answered here rather than
        // by a command that could reach no connection.
        if (onlyKeys(params, {}, command, reply))
        {
            const bool wanted = command == "events.subscribe";
            reply.body["subscribed"] = json::Value(wanted);
            reply.body["changed"] = json::Value(subscribed != wanted);
            subscribed = wanted;
        }
    }
    else
        return errorLine(command, id, kUnknownCommand, "unknown command \"" + command + "\"");

    if (reply.error != nullptr)
    {
        MULTIROOM_DEBUG("control API: {} refused ({}: {})", command, reply.error, reply.message);
        return errorLine(command, id, reply.error, reply.message);
    }

    json::Value response = responseHead(true, command, id);
    for (auto& member : reply.body.members())
        response[member.first] = member.second;
    return encode(response);
}

// ---------------------------------------------------------------------------

ControlApi::Reply ControlApi::status()
{
    Reply reply;
    reply.body["server"] = serverJson();
    reply.body["clients"] = clients().body["clients"];
    reply.body["zones"] = zonesList().body["zones"];
    return reply;
}

ControlApi::Reply ControlApi::clients()
{
    const uint64_t now = clock_.monotonicNowNs();
    json::Value list = json::Value::makeArray();
    for (const RoomRecord& room : rooms_.rooms())
        list.pushBack(clientJson(room, now));

    Reply reply;
    reply.body["clients"] = std::move(list);
    return reply;
}

ControlApi::Reply ControlApi::discoveryClients()
{
    // The list before the clock: a beacon landing in between would otherwise
    // be seen after `now`, and its age would wrap.
    const std::vector<DiscoveredClient> discovered = discovered_.clients();
    const uint64_t now = clock_.monotonicNowNs();
    json::Value list = json::Value::makeArray();
    for (const DiscoveredClient& found : discovered)
    {
        json::Value entry = json::Value::makeObject();
        entry["client"] = json::Value(found.client.str());
        entry["address"] = json::Value(found.address);
        entry["controlPort"] = json::Value(static_cast<uint32_t>(found.controlPort));
        entry["protocolVersion"] = json::Value(found.protocolVersion);
        if (!found.name.empty())
            entry["name"] = json::Value(found.name);
        // Left out rather than sent empty when the beacon did not say.
        if (!found.clientVersion.empty())
            entry["clientVersion"] = json::Value(found.clientVersion);
        if (!found.platform.empty())
            entry["platform"] = json::Value(found.platform);
        if (!found.host.empty())
            entry["host"] = json::Value(found.host);
        // Where the client is pointed, "" for nowhere: with `connected` false,
        // a non-empty one is a client attached to another core.
        if (found.server.has_value())
            entry["server"] = json::Value(*found.server);
        entry["present"] = json::Value(isPresent(found, now));
        entry["lastSeenMsAgo"] = json::Value(static_cast<uint64_t>((now - found.lastSeenNs) / 1000000ULL));
        entry["firstSeenMsAgo"] = json::Value(static_cast<uint64_t>((now - found.firstSeenNs) / 1000000ULL));
        entry["beacons"] = json::Value(found.beacons);
        // The question the caller is asking: is this one already a room here,
        // or does it still need attaching?
        const std::optional<RoomRecord> room = rooms_.find(found.client);
        entry["connected"] = json::Value(room.has_value() && room->connected);
        // Pointed at this core but not (yet) a room: attached here, with no
        // session -- not a client of another core.
        const uint16_t port = roomPort_.load();
        const bool attachedHere = found.server.has_value() && !found.server->empty() &&
                                  port != 0 &&
                                  *found.server == localAddressToward(found.address) + ":" +
                                                       std::to_string(port);
        entry["attachedHere"] = json::Value(attachedHere);
        list.pushBack(std::move(entry));
    }

    Reply reply;
    reply.body["clients"] = std::move(list);
    reply.body["forgotten"] = json::Value(discovered_.forgotten());
    return reply;
}

ControlApi::Reply ControlApi::clientAttach(const json::Value& params)
{
    Reply reply;
    std::string name;
    std::string server;
    if (!requireString(params, "client", name, reply) ||
        !optionalString(params, "server", server, reply))
        return reply;

    // Without a server named, this one: its address on the route to the
    // client, which is the one the client can reach.
    if (server.empty())
    {
        const std::optional<DiscoveredClient> found = discovered_.find(ClientId(name));
        if (!found)
            return Reply::failure(kNotFound, "no beacon from " + name + " has been heard");
        const uint16_t port = roomPort_.load();
        const std::string local = localAddressToward(found->address);
        if (port == 0 || local.empty())
            return Reply::failure(kUnavailable,
                                  "no address of this server is known to reach " + found->address);
        server = local + ":" + std::to_string(port);
    }
    return pointClient(name, server);
}

ControlApi::Reply ControlApi::clientDetach(const json::Value& params)
{
    Reply reply;
    std::string name;
    if (!requireString(params, "client", name, reply))
        return reply;
    return pointClient(name, std::string());
}

ControlApi::Reply ControlApi::callClient(const std::string& name, const std::string& command,
                                         const json::Value& params)
{
    const std::optional<DiscoveredClient> found = discovered_.find(ClientId(name));
    if (!found)
        return Reply::failure(kNotFound, "no beacon from " + name + " has been heard");
    if (!isPresent(*found, clock_.monotonicNowNs()))
        return Reply::failure(kUnavailable, name + " has not sent a beacon recently");

    std::string line;
    std::string error;
    if (!sendClientControl(found->address, found->controlPort, command, params,
                           kClientCallTimeoutMs, line, error))
        return Reply::failure(kFailed, error);

    Reply answer;
    bool ok = false;
    if (!json::parse(line, answer.body, error) || !answer.body.isObject() ||
        !json::getBool(answer.body, "ok", ok))
        return Reply::failure(kFailed, name + " answered with something that is not a response");
    return answer;
}

ControlApi::Reply ControlApi::pointClient(const std::string& name, const std::string& server)
{
    auto params = json::Value::makeObject();
    params["server"] = json::Value(server);
    const Reply called = callClient(name, "setServer", params);
    if (called.error != nullptr)
        return called;
    const json::Value& answer = called.body;

    // The client's own answer: its refusal is passed on with its message.
    bool ok = false;
    json::getBool(answer, "ok", ok);
    if (!ok)
    {
        std::string message;
        json::getString(answer, "message", message);
        return Reply::failure(kFailed, name + " refused: " + message);
    }

    bool changed = false;
    json::getBool(answer, "changed", changed);
    MULTIROOM_INFO("control API: {} pointed at {}", name, server.empty() ? "<no server>" : server);

    Reply reply;
    reply.body["client"] = json::Value(name);
    reply.body["server"] = json::Value(server);
    reply.body["changed"] = json::Value(changed);
    return reply;
}

ControlApi::Reply ControlApi::clientFirmwareSettings(const json::Value& params, const std::string& command)
{
    Reply reply;
    std::string name;
    if (!requireString(params, "client", name, reply)) return reply;
    auto changes = json::Value::makeObject();
    if (command == "setNetwork")
    {
        std::string speed;
        if (!requireString(params, "speedMbps", speed, reply)) return reply;
        if (speed != "auto" && speed != "10" && speed != "100")
            return Reply::failure(kInvalidArgument, "Ethernet speed must be auto, 10 or 100 Mbps");
        changes["ethernet"] = json::Value::makeObject();
        changes["ethernet"]["speedMbps"] = json::Value(speed);
    }
    else if (command == "setLeds")
    {
        bool off = false;
        if (!json::getBool(params, "off", off))
            return Reply::failure(kInvalidArgument, "off must be a boolean");
        changes["off"] = json::Value(off);
    }
    else if (command == "setPower")
    {
        // Either delay or both, in seconds; the client checks the ranges.
        for (const char* key : {"standbyAfterSeconds", "deepSleepAfterSeconds"})
        {
            if (const json::Value* value = params.find(key))
            {
                if (!value->isIntegral())
                    return Reply::failure(kInvalidArgument, std::string(key) + " must be an integer");
                changes[key] = *value;
            }
        }
        if (changes.members().empty())
            return Reply::failure(kInvalidArgument,
                                  "standbyAfterSeconds or deepSleepAfterSeconds is required");
    }
    const Reply called = callClient(name, command, changes);
    if (called.error != nullptr)
        return called;
    const json::Value& answer = called.body;
    bool ok = false;
    json::getBool(answer, "ok", ok);
    if (!ok)
    {
        std::string message, code;
        json::getString(answer, "message", message);
        json::getString(answer, "error", code);
        return Reply::failure(code == "invalidArgument" ? kInvalidArgument :
                              code == "unavailable" || code == "unknownCommand" ? kUnavailable : kFailed,
                              name + " refused: " + message);
    }
    reply.body["client"] = json::Value(name);
    for (const char* key : {"supported", "network", "applyDelaySeconds", "off", "state",
                            "standbyAfterSeconds", "deepSleepAfterSeconds", "deepSleep"})
        if (const auto* value = answer.find(key)) reply.body[key] = *value;
    return reply;
}

ControlApi::Reply ControlApi::zonesList()
{
    json::Value list = json::Value::makeArray();
    for (const ZoneId& id : zones_.ids())
    {
        if (const std::shared_ptr<Zone> zone = zones_.find(id); zone != nullptr)
            list.pushBack(zoneJson(*zone));
    }

    Reply reply;
    reply.body["zones"] = std::move(list);
    return reply;
}

ControlApi::Reply ControlApi::zoneCreate(const json::Value& params)
{
    Reply reply;
    std::string name;
    if (!requireString(params, "zone", name, reply))
        return reply;

    const ZoneId id(name);
    bool created = false;
    if (zones_.find(id) == nullptr)
    {
        const auto made = zones_.create(id);
        if (!made)
        {
            return Reply::failure(made.error().code == ZoneError::Full ? kUnavailable : kFailed,
                                  made.error().detail);
        }
        created = true;
    }

    reply.body["zone"] = json::Value(name);
    reply.body["created"] = json::Value(created);
    return reply;
}

ControlApi::Reply ControlApi::zoneDelete(const json::Value& params)
{
    Reply reply;
    std::string name;
    if (!requireString(params, "zone", name, reply))
        return reply;
    const ZoneId id(name);

    // The rooms stop belonging to it first, so a device answering while the
    // zone is being stopped has nowhere to be admitted to.
    for (const RoomRecord& room : rooms_.rooms())
    {
        if (room.zone == id)
            static_cast<void>(rooms_.update(room.client, [](RoomRecord& r) { r.zone = ZoneId(); }));
    }

    std::vector<ClientId> members;
    if (const std::shared_ptr<Zone> zone = zones_.find(id); zone != nullptr)
        members = zone->memberIds();

    // Stops every room of it and its pipeline. The rooms stay connected and
    // idle: nothing better can be said to them until a zone wants them.
    const bool deleted = zones_.remove(id);
    if (deleted)
    {
        MULTIROOM_INFO("control API: zone {} deleted", name);
        for (const ClientId& client : members)
        {
            json::Value event = eventHead("room.left");
            event["client"] = json::Value(client.str());
            event["zone"] = json::Value(name);
            event["cause"] = json::Value("deleted");
            publish(event);
        }
    }

    reply.body["zone"] = json::Value(name);
    reply.body["deleted"] = json::Value(deleted);
    return reply;
}

ControlApi::Reply ControlApi::zoneSetClients(const json::Value& params)
{
    Reply reply;
    std::string name;
    if (!requireString(params, "zone", name, reply))
        return reply;
    const ZoneId id(name);
    if (zones_.find(id) == nullptr)
        return Reply::failure(kNotFound, "no zone " + name);

    const json::Value* list = params.find("clients");
    if (list == nullptr || !list->isArray())
        return Reply::failure(kInvalidArgument, "\"clients\" must be an array of client ids");

    std::set<ClientId> wanted;
    for (size_t i = 0; i < list->size(); ++i)
    {
        const json::Value& entry = list->at(i);
        if (!entry.isString() || entry.asString().empty())
            return Reply::failure(kInvalidArgument, "every client id must be a non-empty string");
        if (!wanted.insert(ClientId(entry.asString())).second)
            return Reply::failure(kInvalidArgument, "client " + entry.asString() + " is listed twice");
    }

    // Applied before anything else changes and in one step, so that a refusal
    // leaves the old membership whole rather than half replaced -- including
    // against a room that connects, and takes a record, while this runs.
    std::vector<ClientId> added;
    for (const ClientId& client : wanted)
    {
        if (rooms_.zoneFor(client) != id)
            added.push_back(client);
    }
    if (!rooms_.updateAll(added, [&id](RoomRecord& r) { r.zone = id; }))
    {
        return Reply::failure(kUnavailable, "this server keeps " +
                                                 std::to_string(rooms_.maxRooms()) + " rooms");
    }

    std::vector<ClientId> removed;
    for (const RoomRecord& room : rooms_.rooms())
    {
        if (room.zone != id || wanted.contains(room.client))
            continue;
        static_cast<void>(rooms_.update(room.client, [](RoomRecord& r) { r.zone = ZoneId(); }));
        if (const std::shared_ptr<Zone> owner = zones_.zoneOf(room.client);
            owner != nullptr && owner->id() == id)
        {
            releaseRoom(room.client, "removed");
        }
        removed.push_back(room.client);
    }

    for (const ClientId& client : added)
    {
        if (const std::optional<RoomRecord> room = rooms_.find(client); room && room->connected)
            renegotiate(*room, "moved");
    }

    if (!added.empty() || !removed.empty())
        MULTIROOM_INFO("control API: zone {} gains {} room(s) and loses {}", name, added.size(),
                 removed.size());

    reply.body["zone"] = json::Value(name);
    reply.body["added"] = idList(added);
    reply.body["removed"] = idList(removed);
    return reply;
}

ControlApi::Reply ControlApi::sourceStage(const json::Value& params)
{
    Reply reply;
    std::string name;
    SourceSegment segment;
    if (!requireString(params, "zone", name, reply) ||
        !requireString(params, "sourceId", segment.sourceId, reply) ||
        !requireString(params, "mediaUri", segment.mediaUri, reply) ||
        !optionalString(params, "trackId", segment.trackId, reply) ||
        !optionalString(params, "title", segment.title, reply) ||
        !nanoseconds(params, "startOffsetNs", false, segment.startOffsetNs, reply))
    {
        return reply;
    }

    // Media comes from the high-end server and nowhere else: a path, another
    // scheme or another host is refused here, before a zone holds it.
    if (const auto allowed = media_.check(segment.mediaUri); !allowed)
        return Reply::failure(kInvalidArgument, allowed.error().detail);

    const std::shared_ptr<Zone> zone = zones_.find(ZoneId(name));
    if (zone == nullptr)
        return Reply::failure(kNotFound, "no zone " + name);

    // A sourceId names one segment of one zone, not a track: staging the same
    // track twice is two ids. So an id this zone already holds, staged or
    // opened, is this request replayed, and replaying it changes nothing.
    bool known = false;
    for (const SourceSegment& staged : zone->staged())
        known = known || staged.sourceId == segment.sourceId;
    for (const StagedSource& opened : zone->sources())
        known = known || opened.segment.sourceId == segment.sourceId;

    if (!known)
    {
        if (const auto staged = zone->stage(segment); !staged)
        {
            return Reply::failure(staged.error().code == ZoneError::InvalidSegment
                                      ? kInvalidArgument
                                      : kFailed,
                                  staged.error().detail);
        }
        MULTIROOM_INFO("control API: zone {} stages {} ({})", name, segment.sourceId, segment.mediaUri);
    }

    reply.body["zone"] = json::Value(name);
    reply.body["sourceId"] = json::Value(segment.sourceId);
    reply.body["staged"] = json::Value(!known);
    return reply;
}

ControlApi::Reply ControlApi::sourceClearStaged(const json::Value& params)
{
    Reply reply;
    std::string name;
    if (!requireString(params, "zone", name, reply))
        return reply;
    const std::shared_ptr<Zone> zone = zones_.find(ZoneId(name));
    if (zone == nullptr)
        return Reply::failure(kNotFound, "no zone " + name);

    reply.body["zone"] = json::Value(name);
    reply.body["cleared"] = json::Value(static_cast<uint64_t>(zone->clearStaged()));
    return reply;
}

ControlApi::Reply ControlApi::zonePlay(const json::Value& params)
{
    Reply reply;
    std::string name;
    if (!requireString(params, "zone", name, reply))
        return reply;
    const std::shared_ptr<Zone> zone = zones_.find(ZoneId(name));
    if (zone == nullptr)
        return Reply::failure(kNotFound, "no zone " + name);

    if (zone->state() == ZoneState::Failed)
        return Reply::failure(kFailed, zone->lastError());

    if (zone->state() == ZoneState::Idle)
    {
        // Synchronous on purpose, as Zone::play is: a URI that cannot be
        // played is answered here, to the caller that staged it.
        if (const auto opened = zone->play(); !opened)
        {
            return Reply::failure(opened.error().code == ZoneError::NothingStaged ? kUnavailable
                                                                                   : kFailed,
                                  opened.error().detail);
        }
        if (const auto running = zone->run(); !running)
            return Reply::failure(kFailed, running.error().detail);

        MULTIROOM_INFO("control API: zone {} playing as {}", name,
                 describeFormat(zone->sourceFormat()));

        // Until now there was no source to plan a room against, so every
        // connected room that belongs here was told its own ceiling. Each is
        // asked again, and its answer is what admits it.
        for (const RoomRecord& room : rooms_.rooms())
        {
            if (room.zone == zone->id() && room.connected && zones_.zoneOf(room.client) == nullptr)
                renegotiate(room, "moved");
        }
    }
    else if (zone->transport() == TransportState::Paused)
    {
        if (const auto resumed = zone->resumePlayback(); !resumed)
            return Reply::failure(kFailed, resumed.error().detail);
        MULTIROOM_INFO("control API: zone {} resumes", name);
    }

    // A parked device lost its clock and buffered pause position. Prepare
    // again; STREAM_READY feeds the normal recovery and start barrier.
    for (const RoomRecord& room : rooms_.rooms())
    {
        if (room.connected && room.zone == zone->id() && room.streamStandby)
        {
            RoomCommand wake;
            wake.kind = RoomCommandKind::WakeStream;
            wake.client = room.client;
            if (!sendToRoom_(wake))
                MULTIROOM_WARN("control API: {} could not be woken", room.client.str());
        }
        // Asleep with an address to wake: it reconnects and joins like any
        // room coming back, and the zone does not wait for it.
        if (!room.connected && room.zone == zone->id() && room.powerState == "sleep")
        {
            std::string error;
            if (sendMagicPacket(room.wakeOnLan, error))
                MULTIROOM_INFO("control API: woke {} ({}) for zone {}", room.client.str(),
                               room.wakeOnLan, name);
            else
                MULTIROOM_WARN("control API: could not wake {}: {}", room.client.str(), error);
        }
    }
    reply.body["zone"] = json::Value(name);
    reply.body["state"] = json::Value(zoneStateName(zone->state()));
    reply.body["transport"] = json::Value(transportStateName(zone->transport()));
    return reply;
}

ControlApi::Reply ControlApi::zonePause(const json::Value& params)
{
    Reply reply;
    std::string name;
    if (!requireString(params, "zone", name, reply))
        return reply;
    const std::shared_ptr<Zone> zone = zones_.find(ZoneId(name));
    if (zone == nullptr)
        return Reply::failure(kNotFound, "no zone " + name);

    bool paused = false;
    TimelineNs position = zone->playheadNs();
    switch (zone->transport())
    {
    case TransportState::Paused:
        break; // replayed: where it already stands is the answer
    case TransportState::Playing:
    {
        const auto frozen = zone->pausePlayback();
        if (!frozen)
            return Reply::failure(kFailed, frozen.error().detail);
        position = *frozen;
        paused = true;
        MULTIROOM_INFO("control API: zone {} pauses at timeline {} ns", name,
                 std::to_underlying(position));
        replanAloneRooms(*zone);
        break;
    }
    case TransportState::Stopped:
    case TransportState::Starting:
        // Section 37 names a position on a running line, and there is none:
        // a paused start would be a barrier nobody may pass.
        return Reply::failure(kUnavailable, "zone " + name + "'s timeline is " +
                                                transportStateName(zone->transport()));
    }

    reply.body["zone"] = json::Value(name);
    reply.body["paused"] = json::Value(paused);
    reply.body["timelineNs"] = json::Value(std::to_underlying(position));
    return reply;
}

ControlApi::Reply ControlApi::zoneSeek(const json::Value& params)
{
    Reply reply;
    std::string name;
    uint64_t positionNs = 0;
    if (!requireString(params, "zone", name, reply) ||
        !nanoseconds(params, "positionNs", true, positionNs, reply))
    {
        return reply;
    }
    const std::shared_ptr<Zone> zone = zones_.find(ZoneId(name));
    if (zone == nullptr)
        return Reply::failure(kNotFound, "no zone " + name);

    // Synchronous, like zone.play: the source is opened at the offset before
    // anything is cut, so a source that will not open is this answer and the
    // zone plays on.
    const auto sought = zone->seek(positionNs);
    if (!sought)
    {
        switch (sought.error().code)
        {
        case ZoneError::InvalidPosition:
            return Reply::failure(kInvalidArgument, sought.error().detail);
        case ZoneError::NotSeekable:
        case ZoneError::NotStarted:
            return Reply::failure(kUnavailable, sought.error().detail);
        default:
            return Reply::failure(kFailed, sought.error().detail);
        }
    }
    MULTIROOM_INFO("control API: zone {} seeks {} to {} ns", name, sought->sourceId, positionNs);

    reply.body["zone"] = json::Value(name);
    reply.body["sourceId"] = json::Value(sought->sourceId);
    reply.body["positionNs"] = json::Value(positionNs);
    reply.body["cutTimelineNs"] = json::Value(std::to_underlying(sought->cutAt));
    reply.body["timelineNs"] = json::Value(std::to_underlying(sought->resumeAt));
    return reply;
}

ControlApi::Reply ControlApi::zoneStop(const json::Value& params)
{
    Reply reply;
    std::string name;
    if (!requireString(params, "zone", name, reply))
        return reply;
    const std::shared_ptr<Zone> zone = zones_.find(ZoneId(name));
    if (zone == nullptr)
        return Reply::failure(kNotFound, "no zone " + name);

    reply.body["zone"] = json::Value(name);

    const bool lineRunning = zone->transport() != TransportState::Stopped;
    if (!lineRunning && zone->state() != ZoneState::Playing && zone->state() != ZoneState::Ended)
    {
        reply.body["stopped"] = json::Value(false); // replayed, or never played
        return reply;
    }

    const auto stopped = zone->stopPlayback();
    if (!stopped)
        return Reply::failure(kFailed, stopped.error().detail);
    MULTIROOM_INFO("control API: zone {} stopped", name);
    replanAloneRooms(*zone);

    reply.body["stopped"] = json::Value(true);
    if (lineRunning)
        reply.body["timelineNs"] = json::Value(std::to_underlying(*stopped));
    return reply;
}

ControlApi::Reply ControlApi::clientSetGrouped(const json::Value& params)
{
    Reply reply;
    std::string name;
    if (!requireString(params, "client", name, reply))
        return reply;
    const json::Value* value = params.find("grouped");
    if (value == nullptr || !value->isBool())
        return Reply::failure(kInvalidArgument, "\"grouped\" must be true or false");

    const ClientId client(name);
    const bool wanted = value->asBool();
    const std::optional<RoomRecord> before = rooms_.find(client);
    const bool changed = before ? before->grouped != wanted : wanted;

    if (changed)
    {
        if (!rooms_.update(client, [wanted](RoomRecord& r) { r.grouped = wanted; }))
            return Reply::failure(kUnavailable, "this server keeps " +
                                                    std::to_string(rooms_.maxRooms()) + " rooms");
        MULTIROOM_INFO("control API: {} {}", name, wanted ? "grouped" : "not grouped");
        regroup(client);
    }

    reply.body["client"] = json::Value(name);
    reply.body["grouped"] = json::Value(wanted);
    reply.body["changed"] = json::Value(changed);
    return reply;
}

ControlApi::Reply ControlApi::clientSetAudioControl(const json::Value& params)
{
    Reply reply;
    std::string name;
    std::string text;
    if (!requireString(params, "client", name, reply) ||
        !requireString(params, "audioControl", text, reply))
    {
        return reply;
    }

    AudioControl wanted = AudioControl::Auto;
    if (!parseAudioControl(text, wanted))
    {
        return Reply::failure(kInvalidArgument, "\"audioControl\" is " + text +
                                                    ", expected bitPerfect|auto");
    }

    const ClientId client(name);
    const std::optional<RoomRecord> before = rooms_.find(client);
    const bool changed = !before || before->audioControl != wanted;

    if (changed)
    {
        if (!rooms_.update(client, [wanted](RoomRecord& r) { r.audioControl = wanted; }))
            return Reply::failure(kUnavailable, "this server keeps " +
                                                    std::to_string(rooms_.maxRooms()) + " rooms");
        MULTIROOM_INFO("control API: {} audioControl {}", name, text);
        // Re-negotiated whether a zone owns it or not: the client is told its
        // Audio Control in SESSION_CONFIG (section 59), and it bounds what the
        // client may do to the samples as much as what this server may.
        if (const std::optional<RoomRecord> room = rooms_.find(client); room && room->connected)
            renegotiate(*room, "replanned");
    }

    reply.body["client"] = json::Value(name);
    reply.body["audioControl"] = json::Value(text);
    reply.body["changed"] = json::Value(changed);
    return reply;
}

ControlApi::Reply ControlApi::clientSetVolume(const json::Value& params)
{
    Reply reply;
    std::string name;
    if (!requireString(params, "client", name, reply))
        return reply;

    matp::SetVolume request;
    if (const json::Value* value = params.find("value"); value != nullptr)
    {
        if (!value->isNumber() || !(value->asDouble() >= 0.0 && value->asDouble() <= 1.0))
            return Reply::failure(kInvalidArgument, "\"value\" must be a number from 0.0 to 1.0");
        request.hasValue = true;
        request.value = value->asDouble();
    }
    if (const json::Value* muted = params.find("muted"); muted != nullptr)
    {
        if (!muted->isBool())
            return Reply::failure(kInvalidArgument, "\"muted\" must be true or false");
        request.hasMuted = true;
        request.muted = muted->asBool();
    }
    if (const json::Value* at = params.find("applyAtTimelineNs"); at != nullptr)
    {
        if (!at->isIntegral() || at->asInt() < 0)
            return Reply::failure(kInvalidArgument,
                                  "\"applyAtTimelineNs\" must be a non-negative integer");
        request.hasApplyAtTimelineNs = true;
        request.applyAtTimelineNs = static_cast<uint64_t>(at->asInt());
    }
    // makeSetVolume's own rule, answered to the caller that broke it.
    if (!request.hasValue && !request.hasMuted)
        return Reply::failure(kInvalidArgument, "\"value\", \"muted\" or both are required");

    const ClientId client(name);
    const std::shared_ptr<Zone> zone = zones_.zoneOf(client);

    // A timeline position means something only on a line. Stored for later it
    // would be applied at a position of whatever line the room joins next.
    if (request.hasApplyAtTimelineNs && (zone == nullptr || !zone->started()))
    {
        return Reply::failure(kUnavailable, "applyAtTimelineNs needs " + name +
                                                " to be playing in a zone, and it is not");
    }

    const std::optional<RoomRecord> before = rooms_.find(client);
    const bool changed =
        !before || (request.hasValue && before->volume != request.value) ||
        (request.hasMuted && before->muted != request.muted);

    if (changed)
    {
        if (!rooms_.update(client, [&request](RoomRecord& r) {
                if (request.hasValue)
                    r.volume = request.value;
                if (request.hasMuted)
                    r.muted = request.muted;
            }))
        {
            return Reply::failure(kUnavailable, "this server keeps " +
                                                    std::to_string(rooms_.maxRooms()) + " rooms");
        }
        MULTIROOM_INFO("control API: {} volume{}{}", name,
                 request.hasValue ? " " + std::to_string(request.value) : std::string(),
                 request.hasMuted ? (request.muted ? " muted" : " unmuted") : "");

        // A room in no zone, or one that left it in between, is sent what is
        // stored when it is next admitted.
        if (zone != nullptr)
        {
            if (const auto sent = zone->setVolume(client, request); !sent)
                MULTIROOM_DEBUG("control API: {} volume kept for later: {}", name,
                          describe(sent.error()));
        }
    }

    const std::optional<RoomRecord> after = rooms_.find(client);
    reply.body["client"] = json::Value(name);
    if (after && after->volume)
        reply.body["value"] = json::Value(*after->volume);
    if (after && after->muted)
        reply.body["muted"] = json::Value(*after->muted);
    reply.body["changed"] = json::Value(changed);
    return reply;
}

ControlApi::Reply ControlApi::clientRejoin(const json::Value& params)
{
    Reply reply;
    std::string name;
    if (!requireString(params, "client", name, reply))
        return reply;

    const ClientId client(name);
    const std::shared_ptr<Zone> zone = zones_.zoneOf(client);
    if (zone == nullptr)
        return Reply::failure(kUnavailable, name + " is in no zone");

    // Section 32's forced resync is section 43's rejoin: no new trigger, and
    // the rooms it refuses -- parked by a stop, held at a format boundary --
    // are refused here with its reason.
    const auto moved = zone->rejoin(client, RejoinTrigger::HardResync);
    if (!moved)
        return Reply::failure(kUnavailable, moved.error().detail);

    reply.body["client"] = json::Value(name);
    reply.body["zone"] = json::Value(zone->id().str());
    reply.body["timelineNs"] = json::Value(std::to_underlying(*moved));
    return reply;
}

ControlApi::Reply ControlApi::clientSetOutputLatency(const json::Value& params)
{
    Reply reply;
    std::string name;
    uint64_t latencyMs = 0;
    if (!requireString(params, "client", name, reply) ||
        !nanoseconds(params, "latencyMs", true, latencyMs, reply))
    {
        return reply;
    }

    // The room plays this far ahead of the line out of audio it already holds,
    // so the prebuffer is the bound, less one packet so the room is never
    // asked to play the packet that is still arriving.
    const ClientOutputSettings& output = zones_.defaults().output;
    const uint64_t packetNs = static_cast<uint64_t>(output.packetDurationMs) * 1000000ULL;
    const uint64_t ceilingMs =
        millisecondsOf(output.targetBufferNs > packetNs ? output.targetBufferNs - packetNs : 0);
    if (latencyMs > ceilingMs)
    {
        return Reply::failure(kInvalidArgument, "\"latencyMs\" is " + std::to_string(latencyMs) +
                                                    ", the ceiling is " +
                                                    std::to_string(ceilingMs) + " ms");
    }

    const ClientId client(name);
    const std::optional<RoomRecord> before = rooms_.find(client);
    const bool changed = !before || before->outputLatencyMs.value_or(0) != latencyMs;

    bool rejoined = false;
    if (changed)
    {
        if (!rooms_.update(client, [latencyMs](RoomRecord& r) { r.outputLatencyMs = latencyMs; }))
            return Reply::failure(kUnavailable, "this server keeps " +
                                                    std::to_string(rooms_.maxRooms()) + " rooms");
        MULTIROOM_INFO("control API: {} output latency {} ms", name, latencyMs);

        // A room in no zone takes it when it is next admitted.
        if (const std::shared_ptr<Zone> zone = zones_.zoneOf(client); zone != nullptr)
        {
            const auto set = zone->setOutputLatency(client, latencyMs * 1000000ULL);
            rejoined = set && *set;
        }
    }

    reply.body["client"] = json::Value(name);
    reply.body["latencyMs"] = json::Value(latencyMs);
    reply.body["changed"] = json::Value(changed);
    reply.body["rejoined"] = json::Value(rejoined);
    return reply;
}

ControlApi::Reply ControlApi::clientSetEqualizer(const json::Value& params)
{
    Reply reply;
    std::string name;
    if (!requireString(params, "client", name, reply))
        return reply;

    // The whole equalizer every time: a caller that sends one band means one
    // band, not one band changed.
    EqualizerSettings wanted;
    const json::Value* enabled = params.find("enabled");
    if (enabled == nullptr || !enabled->isBool())
        return Reply::failure(kInvalidArgument, "\"enabled\" must be true or false");
    wanted.enabled = enabled->asBool();

    if (const json::Value* preamp = params.find("preampDb"); preamp != nullptr)
    {
        if (!preamp->isNumber())
            return Reply::failure(kInvalidArgument, "\"preampDb\" must be a number");
        wanted.preampDb = preamp->asDouble();
    }

    if (const json::Value* bands = params.find("bands"); bands != nullptr)
    {
        if (!bands->isArray())
            return Reply::failure(kInvalidArgument, "\"bands\" must be an array");
        if (bands->size() > EqualizerSettings::kMaxBands)
            return Reply::failure(kInvalidArgument,
                                  "\"bands\" holds " + std::to_string(bands->size()) +
                                      ", at most " +
                                      std::to_string(EqualizerSettings::kMaxBands));
        for (size_t i = 0; i < bands->size(); ++i)
        {
            const json::Value& item = bands->at(i);
            const std::string at = "band " + std::to_string(i) + ": ";
            if (!item.isObject())
                return Reply::failure(kInvalidArgument, at + "must be an object");
            for (const auto& member : item.members())
            {
                if (member.first != "type" && member.first != "frequencyHz" &&
                    member.first != "gainDb" && member.first != "q")
                    return Reply::failure(kInvalidArgument,
                                          at + "unknown key \"" + member.first + "\"");
            }

            EqBand band;
            std::string type;
            if (!json::getString(item, "type", type) || !parseEqFilterType(type, band.type))
                return Reply::failure(kInvalidArgument,
                                      at + "\"type\" must be "
                                           "peak|lowShelf|highShelf|highPass|lowPass");
            if (!json::getDouble(item, "frequencyHz", band.frequencyHz))
                return Reply::failure(kInvalidArgument, at + "\"frequencyHz\" must be a number");
            if (!json::getDouble(item, "q", band.q))
                return Reply::failure(kInvalidArgument, at + "\"q\" must be a number");
            if (item.find("gainDb") != nullptr && !json::getDouble(item, "gainDb", band.gainDb))
                return Reply::failure(kInvalidArgument, at + "\"gainDb\" must be a number");
            wanted.bands.push_back(band);
        }
    }

    if (const std::string invalid = validateEqualizer(wanted); !invalid.empty())
        return Reply::failure(kInvalidArgument, invalid);

    const ClientId client(name);
    const std::optional<RoomRecord> before = rooms_.find(client);
    const bool changed = !before || before->equalizer != wanted;

    if (changed)
    {
        if (!rooms_.update(client, [&wanted](RoomRecord& r) { r.equalizer = wanted; }))
            return Reply::failure(kUnavailable, "this server keeps " +
                                                    std::to_string(rooms_.maxRooms()) + " rooms");
        MULTIROOM_INFO("control API: {} equalizer {}, {} bands, preamp {} dB", name,
                 wanted.enabled ? "on" : "off", wanted.bands.size(), wanted.preampDb);

        // No re-negotiation and no move: the room's own thread takes it at
        // its next packet. A room in no zone takes it when it is admitted.
        if (const std::shared_ptr<Zone> zone = zones_.zoneOf(client); zone != nullptr)
        {
            if (const std::shared_ptr<ClientOutput> output = zone->member(client))
                output->setEqualizer(wanted);
        }
        // Switched on or off, an Auto room leaves bitperfect for it or goes
        // back; a BitPerfect room is planned the same either way.
        const bool wasOn = before && before->equalizer && before->equalizer->enabled;
        if (wasOn != wanted.enabled && rooms_.preference(client).audioControl == AudioControl::Auto)
            replan(client);
    }

    reply.body["client"] = json::Value(name);
    reply.body["equalizer"] = equalizerJson(wanted);
    reply.body["changed"] = json::Value(changed);
    return reply;
}

ControlApi::Reply ControlApi::clientSetAudioDevice(const json::Value& params)
{
    Reply reply;
    std::string name;
    std::string deviceId;
    if (!requireString(params, "client", name, reply))
        return reply;
    // Empty is allowed and is the client's system default.
    if (params.find("deviceId") == nullptr)
        return Reply::failure(kInvalidArgument, "\"deviceId\" must be a string");
    if (!optionalString(params, "deviceId", deviceId, reply))
        return reply;

    // Not desired state: the client stores the choice in its own config, so
    // nothing here outlives the connection and a room that is not connected
    // cannot be asked.
    const std::optional<RoomRecord> room = rooms_.find(ClientId(name));
    if (!room || !room->connected || !room->hasAudioDevices)
        return Reply::failure(kUnavailable,
                              name + " is not connected or has not reported its audio devices");

    const matp::AudioDeviceList& devices = room->audioDevices;
    const bool known =
        deviceId.empty() ||
        std::any_of(devices.devices.begin(), devices.devices.end(),
                    [&deviceId](const matp::AudioDeviceEntry& d) { return d.id == deviceId; });
    if (!known)
        return Reply::failure(kInvalidArgument, name + " reported no audio device \"" + deviceId + "\"");

    const bool changed = deviceId != devices.configuredDeviceId;
    if (changed)
    {
        RoomCommand command;
        command.kind = RoomCommandKind::SetAudioDevice;
        command.client = room->client;
        command.audioDevice.deviceId = deviceId;
        if (!sendToRoom_(command))
            return Reply::failure(kFailed, name + " could not be sent the device");
        MULTIROOM_INFO("control API: {} audio device {}", name,
                 deviceId.empty() ? "<default>" : deviceId);
    }

    // The client answers with AUDIO_DEVICES; `clients` shows the result.
    reply.body["client"] = json::Value(name);
    reply.body["deviceId"] = json::Value(deviceId);
    reply.body["changed"] = json::Value(changed);
    return reply;
}

ControlApi::Reply ControlApi::clientRescanAudio(const json::Value& params)
{
    Reply reply;
    std::string name;
    if (!requireString(params, "client", name, reply))
        return reply;

    // A room that has reported its devices is one that understands section 56;
    // an older client would ignore the request without a word.
    const std::optional<RoomRecord> room = rooms_.find(ClientId(name));
    if (!room || !room->connected || !room->hasAudioDevices)
        return Reply::failure(kUnavailable,
                              name + " is not connected or has not reported its audio devices");

    RoomCommand command;
    command.kind = RoomCommandKind::RescanAudio;
    command.client = room->client;
    if (!sendToRoom_(command))
        return Reply::failure(kFailed, name + " could not be asked to rescan");
    MULTIROOM_INFO("control API: {} asked to rescan its audio devices", name);

    // Answered before the scan, which takes seconds on the client; its new
    // list arrives as AUDIO_DEVICES and `clients` shows it.
    reply.body["client"] = json::Value(name);
    return reply;
}

void ControlApi::setAudioConfig(AudioConfig audio, AudioPersist persist)
{
    audio_ = std::move(audio);
    persistAudio_ = std::move(persist);
    hasAudio_ = true;
}

ControlApi::Reply ControlApi::audioSettings()
{
    if (!hasAudio_)
        return Reply::failure(kUnavailable, "this core does not report its audio settings");
    Reply reply;
    reply.body["audio"] = server_config::audioToJson(audio_);
    // What audio.configure takes; the rest sizes blocks or the resampler and
    // is read at startup only.
    json::Value live = json::Value::makeArray();
    for (const char* key : {"minimumStartBufferMs", "clientReadyTimeoutMs", "startDelayMs",
                            "seekFlushLeadMs", "seekFillLeadMs", "memoryLimitMb"})
        live.pushBack(json::Value(key));
    reply.body["live"] = std::move(live);

    // Phase 42: the limit and the range audio.configure accepts for it, so a
    // UI can check a value before sending it. The core still checks it.
    json::Value memory = json::Value::makeObject();
    memory["mb"] = json::Value(audio_.memoryLimitMb);
    memory["minMb"] = json::Value(memoryLimitFloorMb());
    if (const std::optional<uint64_t> ceiling = memoryLimitCeilingMb())
        memory["maxMb"] = json::Value(*ceiling);
    memory["usedMb"] = json::Value(megabytesUp(zones_.memory().usedBytes()));
    reply.body["memoryLimit"] = std::move(memory);

    // What resampler.configure takes.
    json::Value resamplers = json::Value::makeArray();
    for (const std::string& id : registry_.ids())
        resamplers.pushBack(json::Value(id));
    reply.body["resamplers"] = std::move(resamplers);
    return reply;
}

uint64_t ControlApi::memoryLimitFloorMb() const
{
    // One zone at the ceiling, and never less than the playing zones hold: a
    // lower limit never takes memory away from a zone that is playing.
    return std::max<uint64_t>(server_config::ceilingZoneMb(audio_),
                              megabytesUp(zones_.memory().usedBytes()));
}

ControlApi::Reply ControlApi::audioConfigure(const json::Value& params)
{
    if (!hasAudio_ || !persistAudio_)
        return Reply::failure(kUnavailable, "this core cannot change its audio settings");
    if (params.members().empty())
        return Reply::failure(kInvalidArgument, "audio.configure needs at least one setting");

    AudioConfig candidate = audio_;
    const std::pair<const char*, uint32_t*> fields[] = {
        {"minimumStartBufferMs", &candidate.minimumStartBufferMs},
        {"clientReadyTimeoutMs", &candidate.clientReadyTimeoutMs},
        {"startDelayMs", &candidate.startDelayMs},
        {"seekFlushLeadMs", &candidate.seekFlushLeadMs},
        {"seekFillLeadMs", &candidate.seekFillLeadMs},
    };
    for (const auto& [key, field] : fields)
    {
        const json::Value* value = params.find(key);
        if (value == nullptr)
            continue;
        if (!value->isIntegral() || value->asInt() < 0 ||
            value->asInt() > static_cast<int64_t>(UINT32_MAX))
            return Reply::failure(kInvalidArgument,
                                  std::string("\"") + key + "\" must be a non-negative integer");
        *field = static_cast<uint32_t>(value->asInt());
    }

    // Phase 42. Checked here whoever sends it, against bounds the file cannot
    // know: what is playing now, and this machine.
    if (const json::Value* value = params.find("memoryLimitMb"); value != nullptr)
    {
        if (!value->isIntegral() || value->asInt() < 0 ||
            value->asInt() > static_cast<int64_t>(UINT32_MAX))
            return Reply::failure(kInvalidArgument,
                                  "\"memoryLimitMb\" must be a non-negative integer");
        const uint64_t mb = static_cast<uint64_t>(value->asInt());
        const uint64_t ceilingZone = server_config::ceilingZoneMb(audio_);
        const uint64_t held = megabytesUp(zones_.memory().usedBytes());
        if (mb < ceilingZone)
            return Reply::failure(kInvalidArgument,
                                  "memoryLimitMb " + std::to_string(mb) +
                                      " is below one zone at " +
                                      describeFormat(audio_.largestFormat()) + ": at least " +
                                      std::to_string(ceilingZone));
        if (mb < held)
            return Reply::failure(kInvalidArgument,
                                  "memoryLimitMb " + std::to_string(mb) +
                                      " is below what the playing zones hold: at least " +
                                      std::to_string(held));
        if (const std::optional<uint64_t> ceiling = memoryLimitCeilingMb(); ceiling && mb > *ceiling)
            return Reply::failure(kInvalidArgument,
                                  "memoryLimitMb " + std::to_string(mb) +
                                      " is above half this machine's memory: at most " +
                                      std::to_string(*ceiling));
        candidate.memoryLimitMb = static_cast<uint32_t>(mb);
    }

    // The config's own rules, checked on the whole file with the change in it,
    // and the file written before anything runs on the new values: a change
    // that is live but not kept would be gone at the next start.
    std::string error;
    if (!persistAudio_(candidate, error))
        return Reply::failure(kInvalidArgument, error);

    const bool changed = candidate.minimumStartBufferMs != audio_.minimumStartBufferMs ||
                         candidate.clientReadyTimeoutMs != audio_.clientReadyTimeoutMs ||
                         candidate.startDelayMs != audio_.startDelayMs ||
                         candidate.seekFlushLeadMs != audio_.seekFlushLeadMs ||
                         candidate.seekFillLeadMs != audio_.seekFillLeadMs ||
                         candidate.memoryLimitMb != audio_.memoryLimitMb;
    audio_ = candidate;

    // From the next play() or boundary on; nothing already playing is cut.
    zones_.memory().setLimitBytes(static_cast<uint64_t>(audio_.memoryLimitMb) * kMb);

    ZoneTiming timing;
    timing.minimumStartBufferNs = static_cast<uint64_t>(audio_.minimumStartBufferMs) * 1000000ULL;
    timing.readyTimeoutNs = static_cast<uint64_t>(audio_.clientReadyTimeoutMs) * 1000000ULL;
    timing.startDelayNs = static_cast<uint64_t>(audio_.startDelayMs) * 1000000ULL;
    timing.seekFlushLeadNs = static_cast<uint64_t>(audio_.seekFlushLeadMs) * 1000000ULL;
    timing.seekFillLeadNs = static_cast<uint64_t>(audio_.seekFillLeadMs) * 1000000ULL;
    zones_.setTiming(timing);
    if (changed)
        MULTIROOM_INFO("control API: start delay {} ms, minimum start buffer {} ms, ready timeout {} ms, "
                 "seek leads {}/{} ms, audio memory {} MB",
                 audio_.startDelayMs, audio_.minimumStartBufferMs, audio_.clientReadyTimeoutMs,
                 audio_.seekFlushLeadMs, audio_.seekFillLeadMs, audio_.memoryLimitMb);

    Reply reply = audioSettings();
    reply.body["changed"] = json::Value(changed);
    return reply;
}

ControlApi::Reply ControlApi::clientSetBuffer(const json::Value& params)
{
    Reply reply;
    std::string name;
    if (!requireString(params, "client", name, reply))
        return reply;
    // The range is the client's to judge -- it refuses with an ERROR rather
    // than clamping -- so only a value that is no target at all stops here.
    const json::Value* value = params.find("bufferMs");
    if (value == nullptr || !value->isIntegral() || value->asInt() <= 0 ||
        value->asInt() > static_cast<int64_t>(UINT32_MAX))
        return Reply::failure(kInvalidArgument, "\"bufferMs\" must be a positive integer");
    const auto bufferMs = static_cast<uint32_t>(value->asInt());

    // Not desired state, like the audio device: the client keeps the target
    // in its own config. A room that has reported one understands section 58;
    // an older client would ignore the request without a word.
    const std::optional<RoomRecord> room = rooms_.find(ClientId(name));
    if (!room || !room->connected || !room->hasBuffer)
        return Reply::failure(kUnavailable,
                              name + " is not connected or has not reported its buffer");

    const bool changed = bufferMs != room->bufferMs;
    if (changed)
    {
        RoomCommand command;
        command.kind = RoomCommandKind::SetBuffer;
        command.client = room->client;
        command.buffer.bufferMs = bufferMs;
        if (!sendToRoom_(command))
            return Reply::failure(kFailed, name + " could not be sent the buffer size");
        MULTIROOM_INFO("control API: {} buffer {} ms", name, bufferMs);
    }

    // The client answers with BUFFER_CONFIG, or refuses with an ERROR and
    // keeps its old target; `clients` shows which.
    reply.body["client"] = json::Value(name);
    reply.body["bufferMs"] = json::Value(bufferMs);
    reply.body["changed"] = json::Value(changed);
    return reply;
}

ControlApi::Reply ControlApi::clientSetName(const json::Value& params)
{
    Reply reply;
    std::string client;
    std::string name;
    if (!requireString(params, "client", client, reply) ||
        !requireString(params, "name", name, reply))
        return reply;
    if (!matp::isValidRoomName(name))
        return Reply::failure(kInvalidArgument, "\"name\" must be 1 to 64 bytes without control "
                                                "characters or surrounding blanks");

    // Not desired state: the client keeps its name (section 60). A room that
    // has said a name on this connection understands the section; an older
    // client would ignore the request without a word.
    const std::optional<RoomRecord> room = rooms_.find(ClientId(client));
    if (!room || !room->connected || !room->hasName)
        return Reply::failure(kUnavailable,
                              client + " is not connected or has not said its name");

    RoomCommand command;
    command.kind = RoomCommandKind::SetRoomName;
    command.client = room->client;
    command.roomName.name = name;
    if (!sendToRoom_(command))
        return Reply::failure(kFailed, client + " could not be sent its name");
    MULTIROOM_INFO("control API: {} to be called \"{}\"", client, name);

    // The client answers with ROOM_NAME -- `room.name` -- or refuses with an
    // ERROR and keeps its old name.
    reply.body["client"] = json::Value(client);
    reply.body["name"] = json::Value(name);
    reply.body["changed"] = json::Value(name != room->name);
    return reply;
}

// A room asleep on Ethernet: one magic packet to the MAC it reported before it
// went away. `wakeOnLan` is the caller's copy of that MAC, used only for a
// room this core does not hold as asleep -- it restarted since.
ControlApi::Reply ControlApi::clientWake(const json::Value& params)
{
    Reply reply;
    std::string name;
    std::string mac;
    if (!requireString(params, "client", name, reply) ||
        !optionalString(params, "wakeOnLan", mac, reply))
    {
        return reply;
    }
    if (!mac.empty() && !isMacAddress(mac))
        return Reply::failure(kInvalidArgument, "\"wakeOnLan\" must be a lower-case MAC address");
    const std::optional<RoomRecord> room = rooms_.find(ClientId(name));
    if (!room && mac.empty())
        return Reply::failure(kNotFound, "no room " + name);
    if (room && !room->connected && room->powerState == "sleep")
        mac = room->wakeOnLan;
    else if ((room && room->connected) || mac.empty())
        return Reply::failure(kUnavailable, name + " is not in a deep sleep the network can end");

    std::string error;
    if (!sendMagicPacket(mac, error))
        return Reply::failure(kFailed, "could not send the magic packet: " + error);
    MULTIROOM_INFO("control API: woke {} ({})", name, mac);
    reply.body["client"] = json::Value(name);
    reply.body["wakeOnLan"] = json::Value(mac);
    return reply;
}

// A room that is gone for good -- a dead device, or one that came back under
// another id. Only one that is away: a connected room would be back at once.
ControlApi::Reply ControlApi::clientForget(const json::Value& params)
{
    Reply reply;
    std::string name;
    if (!requireString(params, "client", name, reply))
        return reply;
    const ClientId client(name);
    const std::optional<RoomRecord> room = rooms_.find(client);
    if (!room)
        return Reply::failure(kNotFound, "no room " + name);
    if (room->connected)
        return Reply::failure(kUnavailable,
                              name + " is connected: only a room that is away can be forgotten");

    releaseRoom(client, "forgotten");
    rooms_.forget(client);
    MULTIROOM_INFO("control API: forgot room {}{}", name,
             room->zone.empty() ? std::string() : " of zone " + room->zone.str());

    json::Value event = eventHead("room.forgotten");
    event["client"] = json::Value(name);
    publish(event);

    reply.body["client"] = json::Value(name);
    if (!room->zone.empty())
        reply.body["zone"] = json::Value(room->zone.str());
    return reply;
}

ControlApi::Reply ControlApi::clientSetDriftCorrection(const json::Value& params)
{
    Reply reply;
    std::string name;
    std::string text;
    if (!requireString(params, "client", name, reply) ||
        !requireString(params, "driftCorrection", text, reply))
        return reply;
    matp::DriftCorrectionMode mode = matp::DriftCorrectionMode::Resampling;
    if (!matp::parseDriftCorrectionMode(text, mode))
        return Reply::failure(kInvalidArgument, "\"driftCorrection\" is " + text +
                                                    ", expected resampling|hardwareClock|auto");

    // Not desired state either: the client keeps it, and only a client that
    // reads drift feedback has the setting at all.
    const std::optional<RoomRecord> room = rooms_.find(ClientId(name));
    if (!room || !room->connected || !room->hasCapabilities || !room->capabilities.driftFeedback)
        return Reply::failure(kUnavailable,
                              name + " is not connected or has no drift-correction setting");
    if (mode == matp::DriftCorrectionMode::HardwareClock && !room->hardwareClock.available)
        return Reply::failure(kInvalidArgument,
                              name + "'s device has no verified hardware clock");

    const bool changed = mode != room->driftCorrection;
    if (changed)
    {
        RoomCommand command;
        command.kind = RoomCommandKind::SetDriftCorrection;
        command.client = room->client;
        command.driftCorrection.driftCorrection = mode;
        if (!sendToRoom_(command))
            return Reply::failure(kFailed, name + " could not be sent the drift correction");
        MULTIROOM_INFO("control API: {} drift correction {}", name, text);
    }

    // The client answers with new capabilities, which renegotiate its stream,
    // or refuses with an ERROR; `clients` shows which.
    reply.body["client"] = json::Value(name);
    reply.body["driftCorrection"] = json::Value(text);
    reply.body["changed"] = json::Value(changed);
    return reply;
}

ControlApi::Reply ControlApi::clientSetVolumeControl(const json::Value& params)
{
    Reply reply;
    std::string name;
    std::string text;
    if (!requireString(params, "client", name, reply) ||
        !requireString(params, "volumeControl", text, reply))
        return reply;
    matp::VolumeControlMode mode = matp::VolumeControlMode::Auto;
    if (!matp::parseVolumeControlMode(text, mode))
        return Reply::failure(kInvalidArgument, "\"volumeControl\" is " + text +
                                                    ", expected auto|hardware|software|disabled");

    // Refused here, not only by the client: under bitPerfect software volume
    // does not exist (section 59), and a request for it is a mistake to say
    // so about rather than to forward.
    const ClientId client(name);
    if (mode == matp::VolumeControlMode::Software &&
        rooms_.preference(client).audioControl == AudioControl::BitPerfect)
        return Reply::failure(kInvalidArgument, "software volume is not available while " +
                                                    name + "'s Audio Control is bitPerfect");

    // Not desired state, like the buffer: the client keeps it in its own
    // config, and one that has reported it understands section 59.
    const std::optional<RoomRecord> room = rooms_.find(client);
    if (!room || !room->connected || !room->hasVolumeConfig)
        return Reply::failure(kUnavailable,
                              name + " is not connected or has not reported its volume control");

    const bool changed = mode != room->volumeConfig.volumeControl;
    if (changed)
    {
        RoomCommand command;
        command.kind = RoomCommandKind::SetVolumeControl;
        command.client = room->client;
        command.volumeControl.volumeControl = mode;
        if (!sendToRoom_(command))
            return Reply::failure(kFailed, name + " could not be sent the volume control");
        MULTIROOM_INFO("control API: {} volume control {}", name, text);
    }

    // The client answers with VOLUME_CONFIG, or refuses with an ERROR and
    // keeps its old setting; `clients` shows which.
    reply.body["client"] = json::Value(name);
    reply.body["volumeControl"] = json::Value(text);
    reply.body["changed"] = json::Value(changed);
    return reply;
}

ControlApi::Reply ControlApi::clientSetInputBindings(const json::Value& params)
{
    Reply reply;
    std::string name;
    std::string inputId;
    if (!requireString(params, "client", name, reply) ||
        !requireString(params, "inputId", inputId, reply))
        return reply;
    const json::Value* bindings = params.find("bindings");
    if (bindings == nullptr || !bindings->isObject())
        return Reply::failure(kInvalidArgument, "\"bindings\" must be an object");

    // Not desired state, like the audio device: the client keeps its bindings
    // in its own config, so only a connected room that has reported them can
    // be asked.
    const std::optional<RoomRecord> room = rooms_.find(ClientId(name));
    if (!room || !room->connected || !room->hasInputs)
        return Reply::failure(kUnavailable, name + " is not connected or has reported no inputs");

    const auto input = std::find_if(room->inputs.begin(), room->inputs.end(),
                                    [&inputId](const matp::InputControl& c) { return c.id == inputId; });
    if (input == room->inputs.end())
        return Reply::failure(kInvalidArgument, name + " reported no input \"" + inputId + "\"");

    // Checked here with the same code the client runs, so a caller learns
    // what is wrong from this answer rather than from an ERROR in the log.
    matp::InputBindings merged = input->bindings;
    std::string error;
    if (!matp::inputBindingsFromJson(*bindings, input->kind, merged, error))
        return Reply::failure(kInvalidArgument, error);

    const bool changed = merged != input->bindings;
    if (changed)
    {
        RoomCommand command;
        command.kind = RoomCommandKind::SetInputBindings;
        command.client = room->client;
        command.inputBindings.inputId = inputId;
        command.inputBindings.bindings = *bindings;
        if (!sendToRoom_(command))
            return Reply::failure(kFailed, name + " could not be sent the bindings");
        MULTIROOM_INFO("control API: {} input {} rebound", name, inputId);
    }

    // The client answers with INPUT_CONTROLS; `clients` shows the result.
    reply.body["client"] = json::Value(name);
    reply.body["inputId"] = json::Value(inputId);
    reply.body["bindings"] = matp::inputBindingsToJson(input->kind, merged);
    reply.body["changed"] = json::Value(changed);
    return reply;
}

ControlApi::Reply ControlApi::resamplerConfigure(const json::Value& params)
{
    Reply reply;
    std::string id;
    if (!requireString(params, "resampler", id, reply))
        return reply;

    const bool changed = id != planner_.get()->resamplerId();
    std::optional<OutputPlanner> created;
    if (changed)
    {
        auto made = OutputPlanner::create(registry_, id);
        if (!made)
            return Reply::failure(kInvalidArgument, made.error().detail);
        created.emplace(std::move(*made));
    }

    // Written before it is used, as audio.configure does: the next core start
    // reads audio.resampler, and a change that is live but not kept would be
    // gone then.
    if (hasAudio_ && persistAudio_ && audio_.resampler != id)
    {
        AudioConfig candidate = audio_;
        candidate.resampler = id;
        std::string error;
        if (!persistAudio_(candidate, error))
            return Reply::failure(kInvalidArgument, error);
        audio_ = std::move(candidate);
    }

    json::Value renegotiated = json::Value::makeArray();
    if (changed)
    {
        // Phase 6's rule: every resampled room of a house is converted by one
        // filter, never a mix. Zones plan from a snapshot of the planner, so
        // it can be swapped while they play; a room planned before the swap
        // keeps the old filter until it is re-negotiated, so every zoned room
        // the new planner serves differently is, at the cost of a short
        // drop-out. A bitperfect room the converter never touched keeps
        // playing.
        planner_.set(std::move(*created));
        MULTIROOM_INFO("control API: resampler {}", id);
        for (const RoomRecord& room : rooms_.rooms())
        {
            const std::shared_ptr<Zone> zone = room.connected ? zones_.zoneOf(room.client) : nullptr;
            if (zone == nullptr || !zone->planOutdated(room.client))
                continue;
            renegotiate(room, "replanned");
            renegotiated.pushBack(json::Value(room.client.str()));
        }
    }

    reply.body["resampler"] = json::Value(id);
    reply.body["variableRatio"] =
        json::Value(planner_.get()->resamplerCapabilities().variableRatio);
    reply.body["changed"] = json::Value(changed);
    reply.body["renegotiated"] = std::move(renegotiated);
    return reply;
}

// ---------------------------------------------------------------------------

void ControlApi::replan(const ClientId& client)
{
    const std::optional<RoomRecord> room = rooms_.find(client);
    if (room && room->connected && !room->zone.empty())
        renegotiate(*room, "replanned");
}

void ControlApi::regroup(const ClientId& client)
{
    // A bitPerfect room is planned the same grouped or not, unless its own
    // clock can be trimmed -- which only the zone that holds it knows. A room
    // left alone gets the plan it had before anyone joined, from its next
    // track while its zone plays.
    if (const std::shared_ptr<Zone> zone = zones_.zoneOf(client))
    {
        const Replan replanned = rooms_.preference(client).grouped ? zone->replanInPlace(client)
                                                                   : zone->replanAlone(client);
        if (replanned == Replan::Renegotiate)
            replan(client);
        return;
    }
    if (rooms_.preference(client).audioControl == AudioControl::Auto)
        replan(client);
}

void ControlApi::replanAloneRooms(Zone& zone)
{
    // A line that does not play has no boundary to wait for.
    for (const ClientId& client : zone.pendingAlone())
    {
        if (zone.replanAlone(client) == Replan::Renegotiate)
            replan(client);
    }
}

void ControlApi::renegotiate(const RoomRecord& room, const char* cause)
{
    // Out of whatever zone it is in first: the stream it has is the old
    // answer's, and a room in a zone keeps being sent it.
    releaseRoom(room.client, cause);

    RoomCommand command;
    command.kind = RoomCommandKind::Renegotiate;
    command.client = room.client;
    if (!sendToRoom_(command))
        MULTIROOM_WARN("control API: {} could not be asked to re-negotiate", room.client.str());
}

void ControlApi::releaseRoom(const ClientId& client, const char* cause)
{
    const std::shared_ptr<Zone> owner = zones_.zoneOf(client);
    zones_.release(client);
    if (owner == nullptr)
        return;

    json::Value event = eventHead("room.left");
    event["client"] = json::Value(client.str());
    event["zone"] = json::Value(owner->id().str());
    event["cause"] = json::Value(cause);
    publish(event);
}

void ControlApi::announceNotReady(const ClientId& client, const ZoneId& zone,
                                  const std::string& reason)
{
    json::Value event = eventHead("room.notReady");
    event["client"] = json::Value(client.str());
    event["zone"] = json::Value(zone.str());
    event["reason"] = json::Value(reason);
    publish(event);
}

void ControlApi::publish(const json::Value& event)
{
    if (publishEvent_)
        publishEvent_(encode(event));
}

std::string ControlApi::eventLine(const ZoneEvent& event)
{
    json::Value out;
    switch (event.kind)
    {
    case ZoneEventKind::SourceNeeded:
        out = eventHead("zone.sourceNeeded");
        out["zone"] = json::Value(event.zone.str());
        out["afterSourceId"] = json::Value(event.sourceId);
        if (event.remainingNs)
            out["remainingNs"] = json::Value(*event.remainingNs);
        break;
    case ZoneEventKind::SourceStarted:
        out = eventHead("zone.sourceStarted");
        out["zone"] = json::Value(event.zone.str());
        out["sourceId"] = json::Value(event.sourceId);
        if (!event.trackId.empty())
            out["trackId"] = json::Value(event.trackId);
        out["timelineNs"] = json::Value(std::to_underlying(event.timelineNs));
        out["serverTimeNs"] = json::Value(event.serverTimeNs);
        out["startOffsetNs"] = json::Value(event.startOffsetNs);
        break;
    case ZoneEventKind::SourceFailed:
        out = eventHead("zone.sourceFailed");
        out["zone"] = json::Value(event.zone.str());
        out["sourceId"] = json::Value(event.sourceId);
        out["reason"] = json::Value(event.reason);
        break;
    case ZoneEventKind::Ended:
        out = eventHead("zone.ended");
        out["zone"] = json::Value(event.zone.str());
        out["afterSourceId"] = json::Value(event.sourceId);
        break;
    case ZoneEventKind::Transport:
        out = eventHead("zone.transport");
        out["zone"] = json::Value(event.zone.str());
        out["state"] = json::Value(transportStateName(event.transport));
        out["timelineNs"] = json::Value(std::to_underlying(event.timelineNs));
        break;
    case ZoneEventKind::RoomNotReady:
        out = eventHead("room.notReady");
        out["client"] = json::Value(event.client.str());
        out["zone"] = json::Value(event.zone.str());
        out["reason"] = json::Value(event.reason);
        break;
    case ZoneEventKind::RoomFellBehind:
        out = eventHead("room.fellBehind");
        out["client"] = json::Value(event.client.str());
        out["zone"] = json::Value(event.zone.str());
        out["trigger"] = json::Value(triggerKey(event.trigger));
        out["timelineNs"] = json::Value(std::to_underlying(event.timelineNs));
        break;
    case ZoneEventKind::RoomLeftBehind:
        out = eventHead("room.leftBehind");
        out["client"] = json::Value(event.client.str());
        out["zone"] = json::Value(event.zone.str());
        out["detail"] = json::Value(event.reason);
        break;
    case ZoneEventKind::RoomUnstable:
        out = eventHead("room.unstable");
        out["client"] = json::Value(event.client.str());
        out["zone"] = json::Value(event.zone.str());
        out["trigger"] = json::Value(triggerKey(event.trigger));
        out["rejoins"] = json::Value(event.rejoins);
        out["backoffMs"] = json::Value(millisecondsOf(event.backoffNs));
        out["detail"] = json::Value(event.reason);
        break;
    case ZoneEventKind::RoomStable:
        out = eventHead("room.stable");
        out["client"] = json::Value(event.client.str());
        out["zone"] = json::Value(event.zone.str());
        out["rejoins"] = json::Value(event.rejoins);
        out["unstableForMs"] = json::Value(millisecondsOf(event.unstableForNs));
        break;
    }
    return encode(out);
}

// ---------------------------------------------------------------------------

ClientSessionHooks ControlApi::sessionHooks()
{
    ClientSessionHooks hooks;

    // What SESSION_CONFIG announces: the planner's answer for the zone this
    // room belongs in, or the room's own ceiling when it belongs in none.
    hooks.onChooseFormat = [this](const ClientSession& session, AudioFormat& format) {
        const bool arriving = rooms_.onCapabilities(session);
        const ClientId& client = session.clientId();
        const ZoneId wanted = rooms_.zoneFor(client);
        const std::shared_ptr<Zone> zone = zones_.find(wanted);

        // The first moment the core knows who a room is and what its device
        // can do. A return is a room its zone kept state for (phase 16).
        if (arriving)
        {
            json::Value event = eventHead("room.connected");
            event["client"] = json::Value(client.str());
            event["deviceName"] = json::Value(session.deviceName());
            if (!session.peerAddress().empty())
                event["address"] = json::Value(session.peerAddress());
            event["reconnect"] = json::Value(zone != nullptr && zone->retains(client));
            if (!wanted.empty())
                event["zone"] = json::Value(wanted.str());
            publish(event);
        }

        if (zone == nullptr)
            return false;

        const auto planned = zone->planFor(client, session.capabilities());
        if (!planned)
        {
            // That room's problem and not the zone's: it is told its own
            // ceiling instead, and status says why it was not admitted.
            MULTIROOM_WARN("room {}: {}", client.str(), describe(planned.error()));
            announceNotReady(client, wanted, describe(planned.error()));
            return false;
        }
        format = planned->outputFormat;
        return true;
    };

    hooks.onStreamReady = [this](const ClientSession& session) {
        const auto previous = rooms_.find(session.clientId());
        rooms_.onStreamReady(session);

        if (!session.powerState().empty() &&
            (!previous || previous->powerState != session.powerState()))
        {
            json::Value event = eventHead("room.powerState");
            event["client"] = json::Value(session.clientId().str());
            event["powerState"] = json::Value(session.powerState());
            if (!session.wakeOnLan().empty())
                event["wakeOnLan"] = json::Value(session.wakeOnLan());
            publish(event);
        }

        const ClientId& client = session.clientId();
        const ZoneId wanted = rooms_.zoneFor(client);

        // Asked into a zone and silent: its device would not open.
        if (!session.streamReady() && !session.streamStandby() && !wanted.empty())
        {
            announceNotReady(client, wanted,
                             session.streamNotReadyReason().empty()
                                 ? std::string("the device did not open")
                                 : session.streamNotReadyReason());
        }

        // A room already in the zone it belongs in has answered again: a
        // device that was reopened, or one that stopped being ready. Section
        // 20's table keeps everything about it and only drops it from
        // scheduling, so it is told rather than re-admitted.
        if (const std::shared_ptr<Zone> owner = zones_.zoneOf(client); owner != nullptr)
        {
            if (owner->id() == wanted)
            {
                owner->onStreamReady(client, session.streamReady(), session.grantedFormat(),
                                     &session.capabilities());
                return;
            }
            releaseRoom(client, "moved"); // a zone it no longer belongs in
        }

        // A device that did not open joins nothing yet; the zone that
        // announced it a format waits for it until the ready timeout.
        if (!session.streamReady() || wanted.empty())
            return;

        if (const auto assigned = zones_.assign(wanted, session.sessionId(), client,
                                                session.capabilities(), session.grantedFormat());
            !assigned)
        {
            // One room refused, and every other room of the zone untouched --
            // the fault-isolation table's second row.
            MULTIROOM_WARN("room {}: {}", client.str(), describe(assigned.error()));
            announceNotReady(client, wanted, describe(assigned.error()));
            return;
        }

        const std::shared_ptr<Zone> zone = zones_.zoneOf(client);
        const std::shared_ptr<ClientOutput> output =
            zone != nullptr ? zone->member(client) : nullptr;
        if (output == nullptr)
            return; // released again by another thread in between

        // What was asked for this room, put back on every admission: a device
        // that was reopened or a room that reconnected starts at the client's
        // own default otherwise (phase 25).
        if (const std::optional<RoomRecord> room = rooms_.find(client);
            room && (room->volume || room->muted))
        {
            matp::SetVolume request;
            request.hasValue = room->volume.has_value();
            request.value = room->volume.value_or(request.value);
            request.hasMuted = room->muted.has_value();
            request.muted = room->muted.value_or(false);
            if (const auto sent = zone->setVolume(client, request); !sent)
                MULTIROOM_WARN("room {}: volume not sent: {}", client.str(), describe(sent.error()));
        }

        // Phase 27's latency, the same way and on the same thread as the
        // admission, so it is in place before this room's first BUFFER_STATUS
        // can make it schedulable.
        if (const std::optional<RoomRecord> room = rooms_.find(client);
            room && room->outputLatencyMs)
        {
            const auto set = zone->setOutputLatency(client, *room->outputLatencyMs * 1000000ULL);
            if (!set)
                MULTIROOM_WARN("room {}: output latency not set: {}", client.str(),
                         describe(set.error()));
        }

        const OutputPlan plan = output->plan();
        json::Value event = eventHead("room.joined");
        event["client"] = json::Value(client.str());
        event["zone"] = json::Value(wanted.str());
        event["bitPerfect"] = json::Value(plan.mode == OutputMode::BitPerfect);
        event["reason"] = json::Value(planReasonText(plan.reason));
        event["outputFormat"] = formatJson(plan.outputFormat);
        event["driftCorrected"] = json::Value(plan.driftCorrected());
        event["driftActuator"] = json::Value(driftActuatorName(plan.driftActuator));
        publish(event);
    };

    hooks.onPrepareStream = [this](const ClientSession& session) {
        if (const std::shared_ptr<Zone> owner = zones_.zoneOf(session.clientId()); owner != nullptr)
            owner->releaseAudioConnection(session.clientId());
    };

    hooks.onBufferStatus = [this](const ClientSession& session, const matp::BufferStatus& status) {
        if (const std::shared_ptr<Zone> owner = zones_.zoneOf(session.clientId());
            owner != nullptr)
        {
            owner->onBufferStatus(session.clientId(), status, session.clockSyncResults());
        }
    };

    hooks.onPlaybackReport = [this](const ClientSession& session,
                                    const matp::PlaybackReport& report) {
        rooms_.onPlaybackReport(session.clientId(), report, clock_.monotonicNowNs());

        // The session's clock model goes with the report: sections 25 and 26
        // are both on the client's own axes, and phase 8's model is the only
        // way onto this server's.
        if (const std::shared_ptr<Zone> owner = zones_.zoneOf(session.clientId());
            owner != nullptr)
        {
            owner->onPlaybackReport(session.clientId(), report, session.clock());
        }
    };

    hooks.onSystemReport = [this](const ClientSession& session, const SystemMetrics& metrics) {
        rooms_.onSystemReport(session, metrics, clock_.monotonicNowNs());
    };

    // What a room's client keeps and reports is announced as it arrives, so
    // that a reader never has to ask again to learn that it changed.
    hooks.onCapabilitiesChanged = [this](const ClientSession& session) {
        json::Value event = eventHead("room.capabilities");
        event["client"] = json::Value(session.clientId().str());
        event["device"] = json::Value(session.deviceName());
        event["capabilities"] = capabilitiesJson(session.capabilities(), session.hasHardwareVolume(),
                                                 session.hardwareVolume(), session.hardwareClock());
        publish(event);
    };

    hooks.onAudioDevices = [this](const ClientSession& session,
                                  const matp::AudioDeviceList& devices) {
        if (!rooms_.onAudioDevices(session, devices))
            return;
        json::Value event = eventHead("room.audioDevices");
        event["client"] = json::Value(session.clientId().str());
        event["audioDevices"] = audioDevicesJson(devices);
        publish(event);
    };

    hooks.onBufferConfig = [this](const ClientSession& session, uint32_t bufferMs) {
        if (!rooms_.onBufferConfig(session, bufferMs))
            return;
        json::Value event = eventHead("room.bufferConfig");
        event["client"] = json::Value(session.clientId().str());
        event["bufferMs"] = json::Value(bufferMs);
        publish(event);
    };

    hooks.onRoomName = [this](const ClientSession& session, const std::string& name) {
        if (!rooms_.onRoomName(session, name))
            return;
        json::Value event = eventHead("room.name");
        event["client"] = json::Value(session.clientId().str());
        event["name"] = json::Value(name);
        publish(event);
    };

    hooks.onAudioControl = [this](const ClientSession& session) {
        return rooms_.preference(session.clientId()).audioControl;
    };

    hooks.onVolumeConfig = [this](const ClientSession& session,
                                  const matp::VolumeConfig& config) {
        if (!rooms_.onVolumeConfig(session, config))
            return;
        json::Value event = eventHead("room.volumeControl");
        event["client"] = json::Value(session.clientId().str());
        event["volumeControl"] = volumeControlJson(config);
        publish(event);
    };

    hooks.onInputControls = [this](const ClientSession& session,
                                   const std::vector<matp::InputControl>& inputs, bool deepSleep) {
        rooms_.onInputControls(session, inputs, deepSleep);
    };

    // Section 57: the core only announces. What `togglePause` or `volumeUp`
    // means -- the zone's queue, the room's desired volume -- is the high-end
    // server's, which owns both.
    hooks.onInputAction = [this](const ClientSession& session,
                                 const matp::InputActionEvent& action) {
        json::Value event = eventHead("room.input");
        event["client"] = json::Value(session.clientId().str());
        if (const std::shared_ptr<Zone> owner = zones_.zoneOf(session.clientId());
            owner != nullptr)
            event["zone"] = json::Value(owner->id().str());
        // The binding's fields in the wire's own spelling, less its envelope.
        for (const json::Value::Member& member : matp::makeInputAction({}, action).members())
        {
            if (member.first != "type" && member.first != "sessionId")
                event[member.first] = member.second;
        }
        publish(event);
    };

    // Section 43: a connection going away is not a room going away. The zone
    // keeps the room's sync state and the directory keeps what was asked for
    // it, so the reconnect is an ordinary handshake and an ordinary admit.
    hooks.onClosed = [this](const ClientSession& session) {
        const bool going = rooms_.onClosed(session);
        const std::shared_ptr<Zone> owner = zones_.zoneOf(session.clientId());
        zones_.disconnect(session.clientId(), session.sessionId());

        // Only the connection the room was known by: a displaced session
        // closing after its successor arrived is not the room going away.
        if (!going)
            return;
        json::Value event = eventHead("room.disconnected");
        event["client"] = json::Value(session.clientId().str());
        event["reason"] = json::Value(disconnectReason(session.lastError()));
        if (owner != nullptr)
            event["zone"] = json::Value(owner->id().str());
        publish(event);
    };

    return hooks;
}

void ControlApi::sampleSystem()
{
    const uint64_t now = clock_.monotonicNowNs();
    if (now < systemNextNs_)
        return;
    systemNextNs_ = now + kSystemSampleIntervalNs;

    const SystemMetrics sample = systemSampler_.sample();
    std::lock_guard<std::mutex> lock(systemMutex_);
    hasSystem_ = true;
    system_ = sample;
}

// ---------------------------------------------------------------------------

json::Value ControlApi::serverJson() const
{
    size_t connected = 0;
    size_t members = 0;
    const std::vector<RoomRecord> rooms = rooms_.rooms();
    for (const RoomRecord& room : rooms)
    {
        connected += room.connected ? 1 : 0;
        members += zones_.zoneOf(room.client) != nullptr ? 1 : 0;
    }

    json::Value server = json::Value::makeObject();
    server["version"] = json::Value(MULTIROOM_SERVER_VERSION);
    server["uptimeMs"] = json::Value(millisecondsOf(clock_.monotonicNowNs() - startedAtNs_));
    if (const std::shared_ptr<const OutputPlanner> planner = planner_.get(); planner->hasResampler())
        server["resampler"] = json::Value(planner->resamplerId());
    server["zones"] = json::Value(static_cast<uint64_t>(zones_.size()));
    server["clients"] = json::Value(static_cast<uint64_t>(rooms.size()));
    server["connected"] = json::Value(static_cast<uint64_t>(connected));
    server["members"] = json::Value(static_cast<uint64_t>(members));
    // Phase 42: every zone's audio together, against its one ceiling.
    server["memoryUsedBytes"] = json::Value(zones_.memory().usedBytes());
    server["memoryLimitBytes"] = json::Value(zones_.memory().limitBytes());

    std::lock_guard<std::mutex> lock(systemMutex_);
    if (hasSystem_)
        server["system"] = matp::makeSystemMetrics(system_);
    return server;
}

json::Value ControlApi::clientJson(const RoomRecord& room, uint64_t nowNs) const
{
    json::Value out = json::Value::makeObject();
    out["clientId"] = json::Value(room.client.str());
    if (!room.name.empty())
        out["name"] = json::Value(room.name);
    out["connected"] = json::Value(room.connected);
    if (room.connected)
        out["session"] = json::Value(room.session.toHex());
    if (!room.deviceName.empty())
        out["device"] = json::Value(room.deviceName);
    if (!room.address.empty())
        out["address"] = json::Value(room.address);

    if (room.hasCapabilities)
        out["capabilities"] =
            capabilitiesJson(room.capabilities, room.hasHardwareVolume, room.hardwareVolume,
                             room.hardwareClock);

    if (room.streamAnswered)
    {
        json::Value stream = json::Value::makeObject();
        stream["ready"] = json::Value(room.streamReady);
        if (!room.powerState.empty())
            stream["powerState"] = json::Value(room.powerState);
        if (room.streamStandby)
            stream["standby"] = json::Value(true);
        if (room.streamReady)
            stream["format"] = formatJson(room.grantedFormat);
        else if (!room.streamNotReadyReason.empty())
            stream["reason"] = json::Value(room.streamNotReadyReason);
        out["stream"] = std::move(stream);
    }
    else if (!room.connected && !room.powerState.empty())
        out["powerState"] = json::Value(room.powerState); // asleep, reported before it left
    if (!room.wakeOnLan.empty())
        out["wakeOnLan"] = json::Value(room.wakeOnLan);

    if (!room.zone.empty())
        out["zone"] = json::Value(room.zone.str());

    // The settings as stored, and where the Audio Control came from: a room
    // with none of its own follows the server's default when that changes.
    const RoomAudioSettings wish = rooms_.preference(room.client);
    json::Value settings = json::Value::makeObject();
    settings["audioControl"] = json::Value(audioControlName(wish.audioControl));
    settings["audioControlIsDefault"] = json::Value(!room.audioControl.has_value());
    settings["grouped"] = json::Value(wish.grouped);
    // Phase 25: what was asked for, and nothing where nothing was. The
    // client's own default is not this server's to report.
    if (room.volume)
        settings["volume"] = json::Value(*room.volume);
    if (room.muted)
        settings["muted"] = json::Value(*room.muted);
    if (room.outputLatencyMs)
        settings["outputLatencyMs"] = json::Value(*room.outputLatencyMs);
    if (room.equalizer)
        settings["equalizer"] = equalizerJson(*room.equalizer);
    out["settings"] = std::move(settings);

    const std::shared_ptr<Zone> zone = zones_.zoneOf(room.client);
    const std::shared_ptr<ClientOutput> output =
        zone != nullptr ? zone->member(room.client) : nullptr;
    out["member"] = json::Value(output != nullptr);

    // Whether the room is corrected, and if not, why. A grouped room asks for
    // it; so does a room resampled because its device cannot play the source,
    // which only a plan knows. Before there is a plan only the grouping and
    // the server's converter can say why not.
    bool requested = wish.grouped;
    json::Value drift = json::Value::makeObject();
    DriftDisabledReason disabledBy = !wish.grouped ? DriftDisabledReason::NotDisabled
                                     : wish.audioControl == AudioControl::BitPerfect
                                         ? DriftDisabledReason::BitperfectForced
                                         : planner_.get()->correctionUnavailable();
    if (output != nullptr)
    {
        const OutputPlan plan = output->plan();
        drift["active"] = json::Value(plan.driftCorrected());
        drift["actuator"] = json::Value(driftActuatorName(plan.driftActuator));
        disabledBy = plan.driftCorrected() ? DriftDisabledReason::NotDisabled : plan.driftDisabledBy;
        requested = requested || plan.driftCorrected() ||
                    disabledBy != DriftDisabledReason::NotDisabled;
    }
    drift["requested"] = json::Value(requested);
    if (requested && disabledBy != DriftDisabledReason::NotDisabled)
    {
        drift["disabledBy"] = json::Value(driftDisabledText(disabledBy));
    }
    // The client's own setting, for a client that has one, and what its
    // hardware clock reported last while it is the actuator.
    if (room.hasCapabilities && room.capabilities.driftFeedback)
    {
        drift["mode"] = json::Value(matp::driftCorrectionModeName(room.driftCorrection));
        drift["hardwareClockAvailable"] = json::Value(room.hardwareClock.available);
    }
    if (room.hasReport && room.lastReport.hasHardwareClock)
    {
        json::Value clock = json::Value::makeObject();
        clock["appliedPpm"] = json::Value(room.lastReport.hardwareClockPpm);
        clock["saturated"] = json::Value(room.lastReport.hardwareClockSaturated);
        if (!room.lastReport.hardwareClockError.empty())
            clock["error"] = json::Value(room.lastReport.hardwareClockError);
        drift["hardwareClock"] = std::move(clock);
    }
    out["driftCorrection"] = std::move(drift);

    if (output != nullptr)
    {
        const OutputPlan plan = output->plan();
        json::Value o = json::Value::makeObject();
        o["state"] = json::Value(outputStateName(output->state()));
        o["sourceFormat"] = formatJson(plan.sourceFormat);
        o["format"] = formatJson(plan.outputFormat);
        o["bitPerfect"] = json::Value(plan.mode == OutputMode::BitPerfect);
        o["reason"] = json::Value(planReasonText(plan.reason));
        o["plan"] = json::Value(describePlan(plan));
        // Left alone while its zone plays: this plan until its next track.
        o["replanPending"] = json::Value(zone->replanPending(room.client));
        if (!plan.resamplerId.empty())
            o["resampler"] = json::Value(plan.resamplerId);
        o["ratio"] = json::Value(output->driftTrim());
        o["packets"] = json::Value(output->packets());
        o["queuedPackets"] = json::Value(static_cast<uint64_t>(output->queuedPackets()));
        o["bufferedMs"] = json::Value(millisecondsOf(output->bufferedNs()));
        // A gap in the room's buffer reports is the first sign of a link
        // going bad, long before it is an eviction.
        if (const std::optional<uint64_t> age = output->bufferReportAgeNs())
            o["bufferReportAgeMs"] = json::Value(millisecondsOf(*age));
        o["longestReportGapMs"] = json::Value(millisecondsOf(output->longestReportGapNs()));
        o["reportGaps"] = json::Value(output->reportGaps());
        o["pacedOnLine"] = json::Value(output->pacedOnLine());
        o["queueFull"] = json::Value(output->queueFullCount());
        o["starved"] = json::Value(output->starvedCount());
        o["connectionDrops"] = json::Value(output->connectionDrops());
        // On and changing samples, which a bitperfect stream never is.
        json::Value equalizer = json::Value::makeObject();
        equalizer["active"] = json::Value(output->equalizing());
        equalizer["clippedSamples"] = json::Value(output->equalizerClippedSamples());
        o["equalizer"] = std::move(equalizer);
        // Phase 39: how far this room has been built past the line, which is
        // negative for a room that is behind it. No line, no figure.
        if (zone->started())
        {
            o["producedAheadNs"] =
                json::Value(static_cast<int64_t>(std::to_underlying(output->producedThroughNs())) -
                            static_cast<int64_t>(std::to_underlying(zone->playheadNs())));
        }
        if (const std::string error = output->lastError(); !error.empty())
            o["lastError"] = json::Value(error);
        out["output"] = std::move(o);

        out["start"] = json::Value(roomStartStateName(zone->roomStart(room.client)));
        if (const std::string detail = zone->roomStartDetail(room.client); !detail.empty())
            out["startDetail"] = json::Value(detail);

        // Honest reporting: what has not been measured is omitted, not zero.
        const ClientSyncStatus sync = zone->syncStatus(room.client);
        json::Value s = json::Value::makeObject();
        if (sync.hasDrift)
        {
            s["driftPpm"] = json::Value(sync.driftPpm);
            s["standardErrorPpm"] = json::Value(sync.standardErrorPpm);
            s["confidence"] = json::Value(sync.confidence);
            s["clockQuality"] = json::Value(audioClockQualityName(sync.clockQuality));
        }
        if (sync.hasOffset)
            s["offsetUs"] = json::Value(static_cast<double>(sync.offsetNs) / 1000.0);
        if (sync.targetOffsetNs != 0)
            s["targetOffsetUs"] = json::Value(static_cast<double>(sync.targetOffsetNs) / 1000.0);
        s["correctionPpm"] = json::Value(sync.correctionPpm);
        s["appliedPpm"] = json::Value(sync.appliedPpm);
        if (sync.hardwareClock)
            s["hardwareTargetPpm"] = json::Value(sync.hardwareTargetPpm);
        out["sync"] = std::move(s);
    }

    // What the room's own card said, and how long ago: a report that stopped
    // arriving is the first thing to know about a room that went quiet.
    if (room.hasReport)
    {
        json::Value r = json::Value::makeObject();
        r["bufferedMs"] = json::Value(millisecondsOf(room.lastReport.bufferedDurationNs));
        r["underruns"] = json::Value(room.lastReport.underruns);
        r["ageMs"] = json::Value(millisecondsOf(nowNs - std::min(nowNs, room.lastReportNs)));
        out["playback"] = std::move(r);
    }

    // Section 55: how the room's machine is doing, and how long ago it said
    // so. Absent until the first report of this connection.
    if (room.hasSystem)
    {
        out["system"] = matp::makeSystemMetrics(room.system);
        out["systemAgeMs"] = json::Value(millisecondsOf(nowNs - std::min(nowNs, room.systemNs)));
    }

    // Section 56, in the client's own spelling. `selected` is absent while the
    // room has no usable device; `configured` is empty for its default.
    if (room.hasAudioDevices)
        out["audioDevices"] = audioDevicesJson(room.audioDevices);

    // Section 58: the size of the room's own buffer, as it last reported it.
    if (room.hasBuffer)
        out["bufferMs"] = json::Value(room.bufferMs);

    // Section 59: the room's Volume Control, the path its client resolved
    // from it and, where that is not the one asked for, why.
    if (room.hasVolumeConfig)
        out["volumeControl"] = volumeControlJson(room.volumeConfig);

    // Section 57, in the client's own spelling.
    if (room.hasInputs)
    {
        out["inputs"] = *matp::makeInputControls({}, room.inputs).find("inputs");
        out["deepSleep"] = json::Value(room.deepSleep);
    }

    // Survives a disconnect, like the room it counts.
    if (const std::shared_ptr<Zone> home = zones_.find(room.zone); home != nullptr)
        out["rejoins"] = json::Value(home->rejoins(room.client));

    // Phase 41: a room that keeps falling behind, and how long it has. What
    // the events said, for a high-end server that was not listening.
    if (output != nullptr)
    {
        const RoomStability stability = zone->stability(room.client);
        out["unstable"] = json::Value(stability.unstable);
        out["backoffMs"] = json::Value(millisecondsOf(stability.backoffNs));
        if (stability.unstable)
        {
            out["episodeRejoins"] = json::Value(stability.rejoins);
            out["unstableSinceNs"] = json::Value(stability.unstableSinceNs);
        }
    }

    return out;
}

json::Value ControlApi::zoneJson(const Zone& zone) const
{
    json::Value out = json::Value::makeObject();
    out["zone"] = json::Value(zone.id().str());
    out["state"] = json::Value(zoneStateName(zone.state()));
    out["transport"] = json::Value(transportStateName(zone.transport()));
    if (const std::string error = zone.lastError(); !error.empty())
        out["lastError"] = json::Value(error);

    const TimelineNs playhead = zone.playheadNs();
    if (zone.started())
    {
        out["timelineNs"] = json::Value(std::to_underlying(playhead));
        // Phase 39: how far the decode has run past the line.
        out["frontierAheadNs"] =
            json::Value(static_cast<int64_t>(std::to_underlying(zone.frontierNs())) -
                        static_cast<int64_t>(std::to_underlying(playhead)));
    }
    if (const AudioFormat format = zone.sourceFormat(); format.isValid())
        out["sourceFormat"] = formatJson(format);

    // The source that is audible, which is not always the one being decoded:
    // the decode runs a lookahead ahead of the rooms, across a boundary too.
    const std::vector<StagedSource> sources = zone.sources();
    const StagedSource* current = nullptr;
    for (const StagedSource& source : sources)
    {
        if (current == nullptr ||
            std::to_underlying(source.timelineStartNs) <= std::to_underlying(playhead))
        {
            current = &source;
        }
    }
    if (current != nullptr)
    {
        json::Value source = segmentJson(current->segment);
        source["timelineStartNs"] = json::Value(std::to_underlying(current->timelineStartNs));
        source["ended"] = json::Value(current->ended);
        out["source"] = std::move(source);
    }

    json::Value staged = json::Value::makeArray();
    for (const SourceSegment& segment : zone.staged())
        staged.pushBack(segmentJson(segment));
    out["staged"] = std::move(staged);

    // Phase 22: what was skipped and why, so a high-end server that polls
    // learns that a track did not play rather than inferring it from a
    // boundary that came early.
    json::Value failed = json::Value::makeArray();
    for (const FailedSource& source : zone.failedSources())
    {
        json::Value entry = json::Value::makeObject();
        entry["sourceId"] = json::Value(source.sourceId);
        entry["reason"] = json::Value(source.reason);
        failed.pushBack(std::move(entry));
    }
    out["failedSources"] = std::move(failed);

    out["members"] = idList(zone.memberIds());

    std::vector<ClientId> wanted;
    for (const RoomRecord& room : rooms_.rooms())
    {
        if (room.zone == zone.id())
            wanted.push_back(room.client);
    }
    out["clients"] = idList(wanted);

    out["startsSent"] = json::Value(zone.startsSent());
    out["rejoins"] = json::Value(zone.rejoinCount());
    // Phase 42: what this zone holds, which is nothing unless it plays.
    out["blocks"] = json::Value(static_cast<uint64_t>(zone.blocks()));
    out["memoryBytes"] = json::Value(zone.memoryBytes());
    out["blocksRefused"] = json::Value(zone.blocksRefused());
    return out;
}

} // namespace multiroom
