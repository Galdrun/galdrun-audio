#include "zone/ZoneManager.h"

#include "util/Logger.h"

#include <utility>

namespace multiroom {

ZoneManager::ZoneManager(AudioMemory& memory, const SharedPlanner& planner,
                         const ResamplerRegistry& registry, IDecoderFactory& decoders,
                         const IClock& clock, ZoneSettings defaults, size_t maxZones)
    : memory_(memory), planner_(planner), registry_(registry), decoders_(decoders), clock_(clock),
      defaults_(std::move(defaults)), maxZones_(maxZones)
{
    timing_.minimumStartBufferNs = defaults_.start.minimumStartBufferNs;
    timing_.readyTimeoutNs = defaults_.start.readyTimeoutNs;
    timing_.startDelayNs = defaults_.start.startDelayNs;
    timing_.seekFlushLeadNs = defaults_.seekFlushLeadNs;
    timing_.seekFillLeadNs = defaults_.seekFillLeadNs;
}

ZoneManager::~ZoneManager()
{
    stop();
}

void ZoneManager::setTiming(const ZoneTiming& timing)
{
    std::vector<std::shared_ptr<Zone>> zones;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        timing_ = timing;
        for (const auto& entry : zones_)
            zones.push_back(entry.second);
    }
    // Outside the map's lock: each zone takes its own.
    for (const std::shared_ptr<Zone>& zone : zones)
        zone->setTiming(timing);
}

ZoneTiming ZoneManager::timing() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return timing_;
}

Result<std::shared_ptr<Zone>, ZoneError> ZoneManager::create(const ZoneId& id)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (zones_.find(id) != zones_.end())
            return fail(ZoneError::AlreadyExists, "zone " + id.str() + " already exists");
        if (zones_.size() >= maxZones_)
            return fail(ZoneError::Full,
                        "this server holds " + std::to_string(maxZones_) + " zones");
    }

    // Built outside the lock: creating a zone allocates its store, and nothing
    // about that needs the map held.
    ZoneSettings settings = defaults_;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        settings.start.minimumStartBufferNs = timing_.minimumStartBufferNs;
        settings.start.readyTimeoutNs = timing_.readyTimeoutNs;
        settings.start.startDelayNs = timing_.startDelayNs;
        settings.seekFlushLeadNs = timing_.seekFlushLeadNs;
        settings.seekFillLeadNs = timing_.seekFillLeadNs;
    }
    Result<std::unique_ptr<Zone>, ZoneError> zone =
        Zone::create(id, memory_, planner_, registry_, decoders_, clock_, settings);
    if (!zone)
        return std::unexpected(zone.error());

    std::shared_ptr<Zone> shared(std::move(*zone));
    {
        // Both checks again: another create() may have inserted while this
        // zone was being built.
        std::lock_guard<std::mutex> lock(mutex_);
        if (zones_.size() >= maxZones_ && zones_.find(id) == zones_.end())
            return fail(ZoneError::Full,
                        "this server holds " + std::to_string(maxZones_) + " zones");
        if (!zones_.emplace(id, shared).second)
            return fail(ZoneError::AlreadyExists, "zone " + id.str() + " already exists");
    }

    MULTIROOM_INFO("zone {} created", id.str());
    return shared;
}

std::shared_ptr<Zone> ZoneManager::find(const ZoneId& id) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = zones_.find(id);
    return found == zones_.end() ? nullptr : found->second;
}

bool ZoneManager::remove(const ZoneId& id)
{
    std::shared_ptr<Zone> ending;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = zones_.find(id);
        if (found == zones_.end())
            return false;
        ending = std::move(found->second);
        zones_.erase(found);

        for (auto entry = membership_.begin(); entry != membership_.end();)
            entry = entry->second == id ? membership_.erase(entry) : std::next(entry);
    }

    // Outside the lock: this joins the zone's pipeline thread and every one of
    // its rooms.
    ending->stop();
    MULTIROOM_INFO("zone {} removed", id.str());
    return true;
}

std::vector<ZoneId> ZoneManager::ids() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ZoneId> out;
    out.reserve(zones_.size());
    for (const auto& entry : zones_)
        out.push_back(entry.first);
    return out;
}

size_t ZoneManager::size() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return zones_.size();
}

// ---------------------------------------------------------------------------

Status<ZoneError> ZoneManager::assign(const ZoneId& zoneId, const SessionId& session,
                                      const ClientId& client,
                                      const ClientCapabilities& capabilities,
                                      const AudioFormat& grantedFormat)
{
    std::shared_ptr<Zone> zone = find(zoneId);
    if (zone == nullptr)
        return fail(ZoneError::NotPlaying, "no zone " + zoneId.str());

    // Membership is exclusive, so a room moving between zones leaves the first
    // one before it joins the second -- and a room re-assigned to the zone it
    // is already in is a rejoin with a new session, which is the reconnect of
    // section 43 seen from here.
    release(client);

    if (const auto admitted = zone->admit(session, client, capabilities, grantedFormat);
        !admitted)
    {
        return admitted;
    }

    {
        // Removed while it admitted the room: remove() has already stopped it
        // and cleared its memberships, so this one is not recorded either.
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = zones_.find(zoneId);
        if (found != zones_.end() && found->second == zone)
        {
            membership_[client] = zoneId;
            return {};
        }
    }
    static_cast<void>(zone->release(client));
    return fail(ZoneError::NotPlaying, "zone " + zoneId.str() + " was removed");
}

void ZoneManager::release(const ClientId& client)
{
    std::shared_ptr<Zone> zone;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = membership_.find(client);
        if (found == membership_.end())
            return;
        const auto inZone = zones_.find(found->second);
        if (inZone != zones_.end())
            zone = inZone->second;
        membership_.erase(found);
    }

    if (zone != nullptr)
        static_cast<void>(zone->release(client));
}

void ZoneManager::disconnect(const ClientId& client, const std::optional<SessionId>& session)
{
    std::shared_ptr<Zone> zone;
    ZoneId zoneId;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = membership_.find(client);
        if (found == membership_.end())
            return;
        zoneId = found->second;
        const auto inZone = zones_.find(found->second);
        if (inZone != zones_.end())
            zone = inZone->second;
    }

    // A successor session holds the room: its membership is not this one's.
    if (zone != nullptr && !zone->disconnect(client, session))
        return;

    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = membership_.find(client);
    if (found != membership_.end() && found->second == zoneId)
        membership_.erase(found);
}

std::shared_ptr<Zone> ZoneManager::zoneOf(const ClientId& client) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = membership_.find(client);
    if (found == membership_.end())
        return nullptr;
    const auto zone = zones_.find(found->second);
    return zone == zones_.end() ? nullptr : zone->second;
}

void ZoneManager::stop()
{
    std::vector<std::shared_ptr<Zone>> ending;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ending.reserve(zones_.size());
        for (const auto& entry : zones_)
            ending.push_back(entry.second);
    }

    for (const auto& zone : ending)
        zone->stop();
}

} // namespace multiroom
