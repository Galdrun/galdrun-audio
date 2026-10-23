#include "control/RoomDirectory.h"

#include <algorithm>
#include <utility>

namespace multiroom {

namespace {

// Something the control API asked for, which keeps a record whatever its
// connection does.
bool configured(const RoomRecord& record)
{
    return !record.zone.empty() || record.audioControl || record.grouped || record.volume ||
           record.muted || record.outputLatencyMs || record.equalizer;
}

// What a connection says about itself and the client says again on every
// session: untrue of the room once that connection has gone or been replaced.
void clearLiveReports(RoomRecord& record)
{
    record.streamAnswered = false;
    record.streamReady = false;
    record.streamStandby = false;
    record.hasSystem = false;
    record.hasAudioDevices = false;
    record.hasBuffer = false;
    record.hasVolumeConfig = false;
    record.hasName = false;
    record.hasInputs = false;
    record.inputs.clear();
    record.deepSleep = false;
}

} // namespace

RoomDirectory::RoomDirectory(RoomAudioSettings defaults, size_t maxRooms)
    : defaults_(defaults), maxRooms_(maxRooms)
{
}

bool RoomDirectory::onCapabilities(const ClientSession& session)
{
    std::lock_guard<std::mutex> lock(mutex_);
    RoomRecord& record = rooms_[session.clientId()];
    record.client = session.clientId();
    const bool arriving = !record.connected || record.session != session.sessionId();

    // A new session is a new connection, and nothing the last one said about
    // its stream is true of this one.
    if (record.session != session.sessionId())
    {
        clearLiveReports(record);
        record.powerState.clear();
        record.wakeOnLan.clear();
        record.grantedFormat = AudioFormat();
        record.streamNotReadyReason.clear();
        record.hasReport = false;
    }

    record.connected = true;
    record.session = session.sessionId();
    record.deviceName = session.deviceName();
    record.address = session.peerAddress();
    record.hasCapabilities = session.hasCapabilities();
    record.capabilities = session.capabilities();
    record.hasHardwareVolume = session.hasHardwareVolume();
    record.hardwareVolume = session.hardwareVolume();
    record.driftCorrection = session.driftCorrection();
    record.hardwareClock = session.hardwareClock();
    return arriving;
}

void RoomDirectory::onStreamReady(const ClientSession& session)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = rooms_.find(session.clientId());
    if (found == rooms_.end() || found->second.session != session.sessionId())
        return;

    RoomRecord& record = found->second;
    record.capabilities = session.capabilities(); // the trim confirmed, or not
    record.streamAnswered = true;
    record.streamReady = session.streamReady();
    record.streamStandby = session.streamStandby();
    record.powerState = session.powerState();
    record.wakeOnLan = session.wakeOnLan();
    record.grantedFormat = session.streamReady() ? session.grantedFormat() : AudioFormat();
    record.streamNotReadyReason = session.streamReady() ? std::string()
                                                        : session.streamNotReadyReason();
}

void RoomDirectory::onPlaybackReport(const ClientId& client, const matp::PlaybackReport& report,
                                     uint64_t nowNs)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = rooms_.find(client);
    if (found == rooms_.end())
        return;
    found->second.hasReport = true;
    found->second.lastReport = report;
    found->second.lastReportNs = nowNs;
}

void RoomDirectory::onSystemReport(const ClientSession& session, const SystemMetrics& metrics,
                                   uint64_t nowNs)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = rooms_.find(session.clientId());
    // Only the connection the record holds: a displaced session still closing
    // must not speak for the one that replaced it.
    if (found == rooms_.end() || found->second.session != session.sessionId())
        return;
    found->second.hasSystem = true;
    found->second.system = metrics;
    found->second.systemNs = nowNs;
}

bool RoomDirectory::onAudioDevices(const ClientSession& session,
                                   const matp::AudioDeviceList& devices)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = rooms_.find(session.clientId());
    if (found == rooms_.end() || found->second.session != session.sessionId())
        return false;
    found->second.hasAudioDevices = true;
    found->second.audioDevices = devices;
    return true;
}

bool RoomDirectory::onBufferConfig(const ClientSession& session, uint32_t bufferMs)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = rooms_.find(session.clientId());
    if (found == rooms_.end() || found->second.session != session.sessionId())
        return false;
    found->second.hasBuffer = true;
    found->second.bufferMs = bufferMs;
    return true;
}

bool RoomDirectory::onRoomName(const ClientSession& session, const std::string& name)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = rooms_.find(session.clientId());
    if (found == rooms_.end() || found->second.session != session.sessionId())
        return false;
    found->second.hasName = true;
    found->second.name = name;
    return true;
}

bool RoomDirectory::onVolumeConfig(const ClientSession& session, const matp::VolumeConfig& config)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = rooms_.find(session.clientId());
    if (found == rooms_.end() || found->second.session != session.sessionId())
        return false;
    found->second.hasVolumeConfig = true;
    found->second.volumeConfig = config;
    return true;
}

void RoomDirectory::onInputControls(const ClientSession& session,
                                    const std::vector<matp::InputControl>& inputs, bool deepSleep)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = rooms_.find(session.clientId());
    if (found == rooms_.end() || found->second.session != session.sessionId())
        return;
    found->second.hasInputs = true;
    found->second.inputs = inputs;
    found->second.deepSleep = deepSleep;
}

bool RoomDirectory::onClosed(const ClientSession& session)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = rooms_.find(session.clientId());
    if (found == rooms_.end() || found->second.session != session.sessionId() ||
        !found->second.connected)
    {
        return false;
    }

    RoomRecord& record = found->second;
    record.connected = false;
    clearLiveReports(record);
    // An announced deep sleep is why it went away; that stays true until the
    // room's next session.
    if (record.powerState != "sleep" && record.powerState != "sleepButtonOnly")
    {
        record.powerState.clear();
        record.wakeOnLan.clear();
    }

    // A sleeping room is kept within the ceiling and not past it: the
    // listener bounds the rooms connected at once, not how many distinct ones
    // connect, say they are going to sleep and go.
    if (!configured(record) && !makeRoomLocked(0, {record.client}))
    {
        record.powerState.clear();
        record.wakeOnLan.clear();
    }
    pruneLocked(found);
    return true;
}

bool RoomDirectory::update(const ClientId& client,
                           const std::function<void(RoomRecord&)>& change)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = rooms_.find(client);
    if (found == rooms_.end())
    {
        if (!makeRoomLocked(1, {}))
            return false;
        found = rooms_.emplace(client, RoomRecord{}).first;
        found->second.client = client;
    }

    change(found->second);
    pruneLocked(found);
    return true;
}

bool RoomDirectory::updateAll(const std::vector<ClientId>& clients,
                              const std::function<void(RoomRecord&)>& change)
{
    std::lock_guard<std::mutex> lock(mutex_);
    size_t missing = 0;
    for (const ClientId& client : clients)
        missing += rooms_.contains(client) ? 0 : 1;
    // Only a record this would make is refused: a directory already past its
    // ceiling, through rooms that connected, still takes changes to the
    // records it holds.
    if (missing != 0 && !makeRoomLocked(missing, clients))
        return false;

    for (const ClientId& client : clients)
    {
        const auto found = rooms_.try_emplace(client).first;
        found->second.client = client;
        change(found->second);
        pruneLocked(found);
    }
    return true;
}

bool RoomDirectory::forget(const ClientId& client)
{
    std::lock_guard<std::mutex> lock(mutex_);
    return rooms_.erase(client) > 0;
}

std::optional<RoomRecord> RoomDirectory::find(const ClientId& client) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = rooms_.find(client);
    if (found == rooms_.end())
        return std::nullopt;
    return found->second;
}

std::vector<RoomRecord> RoomDirectory::rooms() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<RoomRecord> out;
    out.reserve(rooms_.size());
    for (const auto& entry : rooms_)
        out.push_back(entry.second);
    return out;
}

ZoneId RoomDirectory::zoneFor(const ClientId& client) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = rooms_.find(client);
    return found == rooms_.end() ? ZoneId() : found->second.zone;
}

RoomAudioSettings RoomDirectory::preference(const ClientId& client) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    RoomAudioSettings wish = defaults_;
    const auto found = rooms_.find(client);
    if (found != rooms_.end())
    {
        wish.audioControl = found->second.audioControl.value_or(wish.audioControl);
        wish.grouped = found->second.grouped;
        wish.equalized = found->second.equalizer && found->second.equalizer->enabled;
    }
    return wish;
}

size_t RoomDirectory::size() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return rooms_.size();
}

void RoomDirectory::pruneLocked(std::map<ClientId, RoomRecord>::iterator entry)
{
    const RoomRecord& record = entry->second;
    // A room asleep on purpose is kept so it still says so.
    if (!record.connected && !configured(record) && record.powerState.empty())
        rooms_.erase(entry);
}

bool RoomDirectory::makeRoomLocked(size_t needed, const std::vector<ClientId>& keep)
{
    if (rooms_.size() + needed <= maxRooms_)
        return true;

    // What may go: a sleeping room's record and nothing else, because nothing
    // was asked of it and its connection has gone.
    const auto spare = [&keep](const RoomRecord& record) {
        return !record.connected && !configured(record) &&
               std::find(keep.begin(), keep.end(), record.client) == keep.end();
    };
    size_t evictable = 0;
    for (const auto& entry : rooms_)
        evictable += spare(entry.second) ? 1 : 0;
    if (rooms_.size() - evictable + needed > maxRooms_)
        return false; // nothing is evicted for a refusal

    for (auto entry = rooms_.begin(); entry != rooms_.end() && rooms_.size() + needed > maxRooms_;)
        entry = spare(entry->second) ? rooms_.erase(entry) : std::next(entry);
    return true;
}

} // namespace multiroom
