#include "control/RoomDirectory.h"

#include <utility>

namespace multiroom {

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
        record.streamAnswered = false;
        record.streamReady = false;
        record.streamStandby = false;
        record.powerState.clear();
        record.wakeOnLan.clear();
        record.grantedFormat = AudioFormat();
        record.streamNotReadyReason.clear();
        record.hasReport = false;
        record.hasSystem = false;
        record.hasAudioDevices = false;
        record.hasBuffer = false;
        record.hasVolumeConfig = false;
        record.hasName = false;
        record.hasInputs = false;
        record.inputs.clear();
        record.deepSleep = false;
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
    record.streamAnswered = false;
    record.streamReady = false;
    record.streamStandby = false;
    // An announced deep sleep is why it went away; that stays true until the
    // room's next session.
    if (record.powerState != "sleep" && record.powerState != "sleepButtonOnly")
    {
        record.powerState.clear();
        record.wakeOnLan.clear();
    }
    record.hasSystem = false;
    record.hasAudioDevices = false;
    record.hasBuffer = false;
    record.hasVolumeConfig = false;
    record.hasName = false;
    record.hasInputs = false;
    record.inputs.clear();
    record.deepSleep = false;
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
        if (rooms_.size() >= maxRooms_)
            return false;
        found = rooms_.emplace(client, RoomRecord{}).first;
        found->second.client = client;
    }

    change(found->second);
    pruneLocked(found);
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
    if (!record.connected && record.zone.empty() && !record.audioControl &&
        !record.grouped && !record.volume && !record.muted && !record.outputLatencyMs &&
        !record.equalizer && record.powerState.empty())
    {
        rooms_.erase(entry);
    }
}

} // namespace multiroom
