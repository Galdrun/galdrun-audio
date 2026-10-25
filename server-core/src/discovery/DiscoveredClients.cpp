#include "discovery/DiscoveredClients.h"

#include <algorithm>

namespace multiroom {

bool isPresent(const DiscoveredClient& client, uint64_t nowNs) noexcept
{
    return nowNs >= client.lastSeenNs && nowNs - client.lastSeenNs <= kDiscoveryPresentWithinNs;
}

DiscoveredClients::DiscoveredClients(size_t maxClients) : maxClients_(std::max<size_t>(maxClients, 1))
{
}

bool DiscoveredClients::onBeacon(const matp::ClientBeacon& beacon, const std::string& address,
                                 uint64_t nowNs)
{
    const ClientId id(beacon.clientId);
    std::lock_guard<std::mutex> lock(mutex_);

    auto found = clients_.find(id);
    const bool fresh = found == clients_.end();
    if (fresh)
    {
        if (clients_.size() >= maxClients_)
        {
            const auto oldest = std::min_element(
                clients_.begin(), clients_.end(), [](const auto& a, const auto& b) {
                    return a.second.lastSeenNs < b.second.lastSeenNs;
                });
            clients_.erase(oldest);
            ++forgotten_;
        }
        DiscoveredClient entry;
        entry.client = id;
        entry.firstSeenNs = nowNs;
        found = clients_.emplace(id, std::move(entry)).first;
    }

    DiscoveredClient& entry = found->second;
    entry.active = beacon.active;
    entry.address = address;
    entry.controlPort = beacon.controlPort;
    entry.protocolVersion = beacon.protocolVersion;
    entry.name = beacon.name;
    entry.clientVersion = beacon.clientVersion;
    entry.platform = beacon.platform;
    entry.host = beacon.host;
    entry.server = beacon.server;
    entry.lastSeenNs = nowNs;
    ++entry.beacons;
    return fresh;
}

std::vector<DiscoveredClient> DiscoveredClients::clients() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<DiscoveredClient> out;
    out.reserve(clients_.size());
    for (const auto& [id, entry] : clients_)
        out.push_back(entry);
    return out;
}

std::optional<DiscoveredClient> DiscoveredClients::find(const ClientId& client) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = clients_.find(client);
    if (found == clients_.end())
        return std::nullopt;
    return found->second;
}

size_t DiscoveredClients::size() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return clients_.size();
}

uint64_t DiscoveredClients::forgotten() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return forgotten_;
}

} // namespace multiroom
